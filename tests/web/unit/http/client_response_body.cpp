#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <future>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <asio/co_spawn.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/post.hpp>
#include <asio/read.hpp>
#include <asio/read_until.hpp>
#include <asio/redirect_error.hpp>
#include <asio/ssl/stream.hpp>
#include <asio/steady_timer.hpp>
#include <asio/streambuf.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/write.hpp>

#include "ruvia/core/asio_task.h"
#include "ruvia/core/async.h"
#include "ruvia/core/event_loop_attachment.h"
#include "ruvia/core/task_scope.h"
#include "ruvia/core/timer.h"
#include "ruvia/core/worker_signal.h"
#include "ruvia/http/http2_connection.h"
#include "ruvia/http/http2_framing.h"
#include "ruvia/http/http_content_codec.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_response_server.h"
#include "ruvia/http/http_response_stream.h"
#include "ruvia/web/http_client.h"
#include "ruvia/web/http_client_response.h"
#include "ruvia/web/http_client_types.h"

#include "test_harness.h"
#include "test_io_context.h"
#include "test_tls_identity.h"

namespace {
class test_worker final {
public:
    explicit test_worker(asio::io_context& io)
        : attachment_(ruvia::attach_event_loop(io, {.queue_capacity_ = 8})),
          handle_(attachment_.loop().handle()) {}

    ruvia::event_loop_attachment attachment_;
    ruvia::worker_handle handle_;
};

class loopback_response_server final {
public:
    loopback_response_server(asio::io_context& io_context, const ruvia::worker_handle& worker_value,
        std::vector<std::string> bodies,
        std::chrono::milliseconds response_delay = std::chrono::milliseconds::zero(),
        bool close_without_response = false,
        std::size_t responses_before_close = 0)
        : io_context_(io_context),
          acceptor_(io_context, {asio::ip::tcp::v4(), std::uint16_t{0}}),
          done_(worker_value),
          response_delay_(response_delay),
          close_without_response_(close_without_response),
          responses_before_close_(responses_before_close) {
        responses_.reserve(bodies.size());
        for (const auto& body : bodies) {
            responses_.push_back("HTTP/1.1 200 OK\r\nContent-Length: " +
                                 std::to_string(body.size()) +
                                 "\r\nX-Peer: retained\r\nConnection: close\r\n\r\n" + body);
        }
    }

    [[nodiscard]] std::uint16_t port() const {
        return acceptor_.local_endpoint().port();
    }

    void start() {
        accept_next();
    }

    [[nodiscard]] ruvia::task<void> wait() {
        co_await done_.wait();
        if (failure_ != nullptr) {
            std::rethrow_exception(failure_);
        }
    }

private:
    void accept_next() {
        auto socket = std::make_shared<asio::ip::tcp::socket>(io_context_);
        acceptor_.async_accept(*socket, [this, socket](const std::error_code& error) {
            if (error) {
                fail(error);
                return;
            }
            auto request = std::make_shared<asio::streambuf>();
            asio::async_read_until(*socket, *request, "\r\n\r\n",
                [this, socket, request](const std::error_code& read_error, std::size_t) {
                    if (read_error) {
                        fail(read_error);
                        return;
                    }
                    if (close_without_response_ && next_response_ >= responses_before_close_) {
                        std::error_code ignored;
                        socket->close(ignored);
                        done_.notify();
                        return;
                    }
                    const auto response_index = next_response_++;
                    auto write_response = [this, socket, response_index] {
                        asio::async_write(*socket, asio::buffer(responses_[response_index]),
                            [this, socket](const std::error_code& write_error, std::size_t) {
                                if (write_error) {
                                    fail(write_error);
                                    return;
                                }
                                if (next_response_ == responses_.size()) {
                                    done_.notify();
                                } else {
                                    accept_next();
                                }
                            });
                    };
                    if (response_delay_ == std::chrono::milliseconds::zero()) {
                        write_response();
                    } else {
                        auto timer = std::make_shared<asio::steady_timer>(io_context_, response_delay_);
                        timer->async_wait([timer, write_response = std::move(write_response)](
                                              const std::error_code& timer_error) mutable {
                            if (!timer_error) {
                                write_response();
                            }
                        });
                    }
                });
        });
    }

    void fail(const std::error_code& error) {
        if (failure_ == nullptr) {
            failure_ = std::make_exception_ptr(std::system_error(error));
            done_.notify();
        }
    }

    asio::io_context& io_context_;
    asio::ip::tcp::acceptor acceptor_;
    std::vector<std::string> responses_;
    ruvia::worker_signal done_;
    std::exception_ptr failure_;
    std::size_t next_response_{0};
    std::chrono::milliseconds response_delay_;
    bool close_without_response_;
    std::size_t responses_before_close_;
};

[[nodiscard]] ruvia::http_client_config local_http_client_config(std::uint16_t port) {
    return ruvia::http_client_config{.scheme_ = ruvia::http_scheme::http,
        .host_ = "127.0.0.1",
        .port_ = port,
        .protocol_ = ruvia::http_client_protocol::http1_only};
}

class upload_peer final {
public:
    enum class mode_type { chunked,
        known_length,
        continue_value,
        continue_timeout,
        early_final };
    upload_peer(asio::io_context& io, const ruvia::worker_handle& worker_value, mode_type mode)
        : io_(io),
          acceptor_(io, {asio::ip::tcp::v4(), std::uint16_t{0}}),
          done_(worker_value),
          mode_(mode) {}
    std::uint16_t port() const {
        return acceptor_.local_endpoint().port();
    }
    void start() {
        asio::co_spawn(io_, serve(), [this](std::exception_ptr error) { failure_ = error; complete_ = true; done_.notify(); });
    }
    ruvia::task<void> wait() {
        while (!complete_) {
            co_await done_.wait();
        }
        if (failure_) {
            std::rethrow_exception(failure_);
        }
    }
    std::string head_;
    std::string body_;

private:
    asio::awaitable<void> serve() {
        asio::ip::tcp::socket socket(io_);
        co_await acceptor_.async_accept(socket, asio::use_awaitable);
        std::string input;
        const auto head_size = co_await asio::async_read_until(socket, asio::dynamic_buffer(input), "\r\n\r\n", asio::use_awaitable);
        head_.assign(input.data(), head_size);
        input.erase(0, head_size);
        if (mode_ == mode_type::early_final) {
            constexpr std::string_view reply = "HTTP/1.1 413 Content Too Large\r\nContent-Length: 2\r\nConnection: close\r\n\r\nno";
            co_await asio::async_write(socket, asio::buffer(reply), asio::use_awaitable);
            co_return;
        }
        if (mode_ == mode_type::continue_value) {
            if (!input.empty()) {
                throw std::runtime_error("upload arrived before 100 Continue");
            }
            constexpr std::string_view reply = "HTTP/1.1 103 Early Hints\r\nLink: </asset>; rel=preload\r\n\r\nHTTP/1.1 100 Continue\r\n\r\n";
            co_await asio::async_write(socket, asio::buffer(reply), asio::use_awaitable);
        }
        if (mode_ == mode_type::known_length) {
            if (input.size() < 6) {
                co_await asio::async_read(socket, asio::dynamic_buffer(input), asio::transfer_exactly(6 - input.size()), asio::use_awaitable);
            }
        } else if (input.find("0\r\nx-end: retained\r\n\r\n") == std::string::npos) {
            co_await asio::async_read_until(socket, asio::dynamic_buffer(input), "0\r\nx-end: retained\r\n\r\n", asio::use_awaitable);
        }
        body_ = std::move(input);
        if (mode_ == mode_type::known_length) {
            asio::steady_timer completed(io_, std::chrono::milliseconds(2));
            co_await completed.async_wait(asio::use_awaitable);
        }
        constexpr std::string_view reply = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok";
        co_await asio::async_write(socket, asio::buffer(reply), asio::use_awaitable);
    }
    asio::io_context& io_;
    asio::ip::tcp::acceptor acceptor_;
    ruvia::worker_signal done_;
    mode_type mode_;
    std::exception_ptr failure_;
    bool complete_{};
};

class http2_upload_peer final {
public:
    http2_upload_peer(asio::io_context& io, const ruvia::worker_handle& worker_value, bool early)
        : io_(io),
          acceptor_(io, {asio::ip::tcp::v4(), std::uint16_t{0}}),
          done_(worker_value),
          early_(early) {}
    std::uint16_t port() const {
        return acceptor_.local_endpoint().port();
    }
    void start() {
        asio::co_spawn(io_, serve(), [this](std::exception_ptr error) { failure_ = error; complete_ = true; done_.notify(); });
    }
    ruvia::task<void> wait() {
        while (!complete_) {
            co_await done_.wait();
        }
        if (failure_) {
            std::rethrow_exception(failure_);
        }
    }
    std::string body_;
    std::string trailer_;

private:
    asio::awaitable<void> flush(asio::ip::tcp::socket& socket, ruvia::http2_connection& connection) {
        const auto output = connection.pending_output();
        if (!output.empty()) {
            co_await asio::async_write(socket, asio::buffer(output), asio::use_awaitable);
            (void)connection.consume_output(output.size());
        }
    }
    asio::awaitable<void> serve() {
        auto socket = co_await acceptor_.async_accept(asio::use_awaitable);
        auto connection = ruvia::http2_connection::server();
        std::optional<ruvia::http2_request_head_event> lease;
        std::uint32_t stream{};
        bool ended{};
        co_await flush(socket, connection);
        std::array<char, ruvia::http2_client_preface.size()> preface{};
        co_await asio::async_read(socket, asio::buffer(preface), asio::use_awaitable);
        if (connection.feed(std::string_view(preface.data(), preface.size())) != ruvia::http2_feed_result::accepted) {
            throw std::runtime_error("invalid client preface");
        }
        while (!ended) {
            std::array<char, ruvia::http2_frame_header_bytes> header_value{};
            co_await asio::async_read(socket, asio::buffer(header_value), asio::use_awaitable);
            const auto parsed_value = ruvia::parse_http2_frame_header(header_value);
            std::string frame(header_value.data(), header_value.size());
            frame.resize(header_value.size() + parsed_value->length_);
            if (parsed_value->length_ != 0) {
                co_await asio::async_read(socket, asio::buffer(frame.data() + header_value.size(), parsed_value->length_), asio::use_awaitable);
            }
            if (connection.feed(frame) != ruvia::http2_feed_result::accepted) {
                throw std::runtime_error("HTTP/2 upload peer protocol error");
            }
            while (auto event = connection.next_event()) {
                if (auto* head = event->request_head()) {
                    stream = head->stream_id();
                    lease.emplace(std::move(*head));
                    if (early_) {
                        ended = true;
                    } else {
                        const std::array<ruvia::http_header_view, 1> links{{{"link", "</asset>; rel=preload"}}};
                        if (connection.submit_interim_response_head(stream, ruvia::http_interim_response_head(ruvia::http_status_code::from_value(103), links)) != ruvia::http2_submit_status::accepted ||
                            connection.submit_interim_response_head(stream, ruvia::http_interim_response_head(ruvia::http_status_code::from_value(100))) != ruvia::http2_submit_status::accepted) {
                            throw std::runtime_error("HTTP/2 upload peer interim error");
                        }
                    }
                }
                if (auto* chunk = event->message_body_chunk()) {
                    body_.append(chunk->bytes());
                }
                if (const auto* end = event->message_end()) {
                    ended = true;
                    if (!end->trailers().empty()) {
                        trailer_ = end->trailers()[0].value();
                    }
                }
            }
            co_await flush(socket, connection);
        }
        ruvia::http_response response;
        response.status(ruvia::http_status_code::from_value(early_ ? 413 : 200));
        if (connection.submit_streaming_response_head(stream, std::move(response)) != ruvia::http2_submit_status::accepted ||
            connection.submit_data(stream, "ok", ruvia::http2_end_stream::end_stream) != ruvia::http2_data_submit_status::accepted) {
            throw std::runtime_error("HTTP/2 upload peer final error");
        }
        co_await flush(socket, connection);
        // Keep the multiplexed transport alive until its explicit client shutdown.
        std::array<char, 1024> ignored{};
        std::error_code closed;
        while (!closed) {
            co_await socket.async_read_some(asio::buffer(ignored), asio::redirect_error(asio::use_awaitable, closed));
        }
    }
    asio::io_context& io_;
    asio::ip::tcp::acceptor acceptor_;
    ruvia::worker_signal done_;
    bool early_{};
    bool complete_{};
    std::exception_ptr failure_;
};

[[nodiscard]] std::string large_gzip_response_body() {
    const std::string body(128 * 1024, 'z');
    auto result_value = ruvia::encode_http_content(ruvia::http_content_coding::gzip, body,
        {.max_encoded_bytes_ = body.size()});
    if (result_value.encoded() == nullptr) {
        throw std::runtime_error("could not encode fake HTTP response body");
    }
    auto encoded = std::move(*result_value.encoded()).take_bytes();
    return std::string(encoded.data(), encoded.size());
}

class http2_partial_body_peer final {
public:
    explicit http2_partial_body_peer(asio::io_context& io, const ruvia::worker_handle& worker_value,
        std::string body = "partial-body", std::string long_header = {}, std::string long_trailer = {}, bool advertisements = false)
        : io_(io),
          acceptor_(io, {asio::ip::make_address("127.0.0.1"), 0}),
          response_ready_(worker_value),
          body_(std::move(body)),
          long_header_(std::move(long_header)),
          long_trailer_(std::move(long_trailer)),
          advertisements_(advertisements),
          response_sent_(response_sent_promise_.get_future()) {}

