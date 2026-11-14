#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <future>
#include <initializer_list>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <variant>
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
#include <openssl/err.h>
#include <openssl/ssl.h>

#include "blocking_pool.h"
#include "content_decoding_fixture.h"
#include "event_loop.h"
#include "http1_server_response.h"
#include "http2_connection.h"
#include "http_client.h"
#include "http_client_request_view.h"
#include "http_client_types.h"
#include "http_content_codec.h"
#include "http_content_coding.h"
#include "http_header.h"
#include "http_request.h"
#include "http_response_stream.h"
#include "http_server_fixture.h"
#include "model.h"
#include "streaming.h"
#include "task_scope.h"
#include "worker_signal.h"
#include "worker_task_fixture.h"
namespace {
class test_worker final {
public:
    explicit test_worker(asio::io_context& io) :
        attachment_(ruvia::attach_event_loop(io, {.queue_capacity_ = 8, .blocking_pool_ = &host_lookups_})),
        handle_(attachment_.loop().worker()) {}

    test_worker(const test_worker&) = delete;
    test_worker& operator=(const test_worker&) = delete;

    ~test_worker() {
        host_lookups_.join();
    }

    // Declared before the attachment, which borrows it for "localhost" lookups.
    ruvia::blocking_pool host_lookups_{{.thread_count_ = 1}};
    ruvia::event_loop_attachment attachment_;
    ruvia::worker_ref handle_;
};

class loopback_response_server final {
public:
    loopback_response_server(asio::io_context& io_context, const ruvia::worker_ref& worker_value, std::initializer_list<std::string_view> bodies, std::chrono::milliseconds response_delay = std::chrono::milliseconds::zero(), bool close_without_response = false, std::size_t responses_before_close = 0) :
        io_context_(io_context),
        acceptor_(io_context, {asio::ip::tcp::v4(), std::uint16_t{0}}),
        done_(worker_value),
        response_delay_(response_delay),
        close_without_response_(close_without_response),
        responses_before_close_(responses_before_close) {
        responses_.reserve(bodies.size());
        for (const auto& body : bodies) {
            responses_.emplace_back("HTTP/1.1 200 OK\r\nContent-Length: ");
            responses_.back().append(std::to_string(body.size())).append("\r\nX-Peer: retained\r\nConnection: close\r\n\r\n").append(body);
        }
    }

    [[nodiscard]] std::uint16_t port() const {
        return acceptor_.local_endpoint().port();
    }

    void start() {
        accept_next();
    }

    // Runs once on the worker when the next request head has arrived, while its
    // (possibly delayed) response is still pending: an in-flight exchange.
    void on_request(std::function<void()> callback) {
        on_request_ = std::move(callback);
    }

