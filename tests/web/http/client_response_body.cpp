#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
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

#include "ruvia/core/asio_task.h"
#include "ruvia/core/async.h"
#include "ruvia/core/event_loop_attachment.h"
#include "ruvia/core/task_scope.h"
#include "ruvia/core/timer.h"
#include "ruvia/core/worker_signal.h"
#include "ruvia/http/http2_connection.h"
#include "ruvia/http/http_content_codec.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_interim_response.h"
#include "ruvia/http/http_limits.h"
#include "ruvia/http/http_response_server.h"
#include "ruvia/http/http_response_stream.h"
#include "ruvia/web/http_capsule_stream.h"
#include "ruvia/web/http_client.h"
#include "ruvia/web/http_client_response.h"
#include "ruvia/web/http_client_tunnel.h"
#include "ruvia/web/http_client_types.h"
#include "ruvia/web/http_udp_tunnel.h"
#include "ruvia/web/model.h"

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
            asio::async_read_until(*socket, *request, "\r\n\r\n",
                [this, socket, request](const std::error_code& read_error, std::size_t) {
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
    std::function<void()> on_request_;
    std::size_t next_response_{0};
    std::chrono::milliseconds response_delay_;
    bool close_without_response_;
    std::size_t responses_before_close_;
};

[[nodiscard]] ruvia::http_client_config local_http_client_config(std::uint16_t port) {
    return ruvia::http_client_config{.scheme_ = ruvia::http_scheme::http,
        .host_ = "127.0.0.1",
        .port_ = port};
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
          socket_(io),
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
        auto& socket = socket_;
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
    asio::ip::tcp::socket socket_;
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
    cookie_stream_peer(asio::io_context& io, const ruvia::worker_handle& worker_value,
        cookie_peer_mode mode = cookie_peer_mode::http1)
        : io_(io),
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

    std::array<std::string, 2> requests_;

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
        if (mode_ == cookie_peer_mode::http2) {
            co_await serve_http2();
            co_return;
        }
        sockets_[0].emplace(co_await acceptor_.async_accept(asio::use_awaitable));
        std::string request;
        co_await asio::async_read_until(*sockets_[0], asio::dynamic_buffer(request), "\r\n\r\n", asio::use_awaitable);
        const std::string_view streaming_head = mode_ == cookie_peer_mode::http1_replacement
                                                    ? "HTTP/1.1 200 OK\r\nContent-Length: 1\r\nConnection: close\r\nSet-Cookie: sid=old\r\n\r\n"
                                                    : "HTTP/1.1 103 Early Hints\r\nSet-Cookie: informational=ignored; Path=/\r\n\r\n"
                                                      "HTTP/1.1 200 OK\r\nContent-Length: 1\r\nSet-Cookie: session=ready; Path=/\r\n\r\n";
        co_await asio::async_write(*sockets_[0], asio::buffer(streaming_head), asio::use_awaitable);
        for (std::size_t index = 0; index != requests_.size(); ++index) {
            auto& socket = sockets_[index + 1];
            socket.emplace(co_await acceptor_.async_accept(asio::use_awaitable));
            co_await asio::async_read_until(*socket, asio::dynamic_buffer(requests_[index]), "\r\n\r\n", asio::use_awaitable);
            const std::string_view response = mode_ == cookie_peer_mode::http1_replacement && index == 0
                                                  ? "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\nSet-Cookie: sid=new; Path=/scope\r\n\r\n"
                                                  : "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
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

    asio::awaitable<void> serve_http2() {
        sockets_[0].emplace(co_await acceptor_.async_accept(asio::use_awaitable));
        auto& socket = *sockets_[0];
        auto connection = ruvia::http2_connection::server();
        std::optional<ruvia::http2_request_head_event> streaming_request;
        std::array<char, 16384> input{};
        std::pmr::string output;
        std::size_t received = 0;
        for (;;) {
            if (connection.wants_write()) {
                output.clear();
                connection.take_output(output);
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
                        if (connection.submit_interim_response_head(stream_id,
                                ruvia::http_interim_response_head(ruvia::http_status::early_hints, fields)) !=
                            ruvia::http2_submit_status::accepted) {
                            throw std::runtime_error("cookie test interim head rejected");
                        }
                        response.header("set-cookie", "session=ready; Path=/");
                        if (connection.submit_streaming_response_head(stream_id, std::move(response)) !=
                            ruvia::http2_submit_status::accepted) {
                            throw std::runtime_error("cookie test streaming head rejected");
                        }
                        streaming_request.emplace(std::move(*request));
                    } else {
                        if (received == requests_.size()) {
                            throw std::runtime_error("unexpected cookie test request");
                        }
                        requests_[received++].assign(request->request().header("cookie").value_or(std::string_view{}));
                        if (connection.submit_buffered_response(stream_id, response) != ruvia::http2_submit_status::accepted ||
                            connection.release(std::move(*request)) != ruvia::http2_server_request_release_status::released) {
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
    scripted_head_peer(asio::io_context& io, const ruvia::worker_handle& worker_value, std::vector<std::string> heads)
        : acceptor_(io, {asio::ip::tcp::v4(), std::uint16_t{0}}),
          deadline_(io),
          done_(worker_value),
          heads_(std::move(heads)),
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

    std::vector<std::string> requests_;

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
    std::vector<std::string> heads_;
    asio::io_context& io_;
    std::exception_ptr failure_;
    bool complete_{false};
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
        auto response = co_await client.handle().send({.target_ = "/collect"});
        auto cold_read = response.body().read();
        auto cold_text = response.body().text();
        auto collected = co_await response.body().read_all();
        RUVIA_CHECK_EQ(collected.size(), expected.size());
        const std::string_view collected_view(
            reinterpret_cast<const char*>(collected.bytes().data()), collected.size());
        RUVIA_CHECK_EQ(collected_view, expected);
        const auto remaining = co_await response.body().read_all();
        RUVIA_CHECK(remaining.empty());
        RUVIA_CHECK(!(co_await std::move(cold_read)));
        RUVIA_CHECK(!(co_await std::move(cold_text)));
        RUVIA_CHECK(!(co_await response.body().text()));
        co_await client.shutdown();
        RUVIA_CHECK_EQ(collected_view, expected);
    };
    run_operation(worker, io, operation);
}

RUVIA_TEST(client_response_body_claims_its_shared_lane_only_when_started) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    http1_chunked_response_peer peer(io, large_gzip_response_body());
    peer.start();
    ruvia::http_client client(worker.attachment_.loop(), local_http_client_config(peer.port()));
    auto operation = [&]() -> ruvia::task<void> {
        auto response = co_await client.handle().send({.target_ = "/body-lane"});
        auto collect = response.body().read_all();
        auto read = response.body().read();
        auto text = response.body().text();
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
            (void)co_await response.body().read_all();
        } catch (const std::logic_error&) {
            collect_rejected = true;
        }
        peer.release_terminal_chunk();
        co_await readers.join();
        RUVIA_CHECK(read_rejected && text_rejected && collect_rejected);
        RUVIA_CHECK(collected);
        RUVIA_CHECK(!(co_await response.body().text()));
        co_await client.shutdown();
    };
    run_operation(worker, io, operation);
    peer.wait();
}

namespace {
void exercise_final_head_cookies(ruvia::testing::test_context& ruvia_ctx, ruvia::http_client_protocol protocol) {
    for (const auto policy : {ruvia::http_client_received_cookie_policy::retain_and_send,
             ruvia::http_client_received_cookie_policy::ignore}) {
        auto& io = ruvia::test::new_test_io_context();
        test_worker worker(io);
        const bool http2 = protocol == ruvia::http_client_protocol::http2_only;
        cookie_stream_peer peer(io, worker.handle_, http2 ? cookie_peer_mode::http2 : cookie_peer_mode::http1);
        auto config = local_http_client_config(peer.port());
        config.connection_count_ = http2 ? 1 : 2;
        config.protocol_ = protocol;
        config.received_cookies_ = policy;
        config.request_timeout_ = std::chrono::seconds(3);
        ruvia::http_client client(worker.attachment_.loop(), config);
        peer.start();
        auto operation = [&]() -> ruvia::task<void> {
            std::optional<ruvia::http_client_response> streaming;
            streaming.emplace(co_await client.handle().send({.target_ = "/stream"}));
            RUVIA_CHECK(!streaming->body().complete());
            RUVIA_CHECK(streaming->protocol_version() ==
                        (http2 ? ruvia::http_protocol_version::http2 : ruvia::http_protocol_version::http11));
            RUVIA_CHECK_EQ(streaming->informational_responses().size(), std::size_t{1});
            {
                auto response = co_await client.handle().send({.target_ = "/before-abandonment"});
                RUVIA_CHECK((co_await response.body().read_all()).empty());
            }
            streaming.reset();
            {
                auto response = co_await client.handle().send({.target_ = "/after-abandonment"});
                RUVIA_CHECK((co_await response.body().read_all()).empty());
            }
            co_await peer.wait();
            for (const auto& request : peer.requests_) {
                const bool received = request.find("session=ready") != std::string::npos;
                RUVIA_CHECK(received == (policy == ruvia::http_client_received_cookie_policy::retain_and_send));
                RUVIA_CHECK(request.find("informational=ignored") == std::string::npos);
            }
            co_await client.shutdown();
        };
        run_operation(worker, io, operation);
    }
}
}  // namespace

RUVIA_TEST(http_client_retains_final_head_cookies_before_body_completion_and_after_abandonment) {
    exercise_final_head_cookies(ruvia_ctx, ruvia::http_client_protocol::negotiate);
}

RUVIA_TEST(http2_client_retains_final_head_cookies_before_body_completion_and_after_abandonment) {
    exercise_final_head_cookies(ruvia_ctx, ruvia::http_client_protocol::http2_only);
}

RUVIA_TEST(http1_slow_body_completion_does_not_roll_back_newer_final_head_cookies) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    cookie_stream_peer peer(io, worker.handle_, cookie_peer_mode::http1_replacement);
    auto config = local_http_client_config(peer.port());
    config.connection_count_ = 2;
    config.received_cookies_ = ruvia::http_client_received_cookie_policy::retain_and_send;
    config.request_timeout_ = std::chrono::seconds(3);
    ruvia::http_client client(worker.attachment_.loop(), config);
    peer.start();
    auto operation = [&]() -> ruvia::task<void> {
        auto slow = co_await client.handle().send({.target_ = "/scope/stream"});
        RUVIA_CHECK(!slow.body().complete());
        {
            auto update = co_await client.handle().send({.target_ = "/scope/update"});
            RUVIA_CHECK((co_await update.body().read_all()).empty());
        }
        // The gate opens only after the newer final head has updated the jar.
        peer.release_terminal_body();
        const auto body = co_await slow.body().read_all();
        RUVIA_CHECK_EQ(std::string_view(reinterpret_cast<const char*>(body.bytes().data()), body.size()), std::string_view("x"));
        {
            auto check = co_await client.handle().send({.target_ = "/scope/check"});
            RUVIA_CHECK((co_await check.body().read_all()).empty());
        }
        co_await peer.wait();
        RUVIA_CHECK(peer.requests_[0].find("sid=old") != std::string::npos);
        RUVIA_CHECK(peer.requests_[1].find("sid=new") != std::string::npos);
        RUVIA_CHECK(peer.requests_[1].find("sid=old") == std::string::npos);
        co_await client.shutdown();
    };
    run_operation(worker, io, operation);
}

// RFC 6265 section 5.3 step 11: a Domain cookie replaces the host-only cookie
// with the same name and path, since a host-only cookie's domain is the host.
RUVIA_TEST(http_client_replaces_host_only_cookie_with_same_named_domain_cookie) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    scripted_head_peer peer(io, worker.handle_,
        {"HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\nSet-Cookie: sid=host; Path=/\r\n\r\n",
            "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n"
            "Set-Cookie: sid=domain; Domain=127.0.0.1; Path=/\r\n\r\n",
            "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"});
    auto config = local_http_client_config(peer.port());
    // Match the IPv4 peer directly without DNS or IPv6 connection fallback.
    config.received_cookies_ = ruvia::http_client_received_cookie_policy::retain_and_send;
    config.request_timeout_ = std::chrono::seconds(3);
    ruvia::http_client client(worker.attachment_.loop(), config);
    peer.start();
    auto operation = [&]() -> ruvia::task<void> {
        for (const auto* target : {"/host", "/domain", "/check"}) {
            auto response = co_await client.handle().send({.target_ = target});
            RUVIA_CHECK((co_await response.body().read_all()).empty());
        }
        co_await peer.wait();
        RUVIA_CHECK_EQ(peer.requests_.size(), std::size_t{3});
        if (peer.requests_.size() == 3) {
            RUVIA_CHECK(peer.requests_[1].find("sid=host") != std::string::npos);
            RUVIA_CHECK(peer.requests_[2].find("sid=domain") != std::string::npos);
            RUVIA_CHECK(peer.requests_[2].find("sid=host") == std::string::npos);
        }
        co_await client.shutdown();
    };
    run_operation(worker, io, operation);
}

// RFC 9110 section 9.3.8: stored cookies are sensitive and stay out of TRACE.
RUVIA_TEST(http_client_omits_stored_cookies_from_trace) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    scripted_head_peer peer(io, worker.handle_,
        {"HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n\r\n",
            "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"});
    auto config = local_http_client_config(peer.port());
    config.cookies_ = {{"sid", "secret"}};
    config.request_timeout_ = std::chrono::seconds(3);
    ruvia::http_client client(worker.attachment_.loop(), config);
    peer.start();
    auto operation = [&]() -> ruvia::task<void> {
        for (const auto* method : {"TRACE", "GET"}) {
            auto response = co_await client.handle().send({.method_ = method, .target_ = "/"});
            RUVIA_CHECK((co_await response.body().read_all()).empty());
        }
        co_await peer.wait();
        RUVIA_CHECK_EQ(peer.requests_.size(), std::size_t{2});
        if (peer.requests_.size() == 2) {
            RUVIA_CHECK(peer.requests_[0].find("sid=secret") == std::string::npos);
            RUVIA_CHECK(peer.requests_[1].find("sid=secret") != std::string::npos);
        }
        co_await client.shutdown();
    };
    run_operation(worker, io, operation);
}