    void start() {
        asio::co_spawn(io_, serve(), [this](std::exception_ptr failure) {
            failure_ = failure;
            if (failure_ != nullptr) {
                response_ready_published_ = true;
                response_ready_.notify();
            }
        });
    }

    [[nodiscard]] std::uint16_t port() const {
        return acceptor_.local_endpoint().port();
    }

    [[nodiscard]] ruvia::task<void> wait_for_response() {
        while (!response_ready_published_) {
            co_await response_ready_.wait();
        }
        if (failure_ != nullptr) {
            std::rethrow_exception(failure_);
        }
    }

    void wait_for_partial_response() {
        if (response_sent_.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
            throw std::runtime_error("HTTP/2 peer did not send the partial response");
        }
        response_sent_.get();
    }

    [[nodiscard]] bool observed_client_close() const noexcept {
        return observed_client_close_;
    }

    [[nodiscard]] ruvia::task<ruvia::http_priority> wait_for_priority() {
        while (!priority_observed_ && !observed_client_close_ && failure_ == nullptr) {
            co_await response_ready_.wait();
        }
        rethrow_failure();
        if (!priority_observed_) {
            throw std::runtime_error("HTTP/2 connection closed before priority update");
        }
        co_return *priority_observed_;
    }

    void rethrow_failure() const {
        if (failure_ != nullptr) {
            std::rethrow_exception(failure_);
        }
    }

private:
    asio::awaitable<void> send_pending(asio::ip::tcp::socket& socket,
        ruvia::http2_connection& connection) {
        const auto output = connection.pending_output();
        if (!output.empty()) {
            co_await asio::async_write(socket, asio::buffer(output), asio::use_awaitable);
            (void)connection.consume_output(output.size());
        }
    }

    asio::awaitable<void> serve() {
        auto socket = co_await acceptor_.async_accept(asio::use_awaitable);
        auto connection = ruvia::http2_connection::server();
        co_await send_pending(socket, connection);

        std::array<char, ruvia::http2_client_preface.size()> preface{};
        co_await asio::async_read(socket, asio::buffer(preface), asio::use_awaitable);
        if (connection.feed(std::string_view(preface.data(), preface.size())) !=
            ruvia::http2_feed_result::accepted) {
            throw std::runtime_error("HTTP/2 peer rejected the client preface");
        }

        std::uint32_t request_stream = 0;
        std::string request_frame;
        std::optional<ruvia::http2_request_head_event> request_lease;
        while (request_stream == 0) {
            std::array<char, ruvia::http2_frame_header_bytes> header_value{};
            co_await asio::async_read(socket, asio::buffer(header_value), asio::use_awaitable);
            const auto frame_header = ruvia::parse_http2_frame_header(std::span<const char>(header_value));
            if (!frame_header.has_value()) {
                throw std::runtime_error("HTTP/2 peer received an incomplete frame header");
            }
            request_frame.assign(header_value.data(), header_value.size());
            request_frame.resize(header_value.size() + frame_header->length_);
            if (frame_header->length_ != 0) {
                co_await asio::async_read(socket,
                    asio::buffer(request_frame.data() + header_value.size(), frame_header->length_),
                    asio::use_awaitable);
            }
            if (connection.feed(request_frame) == ruvia::http2_feed_result::protocol_failure) {
                throw std::runtime_error("HTTP/2 peer rejected client frames");
            }
            while (auto event = connection.next_event()) {
                if (auto* request = event->request_head()) {
                    request_stream = request->stream_id();
                    request_lease.emplace(std::move(*request));
                }
            }
            co_await send_pending(socket, connection);
        }

        ruvia::http_response response;
        response.status(ruvia::http_status::ok);
        if (advertisements_ && connection.submit_alternative_service_advertisement(request_stream, {}, "h3=\":443\"; ma=60") != ruvia::http2_submit_status::accepted) {
            throw std::runtime_error("HTTP/2 peer could not submit ALTSVC");
        }
        if (!long_header_.empty()) {
            response.header("x-long", long_header_);
        }
        if (long_trailer_.empty()) {
            if (connection.submit_streaming_response_head(request_stream, std::move(response)) !=
                ruvia::http2_submit_status::accepted) {
                throw std::runtime_error("HTTP/2 peer could not submit response headers");
            }
        } else {
            const auto submitted = connection.submit_streaming_response_head(request_stream,
                std::move(response), ruvia::http_response_stream_kind::generic,
                ruvia::http_response_trailer_intent::present);
            if (submitted.submitted() == nullptr) {
                throw std::runtime_error("HTTP/2 peer could not submit response headers");
            }
        }
        if (connection.submit_data(request_stream, body_, ruvia::http2_end_stream::keep_open) !=
            ruvia::http2_data_submit_status::accepted) {
            throw std::runtime_error("HTTP/2 peer could not submit response data");
        }
        if (!long_trailer_.empty()) {
            const std::array trailers{ruvia::http_header_view("x-long-trailer", long_trailer_)};
            const auto section = ruvia::validate_http_response_trailers(trailers);
            if (connection.finish_response(request_stream, section) !=
                ruvia::http2_finish_response_status::accepted) {
                throw std::runtime_error("HTTP/2 peer could not submit response trailers");
            }
        }
        // Keep the request lease until the client closes. Releasing it early
        // would invalidate the server-side request while its connection is active.
        co_await send_pending(socket, connection);
        response_ready_published_ = true;
        response_ready_.notify();
        response_sent_promise_.set_value();

        std::array<char, 1024> input{};
        std::error_code error;
        while (const auto received = co_await socket.async_read_some(asio::buffer(input),
                   asio::redirect_error(asio::use_awaitable, error))) {
            if (connection.feed(std::string_view(input.data(), received)) == ruvia::http2_feed_result::protocol_failure) {
                throw std::runtime_error("HTTP/2 peer rejected late control input");
            }
            while (auto event = connection.next_event()) {
                if (const auto* update = event->priority_update()) {
                    priority_observed_ = update->fields_.request_priority();
                    response_ready_.notify();
                }
            }
            co_await send_pending(socket, connection);
        }
        observed_client_close_ = static_cast<bool>(error);
        response_ready_.notify();
        if (!request_lease.has_value() || connection.release(std::move(*request_lease)) !=
                                              ruvia::http2_server_request_release_status::released) {
            throw std::runtime_error("HTTP/2 peer could not release its request lease");
        }
        request_frame.clear();
    }