    // A client that ended before its connection reached this server would
    // otherwise leave the pending accept, and wait(), unfinished forever.
    void stop_accepting() {
        std::error_code ignored;
        acceptor_.close(ignored);
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
            asio::async_read_until(*socket, *request, "\r\n\r\n", [this, socket, request](const std::error_code& read_error, std::size_t) {
                if (read_error) {
                    fail(read_error);
                    return;
                }
                if (auto callback = std::exchange(on_request_, {})) {
                    callback();
                }
                if (close_without_response_ && next_response_ >= responses_before_close_) {
                    std::error_code ignored;
                    socket->close(ignored);
                    done_.notify();
                    return;
                }
                const auto response_index = next_response_++;
                auto write_response = [this, socket, response_index] {
                    asio::async_write(*socket, asio::buffer(responses_[response_index]), [this, socket](const std::error_code& write_error, std::size_t) {
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
                    timer->async_wait([timer, write_response = std::move(write_response)](const std::error_code& timer_error) mutable {
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
    std::vector<std::string> responses_{};
    ruvia::worker_signal done_;
    std::exception_ptr failure_;
    std::function<void()> on_request_;
    std::size_t next_response_{0};
    std::chrono::milliseconds response_delay_;
    bool close_without_response_;
    std::size_t responses_before_close_;
};

[[nodiscard]] ruvia::http_client_config local_http_client_config(std::string_view base_url) {
    return ruvia::http_client_config{.base_url_ = base_url};
}

class upload_peer final {
public:
    static constexpr std::size_t early_hint_count = 300;
    static constexpr std::size_t rejected_body_size = 32 * 1024;
    enum class mode_type { chunked,
        known_length,
        continue_value,
        continue_timeout,
        early_final,
        early_final_while_uploading,
        early_final_orderly_while_uploading };
    upload_peer(asio::io_context& io, const ruvia::worker_ref& worker_value, mode_type mode) :
        io_(io),
        acceptor_(io, {asio::ip::tcp::v4(), std::uint16_t{0}}),
        socket_(io),
        response_release_(io, asio::steady_timer::time_point::max()),
        done_(worker_value),
        mode_(mode) {}
    std::uint16_t port() const {
        return acceptor_.local_endpoint().port();
    }
    void start() {
        asio::co_spawn(io_, serve(), [this](std::exception_ptr error) {
            std::error_code ignored;
            socket_.close(ignored);
            failure_ = error;
            complete_ = true;
            done_.notify();
        });
    }
    void stop() {
        std::error_code ignored;
        acceptor_.close(ignored);
        socket_.close(ignored);
        response_release_.cancel();
    }
    void release_response() {
        response_release_.expires_at(asio::steady_timer::time_point::min());
    }
    ruvia::task<void> wait() {
        while (!complete_) {
            co_await done_.wait();
        }
        if (failure_) {
            std::rethrow_exception(failure_);
        }
    }
    std::string head_{};
    std::string body_{};
    ruvia::http_request_trailers trailers_;
    std::size_t uploaded_bytes_{0};

private:
    asio::awaitable<void> serve() {
        auto& socket = socket_;
        co_await acceptor_.async_accept(socket, asio::use_awaitable);
        std::string input{};
        const auto head_size = co_await asio::async_read_until(socket, asio::dynamic_buffer(input), "\r\n\r\n", asio::use_awaitable);
        head_.assign(input.data(), head_size);
        input.erase(0, head_size);
        if (mode_ == mode_type::early_final) {
            constexpr std::string_view reply = "HTTP/1.1 413 Content Too Large\r\nContent-Length: 2\r\nConnection: close\r\n\r\nno";
            co_await asio::async_write(socket, asio::buffer(reply), asio::use_awaitable);
            co_return;
        }
        if (mode_ == mode_type::early_final_while_uploading || mode_ == mode_type::early_final_orderly_while_uploading) {
            // Prove that a large upload is already in progress before sending
            // a final response. The client's small response queue exercises
            // body delivery while the upload is stopped.
            if (input.empty()) {
                co_await asio::async_read(socket, asio::dynamic_buffer(input), asio::transfer_exactly(1), asio::use_awaitable);
            }
            uploaded_bytes_ = input.size();
            const std::string body(rejected_body_size, 'r');
            const std::string head = std::move(std::string("HTTP/1.1 413 Content Too Large\r\nContent-Length: ").append(std::to_string(body.size()))) + "\r\nConnection: close\r\n\r\n";
            co_await asio::async_write(socket, asio::buffer(head), asio::use_awaitable);
            co_await asio::async_write(socket, asio::buffer(body), asio::use_awaitable);
            if (mode_ == mode_type::early_final_orderly_while_uploading) {
                // Keep the socket open until the client consumes the response.
                // Closing with unread request bytes can reset the connection and
                // discard response bytes on Windows.
                socket.shutdown(asio::ip::tcp::socket::shutdown_send);
                std::error_code ignored;
                co_await response_release_.async_wait(asio::redirect_error(asio::use_awaitable, ignored));
                co_return;
            }
            asio::steady_timer response_visible(io_, std::chrono::milliseconds(10));
            co_await response_visible.async_wait(asio::use_awaitable);
            std::error_code ignored;
            socket.shutdown(asio::ip::tcp::socket::shutdown_receive, ignored);
            co_return;
        }
        if (mode_ == mode_type::continue_value) {
            if (!input.empty()) {
                throw std::runtime_error("upload arrived before 100 Continue");
            }
            constexpr std::string_view early_hint = "HTTP/1.1 103 Early Hints\r\nLink: </asset>; rel=preload\r\n\r\n";
            std::string reply{};
            reply.reserve(early_hint_count * early_hint.size() + 25);
            for (std::size_t i = 0; i < early_hint_count; ++i) {
                reply.append(early_hint);
            }
            reply.append("HTTP/1.1 100 Continue\r\n\r\n");
            co_await asio::async_write(socket, asio::buffer(reply), asio::use_awaitable);
        }
        if (mode_ == mode_type::known_length) {
            if (input.size() < 6) {
                co_await asio::async_read(socket, asio::dynamic_buffer(input), asio::transfer_exactly(6 - input.size()), asio::use_awaitable);
            }
            body_ = std::move(input);
        } else {
            ruvia::http1_chunked_body_decoder decoder;
            for (;;) {
                const auto decoded = decoder.decode(input);
                if (decoded.failure()) {
                    throw std::runtime_error("invalid upload chunk framing");
                }
                if (const auto* chunk = decoded.body_chunk()) {
                    body_.append(chunk->bytes());
                }
                if (const auto* complete = decoded.complete()) {
                    if (trailers_.append_received(complete->trailers())) {
                        throw std::runtime_error("invalid upload trailers");
                    }
                    break;
                }
                input.erase(0, decoded.consumed_bytes());
                if (decoded.need_more()) {
                    co_await asio::async_read(socket, asio::dynamic_buffer(input), asio::transfer_at_least(1), asio::use_awaitable);
                }
            }
        }
        if (mode_ == mode_type::known_length) {
            asio::steady_timer completed(io_, std::chrono::milliseconds(2));
            co_await completed.async_wait(asio::use_awaitable);
        }
        constexpr std::string_view reply = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok";
        co_await asio::async_write(socket, asio::buffer(reply), asio::use_awaitable);
    }
    asio::io_context& io_;
    asio::ip::tcp::acceptor acceptor_;
    asio::ip::tcp::socket socket_;
    asio::steady_timer response_release_;
    ruvia::worker_signal done_;
    mode_type mode_;
    std::exception_ptr failure_;
    bool complete_{};
};

enum class cookie_peer_mode { http1,
    http1_replacement,
    http2 };

class cookie_stream_peer final {
public:
    cookie_stream_peer(asio::io_context& io, const ruvia::worker_ref& worker_value, cookie_peer_mode mode = cookie_peer_mode::http1) :
        io_(io),
        acceptor_(io, {asio::ip::tcp::v4(), std::uint16_t{0}}),
        deadline_(io),
        gate_(io),
        done_(worker_value),
        mode_(mode) {}

    [[nodiscard]] std::uint16_t port() const {
        return acceptor_.local_endpoint().port();
    }

    void start() {
        deadline_.expires_after(std::chrono::seconds(5));
        deadline_.async_wait([this](const std::error_code& error) {
            if (!error) {
                stop();
            }
        });
        asio::co_spawn(io_, serve(), [this](std::exception_ptr failure) {
            failure_ = std::move(failure);
            complete_ = true;
            deadline_.cancel();
            done_.notify();
        });
    }

    [[nodiscard]] ruvia::task<void> wait() {
        while (!complete_) {
            co_await done_.wait();
        }
        if (failure_) {
            std::rethrow_exception(failure_);
        }
    }

    void release_terminal_body() {
        release_requested_ = true;
        gate_.cancel();
    }

    std::array<std::string, 2> requests_{std::string{}, std::string{}};

private:
    void stop() noexcept {
        std::error_code ignored;
        acceptor_.close(ignored);
        gate_.cancel();
        for (auto& socket : sockets_) {
            if (socket) {
                socket->close(ignored);
            }
        }
    }

    asio::awaitable<void> serve() {
#if defined(RUVIA_ENABLE_HTTP2)
        if (mode_ == cookie_peer_mode::http2) {
            co_await serve_http2();
            co_return;
        }
#endif
        sockets_[0].emplace(co_await acceptor_.async_accept(asio::use_awaitable));
        std::string request{};
        co_await asio::async_read_until(*sockets_[0], asio::dynamic_buffer(request), "\r\n\r\n", asio::use_awaitable);
        const std::string_view streaming_head = mode_ == cookie_peer_mode::http1_replacement
            ? "HTTP/1.1 200 OK\r\nContent-Length: 1\r\nConnection: close\r\nSet-Cookie: sid=old\r\n\r\n"
            : "HTTP/1.1 103 Early Hints\r\nSet-Cookie: informational=ignored; Path=/\r\n\r\nHTTP/1.1 200 OK\r\nContent-Length: 1\r\nSet-Cookie: session=ready; Path=/\r\n\r\n";
        co_await asio::async_write(*sockets_[0], asio::buffer(streaming_head), asio::use_awaitable);
        for (std::size_t index = 0; index != requests_.size(); ++index) {
            auto& socket = sockets_[index + 1];
            socket.emplace(co_await acceptor_.async_accept(asio::use_awaitable));
            co_await asio::async_read_until(*socket, asio::dynamic_buffer(requests_[index]), "\r\n\r\n", asio::use_awaitable);
            const std::string_view response = mode_ == cookie_peer_mode::http1_replacement && index == 0 ? "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\nSet-Cookie: sid=new; Path=/scope\r\n\r\n" : "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            co_await asio::async_write(*socket, asio::buffer(response), asio::use_awaitable);
            std::error_code ignored;
            socket->close(ignored);
            if (mode_ == cookie_peer_mode::http1_replacement && index == 0) {
                if (!release_requested_) {
                    gate_.expires_at(std::chrono::steady_clock::time_point::max());
                    std::error_code error;
                    co_await gate_.async_wait(asio::redirect_error(asio::use_awaitable, error));
                    if (!release_requested_) {
                        throw std::runtime_error("cookie response completion gate expired");
                    }
                }
                constexpr std::string_view body = "x";
                co_await asio::async_write(*sockets_[0], asio::buffer(body), asio::use_awaitable);
                sockets_[0]->close(ignored);
            }
        }
    }

#if defined(RUVIA_ENABLE_HTTP2)
    asio::awaitable<void> serve_http2() {
        sockets_[0].emplace(co_await acceptor_.async_accept(asio::use_awaitable));
        auto& socket = *sockets_[0];
        auto connection = ruvia::http2_connection::server();
        std::optional<ruvia::http2_request_head_event> streaming_request;
        std::array<char, 16384> input{};
        std::string output;
        std::size_t received = 0;
        for (;;) {
            if (connection.wants_write()) {
                ruvia::test::drain_output(connection, output);
                co_await asio::async_write(socket, asio::buffer(output), asio::use_awaitable);
            }
            if (received == requests_.size()) {
                co_return;
            }
            const auto count = co_await socket.async_read_some(asio::buffer(input), asio::use_awaitable);
            const auto bytes = std::string_view(input.data(), count);
            for (;;) {
                const auto status = connection.feed(bytes);
                if (status == ruvia::http2_feed_result::protocol_failure) {
                    throw std::runtime_error("invalid cookie test HTTP/2 input");
                }
                while (auto event = connection.next_event()) {
                    auto* request = event->request_head();
                    if (request == nullptr) {
                        continue;
                    }
                    const auto stream_id = request->stream_id();
                    ruvia::http_response response;
                    if (!streaming_request) {
                        const std::array fields{ruvia::http_header_view{"set-cookie", "informational=ignored; Path=/"}};
                        if (connection.submit_interim_response_head(stream_id, ruvia::http_interim_response_head(ruvia::http_status::early_hints, fields)) != ruvia::http2_submit_status::accepted) {
                            throw std::runtime_error("cookie test interim head rejected");
                        }
                        response.header("set-cookie", "session=ready; Path=/");
                        if (!std::holds_alternative<ruvia::http_response_stream_commit_plan>(connection.submit_streaming_response_head(stream_id, std::move(response)))) {
                            throw std::runtime_error("cookie test streaming head rejected");
                        }
                        streaming_request.emplace(std::move(*request));
                    } else {
                        if (received == requests_.size()) {
                            throw std::runtime_error("unexpected cookie test request");
                        }
                        const auto built = request->build_request();
                        requests_[received++].assign(built.header("cookie").value_or(std::string_view{}));
                        const auto head = connection.submit_response_head(stream_id, response, ruvia::plan_buffered_http_response_write(built.known_method(), response));
                        if (!std::holds_alternative<ruvia::http_buffered_response_write_plan>(head) || connection.release(std::move(*request)) != ruvia::http2_server_request_release_status::released) {
                            throw std::runtime_error("cookie test response rejected");
                        }
                    }
                }
                if (status != ruvia::http2_feed_result::events_pending) {
                    break;
                }
            }
        }
    }

#endif
    asio::io_context& io_;
    asio::ip::tcp::acceptor acceptor_;
    asio::steady_timer deadline_;
    asio::steady_timer gate_;
    ruvia::worker_signal done_;
    cookie_peer_mode mode_;
    std::array<std::optional<asio::ip::tcp::socket>, 3> sockets_;
    std::exception_ptr failure_;
    bool complete_{false};
    bool release_requested_{false};
};

// Answers one request per connection with the next scripted response head and
// records every request head.
class scripted_head_peer final {
public:
    scripted_head_peer(asio::io_context& io, const ruvia::worker_ref& worker_value, std::initializer_list<std::string_view> heads) :
        acceptor_(io, {asio::ip::tcp::v4(), std::uint16_t{0}}),
        deadline_(io),
        done_(worker_value),
        heads_(heads.begin(), heads.end()),
        io_(io) {}

    [[nodiscard]] std::uint16_t port() const {
        return acceptor_.local_endpoint().port();
    }

    void start() {
        deadline_.expires_after(std::chrono::seconds(5));
        deadline_.async_wait([this](const std::error_code& error) {
            if (!error) {
                std::error_code ignored;
                acceptor_.close(ignored);
            }
        });
        asio::co_spawn(io_, serve(), [this](std::exception_ptr failure) {
            failure_ = std::move(failure);
            complete_ = true;
            deadline_.cancel();
            done_.notify();
        });
    }

    [[nodiscard]] ruvia::task<void> wait() {
        while (!complete_) {
            co_await done_.wait();
        }
        if (failure_) {
            std::rethrow_exception(failure_);
        }
    }

    std::vector<std::string> requests_{};

private:
    asio::awaitable<void> serve() {
        for (const auto& head : heads_) {
            auto socket = co_await acceptor_.async_accept(asio::use_awaitable);
            auto& request = requests_.emplace_back();
            co_await asio::async_read_until(socket, asio::dynamic_buffer(request), "\r\n\r\n", asio::use_awaitable);
            co_await asio::async_write(socket, asio::buffer(head), asio::use_awaitable);
        }
    }

    asio::ip::tcp::acceptor acceptor_;
    asio::steady_timer deadline_;
    ruvia::worker_signal done_;
    std::vector<std::string> heads_{};
    asio::io_context& io_;
    std::exception_ptr failure_;
    bool complete_{false};
};

[[nodiscard]] std::string large_gzip_response_body() {
    const std::string body(128 * 1024, 'z');
    auto result_value = ruvia::encode_http_content(ruvia::http_content_coding::gzip, body, {.max_encoded_bytes_ = body.size()});
    if (!result_value.has_value()) {
        throw std::runtime_error("could not encode fake HTTP response body");
    }
    return std::string(result_value->data(), result_value->size());
}

class http1_chunked_response_peer final {
public:
    enum class end_mode_type : unsigned char { terminal_gate,
        wait_for_client_close };

    explicit http1_chunked_response_peer(asio::io_context& io, std::string_view encoded, end_mode_type end_mode = end_mode_type::terminal_gate, std::string_view long_header = {}) :
        io_(io),
        acceptor_(io, {asio::ip::make_address("127.0.0.1"), 0}),
        gate_(io),
        encoded_(encoded),
        long_header_(long_header),
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
        const auto [chunk_end, chunk_error] = std::to_chars(chunk_size.data(), chunk_size.data() + chunk_size.size(), encoded_.size(), 16);
        if (chunk_error != std::errc{}) {
            throw std::runtime_error("HTTP/1 peer could not format chunk size");
        }
        std::string response{
            "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, chunked\r\n"
            "Trailer: X-End\r\nConnection: close\r\n"
        };
        if (!long_header_.empty()) {
            response.append("X-Long: ").append(long_header_).append("\r\n");
        }
        response += "\r\n";
        response.append(chunk_size.data(), chunk_end);
        response.append("\r\n").append(encoded_).append("\r\n");
        co_await asio::async_write(socket, asio::buffer(response), asio::use_awaitable);

        if (end_mode_ == end_mode_type::wait_for_client_close) {
            std::array<char, 1024> input{};
            std::error_code error;
            while (co_await socket.async_read_some(asio::buffer(input), asio::redirect_error(asio::use_awaitable, error))) {
            }
            observed_client_close_ = error == asio::error::eof || error == asio::error::connection_reset || error == asio::error::operation_aborted;
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
    std::string encoded_{};
    std::string long_header_{};
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
    std::move(root).get();
    io.restart();
    if (failure != nullptr) {
        std::rethrow_exception(failure);
    }
}
}  // namespace

TEST(Ruvia, client_response_body_collectors_return_public_results) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    const std::string expected("\0\xff\xc3", 3);
    loopback_response_server server(io, worker.handle_, {expected});
    const std::string client_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(server.port())));
    ruvia::http_client client(worker.attachment_.loop(), local_http_client_config(client_url));
    server.start();
    auto operation = [&]() -> ruvia::task<void> {
        auto response = co_await client.client().get("/collect").send();
        EXPECT_TRUE(!response.stream().complete());
        auto cold_read = response.read();
        auto cold_text = response.stream().text();
        auto collected = co_await response.text();
        EXPECT_TRUE(response.stream().complete());
        EXPECT_EQ(collected.size(), expected.size());
        const std::string_view collected_view = collected.text();
        EXPECT_EQ(collected_view, expected);
        const auto remaining = co_await response.bytes();
        EXPECT_TRUE(remaining.empty());
        EXPECT_TRUE(!(co_await std::move(cold_read)));
        EXPECT_TRUE(!(co_await std::move(cold_text)));
        EXPECT_TRUE(!(co_await response.stream().text()));
        co_await client.shutdown();
        EXPECT_TRUE(response.stream().complete());
        EXPECT_EQ(collected_view, expected);
    };
    run_operation(worker, io, operation);
}

TEST(Ruvia, client_response_body_completion_preserves_truncation_after_buffered_bytes) {
    for (const auto& prefix : {std::string{}, std::string("abc")}) {
        auto& io = ruvia::test::new_test_io_context();
        test_worker worker(io);
        scripted_head_peer peer(io, worker.handle_, {std::string("HTTP/1.1 200 OK\r\nContent-Length: 4\r\nConnection: close\r\n\r\n") + prefix});
        const std::string client_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(peer.port())));
        ruvia::http_client client(worker.attachment_.loop(), local_http_client_config(client_url));
        peer.start();
        auto operation = [&]() -> ruvia::task<void> {
            auto response = co_await client.client().get("/truncated").send();
            std::optional<std::error_code> response_error;
            try {
                (void)co_await response.stream().read_all();
            } catch (const ruvia::http_client_error& error) {
                response_error = error.code();
            }
            EXPECT_TRUE(response_error.has_value());
            std::string received{};
            while (received.size() < prefix.size()) {
                EXPECT_TRUE(!response.stream().complete());
                const auto chunk = co_await response.stream().text();
                if (!chunk) {
                    break;
                }
                received.append(*chunk);
            }
            EXPECT_EQ(received, prefix);
            std::optional<std::error_code> completion_error;
            try {
                (void)response.stream().complete();
            } catch (const ruvia::http_client_error& error) {
                completion_error = error.code();
            }
            EXPECT_TRUE(completion_error == response_error);
            std::optional<std::error_code> read_error;
            try {
                (void)co_await response.stream().read();
            } catch (const ruvia::http_client_error& error) {
                read_error = error.code();
            }
            EXPECT_TRUE(read_error == response_error);
            co_await peer.wait();
            co_await client.shutdown();
        };
        run_operation(worker, io, operation);
    }
}

// A response is complete at its declared Content-Length. Bytes beyond that
// boundary belong to no requested response: retain the complete response and
// retire the connection instead of turning the valid message into a failure.
TEST(Ruvia, http_client_preserves_fixed_length_response_before_unsolicited_bytes) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    scripted_head_peer peer(io, worker.handle_, {"HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nokunsolicited"});
    const std::string client_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(peer.port())));
    ruvia::http_client client(worker.attachment_.loop(), local_http_client_config(client_url));
    peer.start();
    auto operation = [&]() -> ruvia::task<void> {
        auto response = co_await client.client().get("/overlong").send();
        const auto body = co_await response.stream().read_all();
        EXPECT_EQ(body.text(), std::string_view("ok"));
        co_await peer.wait();
        co_await client.shutdown();
    };
    run_operation(worker, io, operation);
}

TEST(Ruvia, client_response_body_claims_its_shared_lane_only_when_started) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    http1_chunked_response_peer peer(io, large_gzip_response_body());
    peer.start();
    const std::string client_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(peer.port())));
    ruvia::http_client client(worker.attachment_.loop(), local_http_client_config(client_url));
    auto operation = [&]() -> ruvia::task<void> {
        auto response = co_await client.client().get("/body-lane").send();
        auto collect = response.stream().read_all();
        auto read = response.stream().read();
        auto text = response.stream().text();
        bool collected = false;
        ruvia::task_scope readers(worker.handle_);
        const auto consume = [&]() -> ruvia::task<void> {
            const auto bytes_value = co_await std::move(collect);
            collected = bytes_value.size() == 128 * 1024;
        };
        readers.spawn(consume());
        bool read_rejected = false;
        bool text_rejected = false;
        bool collect_rejected = false;
        try {
            (void)co_await std::move(read);
        } catch (const std::logic_error&) {
            read_rejected = true;
        }
        try {
            (void)co_await std::move(text);
        } catch (const std::logic_error&) {
            text_rejected = true;
        }
        try {
            (void)co_await response.stream().read_all();
        } catch (const std::logic_error&) {
            collect_rejected = true;
        }
        peer.release_terminal_chunk();
        co_await readers.join();
        EXPECT_TRUE(read_rejected && text_rejected && collect_rejected);
        EXPECT_TRUE(collected);
        EXPECT_TRUE(!(co_await response.stream().text()));
        co_await client.shutdown();
    };
    run_operation(worker, io, operation);
    peer.wait();
}