// RFC 9110 sections 14 and 15.3.7: a range of an encoded representation is not
// decodable alone, so a 206 body is delivered as received.
RUVIA_TEST(http_client_passes_encoded_partial_content_through) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    const std::string gzip_prefix("\x1f\x8b\x08\x00\x00\x00\x00\x00\x00\x03", 10);
    scripted_head_peer peer(io, worker.handle_,
        {"HTTP/1.1 206 Partial Content\r\nContent-Encoding: gzip\r\nContent-Range: bytes 0-9/100\r\n"
         "Content-Length: 10\r\nConnection: close\r\n\r\n" +
            gzip_prefix});
    auto config = local_http_client_config(peer.port());
    config.request_timeout_ = std::chrono::seconds(3);
    ruvia::http_client client(worker.attachment_.loop(), config);
    peer.start();
    auto operation = [&]() -> ruvia::task<void> {
        auto response = co_await client.handle().send({.target_ = "/range"});
        RUVIA_CHECK_EQ(response.status().value(), std::uint16_t{206});
        const auto bytes_value = co_await response.body().read_all();
        RUVIA_CHECK_EQ(std::string_view(reinterpret_cast<const char*>(bytes_value.bytes().data()), bytes_value.size()),
            std::string_view(gzip_prefix));
        co_await peer.wait();
        co_await client.shutdown();
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
        loopback_response_server server(
            io, worker.handle_, {"cancelled"}, std::chrono::milliseconds(350));
        ruvia::http_client client(worker.attachment_.loop(), local_http_client_config(server.port()));
        ruvia::stop_source base_stop;
        ruvia::stop_source derived_stop;
        server.start();

        auto operation = [&]() -> ruvia::task<void> {
            // Stop the in-flight exchange once the peer holds its request.
            server.on_request([&base_stop, &derived_stop, stop_base] {
                (stop_base ? base_stop : derived_stop).request_stop();
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
            server.on_request([&worker] { worker.attachment_.stop(); });
            bool cancelled = false;
            try {
                (void)co_await client.handle().send({.target_ = "/event-loop-stop"});
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
            server.on_request([&client] { client.close(); });
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
        auto response = co_await client.handle().send({.target_ = "/backpressure-stop"});
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
        auto response = co_await client.handle().send({.target_ = "/transfer-gzip-stop"});
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
        auto response = co_await client.handle().send({.target_ = "/transfer-gzip"});
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
            response.emplace(co_await client.handle().send({.target_ = "/retained-response"}));
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
        response.emplace(co_await client->handle().send({.target_ = "/cold-body"}));
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

RUVIA_TEST(http_client_sends_heads_beyond_the_retained_buffer_and_rejects_oversized_heads) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    loopback_response_server server(io, worker.handle_, {"large-head", "small-head"});
    server.start();

    auto operation = [&]() -> ruvia::task<void> {
        ruvia::http_client client(worker.attachment_.loop(), local_http_client_config(server.port()));
        const auto body_text = [](const ruvia::http_client_response_bytes& bytes_value) {
            return std::string(reinterpret_cast<const char*>(bytes_value.bytes().data()), bytes_value.bytes().size());
        };
        // Larger than the head buffer a connection retains, within the limit.
        const std::string large_value(20 * 1024, 'v');
        const std::array large_headers{ruvia::http_header_view{"X-Large", large_value}};
        auto large = co_await client.handle().send({.target_ = "/large", .headers_ = large_headers});
        RUVIA_CHECK(large.status() == ruvia::http_status::ok);
        RUVIA_CHECK_EQ(body_text(co_await large.body().read_all()), std::string("large-head"));
        auto small = co_await client.handle().send({.target_ = "/small"});
        RUVIA_CHECK(small.status() == ruvia::http_status::ok);
        RUVIA_CHECK_EQ(body_text(co_await small.body().read_all()), std::string("small-head"));
        co_await server.wait();

        const std::string oversized_value(ruvia::max_http_header_bytes + 2048, 'o');
        const std::array oversized_headers{ruvia::http_header_view{"X-Oversized", oversized_value}};
        std::optional<ruvia::http_client_error::code_type> rejected;
        try {
            (void)co_await client.handle().send({.target_ = "/oversized", .headers_ = oversized_headers});
        } catch (const ruvia::http_client_error& error) {
            rejected = error.code();
        }
        RUVIA_CHECK(rejected == ruvia::http_client_error::code_type::invalid_request);
        co_await client.shutdown();
    };
    run_operation(worker, io, operation);
}

RUVIA_TEST(client_shutdown_keeps_incomplete_response_error_observable) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    http1_chunked_response_peer peer(io, large_gzip_response_body(),
        http1_chunked_response_peer::end_mode_type::wait_for_client_close);
    peer.start();
    auto config = local_http_client_config(peer.port());

    auto operation = [&]() -> ruvia::task<void> {
        std::optional<ruvia::http_client_response> response;
        bool running_read_failed = false;
        {
            ruvia::http_client client(worker.attachment_.loop(), config);
            response.emplace(co_await client.handle().send({.target_ = "/incomplete-after-close"}));
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
    peer.wait();
    RUVIA_CHECK(peer.observed_client_close());
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
                        .port_ = acceptor.local_endpoint().port()},
                    {.max_retained_bytes_ = expected.size()});
                auto operation = [&]() -> ruvia::task<void> {
                    auto send = client.handle().send({.target_ = "/"});
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

RUVIA_TEST(http_client_connect_failure_message_is_utf8) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    // Bound but not listening: connections are refused, and no other process
    // can claim the port meanwhile.
    asio::ip::tcp::acceptor unavailable(io);
    unavailable.open(asio::ip::tcp::v4());
    unavailable.bind({asio::ip::tcp::v4(), 0});
    auto config = local_http_client_config(unavailable.local_endpoint().port());
    // Windows retries a refused loopback connect for about two seconds.
    config.connect_timeout_ = std::chrono::seconds(20);
    config.acquire_timeout_ = std::chrono::seconds(20);
    config.request_timeout_ = std::chrono::seconds(20);
    ruvia::http_client client(worker.attachment_.loop(), config);
    std::optional<ruvia::http_client_error::code_type> code;
    std::string message;
    auto operation = [&]() -> ruvia::task<void> {
        try {
            (void)co_await client.handle().send({.target_ = "/"});
        } catch (const ruvia::http_client_error& error) {
            code = error.code();
            message = error.what();
        }
        co_await client.shutdown();
    };
    run_operation(worker, io, operation);
    RUVIA_CHECK(code == ruvia::http_client_error::code_type::connect_failed);
    RUVIA_CHECK(!message.empty());
    // The OS describes the refusal in the user's language; the text must be
    // UTF-8 whatever the system code page is, so it can travel in a JSON or
    // text/plain; charset=UTF-8 body. The strict JSON parser rejects ill-formed
    // UTF-8, and the message holds no characters that need escaping.
    RUVIA_CHECK(message.find_first_of("\"\\") == std::string::npos);
    RUVIA_CHECK(ruvia::from_json<ruvia::string>("\"" + message + "\"").has_value());
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
            auto exchange_value = co_await client.handle().open_request({.method_ = "POST", .target_ = "/upload"},
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
            auto second = exchange_value.body().write("def");
            input.assign("xxx");
            std::string trailer_value = "retained";
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
            const auto write_first = [&]() -> ruvia::task<void> { co_await std::move(first); };
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
            RUVIA_CHECK(write_overlap_rejected && end_overlap_rejected);
            auto moved = std::move(exchange_value);
            co_await std::move(second);
            co_await std::move(ending);
            RUVIA_CHECK(moved.body().complete());
            co_await receivers.join();
            RUVIA_CHECK(response_overlap_rejected);
            bool transferred_twice = false;
            try {
                auto duplicate = co_await std::move(receiving_again);
            } catch (const std::logic_error&) {
                transferred_twice = true;
            }
            RUVIA_CHECK(transferred_twice);
            auto& response = *received;
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

RUVIA_TEST(http1_small_upload_completes_with_a_bounded_response_budget) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    upload_peer peer(io, worker.handle_, upload_peer::mode_type::known_length);
    auto config = local_http_client_config(peer.port());
    config.max_response_bytes_ = 1024;
    // Completing a six-byte upload must not require a header-section-sized buffer.
    ruvia::http_client client(worker.attachment_.loop(), config,
        {.max_retained_bytes_ = 4096, .max_in_flight_bytes_ = 32 * 1024});
    peer.start();
    auto operation = [&]() -> ruvia::task<void> {
        std::exception_ptr failure;
        try {
            auto exchange = co_await client.handle().open_request(
                {.method_ = "POST", .target_ = "/upload"}, {.content_length_ = 6, .max_chunk_bytes_ = 3});
            co_await exchange.body().write("abc");
            co_await exchange.body().write("def");
            co_await exchange.body().end();
            RUVIA_CHECK(exchange.body().complete());
            auto response = co_await exchange.response();
            RUVIA_CHECK(response.status() == ruvia::http_status::ok);
            auto bytes = co_await response.body().read_all(16);
            RUVIA_CHECK_EQ(std::string_view(reinterpret_cast<const char*>(bytes.bytes().data()), bytes.size()), "ok");
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
        RUVIA_CHECK_EQ(peer.body_, "abcdef");
    };
    run_operation(worker, io, operation);
}

RUVIA_TEST(http_client_tunnel_claims_output_when_started_and_retires_after_abort) {
    for (const bool aborting : {false, true}) {
        auto& io = ruvia::test::new_test_io_context();
        test_worker worker(io);
        asio::ip::tcp::acceptor acceptor(io, {asio::ip::tcp::v4(), 0});
        ruvia::worker_signal peer_done(worker.handle_);
        std::exception_ptr peer_failure;
        std::string received;
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
        auto config = local_http_client_config(acceptor.local_endpoint().port());
        config.request_timeout_ = std::chrono::seconds(3);
        ruvia::http_client client(worker.attachment_.loop(), config);
        auto operation = [&]() -> ruvia::task<void> {
            auto result = co_await client.handle().open_tunnel({.authority_ = "target.test:443"});
            auto* tunnel = result.tunnel();
            RUVIA_CHECK(tunnel != nullptr);
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
            RUVIA_CHECK_EQ(datagram_adapters_rejected, 2U);
            RUVIA_CHECK(tunnel->status() == ruvia::http_status::ok);
            std::string input = "abc";
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
                RUVIA_CHECK(first_cancelled);
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
                RUVIA_CHECK_EQ(cancelled, 2U);
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
                RUVIA_CHECK_EQ(rejected, 2U);
                RUVIA_CHECK(!first_cancelled);
                co_await std::move(second);
                co_await std::move(ending);
                std::string echoed;
                if (const auto bytes_value = co_await std::move(reading)) {
                    echoed.append(reinterpret_cast<const char*>(bytes_value->data()), bytes_value->size());
                }
                if (const auto bytes_value = co_await std::move(reading_again)) {
                    echoed.append(reinterpret_cast<const char*>(bytes_value->data()), bytes_value->size());
                }
                while (const auto bytes_value = co_await tunnel->read()) {
                    echoed.append(reinterpret_cast<const char*>(bytes_value->data()), bytes_value->size());
                }
                RUVIA_CHECK_EQ(echoed, "abcdef");
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
            RUVIA_CHECK_EQ(received, "abcdef");
        }
    }
}

RUVIA_TEST(http_client_upload_exchange_preserves_early_final_response_and_stops_upload) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    upload_peer peer(io, worker.handle_, upload_peer::mode_type::early_final);
    std::optional<ruvia::http_client> client(std::in_place, worker.attachment_.loop(), local_http_client_config(peer.port()));
    peer.start();
    auto operation = [&]() -> ruvia::task<void> {
        auto exchange_value = co_await client->handle().open_request({.method_ = "POST", .target_ = "/upload"},
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
    config.tls_.ca_file_ = identity.ca_file_.string();
    config.max_response_bytes_ = 8192;
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
            auto response = co_await client.handle().send({.target_ = "/next?exact=body"});
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

namespace {
// A failed client operation must not leave the peer's accept pending, which
// would keep the I/O context running.
struct acceptor_closer final {
    asio::ip::tcp::acceptor& acceptor_;
    ~acceptor_closer() {
        std::error_code ignored;
        acceptor_.close(ignored);
    }
};
}  // namespace

// RFC 9112 §9.8: only close_notify ends a close-delimited body over TLS; a TCP
// close without it may be an attacker truncating the response.
RUVIA_TEST(http1_tls_close_delimited_response_requires_close_notify) {
    for (const bool close_notify : {true, false}) {
        auto& io = ruvia::test::new_test_io_context();
        test_worker worker(io);
        ruvia::test::tls_identity identity("localhost");
        asio::ip::tcp::acceptor acceptor(io, {asio::ip::make_address("127.0.0.1"), 0});
        std::exception_ptr peer_failure;
        ruvia::worker_signal peer_done(worker.handle_);
        bool peer_finished = false;
        const auto serve = [&]() -> asio::awaitable<void> {
            asio::ssl::stream<asio::ip::tcp::socket> stream(
                co_await acceptor.async_accept(asio::use_awaitable), identity.context_);
            co_await stream.async_handshake(asio::ssl::stream_base::server, asio::use_awaitable);
            asio::streambuf request;
            co_await asio::async_read_until(stream, request, "\r\n\r\n", asio::use_awaitable);
            constexpr std::string_view response = "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\npartial";
            co_await asio::async_write(stream, asio::buffer(response), asio::use_awaitable);
            std::error_code ignored;
            if (close_notify) {
                co_await stream.async_shutdown(asio::redirect_error(asio::use_awaitable, ignored));
            } else {
                // A FIN rather than a close: unread client bytes would turn it into a reset.
                stream.next_layer().shutdown(asio::ip::tcp::socket::shutdown_send, ignored);
                std::array<char, 256> discarded;
                while (co_await stream.next_layer().async_read_some(
                    asio::buffer(discarded), asio::redirect_error(asio::use_awaitable, ignored))) {
                }
            }
        };
        asio::co_spawn(io, serve(), [&](std::exception_ptr failure) {
            peer_failure = failure;
            peer_finished = true;
            peer_done.notify();
        });
        auto config = local_http_client_config(acceptor.local_endpoint().port());
        config.scheme_ = ruvia::http_scheme::https;
        config.host_ = "localhost";
        config.tls_.ca_file_ = identity.ca_file_.string();
        config.request_timeout_ = std::chrono::seconds(3);
        ruvia::http_client client(worker.attachment_.loop(), config);
        auto operation = [&]() -> ruvia::task<void> {
            const acceptor_closer closer{acceptor};
            auto response = co_await client.handle().send({.target_ = "/close-delimited"});
            std::optional<std::string> body;
            bool truncated = false;
            try {
                const auto bytes_value = co_await response.body().read_all();
                body.emplace(reinterpret_cast<const char*>(bytes_value.bytes().data()), bytes_value.size());
            } catch (const ruvia::http_client_error& error) {
                truncated = error.code() == ruvia::http_client_error::code_type::io_error;
            }
            if (close_notify) {
                RUVIA_CHECK(body == std::optional<std::string>("partial"));
            } else {
                RUVIA_CHECK(!body.has_value());
                RUVIA_CHECK(truncated);
            }
            co_await client.shutdown();
            while (!peer_finished) {
                co_await peer_done.wait();
            }
            if (peer_failure) {
                std::rethrow_exception(peer_failure);
            }
        };
        run_operation(worker, io, operation);
    }
}

// A response on a connection that is not reused completes even when trailing
// bytes arrive with it: they are discarded with the connection, not parsed.
RUVIA_TEST(http1_response_on_a_closing_connection_discards_trailing_bytes) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    scripted_head_peer peer(io, worker.handle_,
        {"HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Length: 5\r\n\r\nhelloXjunk",
            "HTTP/1.1 407 Proxy Authentication Required\r\nContent-Length: 6\r\n\r\ndeniedHTTP/1.1 200 OK\r\n\r\n"});
    peer.start();
    auto config = local_http_client_config(peer.port());
    config.request_timeout_ = std::chrono::seconds(3);
    ruvia::http_client client(worker.attachment_.loop(), config);
    auto operation = [&]() -> ruvia::task<void> {
        {
            auto response = co_await client.handle().send({.target_ = "/closing"});
            const auto bytes_value = co_await response.body().read_all();
            RUVIA_CHECK_EQ(
                std::string_view(reinterpret_cast<const char*>(bytes_value.bytes().data()), bytes_value.size()), "hello");
        }
        {
            auto result = co_await client.handle().open_tunnel({.authority_ = "target.test:443"});
            auto* response = result.response();
            RUVIA_CHECK(response != nullptr);
            if (response != nullptr) {
                RUVIA_CHECK_EQ(response->status().value(), std::uint16_t{407});
                const auto bytes_value = co_await response->body().read_all();
                RUVIA_CHECK_EQ(std::string_view(reinterpret_cast<const char*>(bytes_value.bytes().data()),
                                   bytes_value.size()),
                    "denied");
            }
        }
        co_await peer.wait();
        RUVIA_CHECK_EQ(client.stats().failed_requests_, std::size_t{0});
        co_await client.shutdown();
    };
    run_operation(worker, io, operation);
}

RUVIA_TEST(http_client_replaces_a_keep_alive_connection_the_peer_closed_while_idle) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    asio::ip::tcp::acceptor acceptor(io, {asio::ip::make_address("127.0.0.1"), 0});
    unsigned connections = 0;
    bool idle_closed = false;
    ruvia::worker_signal idle_closed_signal(worker.handle_);
    std::exception_ptr peer_failure;
    ruvia::worker_signal peer_done(worker.handle_);
    bool peer_finished = false;
    const auto serve = [&]() -> asio::awaitable<void> {
        for (const std::string_view body : {std::string_view("first"), std::string_view("second")}) {
            auto socket = co_await acceptor.async_accept(asio::use_awaitable);
            ++connections;
            asio::streambuf request;
            co_await asio::async_read_until(socket, request, "\r\n\r\n", asio::use_awaitable);
            const std::string response =
                "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + std::string(body);
            co_await asio::async_write(socket, asio::buffer(response), asio::use_awaitable);
            if (!idle_closed) {
                // A keep-alive idle timeout: the peer closes without a request.
                std::error_code ignored;
                socket.close(ignored);
                idle_closed = true;
                idle_closed_signal.notify();
            }
        }
    };
    asio::co_spawn(io, serve(), [&](std::exception_ptr failure) {
        peer_failure = failure;
        peer_finished = true;
        peer_done.notify();
    });
    auto config = local_http_client_config(acceptor.local_endpoint().port());
    config.connection_count_ = 1;
    config.request_timeout_ = std::chrono::seconds(3);
    ruvia::http_client client(worker.attachment_.loop(), config);
    auto operation = [&]() -> ruvia::task<void> {
        const acceptor_closer closer{acceptor};
        for (const std::string_view expected : {std::string_view("first"), std::string_view("second")}) {
            auto response = co_await client.handle().send({.target_ = "/idle"});
            const auto bytes_value = co_await response.body().read_all();
            RUVIA_CHECK_EQ(std::string_view(reinterpret_cast<const char*>(bytes_value.bytes().data()), bytes_value.size()), expected);
            while (!idle_closed) {
                co_await idle_closed_signal.wait();
            }
            // Let the peer's FIN reach the idle pooled socket.
            (void)co_await ruvia::sleep_for(worker.handle_, std::chrono::milliseconds(100));
        }
        while (!peer_finished) {
            co_await peer_done.wait();
        }
        if (peer_failure) {
            std::rethrow_exception(peer_failure);
        }
        RUVIA_CHECK_EQ(client.stats().failed_requests_, std::size_t{0});
        co_await client.shutdown();
    };
    run_operation(worker, io, operation);
    RUVIA_CHECK_EQ(connections, 2U);
}

// The idle probe never decrypts: any record a TLS peer sends while the pooled
// connection idles, a TLS 1.3 session ticket as much as a close_notify,
// retires the connection, and the next request uses a fresh one.
RUVIA_TEST(http_client_tls_idle_probe_retires_a_connection_with_unread_peer_records) {
    for (const bool peer_closes : {false, true}) {
        auto& io = ruvia::test::new_test_io_context();
        test_worker worker(io);
        ruvia::test::tls_identity identity("localhost");
        asio::ip::tcp::acceptor acceptor(io, {asio::ip::make_address("127.0.0.1"), 0});
        unsigned connections = 0;
        bool idle_sent = false;
        ruvia::worker_signal idle_sent_signal(worker.handle_);
        std::exception_ptr peer_failure;
        ruvia::worker_signal peer_done(worker.handle_);
        bool peer_finished = false;
        const auto answer = [](asio::ssl::stream<asio::ip::tcp::socket>& stream,
                                std::string_view body) -> asio::awaitable<void> {
            asio::streambuf request;
            co_await asio::async_read_until(stream, request, "\r\n\r\n", asio::use_awaitable);
            const std::string response =
                "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + std::string(body);
            co_await asio::async_write(stream, asio::buffer(response), asio::use_awaitable);
        };
        const auto serve = [&]() -> asio::awaitable<void> {
            asio::ssl::stream<asio::ip::tcp::socket> first(
                co_await acceptor.async_accept(asio::use_awaitable), identity.context_);
            ++connections;
            co_await first.async_handshake(asio::ssl::stream_base::server, asio::use_awaitable);
            co_await answer(first, "first");
            if (peer_closes) {
                // Starting the shutdown writes close_notify at once; it then
                // waits for the client's reply, which ends when the client
                // drops the retired connection.
                idle_sent = true;
                idle_sent_signal.notify();
                std::error_code ignored;
                co_await first.async_shutdown(asio::redirect_error(asio::use_awaitable, ignored));
            } else {
                if (SSL_new_session_ticket(first.native_handle()) != 1) {
                    throw std::runtime_error("TLS 1.3 was not negotiated");
                }
                // A post-handshake handshake step sends the ticket immediately.
                co_await first.async_handshake(asio::ssl::stream_base::server, asio::use_awaitable);
                idle_sent = true;
                idle_sent_signal.notify();
            }
            // No FIN precedes the client's probe: only the idle record retires
            // the first connection.
            asio::ssl::stream<asio::ip::tcp::socket> second(
                co_await acceptor.async_accept(asio::use_awaitable), identity.context_);
            ++connections;
            co_await second.async_handshake(asio::ssl::stream_base::server, asio::use_awaitable);
            co_await answer(second, "second");
        };
        asio::co_spawn(io, serve(), [&](std::exception_ptr failure) {
            peer_failure = failure;
            peer_finished = true;
            peer_done.notify();
        });
        auto config = local_http_client_config(acceptor.local_endpoint().port());
        config.scheme_ = ruvia::http_scheme::https;
        config.host_ = "localhost";
        config.tls_.ca_file_ = identity.ca_file_.string();
        config.connection_count_ = 1;
        config.request_timeout_ = std::chrono::seconds(3);
        ruvia::http_client client(worker.attachment_.loop(), config);
        auto operation = [&]() -> ruvia::task<void> {
            const acceptor_closer closer{acceptor};
            for (const std::string_view expected : {std::string_view("first"), std::string_view("second")}) {
                auto response = co_await client.handle().send({.target_ = "/idle"});
                const auto bytes_value = co_await response.body().read_all();
                RUVIA_CHECK_EQ(std::string_view(reinterpret_cast<const char*>(bytes_value.bytes().data()), bytes_value.size()), expected);
                while (!idle_sent) {
                    co_await idle_sent_signal.wait();
                }
                // Let the idle record reach the pooled socket.
                (void)co_await ruvia::sleep_for(worker.handle_, std::chrono::milliseconds(100));
            }
            while (!peer_finished) {
                co_await peer_done.wait();
            }
            if (peer_failure) {
                std::rethrow_exception(peer_failure);
            }
            RUVIA_CHECK_EQ(client.stats().failed_requests_, std::size_t{0});
            co_await client.shutdown();
        };
        run_operation(worker, io, operation);
        RUVIA_CHECK_EQ(connections, 2U);
    }
}

RUVIA_TEST(http_client_abandoned_queued_request_is_never_written) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    asio::ip::tcp::acceptor acceptor(io, {asio::ip::make_address("127.0.0.1"), 0});
    std::vector<std::string> request_lines;
    std::exception_ptr peer_failure;
    ruvia::worker_signal peer_done(worker.handle_);
    bool peer_finished = false;
    const auto serve = [&]() -> asio::awaitable<void> {
        auto socket = co_await acceptor.async_accept(asio::use_awaitable);
        std::string buffered;
        for (unsigned index = 0; index != 2; ++index) {
            const auto head_bytes =
                co_await asio::async_read_until(socket, asio::dynamic_buffer(buffered), "\r\n\r\n", asio::use_awaitable);
            request_lines.push_back(buffered.substr(0, buffered.find("\r\n")));
            buffered.erase(0, head_bytes);
            if (!request_lines.back().starts_with("GET ")) {
                co_return;
            }
            if (index == 0) {
                // Keep the only slot leased while the queued request is abandoned.
                asio::steady_timer delay(socket.get_executor(), std::chrono::milliseconds(100));
                co_await delay.async_wait(asio::use_awaitable);
            }
            constexpr std::string_view response = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok";
            co_await asio::async_write(socket, asio::buffer(response), asio::use_awaitable);
        }
    };
    asio::co_spawn(io, serve(), [&](std::exception_ptr failure) {
        peer_failure = failure;
        peer_finished = true;
        peer_done.notify();
    });
    auto config = local_http_client_config(acceptor.local_endpoint().port());
    config.connection_count_ = 1;
    config.request_timeout_ = std::chrono::seconds(3);
    ruvia::http_client client(worker.attachment_.loop(), config);
    auto operation = [&]() -> ruvia::task<void> {
        const acceptor_closer closer{acceptor};
        ruvia::task_scope first(worker.handle_);
        const auto send_first = [&]() -> ruvia::task<void> {
            auto response = co_await client.handle().send({.target_ = "/first"});
            (void)co_await response.body().read_all();
        };
        first.spawn(send_first());
        {
            auto abandoned = co_await client.handle().open_request({.method_ = "POST", .target_ = "/abandoned"}, {});
            // Let the request driver queue behind the leased slot.
            for (unsigned turn = 0; turn != 4; ++turn) {
                (void)co_await ruvia::async_asio([&io](auto done) {
                    asio::post(io, [done = std::move(done)]() mutable { done(std::error_code{}); });
                });
            }
        }
        co_await first.join();
        auto response = co_await client.handle().send({.target_ = "/third"});
        const auto bytes_value = co_await response.body().read_all();
        RUVIA_CHECK_EQ(std::string_view(reinterpret_cast<const char*>(bytes_value.bytes().data()), bytes_value.size()), "ok");
        while (!peer_finished) {
            co_await peer_done.wait();
        }
        if (peer_failure) {
            std::rethrow_exception(peer_failure);
        }
        co_await client.shutdown();
    };
    run_operation(worker, io, operation);
    RUVIA_CHECK_EQ(request_lines.size(), std::size_t{2});
    if (request_lines.size() == 2) {
        RUVIA_CHECK_EQ(request_lines[0], "GET /first HTTP/1.1");
        RUVIA_CHECK_EQ(request_lines[1], "GET /third HTTP/1.1");
    }
}

// Dropping an exchange whose connection is still being established cancels
// that connect: the slot frees at once instead of after connect_timeout_.
RUVIA_TEST(http_client_abandoned_request_cancels_its_connect_in_progress) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    asio::ip::tcp::acceptor acceptor(io, {asio::ip::make_address("127.0.0.1"), 0});
    std::exception_ptr peer_failure;
    bool peer_finished = false;
    ruvia::worker_signal peer_done(worker.handle_);
    // Accepts TCP but never answers the TLS ClientHello, so the client's
    // connect stays in progress until its connect_timeout_ or a cancellation.
    const auto serve = [&]() -> asio::awaitable<void> {
        auto socket = co_await acceptor.async_accept(asio::use_awaitable);
        std::array<char, 1024> input{};
        std::error_code error;
        while (co_await socket.async_read_some(asio::buffer(input), asio::redirect_error(asio::use_awaitable, error))) {
        }
    };
    asio::co_spawn(io, serve(), [&](std::exception_ptr failure) {
        peer_failure = failure;
        peer_finished = true;
        peer_done.notify();
    });
    ruvia::http_client_config config{.scheme_ = ruvia::http_scheme::https,
        .host_ = "127.0.0.1",
        .port_ = acceptor.local_endpoint().port(),
        .connection_count_ = 1,
        .connect_timeout_ = std::chrono::seconds(5),
        .request_timeout_ = std::chrono::seconds(10),
        .tls_ = {.mode_ = ruvia::client_tls_mode::skip_verification}};
    ruvia::http_client client(worker.attachment_.loop(), config);
    auto operation = [&]() -> ruvia::task<void> {
        const acceptor_closer closer{acceptor};
        {
            auto abandoned = co_await client.handle().open_request({.method_ = "POST", .target_ = "/abandoned"}, {});
            // Let the driver reach the TLS handshake.
            (void)co_await ruvia::sleep_for(worker.handle_, std::chrono::milliseconds(100));
            const auto stats = client.stats();
            RUVIA_CHECK_EQ(stats.in_flight_requests_ + stats.buffered_requests_, std::size_t{1});
        }
        const auto started = std::chrono::steady_clock::now();
        for (;;) {
            const auto stats = client.stats();
            if (stats.in_flight_requests_ + stats.buffered_requests_ == 0) {
                break;
            }
            if (std::chrono::steady_clock::now() - started > std::chrono::seconds(2)) {
                throw std::runtime_error("abandoned connect kept its pool slot leased");
            }
            (void)co_await ruvia::sleep_for(worker.handle_, std::chrono::milliseconds(10));
        }
        RUVIA_CHECK_EQ(client.stats().failed_requests_, std::size_t{1});
        while (!peer_finished) {
            co_await peer_done.wait();
        }
        if (peer_failure) {
            std::rethrow_exception(peer_failure);
        }
        co_await client.shutdown();
    };
    run_operation(worker, io, operation);
}

// RFC 9110 §7.5 permits the final response to finish before its request. A
// persistent final response does not cancel content that was already released.
RUVIA_TEST(http_client_finishes_upload_after_an_early_persistent_response_before_reusing_connection) {
    for (const bool expect_continue : {false, true}) {
        for (const bool known_length : {false, true}) {
            auto& io = ruvia::test::new_test_io_context();
            test_worker worker(io);
            asio::ip::tcp::acceptor acceptor(io, {asio::ip::make_address("127.0.0.1"), 0});
            std::exception_ptr peer_failure;
            bool peer_finished = false;
            ruvia::worker_signal peer_done(worker.handle_);
            std::string request_body;
            std::string next_request;
            const auto serve = [&]() -> asio::awaitable<void> {
                auto socket = co_await acceptor.async_accept(asio::use_awaitable);
                std::string buffered;
                const auto head_bytes =
                    co_await asio::async_read_until(socket, asio::dynamic_buffer(buffered), "\r\n\r\n", asio::use_awaitable);
                buffered.erase(0, head_bytes);
                std::string response;
                if (expect_continue) {
                    response += "HTTP/1.1 100 Continue\r\n\r\n";
                }
                response += "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok";
                co_await asio::async_write(socket, asio::buffer(response), asio::use_awaitable);
                std::size_t body_bytes = 3;
                if (known_length) {
                    if (buffered.size() < body_bytes) {
                        co_await asio::async_read(socket, asio::dynamic_buffer(buffered),
                            asio::transfer_exactly(body_bytes - buffered.size()), asio::use_awaitable);
                    }
                } else {
                    body_bytes = co_await asio::async_read_until(
                        socket, asio::dynamic_buffer(buffered), "0\r\nx-end: retained\r\n\r\n", asio::use_awaitable);
                }
                request_body = buffered.substr(0, body_bytes);
                buffered.erase(0, body_bytes);
                const auto next_head_bytes =
                    co_await asio::async_read_until(socket, asio::dynamic_buffer(buffered), "\r\n\r\n", asio::use_awaitable);
                next_request = buffered.substr(0, next_head_bytes);
                constexpr std::string_view final_response =
                    "HTTP/1.1 200 OK\r\nContent-Length: 4\r\nConnection: close\r\n\r\nnext";
                co_await asio::async_write(socket, asio::buffer(final_response), asio::use_awaitable);
            };
            asio::co_spawn(io, serve(), [&](std::exception_ptr failure) {
                peer_failure = failure;
                peer_finished = true;
                peer_done.notify();
            });
            auto config = local_http_client_config(acceptor.local_endpoint().port());
            config.connection_count_ = 1;
            config.request_timeout_ = std::chrono::seconds(3);
            ruvia::http_client client(worker.attachment_.loop(), config);
            auto operation = [&]() -> ruvia::task<void> {
                const acceptor_closer closer{acceptor};
                auto exchange = co_await client.handle().open_request({.method_ = "POST", .target_ = "/early"},
                    {.content_length_ = known_length ? std::optional<std::uint64_t>{3} : std::nullopt,
                        .expectation_ = expect_continue ? ruvia::http_client_request_expectation::continue_value
                                                        : ruvia::http_client_request_expectation::none});
                auto response = co_await exchange.response();
                RUVIA_CHECK_EQ(response.status().value(), std::uint16_t{200});
                const auto early_body = co_await response.body().read_all();
                RUVIA_CHECK_EQ(std::string_view(reinterpret_cast<const char*>(early_body.bytes().data()), early_body.size()), "ok");
                RUVIA_CHECK(response.body().complete());
                RUVIA_CHECK(!exchange.body().complete());
                RUVIA_CHECK_EQ(client.stats().in_flight_requests_, std::size_t{1});
                co_await exchange.body().write("abc");
                const ruvia::http_header_view trailers[] = {{"X-End", "retained"}};
                co_await exchange.body().end(known_length ? std::span<const ruvia::http_header_view>{}
                                                          : std::span<const ruvia::http_header_view>(trailers));
                auto next = co_await client.handle().send({.target_ = "/next"});
                const auto next_body = co_await next.body().read_all();
                RUVIA_CHECK_EQ(std::string_view(reinterpret_cast<const char*>(next_body.bytes().data()), next_body.size()), "next");
                while (!peer_finished) {
                    co_await peer_done.wait();
                }
                if (peer_failure) {
                    std::rethrow_exception(peer_failure);
                }
                co_await client.shutdown();
            };
            run_operation(worker, io, operation);
            RUVIA_CHECK_EQ(request_body, known_length ? "abc" : "3\r\nabc\r\n0\r\nx-end: retained\r\n\r\n");
            RUVIA_CHECK(next_request.starts_with("GET /next HTTP/1.1\r\n"));
        }
    }
}

RUVIA_TEST(http_client_bounds_unfinished_upload_after_response_body_completion) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    asio::ip::tcp::acceptor acceptor(io, {asio::ip::make_address("127.0.0.1"), 0});
    std::exception_ptr peer_failure;
    bool peer_finished = false;
    bool first_closed = false;
    ruvia::worker_signal peer_done(worker.handle_);
    const auto serve = [&]() -> asio::awaitable<void> {
        auto socket = co_await acceptor.async_accept(asio::use_awaitable);
        asio::streambuf request;
        co_await asio::async_read_until(socket, request, "\r\n\r\n", asio::use_awaitable);
        constexpr std::string_view response = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok";
        co_await asio::async_write(socket, asio::buffer(response), asio::use_awaitable);
        std::array<char, 256> input{};
        std::error_code error;
        while (co_await socket.async_read_some(asio::buffer(input), asio::redirect_error(asio::use_awaitable, error))) {
        }
        if (error != asio::error::eof && error != asio::error::connection_reset) {
            throw std::system_error(error);
        }
        first_closed = true;
        peer_done.notify();
        auto next_socket = co_await acceptor.async_accept(asio::use_awaitable);
        asio::streambuf next_request;
        co_await asio::async_read_until(next_socket, next_request, "\r\n\r\n", asio::use_awaitable);
        constexpr std::string_view next_response =
            "HTTP/1.1 200 OK\r\nContent-Length: 4\r\nConnection: close\r\n\r\nnext";
        co_await asio::async_write(next_socket, asio::buffer(next_response), asio::use_awaitable);
    };
    asio::co_spawn(io, serve(), [&](std::exception_ptr failure) {
        peer_failure = failure;
        peer_finished = true;
        first_closed = true;
        peer_done.notify();
    });
    auto config = local_http_client_config(acceptor.local_endpoint().port());
    config.request_timeout_ = std::chrono::milliseconds(100);
    config.connection_count_ = 1;
    ruvia::http_client client(worker.attachment_.loop(), config);
    auto operation = [&]() -> ruvia::task<void> {
        const acceptor_closer closer{acceptor};
        auto exchange = co_await client.handle().open_request({.method_ = "POST", .target_ = "/unfinished"}, {});
        auto response = co_await exchange.response();
        (void)co_await response.body().read_all();
        while (!first_closed) {
            co_await peer_done.wait();
        }
        if (peer_failure) {
            std::rethrow_exception(peer_failure);
        }
        bool stopped = false;
        try {
            co_await exchange.body().write("late");
        } catch (const ruvia::http_client_error& error) {
            stopped = error.code() == ruvia::http_client_error::code_type::cancelled;
        }
        RUVIA_CHECK(stopped);
        auto next = co_await client.handle().send({.target_ = "/after-timeout"});
        const auto next_body = co_await next.body().read_all();
        RUVIA_CHECK_EQ(std::string_view(reinterpret_cast<const char*>(next_body.bytes().data()), next_body.size()), "next");
        RUVIA_CHECK_EQ(client.stats().failed_requests_, std::size_t{1});
        while (!peer_finished) {
            co_await peer_done.wait();
        }
        if (peer_failure) {
            std::rethrow_exception(peer_failure);
        }
        co_await client.shutdown();
        RUVIA_CHECK(!(co_await response.body().text()).has_value());
    };
    run_operation(worker, io, operation);
}

// A transport failure while the response body is still arriving is the real
// cause of the exchange failure, even though the writer sees it first and its
// cleanup cancels the receiver's read; it is reported as such, not as a
// cancellation. The response body is left unread so the receiver sits in the
// backpressure wait without a read in flight and only the upload observes the reset.
RUVIA_TEST(http_client_reports_upload_transport_failure_during_response_body) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    asio::ip::tcp::acceptor acceptor(io, {asio::ip::make_address("127.0.0.1"), 0});
    std::exception_ptr peer_failure;
    bool peer_finished = false;
    ruvia::worker_signal peer_done(worker.handle_);
    const auto serve = [&]() -> asio::awaitable<void> {
        auto socket = co_await acceptor.async_accept(asio::use_awaitable);
        asio::streambuf request;
        co_await asio::async_read_until(socket, request, "\r\n\r\n", asio::use_awaitable);
        constexpr std::string_view response = "HTTP/1.1 200 OK\r\nContent-Length: 1000\r\n\r\n0123456789012345678901234567890123456789";
        co_await asio::async_write(socket, asio::buffer(response), asio::use_awaitable);
        std::array<char, 4096> input{};
        std::size_t received = 0;
        while (received < 64 * 1024) {
            received += co_await socket.async_read_some(asio::buffer(input), asio::use_awaitable);
        }
        // SO_LINGER 0 turns close() into a reset the client's next write observes.
        socket.set_option(asio::socket_base::linger(true, 0));
        socket.close();
    };
    asio::co_spawn(io, serve(), [&](std::exception_ptr failure) {
        peer_failure = failure;
        peer_finished = true;
        peer_done.notify();
    });
    auto config = local_http_client_config(acceptor.local_endpoint().port());
    config.max_response_bytes_ = 16;
    config.connection_count_ = 1;
    ruvia::http_client client(worker.attachment_.loop(), config);
    auto operation = [&]() -> ruvia::task<void> {
        const acceptor_closer closer{acceptor};
        auto exchange = co_await client.handle().open_request({.method_ = "POST", .target_ = "/reset"}, {});
        auto response = co_await exchange.response();
        RUVIA_CHECK(response.status() == ruvia::http_status::ok);
        const std::string chunk(16 * 1024, 'u');
        std::optional<ruvia::http_client_error::code_type> upload_error;
        for (int i = 0; i < 1024 && !upload_error.has_value(); ++i) {
            try {
                co_await exchange.body().write(chunk);
            } catch (const ruvia::http_client_error& error) {
                upload_error = error.code();
            }
        }
        RUVIA_CHECK(upload_error.has_value());
        if (upload_error.has_value()) {
            RUVIA_CHECK(*upload_error == ruvia::http_client_error::code_type::io_error);
        }
        while (!peer_finished) {
            co_await peer_done.wait();
        }
        if (peer_failure) {
            std::rethrow_exception(peer_failure);
        }
        co_await client.shutdown();
    };
    run_operation(worker, io, operation);
}

RUVIA_TEST(http_capsule_client_retains_payloads_across_reads_and_rejects_truncated_frames) {
    for (const bool truncated : {false, true}) {
        auto& io = ruvia::test::new_test_io_context();
        test_worker worker(io);
        asio::ip::tcp::acceptor acceptor(io, {asio::ip::tcp::v4(), 0});
        ruvia::worker_signal peer_done(worker.handle_);
        std::exception_ptr peer_failure;
        std::string first_payload(128 * 1024 + 17, 'a');
        first_payload[4095] = '\0';
        first_payload[8192] = '\xff';
        const std::string second_payload(4096, 'b');
        std::string wire = "HTTP/1.1 200 Connection Established\r\n\r\n";
        const auto append_capsule = [&](std::uint64_t type, std::string_view payload) {
            std::array<char, 16> header;
            const auto encoded = ruvia::encode_http_capsule_header(header, type, payload.size());
            wire.append(header.data(), std::get<std::size_t>(encoded));
            wire.append(payload);
        };
        append_capsule(17, first_payload);
        append_capsule(18, second_payload);
        if (truncated) {
            wire.pop_back();
        } else {
            append_capsule(19, {});
        }
        const auto serve = [&]() -> asio::awaitable<void> {
            auto socket = co_await acceptor.async_accept(asio::use_awaitable);
            asio::streambuf head;
            co_await asio::async_read_until(socket, head, "\r\n\r\n", asio::use_awaitable);
            co_await asio::async_write(socket, asio::buffer(wire), asio::use_awaitable);
            socket.shutdown(asio::ip::tcp::socket::shutdown_send);
        };
        asio::co_spawn(io, serve(), [&](std::exception_ptr failure) {
            peer_failure = failure;
            peer_done.notify();
        });
        ruvia::http_client client(worker.attachment_.loop(), local_http_client_config(acceptor.local_endpoint().port()));
        auto operation = [&]() -> ruvia::task<void> {
            auto result = co_await client.handle().open_tunnel({.authority_ = "target.test:443"});
            if (result.tunnel() == nullptr) {
                throw std::runtime_error("expected an accepted tunnel");
            }
            std::optional<ruvia::http_capsule> retained;
            {
                auto stream = std::move(*result.tunnel()).capsules();
                retained = co_await stream.read();
                RUVIA_CHECK(retained && retained->type() == 17 && retained->payload() == first_payload);
                bool rejected = false;
                try {
                    auto second = co_await stream.read();
                    RUVIA_CHECK(second && second->type() == 18 && second->payload() == second_payload);
                    auto empty = co_await stream.read();
                    RUVIA_CHECK(empty && empty->type() == 19 && empty->payload().empty());
                    RUVIA_CHECK(!(co_await stream.read()));
                    co_await stream.finish();
                } catch (const std::runtime_error&) {
                    rejected = true;
                }
                RUVIA_CHECK_EQ(rejected, truncated);
            }
            RUVIA_CHECK(retained && retained->payload() == first_payload);
            retained.reset();
            co_await client.shutdown();
            co_await peer_done.wait();
            if (peer_failure) {
                std::rethrow_exception(peer_failure);
            }
        };
        run_operation(worker, io, operation);
    }
}