    asio::io_context& io_;
    asio::ip::tcp::acceptor acceptor_;
    ruvia::worker_signal response_ready_;
    std::string body_;
    std::string long_header_;
    std::string long_trailer_;
    bool advertisements_{};
    std::promise<void> response_sent_promise_;
    std::future<void> response_sent_;
    std::exception_ptr failure_;
    bool response_ready_published_{false};
    bool observed_client_close_{false};
    std::optional<ruvia::http_priority> priority_observed_{};
};

class http1_chunked_response_peer final {
public:
    enum class end_mode_type : unsigned char { terminal_gate,
        wait_for_client_close };

    explicit http1_chunked_response_peer(asio::io_context& io, std::string encoded,
        end_mode_type end_mode = end_mode_type::terminal_gate, std::string long_header = {})
        : io_(io),
          acceptor_(io, {asio::ip::make_address("127.0.0.1"), 0}),
          gate_(io),
          encoded_(std::move(encoded)),
          long_header_(std::move(long_header)),
          end_mode_(end_mode),
          done_(done_promise_.get_future()) {}

    void start() {
        asio::co_spawn(io_, serve(), [this](std::exception_ptr failure) {
            failure_ = failure;
            done_promise_.set_value();
        });
    }

    [[nodiscard]] std::uint16_t port() const {
        return acceptor_.local_endpoint().port();
    }

    void release_terminal_chunk() {
        release_requested_ = true;
        gate_.cancel();
    }

    [[nodiscard]] bool observed_client_close() const noexcept {
        return observed_client_close_;
    }

    void wait() {
        if (done_.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
            throw std::runtime_error("HTTP/1 peer did not finish");
        }
        done_.get();
        if (failure_ != nullptr) {
            std::rethrow_exception(failure_);
        }
    }

private:
    asio::awaitable<void> serve() {
        auto socket = co_await acceptor_.async_accept(asio::use_awaitable);
        asio::streambuf request;
        co_await asio::async_read_until(socket, request, "\r\n\r\n", asio::use_awaitable);
        std::array<char, 2 * sizeof(std::size_t)> chunk_size{};
        const auto [chunk_end, chunk_error] = std::to_chars(
            chunk_size.data(), chunk_size.data() + chunk_size.size(), encoded_.size(), 16);
        if (chunk_error != std::errc{}) {
            throw std::runtime_error("HTTP/1 peer could not format chunk size");
        }
        std::string response =
            "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, chunked\r\n"
            "Trailer: X-End\r\nConnection: close\r\n";
        if (!long_header_.empty()) {
            response += "X-Long: " + long_header_ + "\r\n";
        }
        response += "\r\n";
        response.append(chunk_size.data(), chunk_end);
        response += "\r\n" + encoded_ + "\r\n";
        co_await asio::async_write(socket, asio::buffer(response), asio::use_awaitable);

        if (end_mode_ == end_mode_type::wait_for_client_close) {
            std::array<char, 1024> input{};
            std::error_code error;
            while (co_await socket.async_read_some(asio::buffer(input),
                asio::redirect_error(asio::use_awaitable, error))) {
            }
            observed_client_close_ = error == asio::error::eof ||
                                     error == asio::error::connection_reset ||
                                     error == asio::error::operation_aborted;
            co_return;
        }

        if (!release_requested_) {
            gate_.expires_after(std::chrono::seconds(5));
            std::error_code gate_error;
            co_await gate_.async_wait(asio::redirect_error(asio::use_awaitable, gate_error));
            if (gate_error != asio::error::operation_aborted) {
                throw std::runtime_error("HTTP/1 terminal-chunk gate expired");
            }
        }

        constexpr std::string_view terminal = "0\r\nX-End: retained\r\n\r\n";
        co_await asio::async_write(socket, asio::buffer(terminal), asio::use_awaitable);
        std::error_code ignored;
        socket.shutdown(asio::ip::tcp::socket::shutdown_send, ignored);
    }

    asio::io_context& io_;
    asio::ip::tcp::acceptor acceptor_;
    asio::steady_timer gate_;
    std::string encoded_;
    std::string long_header_;
    end_mode_type end_mode_;
    std::promise<void> done_promise_;
    std::future<void> done_;
    std::exception_ptr failure_;
    bool release_requested_{false};
    bool observed_client_close_{false};
};

template <typename operation_type>
void run_operation(test_worker& worker_value, asio::io_context& io, operation_type&& operation) {
    std::exception_ptr failure;
    auto run = [&]() -> ruvia::task<void> {
        try {
            co_await operation();
        } catch (...) {
            failure = std::current_exception();
        }
        worker_value.attachment_.stop();
    };
    auto root = worker_value.attachment_.loop().start(run());
    worker_value.attachment_.run();
    root.get();
    io.restart();
    if (failure != nullptr) {
        std::rethrow_exception(failure);
    }
}
}  // namespace

RUVIA_TEST(client_response_body_collectors_return_public_results) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    const std::string expected("\0\xff\xc3", 3);
    loopback_response_server server(io, worker.handle_, {expected});
    ruvia::http_client client(worker.attachment_.loop(), local_http_client_config(server.port()));
    server.start();
    auto operation = [&]() -> ruvia::task<void> {
        auto response = co_await client.send({.target_ = "/collect"});
        auto collected = co_await response.body().read_all();
        RUVIA_CHECK_EQ(collected.size(), expected.size());
        const std::string_view collected_view(
            reinterpret_cast<const char*>(collected.bytes().data()), collected.size());
        RUVIA_CHECK_EQ(collected_view, expected);
        const auto remaining = co_await response.body().read_all();
        RUVIA_CHECK(remaining.empty());
        RUVIA_CHECK(!(co_await response.body().text()));
        co_await client.shutdown();
        RUVIA_CHECK_EQ(collected_view, expected);
    };
    run_operation(worker, io, operation);
}