namespace {
void exercise_final_head_cookies(ruvia::http_client_versions protocol) {
    for (const auto policy : {ruvia::http_client_received_cookie_policy::retain_and_send, ruvia::http_client_received_cookie_policy::ignore}) {
        auto& io = ruvia::test::new_test_io_context();
        test_worker worker(io);
        const bool http2 = protocol == ruvia::http_client_versions{.http1_ = false};
        cookie_stream_peer peer(io, worker.handle_, http2 ? cookie_peer_mode::http2 : cookie_peer_mode::http1);
        const std::string config_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(peer.port())));
        auto config = local_http_client_config(config_url);
        config.connection_limits_.max_open_ = http2 ? 1 : 2;
        config.versions_ = protocol;
        config.received_cookies_ = policy;
        config.request_timeout_ = std::chrono::seconds(3);
        ruvia::http_client client(worker.attachment_.loop(), config);
        peer.start();
        auto operation = [&]() -> ruvia::task<void> {
            std::optional<ruvia::http_client_response> streaming;
            streaming.emplace(co_await client.client().get("/stream").send());
            EXPECT_TRUE(!streaming->stream().complete());
            EXPECT_TRUE(streaming->protocol_version() == (http2 ? ruvia::http_protocol_version::http2 : ruvia::http_protocol_version::http11));
            EXPECT_EQ(streaming->informational_responses().size(), std::size_t{1});
            {
                auto response = co_await client.client().get("/before-abandonment").send();
                EXPECT_TRUE((co_await response.stream().read_all()).empty());
            }
            streaming.reset();
            {
                auto response = co_await client.client().get("/after-abandonment").send();
                EXPECT_TRUE((co_await response.stream().read_all()).empty());
            }
            co_await peer.wait();
            for (const auto& request : peer.requests_) {
                const bool received = request.find("session=ready") != std::string::npos;
                EXPECT_TRUE(received == (policy == ruvia::http_client_received_cookie_policy::retain_and_send));
                EXPECT_TRUE(request.find("informational=ignored") == std::string::npos);
            }
            co_await client.shutdown();
        };
        run_operation(worker, io, operation);
    }
}
}  // namespace

TEST(Ruvia, http_client_retains_final_head_cookies_before_body_completion_and_after_abandonment) {
    exercise_final_head_cookies(ruvia::http_client_versions{});
}

#if defined(RUVIA_ENABLE_HTTP2)
TEST(Ruvia, http2_client_retains_final_head_cookies_before_body_completion_and_after_abandonment) {
    exercise_final_head_cookies(ruvia::http_client_versions{.http1_ = false});
}

#endif

TEST(Ruvia, http1_slow_body_completion_does_not_roll_back_newer_final_head_cookies) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    cookie_stream_peer peer(io, worker.handle_, cookie_peer_mode::http1_replacement);
    const std::string config_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(peer.port())));
    auto config = local_http_client_config(config_url);
    config.connection_limits_.max_open_ = 2;
    config.received_cookies_ = ruvia::http_client_received_cookie_policy::retain_and_send;
    config.request_timeout_ = std::chrono::seconds(3);
    ruvia::http_client client(worker.attachment_.loop(), config);
    peer.start();
    auto operation = [&]() -> ruvia::task<void> {
        auto slow = co_await client.client().get("/scope/stream").send();
        EXPECT_TRUE(!slow.stream().complete());
        {
            auto update = co_await client.client().get("/scope/update").send();
            EXPECT_TRUE((co_await update.stream().read_all()).empty());
        }
        // The gate opens only after the newer final head has updated the jar.
        peer.release_terminal_body();
        const auto body = co_await slow.stream().read_all();
        EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(body.bytes().data()), body.size()), std::string_view("x"));
        {
            auto check = co_await client.client().get("/scope/check").send();
            EXPECT_TRUE((co_await check.stream().read_all()).empty());
        }
        co_await peer.wait();
        EXPECT_TRUE(peer.requests_[0].find("sid=old") != std::string::npos);
        EXPECT_TRUE(peer.requests_[1].find("sid=new") != std::string::npos);
        EXPECT_TRUE(peer.requests_[1].find("sid=old") == std::string::npos);
        co_await client.shutdown();
    };
    run_operation(worker, io, operation);
}

// RFC 6265 section 5.3 step 11: a Domain cookie replaces the host-only cookie
// with the same name and path, since a host-only cookie's domain is the host.
TEST(Ruvia, http_client_replaces_host_only_cookie_with_same_named_domain_cookie) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    scripted_head_peer peer(io, worker.handle_,
        {"HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\nSet-Cookie: sid=host; Path=/\r\n\r\n",
            "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n"
            "Set-Cookie: sid=domain; Domain=127.0.0.1; Path=/\r\n\r\n",
            "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"});
    const std::string config_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(peer.port())));
    auto config = local_http_client_config(config_url);
    // Match the IPv4 peer directly without DNS or IPv6 connection fallback.
    config.received_cookies_ = ruvia::http_client_received_cookie_policy::retain_and_send;
    config.request_timeout_ = std::chrono::seconds(3);
    ruvia::http_client client(worker.attachment_.loop(), config);
    peer.start();
    auto operation = [&]() -> ruvia::task<void> {
        for (const auto* target : {"/host", "/domain", "/check"}) {
            auto response = co_await client.client().get(target).send();
            EXPECT_TRUE((co_await response.stream().read_all()).empty());
            EXPECT_TRUE(response.stream().complete());
        }
        co_await peer.wait();
        EXPECT_EQ(peer.requests_.size(), std::size_t{3});
        if (peer.requests_.size() == 3) {
            EXPECT_TRUE(peer.requests_[1].find("sid=host") != std::string::npos);
            EXPECT_TRUE(peer.requests_[2].find("sid=domain") != std::string::npos);
            EXPECT_TRUE(peer.requests_[2].find("sid=host") == std::string::npos);
        }
        co_await client.shutdown();
    };
    run_operation(worker, io, operation);
}