RUVIA_TEST(http_client_handle_options_override_pool_timeout_and_start_when_operation_runs) {
    {
        auto& io = ruvia::test::new_test_io_context();
        test_worker worker(io);
        loopback_response_server server(
            io, worker.handle_, {"slow"}, std::chrono::milliseconds(350));
        auto config = local_http_client_config(server.port());
        config.request_timeout_ = std::chrono::milliseconds(50);
        ruvia::http_client client(worker.attachment_.loop(), config);
        server.start();

        auto operation = [&]() -> ruvia::task<void> {
            auto handle = client.with_options({.timeout_ = std::chrono::seconds(2)});
            auto cold = handle.send({.target_ = "/pool-timeout-override"});
            (void)co_await ruvia::sleep_for(worker.handle_, std::chrono::milliseconds(250));
            const auto start = std::chrono::steady_clock::now();
            auto response = co_await std::move(cold);
            const auto elapsed = std::chrono::steady_clock::now() - start;
            RUVIA_CHECK_EQ(response.status(), ruvia::http_status_code::from_value(200));
            RUVIA_CHECK(elapsed >= std::chrono::milliseconds(50));
            RUVIA_CHECK(elapsed < std::chrono::seconds(2));
            co_await server.wait();
            co_await client.shutdown();
        };
        run_operation(worker, io, operation);
    }

    {
        auto& io = ruvia::test::new_test_io_context();
        test_worker worker(io);
        loopback_response_server server(
            io, worker.handle_, {"late"}, std::chrono::milliseconds(350));
        auto config = local_http_client_config(server.port());
        config.request_timeout_ = std::chrono::seconds(5);
        ruvia::http_client client(worker.attachment_.loop(), config);
        server.start();

        auto operation = [&]() -> ruvia::task<void> {
            auto base = client.with_options({.timeout_ = std::chrono::seconds(5)});
            auto shortened = base.with_options({.timeout_ = std::chrono::milliseconds(100)});
            auto extended = shortened.with_options({.timeout_ = std::chrono::seconds(5)});
            auto cold = extended.send({.target_ = "/successive-minimum"});
            (void)co_await ruvia::sleep_for(worker.handle_, std::chrono::milliseconds(250));
            const auto start = std::chrono::steady_clock::now();
            bool timed_out = false;
            try {
                (void)co_await std::move(cold);
            } catch (const ruvia::http_client_error& error) {
                timed_out = error.code() == ruvia::http_client_error::code_type::timeout;
            }
            const auto elapsed = std::chrono::steady_clock::now() - start;
            RUVIA_CHECK(timed_out);
            RUVIA_CHECK(elapsed >= std::chrono::milliseconds(50));
            RUVIA_CHECK(elapsed < std::chrono::seconds(2));
            try {
                co_await server.wait();
            } catch (const std::system_error&) {
                // The peer's delayed response is expected to hit the timed-out socket.
            }
            co_await client.shutdown();
        };
        run_operation(worker, io, operation);
    }

    for (const bool stop_base : {true, false}) {
        auto& io = ruvia::test::new_test_io_context();
        test_worker worker(io);
        loopback_response_server server(
            io, worker.handle_, {"cancelled"}, std::chrono::milliseconds(350));
        ruvia::http_client client(worker.attachment_.loop(), local_http_client_config(server.port()));
        ruvia::stop_source base_stop;
        ruvia::stop_source derived_stop;
        server.start();

        auto operation = [&]() -> ruvia::task<void> {
            asio::steady_timer stop_timer(io);
            stop_timer.expires_after(std::chrono::milliseconds(50));
            stop_timer.async_wait([&base_stop, &derived_stop, stop_base](const std::error_code& error) {
                if (!error) {
                    (stop_base ? base_stop : derived_stop).request_stop();
                }
            });
            auto base = client.with_options({.stop_token_ = base_stop.token()});
            auto copied = base;
            auto derived = copied.with_options({.stop_token_ = derived_stop.token()});
            bool cancelled = false;
            try {
                (void)co_await derived.send({.target_ = stop_base ? "/stop-base" : "/stop-derived"});
            } catch (const ruvia::http_client_error& error) {
                cancelled = error.code() == ruvia::http_client_error::code_type::cancelled;
            }
            RUVIA_CHECK(cancelled);
            try {
                co_await server.wait();
            } catch (const std::system_error&) {
                // The peer's delayed response is expected to hit the cancelled socket.
            }
            co_await client.shutdown();
        };
        run_operation(worker, io, operation);
    }

    {
        auto& io = ruvia::test::new_test_io_context();
        test_worker worker(io);
        loopback_response_server server(
            io, worker.handle_, {"retiring"}, std::chrono::milliseconds(350));
        ruvia::http_client client(worker.attachment_.loop(), local_http_client_config(server.port()));
        server.start();

        auto operation = [&]() -> ruvia::task<void> {
            asio::steady_timer stop_timer(io);
            stop_timer.expires_after(std::chrono::milliseconds(50));
            stop_timer.async_wait([&worker](const std::error_code& error) {
                if (!error) {
                    worker.attachment_.stop();
                }
            });
            bool cancelled = false;
            try {
                (void)co_await client.send({.target_ = "/event-loop-stop"});
            } catch (const ruvia::http_client_error& error) {
                cancelled = error.code() == ruvia::http_client_error::code_type::closing;
            }
            RUVIA_CHECK(cancelled);
            try {
                co_await server.wait();
            } catch (const std::system_error&) {
                // Retirement closes the in-flight TCP exchange.
            }
        };
        run_operation(worker, io, operation);
    }

    {
        auto& io = ruvia::test::new_test_io_context();
        test_worker worker(io);
        loopback_response_server server(
            io, worker.handle_, {"closing"}, std::chrono::milliseconds(350));
        ruvia::http_client client(worker.attachment_.loop(), local_http_client_config(server.port()));
        server.start();

        auto operation = [&]() -> ruvia::task<void> {
            asio::steady_timer close_timer(io);
            close_timer.expires_after(std::chrono::milliseconds(50));
            close_timer.async_wait([&client](const std::error_code& error) {
                if (!error) {
                    client.close();
                }
            });
            auto cold = client.with_options({}).send({.target_ = "/active-close"});
            bool closing = false;
            try {
                (void)co_await std::move(cold);
            } catch (const ruvia::http_client_error& error) {
                closing = error.code() == ruvia::http_client_error::code_type::closing;
            }
            RUVIA_CHECK(closing);
            co_await client.shutdown();
            try {
                co_await server.wait();
            } catch (const std::system_error&) {
                // Pool shutdown closes the in-flight TCP exchange before its delayed reply.
            }
        };
        run_operation(worker, io, operation);
    }
}

RUVIA_TEST(http1_full_response_queue_cancellation_and_deadline_finish_without_reading) {
    for (const bool deadline : {false, true}) {
        auto& io = ruvia::test::new_test_io_context();
        test_worker worker(io);
        loopback_response_server server(io, worker.handle_, {std::string(65536, 'q')});
        auto config = local_http_client_config(server.port());
        config.max_response_bytes_ = 1024;
        ruvia::http_client client(worker.attachment_.loop(), config);
        ruvia::stop_source stop;
        server.start();

        auto operation = [&]() -> ruvia::task<void> {
            auto handle = client.with_options({
                .timeout_ = deadline ? std::optional(std::chrono::milliseconds(40)) : std::nullopt,
                .stop_token_ = stop.token(),
            });
            auto response = co_await handle.send({.target_ = "/backpressure"});
            RUVIA_CHECK_EQ(response.status(), ruvia::http_status::ok);
            RUVIA_CHECK_EQ(client.stats().in_flight_requests_, std::size_t{1});
            // Keep the response alive without reading or abandoning it. The
            // producer must leave its full-queue wait using the terminal event.
            if (deadline) {
                (void)co_await ruvia::sleep_for(worker.handle_, std::chrono::milliseconds(80));
            } else {
                stop.request_stop();
            }
            // Ordered worker turns drain cancellation publication and the
            // producer's scheduled wake, without body-reader side effects.
            for (unsigned turn = 0; turn != 4; ++turn) {
                (void)co_await ruvia::async_asio([&io](auto done) {
                    asio::post(io, [done = std::move(done)]() mutable { done(std::error_code{}); });
                });
            }
            RUVIA_CHECK_EQ(client.stats().failed_requests_, std::size_t{1});
            RUVIA_CHECK_EQ(client.stats().in_flight_requests_, std::size_t{0});
            co_await client.shutdown();
            try {
                co_await server.wait();
            } catch (const std::system_error&) {
                // Cancellation can race the peer's final socket write.
            }
        };
        run_operation(worker, io, operation);
    }
}

RUVIA_TEST(http1_full_response_queue_loop_stop_joins_without_reading) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    loopback_response_server server(io, worker.handle_, {std::string(65536, 'q')});
    auto config = local_http_client_config(server.port());
    config.max_response_bytes_ = 1024;
    ruvia::http_client client(worker.attachment_.loop(), config);
    server.start();
    bool joined = false;
    auto operation = [&]() -> ruvia::task<void> {
        auto response = co_await client.send({.target_ = "/backpressure-stop"});
        RUVIA_CHECK_EQ(response.status(), ruvia::http_status::ok);
        worker.attachment_.stop();
        // The response remains alive across shutdown; its destructor/read()
        // cannot be the event that wakes the producer.
        co_await client.shutdown();
        joined = true;
        try {
            co_await server.wait();
        } catch (const std::system_error&) {
        }
    };
    run_operation(worker, io, operation);
    RUVIA_CHECK(joined);
    RUVIA_CHECK(!worker.handle_.valid());
}