// RFC 6265 section 5.2: nameless cookies are ignored; "Domain=." leaves a
// host-only cookie, an invalid Path selects the default-path, and stored values
// keep their internal whitespace on the way back.
TEST(Ruvia, http_client_jar_follows_user_agent_set_cookie_algorithm) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    scripted_head_peer peer(io, worker.handle_,
        {"HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n"
         "Set-Cookie: nameless\r\nSet-Cookie: =emptyname\r\n"
         "Set-Cookie: f=hello world\r\nSet-Cookie: h=1; Domain=.\r\n"
         "Set-Cookie: p=2; Path=/caf\xc3\xa9\r\nSet-Cookie: q=3; Path=relative\r\n"
         "Set-Cookie: d=4; Domain=127.0.0.1; Domain=\r\n"
         "Set-Cookie: part=insecure; Path=/; Partitioned\r\n\r\n",
            "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"});
    const std::string config_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(peer.port())));
    auto config = local_http_client_config(config_url);
    config.received_cookies_ = ruvia::http_client_received_cookie_policy::retain_and_send;
    config.request_timeout_ = std::chrono::seconds(3);
    ruvia::http_client client(worker.attachment_.loop(), config);
    peer.start();
    auto operation = [&]() -> ruvia::task<void> {
        for (const auto* target : {"/set", "/check"}) {
            auto response = co_await client.client().get(target).send();
            EXPECT_TRUE((co_await response.stream().read_all()).empty());
        }
        co_await peer.wait();
        EXPECT_EQ(peer.requests_.size(), std::size_t{2});
        if (peer.requests_.size() == 2) {
            const auto& check = peer.requests_[1];
            EXPECT_TRUE(check.find("=nameless") == std::string::npos);
            EXPECT_TRUE(check.find("=emptyname") == std::string::npos);
            EXPECT_TRUE(check.find("f=hello world") != std::string::npos);
            EXPECT_TRUE(check.find("h=1") != std::string::npos);
            EXPECT_TRUE(check.find("q=3") != std::string::npos);
            EXPECT_TRUE(check.find("d=4") != std::string::npos);
            // Path=/café is stored as given and does not match "/check".
            EXPECT_TRUE(check.find("p=2") == std::string::npos);
            EXPECT_TRUE(check.find("part=insecure") == std::string::npos);
        }
        co_await client.shutdown();
    };
    run_operation(worker, io, operation);
}

TEST(Ruvia, http_client_merges_explicit_cookie_fields_without_losing_other_headers) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    scripted_head_peer peer(io, worker.handle_, {"HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"});
    const std::string invalid_overridden_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(peer.port())));
    auto invalid_overridden = local_http_client_config(invalid_overridden_url);
    invalid_overridden.cookies_ = {{"sid", "bad value"}, {"sid", "stored"}};
    bool rejected_invalid_overridden = false;
    try {
        ruvia::http_client rejected(worker.attachment_.loop(), invalid_overridden);
    } catch (const std::invalid_argument&) {
        rejected_invalid_overridden = true;
    }
    EXPECT_TRUE(rejected_invalid_overridden);
    const std::string config_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(peer.port())));
    auto config = local_http_client_config(config_url);
    config.max_cookies_ = 2;
    config.max_cookie_bytes_ = 16;
    config.cookies_ = {{"sid", "obsolete-long-value"}, {"mode", "1"}, {"sid", "stored"}};
    ruvia::http_client client(worker.attachment_.loop(), config);
    peer.start();
    auto operation = [&]() -> ruvia::task<void> {
        const std::array fields{ruvia::http_header_view{"Cookie", "a=1"}, ruvia::http_header_view{"X-Test", "present"}, ruvia::http_header_view{"cOoKiE", "b=2"}};
        auto response = co_await client.client().get("/").headers(fields).send();
        EXPECT_TRUE((co_await response.stream().read_all()).empty());
        co_await peer.wait();
        EXPECT_EQ(peer.requests_.size(), std::size_t{1});
        if (!peer.requests_.empty()) {
            const std::string_view request = peer.requests_.front();
            std::size_t cookie_count = 0;
            bool merged = false;
            bool other_header = false;
            auto line_start = request.find("\r\n") + 2;
            while (line_start < request.size()) {
                const auto line_end = request.find("\r\n", line_start);
                if (line_end == std::string_view::npos || line_end == line_start) {
                    break;
                }
                const auto line = std::string_view(request).substr(line_start, line_end - line_start);
                const auto colon = line.find(':');
                if (colon != std::string_view::npos) {
                    const auto name = line.substr(0, colon);
                    const auto value = line.substr(colon + 1);
                    if (ruvia::http_ascii_equals_ignore_case(name, "cookie")) {
                        ++cookie_count;
                        merged = value == " a=1; b=2; sid=stored; mode=1";
                    } else if (ruvia::http_ascii_equals_ignore_case(name, "x-test")) {
                        other_header = value == " present";
                    }
                }
                line_start = line_end + 2;
            }
            EXPECT_EQ(cookie_count, std::size_t{1});
            EXPECT_TRUE(merged);
            EXPECT_TRUE(other_header);
        }
        co_await client.shutdown();
    };
    run_operation(worker, io, operation);
}

// RFC 9110 section 9.3.8: stored cookies are sensitive and stay out of TRACE.
TEST(Ruvia, http_client_omits_stored_cookies_from_trace) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    scripted_head_peer peer(io, worker.handle_, {"HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n\r\n", "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"});
    const std::string config_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(peer.port())));
    auto config = local_http_client_config(config_url);
    config.cookies_ = {{"sid", "secret"}};
    config.request_timeout_ = std::chrono::seconds(3);
    ruvia::http_client client(worker.attachment_.loop(), config);
    peer.start();
    auto operation = [&]() -> ruvia::task<void> {
        for (const auto* method : {"TRACE", "GET"}) {
            auto response = co_await client.client().request(method, "/").send();
            EXPECT_TRUE((co_await response.stream().read_all()).empty());
        }
        co_await peer.wait();
        EXPECT_EQ(peer.requests_.size(), std::size_t{2});
        if (peer.requests_.size() == 2) {
            EXPECT_TRUE(peer.requests_[0].find("sid=secret") == std::string::npos);
            EXPECT_TRUE(peer.requests_[1].find("sid=secret") != std::string::npos);
        }
        co_await client.shutdown();
    };
    run_operation(worker, io, operation);
}

// RFC 9110 sections 14 and 15.3.7: a range of an encoded representation is not
// decodable alone, so a 206 body is delivered as received.
TEST(Ruvia, http_client_passes_encoded_partial_content_through) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    const std::string gzip_prefix("\x1f\x8b\x08\x00\x00\x00\x00\x00\x00\x03", 10);
    scripted_head_peer peer(io, worker.handle_,
        {"HTTP/1.1 206 Partial Content\r\nContent-Encoding: gzip\r\nContent-Range: bytes 0-9/100\r\n"
         "Content-Length: 10\r\nConnection: close\r\n\r\n" +
            gzip_prefix});
    const std::string config_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(peer.port())));
    auto config = local_http_client_config(config_url);
    config.request_timeout_ = std::chrono::seconds(3);
    ruvia::http_client client(worker.attachment_.loop(), config);
    peer.start();
    auto operation = [&]() -> ruvia::task<void> {
        auto response = co_await client.client().get("/range").send();
        EXPECT_EQ(response.status().value(), std::uint16_t{206});
        const auto bytes_value = co_await response.stream().read_all();
        EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(bytes_value.bytes().data()), bytes_value.size()), std::string_view(gzip_prefix));
        co_await peer.wait();
        co_await client.shutdown();
    };
    run_operation(worker, io, operation);
}

// RFC 9110 §8.4.1 / §12.5.3: content codings are an open registry and a request
// without Accept-Encoding accepts any of them. A stack the client cannot fully
// decode is delivered as received, with Content-Encoding kept.
TEST(Ruvia, http_client_passes_unsupported_content_codings_through) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    const std::string opaque("\x01opaque\xff", 8);
    scripted_head_peer peer(io, worker.handle_, {std::string("HTTP/1.1 200 OK\r\nContent-Encoding: x-custom\r\nContent-Length: 8\r\nConnection: close\r\n\r\n") + opaque, std::string("HTTP/1.1 200 OK\r\nContent-Encoding: gzip, compress\r\nContent-Length: 8\r\nConnection: close\r\n\r\n") + opaque});
    const std::string config_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(peer.port())));
    auto config = local_http_client_config(config_url);
    config.request_timeout_ = std::chrono::seconds(3);
    ruvia::http_client client(worker.attachment_.loop(), config);
    peer.start();
    auto operation = [&]() -> ruvia::task<void> {
        for (const std::string_view coding : {std::string_view("x-custom"), std::string_view("gzip, compress")}) {
            auto response = co_await client.client().get("/coded").send();
            EXPECT_EQ(response.status().value(), std::uint16_t{200});
            EXPECT_TRUE(response.header("Content-Encoding") == std::optional<std::string_view>(coding));
            const auto bytes_value = co_await response.stream().read_all();
            EXPECT_EQ(bytes_value.text(), std::string_view(opaque));
        }
        co_await peer.wait();
        co_await client.shutdown();
    };
    run_operation(worker, io, operation);
}

TEST(Ruvia, http_client_strips_decoded_content_encoding_and_rejects_overdeep_known_stacks) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    const auto gz_hello = *ruvia::encode_http_content(ruvia::http_content_coding::gzip, "hello", {.max_encoded_bytes_ = 1024});
    scripted_head_peer peer(io, worker.handle_,
        {std::move(std::string("HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\nContent-Length: ").append(std::to_string(gz_hello.size()))) + "\r\nConnection: close\r\n\r\n" + std::string(gz_hello),
            std::move(std::string("HTTP/1.1 200 OK\r\nContent-Encoding: identity, identity, identity, identity, identity, "
                                  "identity, identity, identity, gzip\r\nContent-Length: ")
                    .append(std::to_string(gz_hello.size()))) +
                "\r\nConnection: close\r\n\r\n" + std::string(gz_hello),
            std::move(std::string("HTTP/1.1 200 OK\r\nContent-Encoding: gzip, gzip, gzip, gzip, gzip, gzip, gzip, gzip, gzip\r\n"
                                  "Content-Length: ")
                    .append(std::to_string(gz_hello.size()))) +
                "\r\nConnection: close\r\n\r\n" + std::string(gz_hello)});
    const std::string config_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(peer.port())));
    auto config = local_http_client_config(config_url);
    config.request_timeout_ = std::chrono::seconds(3);
    ruvia::http_client client(worker.attachment_.loop(), config);
    peer.start();
    auto operation = [&]() -> ruvia::task<void> {
        for (int i = 0; i < 2; ++i) {
            auto response = co_await client.client().get("/decoded").send();
            EXPECT_EQ(response.status().value(), std::uint16_t{200});
            const auto bytes_value = co_await response.stream().read_all();
            EXPECT_EQ(bytes_value.text(), std::string_view("hello"));
            EXPECT_TRUE(!response.header("Content-Encoding").has_value());
            EXPECT_TRUE(!response.header("Content-Length").has_value());
        }
        bool rejected_overdeep = false;
        try {
            auto response = co_await client.client().get("/overdeep").send();
            (void)co_await response.stream().read_all();
        } catch (const ruvia::http_client_error& error) {
            rejected_overdeep = error.code() == ruvia::make_error_code(ruvia::http_client_errc::protocol_error);
        }
        EXPECT_TRUE(rejected_overdeep);
        co_await peer.wait();
        co_await client.shutdown();
    };
    run_operation(worker, io, operation);
}

TEST(Ruvia, http_client_preserves_large_unsigned_timeout_bounds) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    loopback_response_server server(io, worker.handle_, {"first", "second"}, std::chrono::milliseconds(10));
    const std::string config_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(server.port())));
    auto config = local_http_client_config(config_url);
    const auto large = std::chrono::duration<std::uint64_t, std::milli>{std::numeric_limits<std::uint64_t>::max()};
    config.connect_timeout_ = large;
    config.write_timeout_ = large;
    config.read_timeout_ = large;
    config.request_timeout_ = large;
    config.acquire_timeout_ = large;
    config.idle_timeout_ = large;
    config.http2_ping_interval_ = large;
    config.push_.timeout_ = large;
    ruvia::http_client client(worker.attachment_.loop(), config);
    server.start();
    auto operation = [&]() -> ruvia::task<void> {
        auto first = co_await client.client().get("/large-config").send();
        auto first_body = co_await first.stream().read_all();
        EXPECT_EQ(first_body.text(), std::string_view("first"));
        auto second = co_await client.client().with_options({.timeout_ = large}).get("/large-operation").send();
        auto second_body = co_await second.stream().read_all();
        EXPECT_EQ(second_body.text(), std::string_view("second"));
        co_await server.wait();
        co_await client.shutdown();
    };
    run_operation(worker, io, operation);
}

TEST(Ruvia, http_client_ref_options_override_pool_timeout_and_start_when_operation_runs) {
    {
        auto& io = ruvia::test::new_test_io_context();
        test_worker worker(io);
        loopback_response_server server(io, worker.handle_, {"slow"}, std::chrono::milliseconds(350));
        const std::string config_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(server.port())));
        auto config = local_http_client_config(config_url);
        config.request_timeout_ = std::chrono::milliseconds(50);
        ruvia::http_client client(worker.attachment_.loop(), config);
        server.start();

        auto operation = [&]() -> ruvia::task<void> {
            auto handle = client.client().with_options({.timeout_ = std::chrono::seconds(2)});
            auto cold = handle.get("/pool-timeout-override").send();
            (void)co_await ruvia::sleep_for(worker.handle_, std::chrono::milliseconds(250));
            const auto start = std::chrono::steady_clock::now();
            auto response = co_await std::move(cold);
            const auto elapsed = std::chrono::steady_clock::now() - start;
            EXPECT_EQ(response.status(), ruvia::http_status_code::from_value(200));
            EXPECT_TRUE(elapsed >= std::chrono::milliseconds(50));
            EXPECT_TRUE(elapsed < std::chrono::seconds(2));
            co_await server.wait();
            co_await client.shutdown();
        };
        run_operation(worker, io, operation);
    }

    {
        auto& io = ruvia::test::new_test_io_context();
        test_worker worker(io);
        loopback_response_server server(io, worker.handle_, {"late"}, std::chrono::milliseconds(350));
        const std::string config_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(server.port())));
        auto config = local_http_client_config(config_url);
        config.request_timeout_ = std::chrono::seconds(5);
        ruvia::http_client client(worker.attachment_.loop(), config);
        server.start();

        auto operation = [&]() -> ruvia::task<void> {
            auto base = client.client().with_options({.timeout_ = std::chrono::seconds(5)});
            auto shortened = base.with_options({.timeout_ = std::chrono::milliseconds(100)});
            auto extended = shortened.with_options({.timeout_ = std::chrono::seconds(5)});
            auto cold = extended.get("/successive-minimum").send();
            (void)co_await ruvia::sleep_for(worker.handle_, std::chrono::milliseconds(250));
            const auto start = std::chrono::steady_clock::now();
            bool timed_out = false;
            try {
                (void)co_await std::move(cold);
            } catch (const ruvia::http_client_error& error) {
                timed_out = error.code() == std::make_error_code(std::errc::timed_out);
            }
            const auto elapsed = std::chrono::steady_clock::now() - start;
            EXPECT_TRUE(timed_out);
            EXPECT_TRUE(elapsed >= std::chrono::milliseconds(50));
            EXPECT_TRUE(elapsed < std::chrono::seconds(2));
            // The deadline may expire before the connection reaches the peer.
            server.stop_accepting();
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
        loopback_response_server server(io, worker.handle_, {"cancelled"}, std::chrono::milliseconds(350));
        const std::string client_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(server.port())));
        ruvia::http_client client(worker.attachment_.loop(), local_http_client_config(client_url));
        ruvia::stop_source base_stop;
        ruvia::stop_source derived_stop;
        server.start();

        auto operation = [&]() -> ruvia::task<void> {
            // Stop the in-flight exchange once the peer holds its request.
            server.on_request([&base_stop, &derived_stop, stop_base] {
                (stop_base ? base_stop : derived_stop).request_stop();
            });
            ruvia::stop_group cancellation{base_stop.token(), derived_stop.token()};
            auto base = client.client().with_options({.stop_token_ = base_stop.token()});
            auto copied = base;
            auto derived = copied.with_options({.stop_token_ = cancellation.token()});
            bool cancelled = false;
            try {
                (void)co_await derived.get(stop_base ? "/stop-base" : "/stop-derived").send();
            } catch (const ruvia::http_client_error& error) {
                cancelled = error.code() == std::make_error_code(std::errc::operation_canceled);
            }
            EXPECT_TRUE(cancelled);
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
        loopback_response_server server(io, worker.handle_, {"retiring"}, std::chrono::milliseconds(350));
        const std::string client_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(server.port())));
        ruvia::http_client client(worker.attachment_.loop(), local_http_client_config(client_url));
        server.start();

        auto operation = [&]() -> ruvia::task<void> {
            server.on_request([&worker] {
                worker.attachment_.stop();
            });
            bool cancelled = false;
            try {
                (void)co_await client.client().get("/event-loop-stop").send();
            } catch (const ruvia::http_client_error& error) {
                cancelled = error.code() == ruvia::make_error_code(ruvia::http_client_errc::closing);
            }
            EXPECT_TRUE(cancelled);
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
        loopback_response_server server(io, worker.handle_, {"closing"}, std::chrono::milliseconds(350));
        const std::string client_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(server.port())));
        ruvia::http_client client(worker.attachment_.loop(), local_http_client_config(client_url));
        server.start();

        auto operation = [&]() -> ruvia::task<void> {
            server.on_request([&client] {
                client.close();
            });
            auto cold = client.client().get("/active-close").send();
            bool closing = false;
            try {
                (void)co_await std::move(cold);
            } catch (const ruvia::http_client_error& error) {
                closing = error.code() == ruvia::make_error_code(ruvia::http_client_errc::closing);
            }
            EXPECT_TRUE(closing);
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

TEST(Ruvia, http1_full_response_queue_cancellation_and_deadline_finish_without_reading) {
    for (const bool deadline : {false, true}) {
        auto& io = ruvia::test::new_test_io_context();
        test_worker worker(io);
        loopback_response_server server(io, worker.handle_, {std::string(65536, 'q')});
        const std::string config_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(server.port())));
        auto config = local_http_client_config(config_url);
        config.max_response_bytes_ = 1024;
        ruvia::http_client client(worker.attachment_.loop(), config);
        ruvia::stop_source stop;
        server.start();

        auto operation = [&]() -> ruvia::task<void> {
            auto handle = client.client().with_options({
                .timeout_ = deadline ? std::optional(std::chrono::milliseconds(40)) : std::nullopt,
                .stop_token_ = stop.token(),
            });
            auto response = co_await handle.get("/backpressure").send();
            EXPECT_EQ(response.status(), ruvia::http_status::ok);
            EXPECT_EQ(client.client().stats().in_flight_requests_, std::size_t{1});
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
                    asio::post(io, [done = std::move(done)]() mutable {
                        done(std::error_code{});
                    });
                });
            }
            EXPECT_EQ(client.client().stats().failed_requests_, std::size_t{1});
            EXPECT_EQ(client.client().stats().in_flight_requests_, std::size_t{0});
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

TEST(Ruvia, http1_full_response_queue_loop_stop_joins_without_reading) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    loopback_response_server server(io, worker.handle_, {std::string(65536, 'q')});
    const std::string config_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(server.port())));
    auto config = local_http_client_config(config_url);
    config.max_response_bytes_ = 1024;
    ruvia::http_client client(worker.attachment_.loop(), config);
    server.start();
    bool joined = false;
    auto operation = [&]() -> ruvia::task<void> {
        auto response = co_await client.client().get("/backpressure-stop").send();
        EXPECT_EQ(response.status(), ruvia::http_status::ok);
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
    EXPECT_TRUE(joined);
    EXPECT_TRUE(!worker.handle_.valid());
}

TEST(Ruvia, http1_transfer_gzip_full_queue_cancel_and_deadline_without_reading) {
    const auto encoded = large_gzip_response_body();
    for (const bool deadline : {false, true}) {
        auto& io = ruvia::test::new_test_io_context();
        test_worker worker(io);
        http1_chunked_response_peer peer(io, encoded, http1_chunked_response_peer::end_mode_type::wait_for_client_close);
        const std::string config_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(peer.port())));
        auto config = local_http_client_config(config_url);
        config.max_response_bytes_ = 1024;
        ruvia::http_client client(worker.attachment_.loop(), config);
        ruvia::stop_source stop;
        peer.start();

        auto operation = [&]() -> ruvia::task<void> {
            auto handle = client.client().with_options({
                .timeout_ = deadline ? std::optional(std::chrono::milliseconds(200)) : std::nullopt,
                .stop_token_ = stop.token(),
            });
            auto response = co_await handle.get("/transfer-gzip-backpressure").send();
            EXPECT_EQ(response.status(), ruvia::http_status::ok);
            EXPECT_EQ(client.client().stats().in_flight_requests_, std::size_t{1});
            for (unsigned turn = 0; turn != 4; ++turn) {
                (void)co_await ruvia::async_asio([&io](auto done) {
                    asio::post(io, [done = std::move(done)]() mutable {
                        done(std::error_code{});
                    });
                });
            }
            EXPECT_TRUE(client.client().stats().plaintext_bytes_received_ >= encoded.size());

            if (deadline) {
                (void)co_await ruvia::sleep_for(worker.handle_, std::chrono::milliseconds(300));
            } else {
                stop.request_stop();
            }
            for (unsigned turn = 0; turn != 4; ++turn) {
                (void)co_await ruvia::async_asio([&io](auto done) {
                    asio::post(io, [done = std::move(done)]() mutable {
                        done(std::error_code{});
                    });
                });
            }
            EXPECT_EQ(client.client().stats().failed_requests_, std::size_t{1});
            EXPECT_EQ(client.client().stats().in_flight_requests_, std::size_t{0});
            // Keep response alive through join. No body read or response
            // destruction may be what releases the producer's full queue.
            co_await client.shutdown();
        };
        run_operation(worker, io, operation);
        peer.wait();
        EXPECT_TRUE(peer.observed_client_close());
    }
}