RUVIA_TEST(http1_transfer_gzip_full_queue_cancel_and_deadline_without_reading) {
    const auto encoded = large_gzip_response_body();
    for (const bool deadline : {false, true}) {
        auto& io = ruvia::test::new_test_io_context();
        test_worker worker(io);
        http1_chunked_response_peer peer(
            io, encoded, http1_chunked_response_peer::end_mode_type::wait_for_client_close);
        auto config = local_http_client_config(peer.port());
        config.max_response_bytes_ = 1024;
        ruvia::http_client client(worker.attachment_.loop(), config);
        ruvia::stop_source stop;
        peer.start();

        auto operation = [&]() -> ruvia::task<void> {
            auto handle = client.with_options({
                .timeout_ = deadline ? std::optional(std::chrono::milliseconds(200)) : std::nullopt,
                .stop_token_ = stop.token(),
            });
            auto response = co_await handle.send({.target_ = "/transfer-gzip-backpressure"});
            RUVIA_CHECK_EQ(response.status(), ruvia::http_status::ok);
            RUVIA_CHECK_EQ(client.stats().in_flight_requests_, std::size_t{1});
            for (unsigned turn = 0; turn != 4; ++turn) {
                (void)co_await ruvia::async_asio([&io](auto done) {
                    asio::post(io, [done = std::move(done)]() mutable { done(std::error_code{}); });
                });
            }
            RUVIA_CHECK(client.stats().bytes_received_ >= encoded.size());

            if (deadline) {
                (void)co_await ruvia::sleep_for(worker.handle_, std::chrono::milliseconds(300));
            } else {
                stop.request_stop();
            }
            for (unsigned turn = 0; turn != 4; ++turn) {
                (void)co_await ruvia::async_asio([&io](auto done) {
                    asio::post(io, [done = std::move(done)]() mutable { done(std::error_code{}); });
                });
            }
            RUVIA_CHECK_EQ(client.stats().failed_requests_, std::size_t{1});
            RUVIA_CHECK_EQ(client.stats().in_flight_requests_, std::size_t{0});
            // Keep response alive through join. No body read or response
            // destruction may be what releases the producer's full queue.
            co_await client.shutdown();
        };
        run_operation(worker, io, operation);
        peer.wait();
        RUVIA_CHECK(peer.observed_client_close());
    }
}

RUVIA_TEST(http1_transfer_gzip_full_queue_loop_stop_joins_without_reading) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    const auto encoded = large_gzip_response_body();
    http1_chunked_response_peer peer(
        io, encoded, http1_chunked_response_peer::end_mode_type::wait_for_client_close);
    auto config = local_http_client_config(peer.port());
    config.max_response_bytes_ = 1024;
    ruvia::http_client client(worker.attachment_.loop(), config);
    peer.start();
    bool joined = false;
    auto operation = [&]() -> ruvia::task<void> {
        auto response = co_await client.send({.target_ = "/transfer-gzip-stop"});
        RUVIA_CHECK_EQ(response.status(), ruvia::http_status::ok);
        RUVIA_CHECK_EQ(client.stats().in_flight_requests_, std::size_t{1});
        for (unsigned turn = 0; turn != 4; ++turn) {
            (void)co_await ruvia::async_asio([&io](auto done) {
                asio::post(io, [done = std::move(done)]() mutable { done(std::error_code{}); });
            });
        }
        RUVIA_CHECK(client.stats().bytes_received_ >= encoded.size());
        worker.attachment_.stop();
        co_await client.shutdown();
        joined = true;
    };
    run_operation(worker, io, operation);
    peer.wait();
    RUVIA_CHECK(joined);
    RUVIA_CHECK(peer.observed_client_close());
    RUVIA_CHECK(!worker.handle_.valid());
}

RUVIA_TEST(http1_transfer_gzip_chunked_streams_before_terminal_chunk_and_bounds_each_decode) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    const std::string expected(128 * 1024, 'z');
    auto encoded_result = ruvia::encode_http_content(ruvia::http_content_coding::gzip, expected,
        {.max_encoded_bytes_ = expected.size()});
    RUVIA_CHECK(encoded_result.encoded() != nullptr);
    auto encoded = std::move(*encoded_result.encoded()).take_bytes();
    http1_chunked_response_peer peer(io, std::string(encoded.data(), encoded.size()));
    auto config = local_http_client_config(peer.port());
    config.max_response_bytes_ = 1024;
    ruvia::http_client client(worker.attachment_.loop(), config);
    peer.start();

    std::size_t decoded_bytes = 0;
    auto operation = [&]() -> ruvia::task<void> {
        auto response = co_await client.send({.target_ = "/transfer-gzip"});
        RUVIA_CHECK_EQ(response.status(), ruvia::http_status::ok);
        RUVIA_CHECK_EQ(client.stats().in_flight_requests_, std::size_t{1});

        const auto first = co_await response.body().read();
        RUVIA_CHECK(first.has_value());
        RUVIA_CHECK(first->size() <= config.max_response_bytes_);
        RUVIA_CHECK(!response.body().complete());
        const auto first_view = std::string_view(
            reinterpret_cast<const char*>(first->data()), first->size());
        decoded_bytes += first->size();

        // Let the producer decode already-buffered compressed bytes while the
        // consumer's borrowed view remains live; only pending storage may grow.
        for (unsigned turn = 0; turn != 4; ++turn) {
            (void)co_await ruvia::async_asio([&io](auto done) {
                asio::post(io, [done = std::move(done)]() mutable { done(std::error_code{}); });
            });
        }
        RUVIA_CHECK_EQ(std::string_view(reinterpret_cast<const char*>(first->data()), first->size()),
            first_view);
        peer.release_terminal_chunk();

        while (const auto chunk = co_await response.body().read()) {
            RUVIA_CHECK(chunk->size() <= config.max_response_bytes_);
            decoded_bytes += chunk->size();
        }
        RUVIA_CHECK(response.body().complete());
        RUVIA_CHECK_EQ(response.trailer("x-end"), std::optional<std::string_view>("retained"));
        RUVIA_CHECK_EQ(client.stats().in_flight_requests_, std::size_t{0});
        co_await client.shutdown();
    };
    run_operation(worker, io, operation);
    peer.wait();
    RUVIA_CHECK_EQ(decoded_bytes, expected.size());
    RUVIA_CHECK(decoded_bytes > config.max_response_bytes_);
}

RUVIA_TEST(http1_response_storage_remains_worker_owned_after_client_destruction) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    const std::string expected(128 * 1024, 'z');
    const auto encoded = large_gzip_response_body();
    const std::string long_header(32 * 1024, 'h');
    http1_chunked_response_peer peer(io, encoded,
        http1_chunked_response_peer::end_mode_type::terminal_gate, long_header);
    peer.start();

    auto operation = [&]() -> ruvia::task<void> {
        std::optional<ruvia::http_client_response> response;
        std::optional<ruvia::http_client_response_bytes> retained_body;
        std::string collected;
        {
            ruvia::http_client client(worker.attachment_.loop(), local_http_client_config(peer.port()));
            response.emplace(co_await client.send({.target_ = "/retained-response"}));
            const auto first = co_await response->body().read();
            RUVIA_CHECK(first.has_value());
            if (first) {
                collected.append(reinterpret_cast<const char*>(first->data()), first->size());
            }
            peer.release_terminal_chunk();
            retained_body.emplace(co_await response->body().read_all());
            co_await client.shutdown();
        }

        const auto headers = response->headers();
        const auto long_value = response->header("x-long");
        RUVIA_CHECK(long_value.has_value());
        RUVIA_CHECK_EQ(long_value->size(), long_header.size());
        RUVIA_CHECK(std::ranges::all_of(*long_value, [](char value) { return value == 'h'; }));
        RUVIA_CHECK(!headers.empty());
        const auto text = co_await response->body().text();
        RUVIA_CHECK(!text.has_value());
        RUVIA_CHECK(!(co_await response->body().read()).has_value());
        const auto remaining = co_await response->body().read_all();
        RUVIA_CHECK(remaining.empty());
        const auto retained_view = retained_body->bytes();
        collected.append(reinterpret_cast<const char*>(retained_view.data()), retained_view.size());
        RUVIA_CHECK_EQ(collected, expected);
        RUVIA_CHECK_EQ(response->trailer("x-end"), std::optional<std::string_view>("retained"));
        response.reset();
        RUVIA_CHECK_EQ(retained_body->bytes().size(), retained_view.size());
        RUVIA_CHECK_EQ(std::string_view(reinterpret_cast<const char*>(retained_body->bytes().data()),
                           retained_body->bytes().size()),
            expected.substr(expected.size() - retained_view.size()));
        retained_body.reset();
    };
    run_operation(worker, io, operation);
    peer.wait();
}

RUVIA_TEST(client_response_cold_collection_survives_client_and_expires_with_response) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    loopback_response_server server(io, worker.handle_, {"cold-body"});
    server.start();

    auto operation = [&]() -> ruvia::task<void> {
        auto client = std::make_unique<ruvia::http_client>(
            worker.attachment_.loop(), local_http_client_config(server.port()));
        std::optional<ruvia::http_client_response> response;
        response.emplace(co_await client->send({.target_ = "/cold-body"}));
        auto cold = response->body().read_all();
        co_await client->shutdown();
        client.reset();

        auto retained = co_await std::move(cold);
        RUVIA_CHECK_EQ(std::string_view(reinterpret_cast<const char*>(retained.bytes().data()),
                           retained.bytes().size()),
            std::string_view("cold-body"));
        auto expired = response->body().read_all();
        response.reset();
        bool rejected = false;
        try {
            (void)co_await std::move(expired);
        } catch (const std::logic_error&) {
            rejected = true;
        }
        RUVIA_CHECK(rejected);
        co_await server.wait();
    };
    run_operation(worker, io, operation);
}