TEST(Ruvia, http1_transfer_gzip_full_queue_loop_stop_joins_without_reading) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    const auto encoded = large_gzip_response_body();
    http1_chunked_response_peer peer(io, encoded, http1_chunked_response_peer::end_mode_type::wait_for_client_close);
    const std::string config_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(peer.port())));
    auto config = local_http_client_config(config_url);
    config.max_response_bytes_ = 1024;
    ruvia::http_client client(worker.attachment_.loop(), config);
    peer.start();
    bool joined = false;
    auto operation = [&]() -> ruvia::task<void> {
        auto response = co_await client.client().get("/transfer-gzip-stop").send();
        EXPECT_EQ(response.status(), ruvia::http_status::ok);
        EXPECT_EQ(client.client().stats().in_flight_requests_, std::size_t{1});
        for (unsigned turn = 0; turn != 4; ++turn) {
            (void)co_await ruvia::async_asio([&io](auto done) {
                asio::post(io, [done = std::move(done)]() mutable {
                    done(std::error_code{});
                });
            });
        }
        EXPECT_TRUE(client.client().stats().plaintext_bytes_received_ >= encoded.size());
        worker.attachment_.stop();
        co_await client.shutdown();
        joined = true;
    };
    run_operation(worker, io, operation);
    peer.wait();
    EXPECT_TRUE(joined);
    EXPECT_TRUE(peer.observed_client_close());
    EXPECT_TRUE(!worker.handle_.valid());
}

TEST(Ruvia, http1_transfer_gzip_chunked_streams_before_terminal_chunk_and_bounds_each_decode) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    const std::string expected(128 * 1024, 'z');
    auto encoded_result = ruvia::encode_http_content(ruvia::http_content_coding::gzip, expected, {.max_encoded_bytes_ = expected.size()});
    EXPECT_TRUE(encoded_result.has_value());
    if (!encoded_result.has_value()) {
        return;
    }
    http1_chunked_response_peer peer(io, std::string(encoded_result->data(), encoded_result->size()));
    const std::string config_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(peer.port())));
    auto config = local_http_client_config(config_url);
    config.max_response_bytes_ = 1024;
    ruvia::http_client client(worker.attachment_.loop(), config);
    peer.start();

    std::size_t decoded_bytes = 0;
    auto operation = [&]() -> ruvia::task<void> {
        auto response = co_await client.client().get("/transfer-gzip").send();
        EXPECT_EQ(response.status(), ruvia::http_status::ok);
        EXPECT_EQ(client.client().stats().in_flight_requests_, std::size_t{1});

        const auto first = co_await response.stream().read();
        EXPECT_TRUE(first.has_value());
        EXPECT_TRUE(first->size() <= config.max_response_bytes_);
        EXPECT_TRUE(!response.stream().complete());
        const auto first_view = std::string_view(reinterpret_cast<const char*>(first->data()), first->size());
        decoded_bytes += first->size();

        // Let the producer decode already-buffered compressed bytes while the
        // consumer's borrowed view remains live; only pending storage may grow.
        for (unsigned turn = 0; turn != 4; ++turn) {
            (void)co_await ruvia::async_asio([&io](auto done) {
                asio::post(io, [done = std::move(done)]() mutable {
                    done(std::error_code{});
                });
            });
        }
        EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(first->data()), first->size()), first_view);
        peer.release_terminal_chunk();

        while (const auto chunk = co_await response.stream().read()) {
            EXPECT_TRUE(chunk->size() <= config.max_response_bytes_);
            decoded_bytes += chunk->size();
        }
        EXPECT_TRUE(response.stream().complete());
        EXPECT_EQ(response.trailer("x-end"), std::optional<std::string_view>("retained"));
        EXPECT_EQ(client.client().stats().in_flight_requests_, std::size_t{0});
        co_await client.shutdown();
    };
    run_operation(worker, io, operation);
    peer.wait();
    EXPECT_EQ(decoded_bytes, expected.size());
    EXPECT_TRUE(decoded_bytes > config.max_response_bytes_);
}

TEST(Ruvia, http1_response_storage_remains_worker_owned_after_client_destruction) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    const std::string expected(128 * 1024, 'z');
    const auto encoded = large_gzip_response_body();
    const std::string long_header(32 * 1024, 'h');
    http1_chunked_response_peer peer(io, encoded, http1_chunked_response_peer::end_mode_type::terminal_gate, long_header);
    peer.start();

    auto operation = [&]() -> ruvia::task<void> {
        std::optional<ruvia::http_client_response> response;
        std::optional<ruvia::http_client_response_bytes> retained_body;
        std::string collected{};
        {
            const std::string client_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(peer.port())));
            ruvia::http_client client(worker.attachment_.loop(), local_http_client_config(client_url));
            response.emplace(co_await client.client().get("/retained-response").send());
            const auto first = co_await response->stream().read();
            EXPECT_TRUE(first.has_value());
            if (first) {
                collected.append(reinterpret_cast<const char*>(first->data()), first->size());
            }
            peer.release_terminal_chunk();
            retained_body.emplace(co_await response->stream().read_all());
            co_await client.shutdown();
        }

        const auto headers = response->headers();
        const auto long_value = response->header("x-long");
        EXPECT_TRUE(long_value.has_value());
        EXPECT_EQ(long_value->size(), long_header.size());
        EXPECT_TRUE(std::ranges::all_of(*long_value, [](char value) {
            return value == 'h';
        }));
        EXPECT_TRUE(!headers.empty());
        const auto text = co_await response->stream().text();
        EXPECT_TRUE(!text.has_value());
        EXPECT_TRUE(!(co_await response->stream().read()).has_value());
        const auto remaining = co_await response->stream().read_all();
        EXPECT_TRUE(remaining.empty());
        const auto retained_view = retained_body->bytes();
        collected.append(reinterpret_cast<const char*>(retained_view.data()), retained_view.size());
        EXPECT_EQ(collected, expected);
        EXPECT_EQ(response->trailer("x-end"), std::optional<std::string_view>("retained"));
        response.reset();
        EXPECT_EQ(retained_body->bytes().size(), retained_view.size());
        EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(retained_body->bytes().data()), retained_body->bytes().size()), std::string_view(expected).substr(expected.size() - retained_view.size()));
        retained_body.reset();
    };
    run_operation(worker, io, operation);
    peer.wait();
}

TEST(Ruvia, client_response_cold_collection_survives_client_and_expires_with_response) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    loopback_response_server server(io, worker.handle_, {"cold-body"});
    server.start();

    auto operation = [&]() -> ruvia::task<void> {
        const std::string client_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(server.port())));
        auto client = std::make_unique<ruvia::http_client>(worker.attachment_.loop(), local_http_client_config(client_url));
        std::optional<ruvia::http_client_response> response;
        response.emplace(co_await client->client().get("/cold-body").send());
        auto cold = response->stream().read_all();
        co_await client->shutdown();
        client.reset();

        auto retained = co_await std::move(cold);
        EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(retained.bytes().data()), retained.bytes().size()), std::string_view("cold-body"));
        auto expired = response->stream().read_all();
        response.reset();
        bool rejected = false;
        try {
            (void)co_await std::move(expired);
        } catch (const std::logic_error&) {
            rejected = true;
        }
        EXPECT_TRUE(rejected);
        co_await server.wait();
    };
    run_operation(worker, io, operation);
}

TEST(Ruvia, http_client_sends_heads_beyond_the_retained_buffer_and_rejects_oversized_heads) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    loopback_response_server server(io, worker.handle_, {"large-head", "small-head"});
    server.start();

    auto operation = [&]() -> ruvia::task<void> {
        const std::string client_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(server.port())));
        ruvia::http_client client(worker.attachment_.loop(), local_http_client_config(client_url));
        const auto body_text = [](const ruvia::http_client_response_bytes& bytes_value) {
            return std::string(reinterpret_cast<const char*>(bytes_value.bytes().data()), bytes_value.bytes().size());
        };
        // Larger than the head buffer a connection retains, within the limit.
        const std::string large_value(20 * 1024, 'v');
        const std::array large_headers{ruvia::http_header_view{"X-Large", large_value}};
        auto large = co_await client.client().get("/large").headers(large_headers).send();
        EXPECT_TRUE(large.status() == ruvia::http_status::ok);
        EXPECT_EQ(body_text(co_await large.stream().read_all()), std::string("large-head"));
        auto small = co_await client.client().get("/small").send();
        EXPECT_TRUE(small.status() == ruvia::http_status::ok);
        EXPECT_EQ(body_text(co_await small.stream().read_all()), std::string("small-head"));
        co_await server.wait();

        const std::string oversized_value(ruvia::max_http_header_bytes + 2048, 'o');
        const std::array oversized_headers{ruvia::http_header_view{"X-Oversized", oversized_value}};
        std::optional<std::error_code> rejected;
        try {
            (void)co_await client.client().get("/oversized").headers(oversized_headers).send();
        } catch (const ruvia::http_client_error& error) {
            rejected = error.code();
        }
        EXPECT_TRUE(rejected == ruvia::make_error_code(ruvia::http_client_errc::invalid_request));
        co_await client.shutdown();
    };
    run_operation(worker, io, operation);
}

TEST(Ruvia, http_client_rejects_connection_options_naming_request_origin_metadata) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    loopback_response_server server(io, worker.handle_, {"hop-by-hop"});
    server.start();

    auto operation = [&]() -> ruvia::task<void> {
        const std::string client_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(server.port())));
        ruvia::http_client client(worker.attachment_.loop(), local_http_client_config(client_url));
        const std::array hop_by_hop{ruvia::http_header_view{"Connection", "X-Hop, Proxy-Authorization"}, ruvia::http_header_view{"X-Hop", "1"}};
        auto accepted = co_await client.client().get("/hop").headers(hop_by_hop).send();
        EXPECT_TRUE(accepted.status() == ruvia::http_status::ok);
        (void)co_await accepted.stream().read_all();
        co_await server.wait();

        for (const std::string_view nominated : {"Forwarded", "X-Forwarded-For", "X-Forwarded-Proto", "X-Forwarded-Host"}) {
            const std::array headers{ruvia::http_header_view{"Connection", nominated}};
            std::optional<std::error_code> rejected;
            try {
                (void)co_await client.client().get("/origin").headers(headers).send();
            } catch (const ruvia::http_client_error& error) {
                rejected = error.code();
            }
            EXPECT_TRUE(rejected == ruvia::make_error_code(ruvia::http_client_errc::invalid_request));
        }
        co_await client.shutdown();
    };
    run_operation(worker, io, operation);
}

TEST(Ruvia, client_shutdown_keeps_incomplete_response_error_observable) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    http1_chunked_response_peer peer(io, large_gzip_response_body(), http1_chunked_response_peer::end_mode_type::wait_for_client_close);
    peer.start();
    const std::string config_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(peer.port())));
    auto config = local_http_client_config(config_url);

    auto operation = [&]() -> ruvia::task<void> {
        std::optional<ruvia::http_client_response> response;
        bool running_read_failed = false;
        {
            ruvia::http_client client(worker.attachment_.loop(), config);
            response.emplace(co_await client.client().get("/incomplete-after-close").send());
            asio::steady_timer close_timer(io);
            close_timer.expires_after(std::chrono::milliseconds(50));
            close_timer.async_wait([&client](const std::error_code& error) {
                if (!error) {
                    client.close();
                }
            });
            try {
                (void)co_await response->stream().read_all();
            } catch (const ruvia::http_client_error& error) {
                running_read_failed = error.code() == ruvia::make_error_code(ruvia::http_client_errc::closing) || error.code() == std::make_error_code(std::errc::operation_canceled) || error.code() == ruvia::make_error_code(ruvia::http_client_errc::io_error);
            }
            EXPECT_TRUE(running_read_failed);
            co_await client.shutdown();
        }
        bool retained_response_failed = false;
        try {
            (void)co_await response->stream().read_all();
        } catch (const ruvia::http_client_error& error) {
            retained_response_failed = error.code() == ruvia::make_error_code(ruvia::http_client_errc::closing) || error.code() == std::make_error_code(std::errc::operation_canceled) || error.code() == ruvia::make_error_code(ruvia::http_client_errc::io_error);
        }
        EXPECT_TRUE(retained_response_failed);
        std::optional<std::error_code> read_error;
        try {
            while (co_await response->stream().read()) {
            }
        } catch (const ruvia::http_client_error& error) {
            read_error = error.code();
        }
        EXPECT_TRUE(read_error.has_value());
        std::optional<std::error_code> completion_error;
        try {
            (void)response->stream().complete();
        } catch (const ruvia::http_client_error& error) {
            completion_error = error.code();
        }
        EXPECT_TRUE(completion_error == read_error);
        response.reset();
    };
    run_operation(worker, io, operation);
    peer.wait();
    EXPECT_TRUE(peer.observed_client_close());
}

TEST(Ruvia, client_body_result_uses_pool_budget_after_client_and_worker_teardown) {
    std::optional<ruvia::http_client_response_bytes> retained;
    std::string expected(
        "\0\xff"
        "data",
        6);
    {
        auto& io = ruvia::test::new_test_io_context();
        {
            test_worker worker(io);
            asio::ip::tcp::acceptor acceptor(io, {asio::ip::tcp::v4(), std::uint16_t{0}});
            asio::ip::tcp::socket server_socket(io);
            ruvia::worker_signal server_done(worker.handle_);
            std::exception_ptr server_failure;
            const std::string response_wire = "HTTP/1.1 200 OK\r\nContent-Length: 6\r\nConnection: close\r\n\r\n" + expected;
            acceptor.async_accept(server_socket, [&](const std::error_code& error) {
                if (error) {
                    server_failure = std::make_exception_ptr(std::system_error(error));
                    server_done.notify();
                    return;
                }
                asio::async_write(server_socket, asio::buffer(response_wire), [&](const std::error_code& write_error, std::size_t) {
                    if (write_error) {
                        server_failure = std::make_exception_ptr(std::system_error(write_error));
                    }
                    server_done.notify();
                });
            });
            {
                const std::string client_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(acceptor.local_endpoint().port())));
                ruvia::http_client client(worker.attachment_.loop(), {.base_url_ = client_url}, {.result_budget_ = {.max_retained_bytes_ = expected.size()}});
                auto operation = [&]() -> ruvia::task<void> {
                    auto send = client.client().get("/").send();
                    auto response = co_await std::move(send);
                    auto bytes_value = co_await response.stream().read_all();
                    retained.emplace(std::move(bytes_value));
                    co_await server_done.wait();
                    if (server_failure != nullptr) {
                        std::rethrow_exception(server_failure);
                    }
                    EXPECT_EQ(retained->bytes().size(), expected.size());
                    co_await client.shutdown();
                };
                run_operation(worker, io, operation);
            }
        }
    }

    std::atomic_bool contents_matched{false};
    std::thread destroyer([bytes = std::move(*retained), expected = std::move(expected), &contents_matched]() {
        const auto actual = bytes.bytes();
        const auto wanted = std::as_bytes(std::span(expected.data(), expected.size()));
        contents_matched.store(actual.size() == wanted.size() && std::equal(actual.begin(), actual.end(), wanted.begin()), std::memory_order_release);
    });
    destroyer.join();
    EXPECT_TRUE(contents_matched.load(std::memory_order_acquire));
}

TEST(Ruvia, client_result_budget_rejects_zero_limit_at_client_startup) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    bool rejected_at_client_startup = false;
    try {
        ruvia::http_client client(worker.attachment_.loop(), {.base_url_ = "http://127.0.0.1"}, {.result_budget_ = {.max_retained_bytes_ = 0}});
    } catch (const std::invalid_argument&) {
        rejected_at_client_startup = true;
    }
    EXPECT_TRUE(rejected_at_client_startup);
}

TEST(Ruvia, http_client_connect_failure_message_is_utf8) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    // Bound but not listening: connections are refused, and no other process
    // can claim the port meanwhile.
    asio::ip::tcp::acceptor unavailable(io);
    unavailable.open(asio::ip::tcp::v4());
    unavailable.bind({asio::ip::tcp::v4(), 0});
    const std::string config_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(unavailable.local_endpoint().port())));
    auto config = local_http_client_config(config_url);
    // Windows retries a refused loopback connect for about two seconds.
    config.connect_timeout_ = std::chrono::seconds(20);
    config.acquire_timeout_ = std::chrono::seconds(20);
    config.request_timeout_ = std::chrono::seconds(20);
    ruvia::http_client client(worker.attachment_.loop(), config);
    std::optional<std::error_code> code;
    std::string message{};
    auto operation = [&]() -> ruvia::task<void> {
        try {
            (void)co_await client.client().get("/").send();
        } catch (const ruvia::http_client_error& error) {
            code = error.code();
            message = error.what();
        }
        co_await client.shutdown();
    };
    run_operation(worker, io, operation);
    EXPECT_TRUE(code == ruvia::make_error_code(ruvia::http_client_errc::connect_failed));
    EXPECT_TRUE(!message.empty());
    // The OS describes the refusal in the user's language; the text must be
    // UTF-8 whatever the system code page is, so it can travel in a JSON or
    // text/plain; charset=UTF-8 body. The strict JSON parser rejects ill-formed
    // UTF-8, and the message holds no characters that need escaping.
    EXPECT_TRUE(message.find_first_of("\"\\") == std::string::npos);
    const auto decoded = ruvia::from_json<ruvia::string>(std::string("\"") + message + "\"");
    EXPECT_EQ(decoded.view(), std::string_view(message));
}

TEST(Ruvia, http_client_upload_exchange_owns_chunks_and_trailers_and_drives_continue) {
    for (const auto mode : {upload_peer::mode_type::chunked, upload_peer::mode_type::known_length, upload_peer::mode_type::continue_value, upload_peer::mode_type::continue_timeout}) {
        auto& io = ruvia::test::new_test_io_context();
        test_worker worker(io);
        upload_peer peer(io, worker.handle_, mode);
        const std::string config_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(peer.port())));
        auto config = local_http_client_config(config_url);
        config.request_timeout_ = std::chrono::seconds(3);
        ruvia::http_client client(worker.attachment_.loop(), config);
        peer.start();
        auto operation = [&]() -> ruvia::task<void> {
            const bool known = mode == upload_peer::mode_type::known_length;
            const bool expect_continue = mode == upload_peer::mode_type::continue_value || mode == upload_peer::mode_type::continue_timeout;
            auto exchange_value = co_await client.client().request("POST", "/upload").upload({.content_length_ = known ? std::optional<ruvia::http_content_length>{6} : std::nullopt, .expectation_ = expect_continue ? ruvia::http_client_request_expectation::continue_value : ruvia::http_client_request_expectation::none, .max_chunk_bytes_ = 3, .continue_timeout_ = std::chrono::milliseconds(20)});
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
            EXPECT_TRUE(too_large);
            std::string input{"abc"};
            auto first = exchange_value.body().write(input);
            auto second = exchange_value.body().write("def");
            input.assign("xxx");
            std::string trailer_value{"retained"};
            const std::array<ruvia::http_header_view, 1> fields_value{{{"X-End", trailer_value}}};
            auto ending = exchange_value.body().end(known ? std::span<const ruvia::http_header_view>{} : std::span<const ruvia::http_header_view>(fields_value));
            trailer_value.assign("changed");
            auto receiving = exchange_value.response();
            auto receiving_overlap = exchange_value.response();
            auto receiving_again = exchange_value.response();
            std::optional<ruvia::http_client_response> received;
            bool response_overlap_rejected = false;
            ruvia::task_scope receivers(worker.handle_);
            const auto receive = [&]() -> ruvia::task<void> {
                received.emplace(co_await std::move(receiving));
            };
            const auto reject_response_overlap = [&]() -> ruvia::task<void> {
                try {
                    auto duplicate = co_await std::move(receiving_overlap);
                } catch (const std::logic_error&) {
                    response_overlap_rejected = true;
                }
            };
            receivers.spawn(receive());
            receivers.spawn(reject_response_overlap());
            ruvia::task_scope writers(worker.handle_);
            const auto write_first = [&]() -> ruvia::task<void> {
                co_await std::move(first);
            };
            writers.spawn(write_first());
            bool write_overlap_rejected = false;
            bool end_overlap_rejected = false;
            try {
                co_await exchange_value.body().write("bad");
            } catch (const std::logic_error&) {
                write_overlap_rejected = true;
            }
            try {
                co_await exchange_value.body().end();
            } catch (const std::logic_error&) {
                end_overlap_rejected = true;
            }
            co_await writers.join();
            EXPECT_TRUE(write_overlap_rejected && end_overlap_rejected);
            auto moved = std::move(exchange_value);
            co_await std::move(second);
            co_await std::move(ending);
            EXPECT_TRUE(moved.body().complete());
            co_await receivers.join();
            EXPECT_TRUE(response_overlap_rejected);
            bool transferred_twice = false;
            try {
                auto duplicate = co_await std::move(receiving_again);
            } catch (const std::logic_error&) {
                transferred_twice = true;
            }
            EXPECT_TRUE(transferred_twice);
            auto& response = *received;
            auto bytes_value = co_await response.stream().read_all(16);
            EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(bytes_value.bytes().data()), bytes_value.size()), "ok");
            const auto informational = response.informational_responses();
            EXPECT_EQ(informational.size(), mode == upload_peer::mode_type::continue_value ? upload_peer::early_hint_count + 1 : std::size_t{0});
            if (mode == upload_peer::mode_type::continue_value && informational.size() == upload_peer::early_hint_count + 1) {
                for (std::size_t i = 0; i < upload_peer::early_hint_count; ++i) {
                    EXPECT_EQ(informational[i].status().value(), std::uint16_t{103});
                    const auto fields = informational[i].headers();
                    EXPECT_EQ(fields.size(), std::size_t{1});
                    if (fields.size() == 1) {
                        EXPECT_EQ(fields[0].value(), "</asset>; rel=preload");
                    }
                }
                EXPECT_EQ(informational.back().status().value(), std::uint16_t{100});
            }
            co_await peer.wait();
            EXPECT_EQ(peer.body_, "abcdef");
            EXPECT_TRUE(peer.trailers_.field("x-end") == (known ? std::nullopt : std::optional<std::string_view>("retained")));
            EXPECT_TRUE((peer.head_.find("Expect: 100-continue") != std::string::npos) == expect_continue);
            co_await client.shutdown();
        };
        run_operation(worker, io, operation);
    }
}