RUVIA_TEST(http2_client_observes_alternative_service_frame_as_independently_owned_result) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    http2_partial_body_peer peer(io, worker.handle_, "partial-body", {}, {}, true);
    peer.start();
    auto config = local_http_client_config(peer.port());
    config.protocol_ = ruvia::http_client_protocol::http2_only;
    config.advertisements_.receive_alternative_services_ = true;
    auto operation = [&]() -> ruvia::task<void> {
        std::optional<ruvia::http_client_advertisement> retained;
        {
            ruvia::http_client client(worker.attachment_.loop(), config);
            std::exception_ptr failure;
            try {
                auto response = co_await client.send({.target_ = "/advertisement"});
                retained = client.next_advertisement();
                RUVIA_CHECK(retained && retained->alternative_service() && !retained->origins());
                if (retained && retained->alternative_service()) {
                    RUVIA_CHECK(retained->protocol_version() == ruvia::http_protocol_version::http2 && retained->connection_slot() == 0);
                    RUVIA_CHECK(retained->alternative_service()->stream_id_ == 1);
                    RUVIA_CHECK(retained->alternative_service()->origin_.empty());
                    RUVIA_CHECK(retained->alternative_service()->field_value_ == "h3=\":443\"; ma=60");
                }
                RUVIA_CHECK(!client.next_advertisement());
                RUVIA_CHECK(client.stats().dropped_advertisements_ == 0);
            } catch (...) {
                failure = std::current_exception();
            }
            co_await client.shutdown();
            if (failure) {
                std::rethrow_exception(failure);
            }
        }
        peer.rethrow_failure();
        if (retained && retained->alternative_service()) {
            RUVIA_CHECK(retained->alternative_service()->field_value_ == "h3=\":443\"; ma=60");
        }
    };
    run_operation(worker, io, operation);
}

RUVIA_TEST(http2_client_response_reprioritizes_live_stream_and_rejects_invalid_urgency) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    http2_partial_body_peer peer(io, worker.handle_);
    peer.start();
    auto config = local_http_client_config(peer.port());
    config.protocol_ = ruvia::http_client_protocol::http2_only;
    auto operation = [&]() -> ruvia::task<void> {
        ruvia::http_client client(worker.attachment_.loop(), config);
        std::exception_ptr failure;
        try {
            auto response = co_await client.send({.target_ = "/priority"});
            response.reprioritize({.urgency_ = 0, .incremental_ = true});
            const auto priority = co_await peer.wait_for_priority();
            RUVIA_CHECK(priority.urgency_ == 0 && priority.incremental_);
            bool rejected = false;
            try {
                response.reprioritize({.urgency_ = 8});
            } catch (const std::invalid_argument&) {
                rejected = true;
            }
            RUVIA_CHECK(rejected);
            const auto payload_value = co_await response.body().text();
            RUVIA_CHECK(payload_value && *payload_value == "partial-body");
        } catch (...) {
            failure = std::current_exception();
        }
        co_await client.shutdown();
        peer.rethrow_failure();
        if (failure) {
            std::rethrow_exception(failure);
        }
    };
    run_operation(worker, io, operation);
}

RUVIA_TEST(http2_completed_response_storage_survives_client_teardown) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    const std::string body(8192, 'h');
    const std::string long_header(32 * 1024, 'H');
    const std::string long_trailer(32 * 1024, 'T');
    http2_partial_body_peer peer(io, worker.handle_, body, long_header, long_trailer);
    http2_partial_body_peer queued_peer(io, worker.handle_, body, long_header, long_trailer);
    peer.start();
    queued_peer.start();
    auto config = local_http_client_config(peer.port());
    config.protocol_ = ruvia::http_client_protocol::http2_only;

    auto operation = [&]() -> ruvia::task<void> {
        std::optional<ruvia::http_client_response> response;
        std::optional<std::span<const std::byte>> body_view;
        auto client = std::make_unique<ruvia::http_client>(worker.attachment_.loop(), config);
        response.emplace(co_await client->send({.target_ = "/retained-h2"}));
        co_await peer.wait_for_response();
        for (unsigned attempt_value = 0; response->trailers().empty() && attempt_value < 500; ++attempt_value) {
            (void)co_await ruvia::sleep_for(worker.handle_, std::chrono::milliseconds(1));
        }
        RUVIA_CHECK(!response->trailers().empty());
        const auto chunk = co_await response->body().read();
        RUVIA_CHECK(chunk.has_value());
        body_view = *chunk;
        auto cold = response->body().read_all();
        co_await client->shutdown();
        client.reset();

        const auto headers = response->headers();
        const auto trailers = response->trailers();
        RUVIA_CHECK(!headers.empty());
        RUVIA_CHECK(!trailers.empty());
        RUVIA_CHECK_EQ(response->header("x-long"),
            std::optional<std::string_view>(std::string_view(long_header)));
        RUVIA_CHECK_EQ(response->trailer("x-long-trailer"),
            std::optional<std::string_view>(std::string_view(long_trailer)));
        RUVIA_CHECK(body_view.has_value());
        if (body_view) {
            RUVIA_CHECK_EQ(std::string_view(reinterpret_cast<const char*>(body_view->data()),
                               body_view->size()),
                body);
        }

        loopback_response_server next_server(io, worker.handle_, {"next-client"});
        next_server.start();
        {
            ruvia::http_client next_client(worker.attachment_.loop(), local_http_client_config(next_server.port()));
            auto next_response = co_await next_client.send({.target_ = "/new-client"});
            auto next_body = co_await next_response.body().read_all();
            RUVIA_CHECK_EQ(std::string_view(reinterpret_cast<const char*>(next_body.bytes().data()),
                               next_body.bytes().size()),
                std::string_view("next-client"));
            co_await next_client.shutdown();
        }
        co_await next_server.wait();
        const auto retained_header = std::ranges::find_if(headers, [](const auto& header_value) {
            return header_value.name() == "x-long";
        });
        const auto retained_trailer = std::ranges::find_if(trailers, [](const auto& header_value) {
            return header_value.name() == "x-long-trailer";
        });
        RUVIA_CHECK(retained_header != headers.end());
        RUVIA_CHECK(retained_trailer != trailers.end());
        if (retained_header != headers.end()) {
            RUVIA_CHECK_EQ(retained_header->value(), std::string_view(long_header));
        }
        if (retained_trailer != trailers.end()) {
            RUVIA_CHECK_EQ(retained_trailer->value(), std::string_view(long_trailer));
        }
        if (body_view) {
            RUVIA_CHECK_EQ(std::string_view(reinterpret_cast<const char*>(body_view->data()),
                               body_view->size()),
                body);
        }
        auto retained = co_await std::move(cold);
        RUVIA_CHECK(retained.empty());
        response.reset();

        auto queued_config = config;
        queued_config.port_ = queued_peer.port();
        auto queued_client = std::make_unique<ruvia::http_client>(worker.attachment_.loop(), queued_config);
        std::optional<ruvia::http_client_response> queued_response;
        queued_response.emplace(co_await queued_client->send({.target_ = "/queued-body"}));
        co_await queued_peer.wait_for_response();
        for (unsigned attempt_value = 0; queued_response->trailers().empty() && attempt_value < 500; ++attempt_value) {
            (void)co_await ruvia::sleep_for(worker.handle_, std::chrono::milliseconds(1));
        }
        RUVIA_CHECK(!queued_response->trailers().empty());
        auto queued_body = queued_response->body().read_all();
        co_await queued_client->shutdown();
        queued_client.reset();
        auto queued_bytes = co_await std::move(queued_body);
        RUVIA_CHECK_EQ(std::string_view(reinterpret_cast<const char*>(queued_bytes.bytes().data()),
                           queued_bytes.bytes().size()),
            body);
        RUVIA_CHECK_EQ(queued_response->header("x-long"),
            std::optional<std::string_view>(std::string_view(long_header)));
        queued_response.reset();
        RUVIA_CHECK_EQ(std::string_view(reinterpret_cast<const char*>(queued_bytes.bytes().data()),
                           queued_bytes.bytes().size()),
            body);
    };
    run_operation(worker, io, operation);
    peer.wait_for_partial_response();
    queued_peer.wait_for_partial_response();
    peer.rethrow_failure();
    queued_peer.rethrow_failure();
    RUVIA_CHECK(peer.observed_client_close());
    RUVIA_CHECK(queued_peer.observed_client_close());
}