TEST(Ruvia, http1_small_upload_completes_with_a_bounded_response_budget) {
    for (const auto mode : {upload_peer::mode_type::known_length, upload_peer::mode_type::chunked}) {
        const bool known = mode == upload_peer::mode_type::known_length;
        auto& io = ruvia::test::new_test_io_context();
        test_worker worker(io);
        upload_peer peer(io, worker.handle_, mode);
        const std::string config_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(peer.port())));
        auto config = local_http_client_config(config_url);
        config.max_response_bytes_ = 1024;
        // Completing a six-byte upload (with or without trailers) must not
        // require a header-section-sized buffer from the response budget.
        ruvia::http_client client(worker.attachment_.loop(), config, {.result_budget_ = {.max_retained_bytes_ = 4096, .max_in_flight_bytes_ = 32 * 1024}});
        peer.start();
        auto operation = [&]() -> ruvia::task<void> {
            std::exception_ptr failure;
            try {
                auto exchange = co_await client.client().post("/upload").upload({.content_length_ = known ? std::optional<ruvia::http_content_length>{6} : std::nullopt, .max_chunk_bytes_ = 3});
                co_await exchange.body().write("abc");
                co_await exchange.body().write("def");
                const std::array<ruvia::http_header_view, 1> trailers{{{"X-End", "retained"}}};
                co_await exchange.body().end(known ? std::span<const ruvia::http_header_view>{} : std::span<const ruvia::http_header_view>(trailers));
                EXPECT_TRUE(exchange.body().complete());
                auto response = co_await exchange.response();
                EXPECT_TRUE(response.status() == ruvia::http_status::ok);
                auto bytes = co_await response.stream().read_all(16);
                EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(bytes.bytes().data()), bytes.size()), "ok");
            } catch (...) {
                failure = std::current_exception();
            }
            co_await client.shutdown();
            if (failure) {
                peer.stop();
            }
            try {
                co_await peer.wait();
            } catch (...) {
                if (!failure) {
                    failure = std::current_exception();
                }
            }
            if (failure) {
                std::rethrow_exception(failure);
            }
            EXPECT_EQ(peer.body_, "abcdef");
            EXPECT_TRUE(peer.trailers_.field("x-end") == (known ? std::nullopt : std::optional<std::string_view>("retained")));
        };
        run_operation(worker, io, operation);
    }
}

TEST(Ruvia, http_client_tunnel_claims_output_when_started_and_retires_after_abort) {
    for (const bool aborting : {false, true}) {
        auto& io = ruvia::test::new_test_io_context();
        test_worker worker(io);
        asio::ip::tcp::acceptor acceptor(io, {asio::ip::tcp::v4(), 0});
        ruvia::worker_signal peer_done(worker.handle_);
        std::exception_ptr peer_failure;
        std::string received{};
        const auto serve = [&]() -> asio::awaitable<void> {
            auto socket = co_await acceptor.async_accept(asio::use_awaitable);
            asio::streambuf head;
            co_await asio::async_read_until(socket, head, "\r\n\r\n", asio::use_awaitable);
            constexpr std::string_view accepted = "HTTP/1.1 200 Connection Established\r\n\r\n";
            co_await asio::async_write(socket, asio::buffer(accepted), asio::use_awaitable);
            if (!aborting) {
                co_await asio::async_read(socket, asio::dynamic_buffer(received), asio::transfer_exactly(6), asio::use_awaitable);
                co_await asio::async_write(socket, asio::buffer(received), asio::use_awaitable);
            }
            std::array<char, 64> discarded;
            std::error_code error;
            while (co_await socket.async_read_some(asio::buffer(discarded), asio::redirect_error(asio::use_awaitable, error))) {
            }
            if (error != asio::error::eof && error != asio::error::connection_reset) {
                throw std::system_error(error);
            }
        };
        asio::co_spawn(io, serve(), [&](std::exception_ptr failure) {
            peer_failure = failure;
            peer_done.notify();
        });
        const std::string config_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(acceptor.local_endpoint().port())));
        auto config = local_http_client_config(config_url);
        config.request_timeout_ = std::chrono::seconds(3);
        ruvia::http_client client(worker.attachment_.loop(), config);
        auto operation = [&]() -> ruvia::task<void> {
            auto result = co_await client.client().connect("target.test:443").open();
            auto* tunnel = result.tunnel();
            EXPECT_TRUE(tunnel != nullptr);
            if (tunnel == nullptr) {
                co_return;
            }
            unsigned datagram_adapters_rejected = 0;
            try {
                auto datagrams = std::move(*tunnel).datagrams();
                static_cast<void>(datagrams);
            } catch (const std::logic_error&) {
                ++datagram_adapters_rejected;
            }
            try {
                auto udp = std::move(*tunnel).udp();
                static_cast<void>(udp);
            } catch (const std::logic_error&) {
                ++datagram_adapters_rejected;
            }
            EXPECT_EQ(datagram_adapters_rejected, 2U);
            EXPECT_TRUE(tunnel->status() == ruvia::http_status::ok);
            std::string input{"abc"};
            auto first = tunnel->write(input);
            auto second = tunnel->write("def");
            auto ending = tunnel->finish();
            auto reading = tunnel->read();
            auto reading_again = tunnel->read();
            input.assign("changed");
            bool first_cancelled = false;
            ruvia::task_scope writers(worker.handle_);
            const auto send_first = [&]() -> ruvia::task<void> {
                try {
                    co_await std::move(first);
                } catch (const ruvia::http_client_error&) {
                    first_cancelled = true;
                }
            };
            writers.spawn(send_first());
            if (aborting) {
                tunnel->abort();
                co_await writers.join();
                EXPECT_TRUE(first_cancelled);
                unsigned cancelled = 0;
                try {
                    co_await std::move(second);
                } catch (const ruvia::http_client_error&) {
                    ++cancelled;
                }
                try {
                    co_await std::move(ending);
                } catch (const ruvia::http_client_error&) {
                    ++cancelled;
                }
                EXPECT_EQ(cancelled, 2U);
            } else {
                unsigned rejected = 0;
                try {
                    co_await tunnel->write("bad");
                } catch (const std::logic_error&) {
                    ++rejected;
                }
                try {
                    co_await tunnel->finish();
                } catch (const std::logic_error&) {
                    ++rejected;
                }
                co_await writers.join();
                EXPECT_EQ(rejected, 2U);
                EXPECT_TRUE(!first_cancelled);
                co_await std::move(second);
                co_await std::move(ending);
                std::string echoed{};
                if (const auto bytes_value = co_await std::move(reading)) {
                    echoed.append(reinterpret_cast<const char*>(bytes_value->data()), bytes_value->size());
                }
                if (const auto bytes_value = co_await std::move(reading_again)) {
                    echoed.append(reinterpret_cast<const char*>(bytes_value->data()), bytes_value->size());
                }
                while (const auto bytes_value = co_await tunnel->read()) {
                    echoed.append(reinterpret_cast<const char*>(bytes_value->data()), bytes_value->size());
                }
                EXPECT_EQ(echoed, "abcdef");
                co_await tunnel->finish();
            }
            co_await peer_done.wait();
            if (peer_failure) {
                std::rethrow_exception(peer_failure);
            }
            co_await client.shutdown();
        };
        run_operation(worker, io, operation);
        if (!aborting) {
            EXPECT_EQ(received, "abcdef");
        }
    }
}

TEST(Ruvia, http_client_upload_exchange_preserves_early_final_response_and_stops_upload) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    upload_peer peer(io, worker.handle_, upload_peer::mode_type::early_final);
    const std::string client_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(peer.port())));
    std::optional<ruvia::http_client> client(std::in_place, worker.attachment_.loop(), local_http_client_config(client_url));
    peer.start();
    auto operation = [&]() -> ruvia::task<void> {
        auto exchange_value = co_await client->client().request("POST", "/upload").upload({.expectation_ = ruvia::http_client_request_expectation::continue_value});
        auto response = co_await exchange_value.response();
        EXPECT_EQ(response.status().value(), std::uint16_t{413});
        bool cancelled = false;
        try {
            co_await exchange_value.body().write("rejected");
        } catch (const ruvia::http_client_error& error) {
            cancelled = error.code() == std::make_error_code(std::errc::operation_canceled);
        }
        EXPECT_TRUE(cancelled);
        auto bytes_value = co_await response.stream().read_all(16);
        EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(bytes_value.bytes().data()), bytes_value.size()), "no");
        co_await peer.wait();
        co_await client->shutdown();
        client.reset();
        // The response domain keeps the exchange's output signals alive after
        // client destruction, including the stopped, unfinished upload path.
        EXPECT_EQ(response.status().value(), std::uint16_t{413});
        EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(bytes_value.bytes().data()), bytes_value.size()), "no");
    };
    run_operation(worker, io, operation);
}

namespace {
void exercise_early_response_during_upload(upload_peer::mode_type mode) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    upload_peer peer(io, worker.handle_, mode);
    const std::string config_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(peer.port())));
    auto config = local_http_client_config(config_url);
    config.max_response_bytes_ = 1024;
    config.request_timeout_ = std::chrono::seconds(3);
    ruvia::http_client client(worker.attachment_.loop(), config);
    peer.start();
    auto operation = [&]() -> ruvia::task<void> {
        constexpr std::size_t upload_size = 8 * 1024 * 1024;
        auto exchange = co_await client.client().post("/upload").upload({.max_chunk_bytes_ = upload_size});
        std::string upload(upload_size, 'u');
        bool upload_failed = false;
        ruvia::task_scope writers(worker.handle_);
        const auto write = [&]() -> ruvia::task<void> {
            try {
                co_await exchange.body().write(upload);
            } catch (const ruvia::http_client_error&) {
                upload_failed = true;
            }
        };
        writers.spawn(write());
        auto response = co_await exchange.response();
        EXPECT_EQ(response.status().value(), std::uint16_t{413});
        co_await writers.join();
        EXPECT_TRUE(upload_failed);
        if (mode == upload_peer::mode_type::early_final_while_uploading) {
            // Let the transport write observe the peer close before resuming
            // response body consumption.
            (void)co_await ruvia::sleep_for(worker.handle_, std::chrono::milliseconds(100));
        }
        std::size_t response_bytes = 0;
        while (const auto chunk = co_await response.stream().read()) {
            response_bytes += chunk->size();
            EXPECT_TRUE(std::all_of(chunk->begin(), chunk->end(), [](std::byte value) {
                return value == std::byte{'r'};
            }));
        }
        EXPECT_EQ(response_bytes, upload_peer::rejected_body_size);
        EXPECT_TRUE(response.stream().complete());
        peer.release_response();
        co_await peer.wait();
        EXPECT_TRUE(client.client().stats().plaintext_bytes_sent_ >= peer.head_.size() + peer.uploaded_bytes_);
        co_await client.shutdown();
    };
    run_operation(worker, io, operation);
}
}  // namespace

TEST(Ruvia, http_client_early_response_stops_upload_and_preserves_body) {
    exercise_early_response_during_upload(upload_peer::mode_type::early_final_orderly_while_uploading);
}

#if !defined(_WIN32)
TEST(Ruvia, http_client_upload_write_failure_preserves_complete_early_response) {
    exercise_early_response_during_upload(upload_peer::mode_type::early_final_while_uploading);
}
#endif