RUVIA_TEST(client_shutdown_keeps_incomplete_response_error_observable) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    http2_partial_body_peer peer(io, worker.handle_);
    peer.start();
    auto config = local_http_client_config(peer.port());
    config.protocol_ = ruvia::http_client_protocol::http2_only;

    auto operation = [&]() -> ruvia::task<void> {
        std::optional<ruvia::http_client_response> response;
        bool running_read_failed = false;
        {
            ruvia::http_client client(worker.attachment_.loop(), config);
            response.emplace(co_await client.send({.target_ = "/incomplete-after-close"}));
            asio::steady_timer close_timer(io);
            close_timer.expires_after(std::chrono::milliseconds(50));
            close_timer.async_wait([&client](const std::error_code& error) {
                if (!error) {
                    client.close();
                }
            });
            try {
                (void)co_await response->body().read_all();
            } catch (const ruvia::http_client_error& error) {
                running_read_failed = error.code() == ruvia::http_client_error::code_type::closing ||
                                      error.code() == ruvia::http_client_error::code_type::cancelled ||
                                      error.code() == ruvia::http_client_error::code_type::io_error;
            }
            RUVIA_CHECK(running_read_failed);
            co_await client.shutdown();
        }
        bool retained_response_failed = false;
        try {
            (void)co_await response->body().read_all();
        } catch (const ruvia::http_client_error& error) {
            retained_response_failed = error.code() == ruvia::http_client_error::code_type::closing ||
                                       error.code() == ruvia::http_client_error::code_type::cancelled ||
                                       error.code() == ruvia::http_client_error::code_type::io_error;
        }
        RUVIA_CHECK(retained_response_failed);
        response.reset();
    };
    run_operation(worker, io, operation);
    peer.wait_for_partial_response();
    peer.rethrow_failure();
    RUVIA_CHECK(peer.observed_client_close());
}

RUVIA_TEST(http2_event_loop_stop_joins_reader_writer_with_a_partial_response_body) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    http2_partial_body_peer peer(io, worker.handle_);
    peer.start();
    auto config = local_http_client_config(peer.port());
    config.protocol_ = ruvia::http_client_protocol::http2_only;
    ruvia::http_client client(worker.attachment_.loop(), config);

    auto operation = [&]() -> ruvia::task<void> {
        auto response = co_await client.send({.target_ = "/partial"});
        RUVIA_CHECK_EQ(response.status(), ruvia::http_status_code::from_value(200));
        asio::steady_timer stop_timer(io);
        stop_timer.expires_after(std::chrono::milliseconds(100));
        stop_timer.async_wait([&worker](const std::error_code& error) {
            if (!error) {
                worker.attachment_.stop();
            }
        });
        bool cancelled = false;
        try {
            (void)co_await response.body().read_all();
        } catch (const ruvia::http_client_error& error) {
            // A started body operation can observe its client stop token before
            // the pool-close outcome. Both are terminal cancellation, not a
            // protocol failure from the peer's deliberately open stream.
            cancelled = error.code() == ruvia::http_client_error::code_type::cancelled ||
                        error.code() == ruvia::http_client_error::code_type::closing;
        }
        RUVIA_CHECK(cancelled);
    };
    run_operation(worker, io, operation);
    peer.wait_for_partial_response();
    peer.rethrow_failure();
    RUVIA_CHECK(peer.observed_client_close());
    RUVIA_CHECK(!client.worker().valid());
    RUVIA_CHECK(!client.worker().accepting());
}

RUVIA_TEST(client_body_result_uses_pool_budget_after_client_and_worker_teardown) {
    std::optional<ruvia::http_client_response_bytes> retained;
    std::string expected(
        "\0\xff"
        "data",
        6);
    {
        auto& io = ruvia::test::new_test_io_context();
        {
            test_worker worker(io);
            asio::ip::tcp::acceptor acceptor(
                io, {asio::ip::tcp::v4(), std::uint16_t{0}});
            asio::ip::tcp::socket server_socket(io);
            ruvia::worker_signal server_done(worker.handle_);
            std::exception_ptr server_failure;
            const std::string response_wire =
                "HTTP/1.1 200 OK\r\nContent-Length: 6\r\nConnection: close\r\n\r\n" +
                expected;
            acceptor.async_accept(server_socket, [&](const std::error_code& error) {
                if (error) {
                    server_failure = std::make_exception_ptr(std::system_error(error));
                    server_done.notify();
                    return;
                }
                asio::async_write(server_socket, asio::buffer(response_wire),
                    [&](const std::error_code& write_error, std::size_t) {
                        if (write_error) {
                            server_failure = std::make_exception_ptr(std::system_error(write_error));
                        }
                        server_done.notify();
                    });
            });
            {
                ruvia::http_client client(worker.attachment_.loop(),
                    {.scheme_ = ruvia::http_scheme::http,
                        .host_ = "127.0.0.1",
                        .port_ = acceptor.local_endpoint().port(),
                        .protocol_ = ruvia::http_client_protocol::http1_only},
                    {.max_retained_bytes_ = expected.size()});
                auto operation = [&]() -> ruvia::task<void> {
                    auto send = client.send({.target_ = "/"});
                    auto response = co_await std::move(send);
                    auto bytes_value = co_await response.body().read_all();
                    retained.emplace(std::move(bytes_value));
                    co_await server_done.wait();
                    if (server_failure != nullptr) {
                        std::rethrow_exception(server_failure);
                    }
                    RUVIA_CHECK_EQ(retained->bytes().size(), expected.size());
                    co_await client.shutdown();
                };
                run_operation(worker, io, operation);
            }
        }
    }

    std::atomic_bool contents_matched{false};
    std::thread destroyer([bytes = std::move(*retained), expected = std::move(expected),
                              &contents_matched]() {
        const auto actual = bytes.bytes();
        const auto wanted = std::as_bytes(std::span(expected.data(), expected.size()));
        contents_matched.store(actual.size() == wanted.size() &&
                                   std::equal(actual.begin(), actual.end(), wanted.begin()),
            std::memory_order_release);
    });
    destroyer.join();
    RUVIA_CHECK(contents_matched.load(std::memory_order_acquire));
}

RUVIA_TEST(client_result_budget_rejects_zero_limit_at_client_startup) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    bool rejected_at_client_startup = false;
    try {
        ruvia::http_client client(worker.attachment_.loop(), {.host_ = "example.test"},
            {.max_retained_bytes_ = 0});
    } catch (const std::invalid_argument&) {
        rejected_at_client_startup = true;
    }
    RUVIA_CHECK(rejected_at_client_startup);
}

RUVIA_TEST(http_client_upload_exchange_owns_chunks_and_trailers_and_drives_continue) {
    for (const auto mode : {upload_peer::mode_type::chunked, upload_peer::mode_type::known_length,
             upload_peer::mode_type::continue_value, upload_peer::mode_type::continue_timeout}) {
        auto& io = ruvia::test::new_test_io_context();
        test_worker worker(io);
        upload_peer peer(io, worker.handle_, mode);
        auto config = local_http_client_config(peer.port());
        config.request_timeout_ = std::chrono::seconds(3);
        ruvia::http_client client(worker.attachment_.loop(), config);
        peer.start();
        auto operation = [&]() -> ruvia::task<void> {
            const bool known = mode == upload_peer::mode_type::known_length;
            const bool expect_continue = mode == upload_peer::mode_type::continue_value || mode == upload_peer::mode_type::continue_timeout;
            auto exchange_value = co_await client.open_request({.method_ = "POST", .target_ = "/upload"},
                {.content_length_ = known ? std::optional<std::uint64_t>{6} : std::nullopt,
                    .expectation_ = expect_continue ? ruvia::http_client_request_expectation::continue_value : ruvia::http_client_request_expectation::none,
                    .max_chunk_bytes_ = 3,
                    .continue_timeout_ = std::chrono::milliseconds(20)});
            {
                auto cold = exchange_value.body().write("bad");
            }
            {
                auto cold = exchange_value.response();
            }
            bool too_large = false;
            try {
                auto rejected = exchange_value.body().write("four");
            } catch (const std::length_error&) {
                too_large = true;
            }
            RUVIA_CHECK(too_large);
            std::string input = "abc";
            auto first = exchange_value.body().write(input);
            input.assign("xxx");
            co_await std::move(first);
            auto moved = std::move(exchange_value);
            co_await moved.body().write("def");
            std::string trailer_value = "retained";
            const std::array<ruvia::http_header_view, 1> fields_value{{{"X-End", trailer_value}}};
            if (known) {
                co_await moved.body().end();
            } else {
                auto ending = moved.body().end(fields_value);
                trailer_value.assign("changed");
                co_await std::move(ending);
            }
            RUVIA_CHECK(moved.body().complete());
            auto response = co_await moved.response();
            auto bytes_value = co_await response.body().read_all(16);
            RUVIA_CHECK_EQ(std::string_view(reinterpret_cast<const char*>(bytes_value.bytes().data()), bytes_value.size()), "ok");
            RUVIA_CHECK_EQ(response.informational_responses().size(), mode == upload_peer::mode_type::continue_value ? std::size_t{2} : std::size_t{0});
            if (mode == upload_peer::mode_type::continue_value) {
                RUVIA_CHECK_EQ(response.informational_responses()[0].status().value(), std::uint16_t{103});
                RUVIA_CHECK_EQ(response.informational_responses()[0].headers()[0].value(), "</asset>; rel=preload");
            }
            co_await peer.wait();
            RUVIA_CHECK_EQ(peer.body_, known ? "abcdef" : "3\r\nabc\r\n3\r\ndef\r\n0\r\nx-end: retained\r\n\r\n");
            RUVIA_CHECK((peer.head_.find("Expect: 100-continue") != std::string::npos) == expect_continue);
            co_await client.shutdown();
        };
        run_operation(worker, io, operation);
    }
}

RUVIA_TEST(http_client_upload_exchange_preserves_early_final_response_and_stops_upload) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    upload_peer peer(io, worker.handle_, upload_peer::mode_type::early_final);
    std::optional<ruvia::http_client> client(std::in_place, worker.attachment_.loop(), local_http_client_config(peer.port()));
    peer.start();
    auto operation = [&]() -> ruvia::task<void> {
        auto exchange_value = co_await client->open_request({.method_ = "POST", .target_ = "/upload"},
            {.expectation_ = ruvia::http_client_request_expectation::continue_value});
        auto response = co_await exchange_value.response();
        RUVIA_CHECK_EQ(response.status().value(), std::uint16_t{413});
        bool cancelled = false;
        try {
            co_await exchange_value.body().write("rejected");
        } catch (const ruvia::http_client_error& error) {
            cancelled = error.code() == ruvia::http_client_error::code_type::cancelled;
        }
        RUVIA_CHECK(cancelled);
        auto bytes_value = co_await response.body().read_all(16);
        RUVIA_CHECK_EQ(std::string_view(reinterpret_cast<const char*>(bytes_value.bytes().data()), bytes_value.size()), "no");
        co_await peer.wait();
        co_await client->shutdown();
        client.reset();
        // The response domain keeps the exchange's output signals alive after
        // client destruction, including the stopped, unfinished upload path.
        RUVIA_CHECK_EQ(response.status().value(), std::uint16_t{413});
        RUVIA_CHECK_EQ(std::string_view(reinterpret_cast<const char*>(bytes_value.bytes().data()), bytes_value.size()), "no");
    };
    run_operation(worker, io, operation);
}

RUVIA_TEST(http_client_http2_upload_exchange_flow_control_trailers_and_early_final) {
    for (const bool early : {false, true}) {
        auto& io = ruvia::test::new_test_io_context();
        test_worker worker(io);
        http2_upload_peer peer(io, worker.handle_, early);
        auto config = local_http_client_config(peer.port());
        config.protocol_ = ruvia::http_client_protocol::http2_only;
        config.request_timeout_ = std::chrono::seconds(3);
        ruvia::http_client client(worker.attachment_.loop(), config);
        peer.start();
        auto operation = [&]() -> ruvia::task<void> {
            constexpr std::size_t chunk_size = 64 * 1024;
            auto exchange_value = co_await client.open_request({.method_ = "POST", .target_ = "/upload"},
                {.content_length_ = chunk_size * 4, .expectation_ = ruvia::http_client_request_expectation::continue_value});
            if (!early) {
                const std::string input(chunk_size, 'p');
                for (unsigned i = 0; i < 4; ++i) {
                    co_await exchange_value.body().write(input);
                }
                const std::array<ruvia::http_header_view, 1> trailers{{{"x-end", "retained"}}};
                co_await exchange_value.body().end(trailers);
            }
            auto response = co_await exchange_value.response();
            RUVIA_CHECK_EQ(response.status().value(), early ? 413 : 200);
            auto bytes_value = co_await response.body().read_all(16);
            RUVIA_CHECK_EQ(std::string_view(reinterpret_cast<const char*>(bytes_value.bytes().data()), bytes_value.size()), "ok");
            if (!early) {
                RUVIA_CHECK_EQ(peer.body_.size(), chunk_size * 4);
                RUVIA_CHECK_EQ(peer.trailer_, "retained");
                RUVIA_CHECK_EQ(response.informational_responses().size(), std::size_t{2});
                if (response.informational_responses().size() == 2) {
                    RUVIA_CHECK_EQ(response.informational_responses()[0].headers()[0].value(), "</asset>; rel=preload");
                }
            } else {
                bool stopped = false;
                try {
                    co_await exchange_value.body().end();
                } catch (const ruvia::http_client_error& error) {
                    stopped = error.code() == ruvia::http_client_error::code_type::cancelled;
                }
                RUVIA_CHECK(stopped);
            }
            co_await client.shutdown();
            co_await peer.wait();
        };
        run_operation(worker, io, operation);
    }
}

namespace {
void exercise_tls_response_retirement(ruvia::testing::test_context& ruvia_ctx, bool join_before_reuse) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    ruvia::test::tls_identity identity("localhost");
    asio::ip::tcp::acceptor acceptor(io, {asio::ip::make_address("127.0.0.1"), 0});
    const std::string large(262163, 'b');
    const std::array<std::string_view, 2> expected{"fresh connection body", "healthy reused body"};
    unsigned connections = 0;
    unsigned requests = 0;
    bool cancelled_socket_closed = false;
    std::exception_ptr peer_failure;
    ruvia::worker_signal peer_done(worker.handle_);
    const auto serve = [&]() -> asio::awaitable<void> {
        {
            asio::ssl::stream<asio::ip::tcp::socket> stream(co_await acceptor.async_accept(asio::use_awaitable), identity.context_);
            ++connections;
            co_await stream.async_handshake(asio::ssl::stream_base::server, asio::use_awaitable);
            asio::streambuf request;
            co_await asio::async_read_until(stream, request, "\r\n\r\n", asio::use_awaitable);
            ++requests;
            const std::string response = "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(large.size()) + "\r\n\r\n" + large;
            std::error_code error;
            co_await asio::async_write(stream, asio::buffer(response), asio::redirect_error(asio::use_awaitable, error));
            std::array<char, 1024> bytes_value{};
            while (co_await stream.async_read_some(asio::buffer(bytes_value), asio::redirect_error(asio::use_awaitable, error))) {
            }
            cancelled_socket_closed = error == asio::error::eof || error == asio::error::connection_reset || error == asio::ssl::error::stream_truncated;
        }
        asio::ssl::stream<asio::ip::tcp::socket> stream(co_await acceptor.async_accept(asio::use_awaitable), identity.context_);
        ++connections;
        co_await stream.async_handshake(asio::ssl::stream_base::server, asio::use_awaitable);
        for (const auto body : expected) {
            asio::streambuf request;
            co_await asio::async_read_until(stream, request, "\r\n\r\n", asio::use_awaitable);
            ++requests;
            const std::string response = "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + std::string(body);
            co_await asio::async_write(stream, asio::buffer(response), asio::use_awaitable);
        }
    };
    asio::co_spawn(io, serve(), [&](std::exception_ptr failure) {
        peer_failure = failure;
        peer_done.notify();
    });
    auto config = local_http_client_config(acceptor.local_endpoint().port());
    config.scheme_ = ruvia::http_scheme::https;
    config.host_ = "localhost";
    const auto ca = identity.ca_file_.string();
    config.ca_file_ = ca;
    config.max_response_bytes_ = 8192;
    config.protocol_ = join_before_reuse ? ruvia::http_client_protocol::http1_only : ruvia::http_client_protocol::negotiate;
    ruvia::http_client client(worker.attachment_.loop(), config);
    auto operation = [&]() -> ruvia::task<void> {
        ruvia::stop_source cancellation;
        {
            auto response = co_await client.with_options({.stop_token_ = cancellation.token()}).send({.target_ = "/partial"});
            const auto first = co_await response.body().text();
            RUVIA_CHECK(first.has_value());
            RUVIA_CHECK(!first->empty());
            RUVIA_CHECK(first->size() < large.size());
            RUVIA_CHECK(std::ranges::all_of(*first, [](char value) { return value == 'b'; }));
            cancellation.request_stop();
            // Cancellation itself must not invalidate a previously returned borrow.
            RUVIA_CHECK(std::ranges::all_of(*first, [](char value) { return value == 'b'; }));
        }
        if (join_before_reuse) {
            while (client.stats().in_flight_requests_ != 0) {
                (void)co_await ruvia::async_asio([&io](auto handler) {
                    asio::post(io, [handler = std::move(handler)]() mutable { handler(std::error_code{}); });
                });
            }
            RUVIA_CHECK_EQ(client.stats().failed_requests_, std::size_t{1});
        }
        for (const auto body : expected) {
            auto response = co_await client.send({.target_ = "/next?exact=body"});
            RUVIA_CHECK(response.protocol_version() == ruvia::http_protocol_version::http11);
            const auto bytes_value = co_await response.body().read_all();
            RUVIA_CHECK_EQ(std::string_view(reinterpret_cast<const char*>(bytes_value.bytes().data()), bytes_value.size()), body);
            RUVIA_CHECK(!(co_await response.body().read()));
        }
        co_await peer_done.wait();
        if (peer_failure) {
            std::rethrow_exception(peer_failure);
        }
        RUVIA_CHECK_EQ(client.stats().failed_requests_, std::size_t{1});
        co_await client.shutdown();
    };
    run_operation(worker, io, operation);
    RUVIA_CHECK(cancelled_socket_closed);
    RUVIA_CHECK_EQ(connections, 2U);
    RUVIA_CHECK_EQ(requests, 3U);
}
}  // namespace

RUVIA_TEST(http1_tls_partial_response_cancellation_retires_transport_and_preserves_healthy_reuse) {
    exercise_tls_response_retirement(ruvia_ctx, true);
}

RUVIA_TEST(http1_tls_queued_reconnect_joins_cancelled_producer_before_replacing_transport) {
    exercise_tls_response_retirement(ruvia_ctx, false);
}
