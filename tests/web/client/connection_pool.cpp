#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <initializer_list>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <asio/co_spawn.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/post.hpp>
#include <asio/read.hpp>
#include <asio/read_until.hpp>
#include <asio/redirect_error.hpp>
#include <asio/steady_timer.hpp>
#include <asio/streambuf.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/write.hpp>

#include "blocking_pool.h"
#include "content_decoding_fixture.h"
#include "event_loop.h"
#include "http1_server_response.h"
#include "http_client.h"
#include "http_client_fixture.h"
#include "http_client_request_view.h"
#include "http_client_types.h"
#include "http_connect_udp.h"
#include "http_header.h"
#include "http_request.h"
#include "http_server_fixture.h"
#include "stop_token.h"
#include "streaming.h"
#include "task.h"
#include "task_scope.h"
#include "worker_signal.h"
#include "worker_task_fixture.h"

namespace {
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

}  // namespace

// A response on a connection that is not reused completes even when trailing
// bytes arrive with it: they are discarded with the connection, not parsed.
TEST(Ruvia, http1_response_on_a_closing_connection_discards_trailing_bytes) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    scripted_head_peer peer(io, worker.handle_, {"HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Length: 5\r\n\r\nhelloXjunk", "HTTP/1.1 407 Proxy Authentication Required\r\nContent-Length: 6\r\n\r\ndeniedHTTP/1.1 200 OK\r\n\r\n"});
    peer.start();
    const std::string config_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(peer.port())));
    auto config = local_http_client_config(config_url);
    config.request_timeout_ = std::chrono::seconds(3);
    ruvia::http_client client(worker.attachment_.loop(), config);
    auto operation = [&]() -> ruvia::task<void> {
        {
            auto response = co_await client.client().get("/closing").send();
            const auto bytes_value = co_await response.stream().read_all();
            EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(bytes_value.bytes().data()), bytes_value.size()), "hello");
        }
        {
            auto result = co_await client.client().connect("target.test:443").open();
            auto* response = result.response();
            EXPECT_TRUE(response != nullptr);
            if (response != nullptr) {
                EXPECT_EQ(response->status().value(), std::uint16_t{407});
                const auto bytes_value = co_await response->stream().read_all();
                EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(bytes_value.bytes().data()), bytes_value.size()), "denied");
            }
        }
        co_await peer.wait();
        EXPECT_EQ(client.client().stats().failed_requests_, std::size_t{0});
        co_await client.shutdown();
    };
    run_operation(worker, io, operation);
}

TEST(Ruvia, http_client_replaces_a_keep_alive_connection_the_peer_closed_while_idle) {
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
            const std::string response = std::move(std::string("HTTP/1.1 200 OK\r\nContent-Length: ").append(std::to_string(body.size()))) + "\r\n\r\n" + std::string(body);
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
    const std::string config_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(acceptor.local_endpoint().port())));
    auto config = local_http_client_config(config_url);
    config.connection_limits_.max_open_ = 1;
    config.request_timeout_ = std::chrono::seconds(3);
    ruvia::http_client client(worker.attachment_.loop(), config);
    auto operation = [&]() -> ruvia::task<void> {
        const acceptor_closer closer{acceptor};
        for (const std::string_view expected : {std::string_view("first"), std::string_view("second")}) {
            auto response = co_await client.client().get("/idle").send();
            const auto bytes_value = co_await response.stream().read_all();
            EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(bytes_value.bytes().data()), bytes_value.size()), expected);
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
        const auto stats = client.client().stats();
        EXPECT_EQ(stats.failed_requests_, std::size_t{0});
        EXPECT_EQ(stats.idle_connections_with_pending_data_, std::size_t{0});
        co_await client.shutdown();
    };
    run_operation(worker, io, operation);
    EXPECT_EQ(connections, 2U);
}

TEST(Ruvia, http_client_retries_only_idempotent_requests_after_a_reused_connection_fails_before_its_response) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    asio::ip::tcp::acceptor acceptor(io, {asio::ip::make_address("127.0.0.1"), 0});
    unsigned connections = 0;
    unsigned retried_requests = 0;
    unsigned non_idempotent_requests = 0;
    std::exception_ptr peer_failure;
    ruvia::worker_signal peer_done(worker.handle_);
    bool peer_finished = false;
    const auto read_request = [](asio::ip::tcp::socket& socket) -> asio::awaitable<void> {
        asio::streambuf request;
        co_await asio::async_read_until(socket, request, "\r\n\r\n", asio::use_awaitable);
    };
    const auto answer = [](asio::ip::tcp::socket& socket, std::string_view body, std::string_view connection) -> asio::awaitable<void> {
        const std::string response = std::move(std::string("HTTP/1.1 200 OK\r\nContent-Length: ").append(std::to_string(body.size()))) + "\r\nConnection: " + std::string(connection) + "\r\n\r\n" + std::string(body);
        co_await asio::async_write(socket, asio::buffer(response), asio::use_awaitable);
    };
    const auto serve = [&]() -> asio::awaitable<void> {
        auto first = co_await acceptor.async_accept(asio::use_awaitable);
        ++connections;
        co_await read_request(first);
        co_await answer(first, "prime", "keep-alive");
        co_await read_request(first);
        ++retried_requests;
        std::error_code ignored;
        first.close(ignored);

        std::error_code accept_error;
        auto second = co_await acceptor.async_accept(asio::redirect_error(asio::use_awaitable, accept_error));
        if (accept_error == asio::error::operation_aborted) {
            co_return;
        }
        if (accept_error) {
            throw std::system_error(accept_error);
        }
        ++connections;
        co_await read_request(second);
        ++retried_requests;
        co_await answer(second, "retried", "keep-alive");
        co_await read_request(second);
        ++non_idempotent_requests;
        acceptor.close(ignored);
        second.close(ignored);
    };
    asio::co_spawn(io, serve(), [&](std::exception_ptr failure) {
        peer_failure = failure;
        peer_finished = true;
        peer_done.notify();
    });
    const std::string config_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(acceptor.local_endpoint().port())));
    auto config = local_http_client_config(config_url);
    config.connection_limits_.max_open_ = 1;
    config.request_timeout_ = std::chrono::seconds(3);
    ruvia::http_client client(worker.attachment_.loop(), config);
    auto operation = [&]() -> ruvia::task<void> {
        const acceptor_closer closer{acceptor};
        auto first = co_await client.client().get("/prime").send();
        const auto first_body = co_await first.stream().read_all();
        EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(first_body.bytes().data()), first_body.size()), "prime");

        std::optional<std::string> retry_body;
        try {
            auto response = co_await client.client().get("/retry").send();
            const auto bytes_value = co_await response.stream().read_all();
            retry_body.emplace(reinterpret_cast<const char*>(bytes_value.bytes().data()), bytes_value.size());
        } catch (const ruvia::http_client_error&) {
            std::error_code ignored;
            acceptor.close(ignored);
        }
        std::optional<std::error_code> post_failure;
        if (retry_body.has_value()) {
            try {
                (void)co_await client.client().request("POST", "/post").send();
            } catch (const ruvia::http_client_error& error) {
                post_failure = error.code();
            }
        }
        while (!peer_finished) {
            co_await peer_done.wait();
        }
        if (peer_failure) {
            std::rethrow_exception(peer_failure);
        }
        EXPECT_TRUE(retry_body == std::optional<std::string>(std::string("retried")));
        EXPECT_TRUE(post_failure == ruvia::make_error_code(ruvia::http_client_errc::io_error));
        const auto stats = client.client().stats();
        EXPECT_EQ(stats.completed_requests_, std::size_t{2});
        EXPECT_EQ(stats.failed_requests_, std::size_t{1});
        co_await client.shutdown();
    };
    run_operation(worker, io, operation);
    EXPECT_EQ(connections, 2U);
    EXPECT_EQ(retried_requests, 2U);
    EXPECT_EQ(non_idempotent_requests, 1U);
}

TEST(Ruvia, http_client_abandoned_queued_request_is_never_written) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    asio::ip::tcp::acceptor acceptor(io, {asio::ip::make_address("127.0.0.1"), 0});
    std::vector<std::string> request_lines{};
    std::exception_ptr peer_failure;
    ruvia::worker_signal peer_done(worker.handle_);
    bool peer_finished = false;
    const auto serve = [&]() -> asio::awaitable<void> {
        auto socket = co_await acceptor.async_accept(asio::use_awaitable);
        std::string buffered{};
        for (unsigned index = 0; index != 2; ++index) {
            const auto head_bytes = co_await asio::async_read_until(socket, asio::dynamic_buffer(buffered), "\r\n\r\n", asio::use_awaitable);
            request_lines.emplace_back(std::string_view(buffered).substr(0, buffered.find("\r\n")));
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
    const std::string config_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(acceptor.local_endpoint().port())));
    auto config = local_http_client_config(config_url);
    config.connection_limits_.max_open_ = 1;
    config.request_timeout_ = std::chrono::seconds(3);
    ruvia::http_client client(worker.attachment_.loop(), config);
    auto operation = [&]() -> ruvia::task<void> {
        const acceptor_closer closer{acceptor};
        ruvia::task_scope first(worker.handle_);
        const auto send_first = [&]() -> ruvia::task<void> {
            auto response = co_await client.client().get("/first").send();
            (void)co_await response.stream().read_all();
        };
        first.spawn(send_first());
        {
            auto abandoned = co_await client.client().request("POST", "/abandoned").upload({});
            // Let the request driver queue behind the leased slot.
            for (unsigned turn = 0; turn != 4; ++turn) {
                (void)co_await ruvia::async_asio([&io](auto done) {
                    asio::post(io, [done = std::move(done)]() mutable {
                        done(std::error_code{});
                    });
                });
            }
        }
        co_await first.join();
        auto response = co_await client.client().get("/third").send();
        const auto bytes_value = co_await response.stream().read_all();
        EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(bytes_value.bytes().data()), bytes_value.size()), "ok");
        while (!peer_finished) {
            co_await peer_done.wait();
        }
        if (peer_failure) {
            std::rethrow_exception(peer_failure);
        }
        co_await client.shutdown();
    };
    run_operation(worker, io, operation);
    EXPECT_EQ(request_lines.size(), std::size_t{2});
    if (request_lines.size() == 2) {
        EXPECT_EQ(request_lines[0], "GET /first HTTP/1.1");
        EXPECT_EQ(request_lines[1], "GET /third HTTP/1.1");
    }
}

// Dropping an exchange whose connection is still being established cancels
// that connect: the slot frees at once instead of after connect_timeout_.
TEST(Ruvia, http_client_abandoned_request_cancels_its_connect_in_progress) {
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
    const std::string client_url = std::move(std::string("https://127.0.0.1:").append(std::to_string(acceptor.local_endpoint().port())));
    ruvia::http_client_config config{.base_url_ = client_url, .connection_limits_ = {.max_open_ = 1}, .connect_timeout_ = std::chrono::seconds(5), .request_timeout_ = std::chrono::seconds(10)};
    ruvia::http_client client(worker.attachment_.loop(), config);
    auto operation = [&]() -> ruvia::task<void> {
        const acceptor_closer closer{acceptor};
        {
            auto abandoned = co_await client.client().request("POST", "/abandoned").upload({});
            // Let the driver reach the TLS handshake.
            (void)co_await ruvia::sleep_for(worker.handle_, std::chrono::milliseconds(100));
            const auto stats = client.client().stats();
            EXPECT_EQ(stats.in_flight_requests_ + stats.buffered_requests_, std::size_t{1});
        }
        const auto started = std::chrono::steady_clock::now();
        for (;;) {
            const auto stats = client.client().stats();
            if (stats.in_flight_requests_ + stats.buffered_requests_ == 0) {
                break;
            }
            if (std::chrono::steady_clock::now() - started > std::chrono::seconds(2)) {
                throw std::runtime_error("abandoned connect kept its pool slot leased");
            }
            (void)co_await ruvia::sleep_for(worker.handle_, std::chrono::milliseconds(10));
        }
        EXPECT_EQ(client.client().stats().failed_requests_, std::size_t{1});
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
TEST(Ruvia, http_client_finishes_upload_after_an_early_persistent_response_before_reusing_connection) {
    enum class release_mode { immediate,
        continued,
        timed_out,
        submillisecond_timeout };
    for (const auto mode : {release_mode::immediate, release_mode::continued, release_mode::timed_out, release_mode::submillisecond_timeout}) {
        const bool expect_continue = mode != release_mode::immediate;
        const bool timeout_release = mode == release_mode::timed_out || mode == release_mode::submillisecond_timeout;
        for (const bool known_length : {false, true}) {
            auto& io = ruvia::test::new_test_io_context();
            test_worker worker(io);
            asio::ip::tcp::acceptor acceptor(io, {asio::ip::make_address("127.0.0.1"), 0});
            std::exception_ptr peer_failure;
            bool peer_finished = false;
            ruvia::worker_signal peer_done(worker.handle_);
            std::string request_body{};
            ruvia::http_request_trailers request_trailers;
            std::string next_request{};
            const auto serve = [&]() -> asio::awaitable<void> {
                auto socket = co_await acceptor.async_accept(asio::use_awaitable);
                std::string buffered{};
                const auto head_bytes = co_await asio::async_read_until(socket, asio::dynamic_buffer(buffered), "\r\n\r\n", asio::use_awaitable);
                buffered.erase(0, head_bytes);
                if (timeout_release) {
                    if (buffered.empty()) {
                        co_await asio::async_read(socket, asio::dynamic_buffer(buffered), asio::transfer_at_least(1), asio::use_awaitable);
                    }
                }
                std::string response{};
                if (expect_continue && !timeout_release) {
                    response += "HTTP/1.1 100 Continue\r\n\r\n";
                }
                response += "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok";
                co_await asio::async_write(socket, asio::buffer(response), asio::use_awaitable);
                if (known_length) {
                    if (buffered.size() < 3) {
                        co_await asio::async_read(socket, asio::dynamic_buffer(buffered), asio::transfer_exactly(3 - buffered.size()), asio::use_awaitable);
                    }
                    request_body.assign(buffered.data(), 3);
                    buffered.erase(0, 3);
                } else {
                    ruvia::http1_chunked_body_decoder decoder;
                    for (;;) {
                        const auto decoded = decoder.decode(buffered);
                        if (decoded.failure()) {
                            throw std::runtime_error("invalid upload chunk framing");
                        }
                        if (const auto* chunk = decoded.body_chunk()) {
                            request_body.append(chunk->bytes());
                        }
                        const bool complete = decoded.complete() != nullptr;
                        if (complete && request_trailers.append_received(decoded.complete()->trailers())) {
                            throw std::runtime_error("invalid upload trailers");
                        }
                        buffered.erase(0, decoded.consumed_bytes());
                        if (complete) {
                            break;
                        }
                        if (decoded.need_more()) {
                            co_await asio::async_read(socket, asio::dynamic_buffer(buffered), asio::transfer_at_least(1), asio::use_awaitable);
                        }
                    }
                }
                const auto next_head_bytes = co_await asio::async_read_until(socket, asio::dynamic_buffer(buffered), "\r\n\r\n", asio::use_awaitable);
                next_request.assign(buffered.data(), next_head_bytes);
                constexpr std::string_view final_response = "HTTP/1.1 200 OK\r\nContent-Length: 4\r\nConnection: close\r\n\r\nnext";
                co_await asio::async_write(socket, asio::buffer(final_response), asio::use_awaitable);
            };
            asio::co_spawn(io, serve(), [&](std::exception_ptr failure) {
                peer_failure = failure;
                peer_finished = true;
                peer_done.notify();
            });
            const std::string config_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(acceptor.local_endpoint().port())));
            auto config = local_http_client_config(config_url);
            config.connection_limits_.max_open_ = 1;
            config.request_timeout_ = std::chrono::seconds(3);
            ruvia::http_client client(worker.attachment_.loop(), config);
            auto operation = [&]() -> ruvia::task<void> {
                const acceptor_closer closer{acceptor};
                ruvia::http_client_upload_config upload{.content_length_ = known_length ? std::optional<ruvia::http_content_length>{3} : std::nullopt, .expectation_ = expect_continue ? ruvia::http_client_request_expectation::continue_value : ruvia::http_client_request_expectation::none, .continue_timeout_ = std::chrono::duration<std::uint64_t, std::milli>{std::numeric_limits<std::uint64_t>::max()}};
                if (timeout_release) {
                    upload.continue_timeout_ = mode == release_mode::submillisecond_timeout ? std::chrono::microseconds(1) : std::chrono::microseconds(20000);
                }
                auto exchange = co_await client.client().request("POST", "/early").upload(upload);
                if (timeout_release) {
                    co_await exchange.body().write("a");
                }
                auto response = co_await exchange.response();
                EXPECT_EQ(response.status().value(), std::uint16_t{200});
                const auto early_body = co_await response.stream().read_all();
                EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(early_body.bytes().data()), early_body.size()), "ok");
                EXPECT_TRUE(response.stream().complete());
                EXPECT_TRUE(!exchange.body().complete());
                EXPECT_EQ(client.client().stats().in_flight_requests_, std::size_t{1});
                co_await exchange.body().write(timeout_release ? "bc" : "abc");
                const ruvia::http_header_view trailers[] = {{"X-End", "retained"}};
                co_await exchange.body().end(known_length ? std::span<const ruvia::http_header_view>{} : std::span<const ruvia::http_header_view>(trailers));
                auto next = co_await client.client().get("/next").send();
                const auto next_body = co_await next.stream().read_all();
                EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(next_body.bytes().data()), next_body.size()), "next");
                while (!peer_finished) {
                    co_await peer_done.wait();
                }
                if (peer_failure) {
                    std::rethrow_exception(peer_failure);
                }
                co_await client.shutdown();
            };
            run_operation(worker, io, operation);
            EXPECT_EQ(request_body, "abc");
            EXPECT_TRUE(request_trailers.field("x-end") == (known_length ? std::nullopt : std::optional<std::string_view>("retained")));
            ruvia::http1_server_request_parse_state parsed;
            parsed.parse_head(next_request);
            EXPECT_TRUE(parsed.head_ready());
            EXPECT_EQ(parsed.request().method(), "GET");
            EXPECT_EQ(parsed.request().target(), "/next");
        }
    }
}

TEST(Ruvia, http_client_abandoned_upload_cancels_its_retained_incomplete_response) {
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
        constexpr std::string_view head = "HTTP/1.1 200 OK\r\nContent-Length: 1\r\n\r\n";
        co_await asio::async_write(socket, asio::buffer(head), asio::use_awaitable);
        std::array<char, 1> input;
        std::error_code error;
        const auto bytes = co_await socket.async_read_some(asio::buffer(input), asio::redirect_error(asio::use_awaitable, error));
        if (bytes != 0 || (error != asio::error::eof && error != asio::error::connection_reset)) {
            throw std::runtime_error("abandoned HTTP/1 upload did not close");
        }
        auto next_socket = co_await acceptor.async_accept(asio::use_awaitable);
        asio::streambuf next_request;
        co_await asio::async_read_until(next_socket, next_request, "\r\n\r\n", asio::use_awaitable);
        constexpr std::string_view next_response = "HTTP/1.1 200 OK\r\nContent-Length: 4\r\nConnection: close\r\n\r\nnext";
        co_await asio::async_write(next_socket, asio::buffer(next_response), asio::use_awaitable);
    };
    asio::co_spawn(io, serve(), [&](std::exception_ptr failure) {
        peer_failure = failure;
        peer_finished = true;
        peer_done.notify();
    });
    const std::string config_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(acceptor.local_endpoint().port())));
    auto config = local_http_client_config(config_url);
    config.connection_limits_.max_open_ = 1;
    // A broken abandonment path fails as timeout rather than hanging the suite.
    config.request_timeout_ = std::chrono::seconds(1);
    ruvia::http_client client(worker.attachment_.loop(), config);
    auto operation = [&]() -> ruvia::task<void> {
        const acceptor_closer closer{acceptor};
        std::optional<ruvia::http_client_response> response;
        {
            auto exchange = co_await client.client().post("/abandoned").upload({.content_length_ = 6});
            response.emplace(co_await exchange.response());
            EXPECT_TRUE(!response->stream().complete());
        }
        bool cancelled = false;
        try {
            (void)co_await response->stream().read_all();
        } catch (const ruvia::http_client_error& error) {
            cancelled = error.code() == std::make_error_code(std::errc::operation_canceled);
        }
        EXPECT_TRUE(cancelled);
        auto next = co_await client.client().get("/next").send();
        const auto next_body = co_await next.stream().read_all();
        EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(next_body.bytes().data()), next_body.size()), "next");
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

TEST(Ruvia, http_client_bounds_unfinished_upload_after_response_body_completion) {
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
        constexpr std::string_view next_response = "HTTP/1.1 200 OK\r\nContent-Length: 4\r\nConnection: close\r\n\r\nnext";
        co_await asio::async_write(next_socket, asio::buffer(next_response), asio::use_awaitable);
    };
    asio::co_spawn(io, serve(), [&](std::exception_ptr failure) {
        peer_failure = failure;
        peer_finished = true;
        first_closed = true;
        peer_done.notify();
    });
    const std::string config_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(acceptor.local_endpoint().port())));
    auto config = local_http_client_config(config_url);
    config.request_timeout_ = std::chrono::milliseconds(100);
    config.connection_limits_.max_open_ = 1;
    ruvia::http_client client(worker.attachment_.loop(), config);
    auto operation = [&]() -> ruvia::task<void> {
        const acceptor_closer closer{acceptor};
        auto exchange = co_await client.client().request("POST", "/unfinished").upload({});
        auto response = co_await exchange.response();
        (void)co_await response.stream().read_all();
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
            stopped = error.code() == std::make_error_code(std::errc::operation_canceled);
        }
        EXPECT_TRUE(stopped);
        auto next = co_await client.client().get("/after-timeout").send();
        const auto next_body = co_await next.stream().read_all();
        EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(next_body.bytes().data()), next_body.size()), "next");
        EXPECT_EQ(client.client().stats().failed_requests_, std::size_t{1});
        while (!peer_finished) {
            co_await peer_done.wait();
        }
        if (peer_failure) {
            std::rethrow_exception(peer_failure);
        }
        co_await client.shutdown();
        EXPECT_TRUE(!(co_await response.stream().text()).has_value());
        EXPECT_TRUE(response.stream().complete());
    };
    run_operation(worker, io, operation);
}

// A transport failure while the response body is still arriving is the real
// cause of the exchange failure, even though the writer sees it first and its
// cleanup cancels the receiver's read; it is reported as such, not as a
// cancellation. The response body is left unread so the receiver sits in the
// backpressure wait without a read in flight and only the upload observes the reset.
TEST(Ruvia, http_client_reports_upload_transport_failure_during_response_body) {
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
    const std::string config_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(acceptor.local_endpoint().port())));
    auto config = local_http_client_config(config_url);
    config.max_response_bytes_ = 16;
    config.connection_limits_.max_open_ = 1;
    ruvia::http_client client(worker.attachment_.loop(), config);
    auto operation = [&]() -> ruvia::task<void> {
        const acceptor_closer closer{acceptor};
        auto exchange = co_await client.client().request("POST", "/reset").upload({});
        auto response = co_await exchange.response();
        EXPECT_TRUE(response.status() == ruvia::http_status::ok);
        const std::string chunk(16 * 1024, 'u');
        std::optional<std::error_code> upload_error;
        for (int i = 0; i < 1024 && !upload_error.has_value(); ++i) {
            try {
                co_await exchange.body().write(chunk);
            } catch (const ruvia::http_client_error& error) {
                upload_error = error.code();
            }
        }
        EXPECT_TRUE(upload_error.has_value());
        if (upload_error.has_value()) {
            EXPECT_TRUE(*upload_error == ruvia::make_error_code(ruvia::http_client_errc::io_error));
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

TEST(Ruvia, http_capsule_client_retains_payloads_across_reads_and_rejects_truncated_frames) {
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
        std::string wire{"HTTP/1.1 200 Connection Established\r\n\r\n"};
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
        const std::string client_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(acceptor.local_endpoint().port())));
        ruvia::http_client client(worker.attachment_.loop(), local_http_client_config(client_url));
        auto operation = [&]() -> ruvia::task<void> {
            auto result = co_await client.client().connect("target.test:443").open();
            if (result.tunnel() == nullptr) {
                throw std::runtime_error("expected an accepted tunnel");
            }
            std::optional<ruvia::http_capsule> retained;
            {
                auto stream = std::move(*result.tunnel()).capsules();
                retained = co_await stream.read();
                EXPECT_TRUE(retained && retained->type() == 17 && retained->payload() == first_payload);
                bool rejected = false;
                try {
                    auto second = co_await stream.read();
                    EXPECT_TRUE(second && second->type() == 18 && second->payload() == second_payload);
                    auto empty = co_await stream.read();
                    EXPECT_TRUE(empty && empty->type() == 19 && empty->payload().empty());
                    EXPECT_TRUE(!(co_await stream.read()));
                    co_await stream.finish();
                } catch (const std::runtime_error&) {
                    rejected = true;
                }
                EXPECT_EQ(rejected, truncated);
            }
            EXPECT_TRUE(retained && retained->payload() == first_payload);
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

// RFC 6265 section 5.4: stored and configured cookies belong to the fixed
// origin. A CONNECT-UDP tunnel to another authority carries none of them, and
// that authority's Set-Cookie does not enter the origin's jar.
TEST(Ruvia, http_client_scopes_cookies_to_the_origin_authority_for_tunnels) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    scripted_head_peer peer(io, worker.handle_,
        {"HTTP/1.1 407 Proxy Authentication Required\r\nSet-Cookie: tunnel=other\r\nContent-Length: 0\r\n"
         "Connection: close\r\n\r\n",
            "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"});
    const std::string config_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(peer.port())));
    auto config = local_http_client_config(config_url);
    config.cookies_ = {{"sid", "secret"}};
    config.received_cookies_ = ruvia::http_client_received_cookie_policy::retain_and_send;
    config.request_timeout_ = std::chrono::seconds(3);
    ruvia::http_client client(worker.attachment_.loop(), config);
    peer.start();
    auto operation = [&]() -> ruvia::task<void> {
        const std::array fields{ruvia::http_header_view{"Capsule-Protocol", "?1"}};
        auto result = co_await client.client().connect("proxy.test:8443").protocol("connect-udp").target("/.well-known/masque/udp/target.test/53/").headers(fields).open();
        EXPECT_TRUE(result.tunnel() == nullptr);
        if (auto* rejected = result.response()) {
            EXPECT_EQ(rejected->status().value(), std::uint16_t{407});
            EXPECT_TRUE((co_await rejected->stream().read_all()).empty());
        }
        auto origin = co_await client.client().get("/").send();
        EXPECT_TRUE((co_await origin.stream().read_all()).empty());
        co_await peer.wait();
        EXPECT_EQ(peer.requests_.size(), std::size_t{2});
        if (peer.requests_.size() == 2) {
            EXPECT_TRUE(peer.requests_[0].find("secret") == std::string::npos);
            EXPECT_TRUE(peer.requests_[1].find("sid=secret") != std::string::npos);
            EXPECT_TRUE(peer.requests_[1].find("tunnel=other") == std::string::npos);
        }
        co_await client.shutdown();
    };
    run_operation(worker, io, operation);
}

// read_timeout_ bounds each response read rather than the exchange: a peer
// that stops sending the head, or the body after its head, fails the response
// with timeout long before request_timeout_ and loses the connection.
TEST(Ruvia, http1_client_read_timeout_bounds_a_stalled_response_head_and_body) {
    for (const bool head_sent : {false, true}) {
        auto& io = ruvia::test::new_test_io_context();
        test_worker worker(io);
        asio::ip::tcp::acceptor acceptor(io, {asio::ip::make_address("127.0.0.1"), 0});
        bool client_closed = false;
        std::exception_ptr peer_failure;
        ruvia::worker_signal peer_done(worker.handle_);
        bool peer_finished = false;
        const auto serve = [&]() -> asio::awaitable<void> {
            auto socket = co_await acceptor.async_accept(asio::use_awaitable);
            asio::streambuf request;
            co_await asio::async_read_until(socket, request, "\r\n\r\n", asio::use_awaitable);
            if (head_sent) {
                const std::string partial("HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nab");
                co_await asio::async_write(socket, asio::buffer(partial), asio::use_awaitable);
            }
            // Stall until the client abandons the read and closes.
            std::array<char, 64> input{};
            std::error_code error;
            while (!error) {
                (void)co_await socket.async_read_some(asio::buffer(input), asio::redirect_error(asio::use_awaitable, error));
            }
            client_closed = true;
        };
        asio::co_spawn(io, serve(), [&](std::exception_ptr failure) {
            peer_failure = failure;
            peer_finished = true;
            peer_done.notify();
        });
        const std::string config_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(acceptor.local_endpoint().port())));
        auto config = local_http_client_config(config_url);
        config.read_timeout_ = std::chrono::milliseconds(100);
        config.request_timeout_ = std::chrono::seconds(20);
        ruvia::http_client client(worker.attachment_.loop(), config);
        auto operation = [&]() -> ruvia::task<void> {
            const acceptor_closer closer{acceptor};
            const auto started = std::chrono::steady_clock::now();
            std::optional<std::error_code> code;
            try {
                auto response = co_await client.client().get("/stall").send();
                EXPECT_TRUE(head_sent);
                (void)co_await response.stream().read_all();
            } catch (const ruvia::http_client_error& error) {
                code = error.code();
            }
            EXPECT_TRUE(code == std::make_error_code(std::errc::timed_out));
            EXPECT_TRUE(std::chrono::steady_clock::now() - started < std::chrono::seconds(10));
            while (!peer_finished) {
                co_await peer_done.wait();
            }
            if (peer_failure) {
                std::rethrow_exception(peer_failure);
            }
            EXPECT_TRUE(client_closed);
            co_await client.shutdown();
        };
        run_operation(worker, io, operation);
    }
}

// idle_timeout_ closes a pooled keep-alive connection that no exchange reused,
// without a request; the next request opens a new connection.
TEST(Ruvia, http1_client_idle_timeout_closes_an_unused_pooled_connection) {
    auto& io = ruvia::test::new_test_io_context();
    test_worker worker(io);
    asio::ip::tcp::acceptor acceptor(io, {asio::ip::make_address("127.0.0.1"), 0});
    std::optional<asio::ip::tcp::socket> idle_socket;
    asio::steady_timer guard(io);
    unsigned connections = 0;
    bool idle_closed = false;
    std::chrono::steady_clock::duration idle_for{};
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
            const std::string response = std::move(std::string("HTTP/1.1 200 OK\r\nContent-Length: ").append(std::to_string(body.size()))) + "\r\n\r\n" + std::string(body);
            co_await asio::async_write(socket, asio::buffer(response), asio::use_awaitable);
            if (idle_closed) {
                continue;
            }
            // Keep the connection open; only the client's idle bound ends it.
            const auto responded = std::chrono::steady_clock::now();
            idle_socket.emplace(std::move(socket));
            std::array<char, 64> input{};
            std::error_code error;
            while (!error) {
                (void)co_await idle_socket->async_read_some(asio::buffer(input), asio::redirect_error(asio::use_awaitable, error));
            }
            idle_for = std::chrono::steady_clock::now() - responded;
            idle_closed = true;
            idle_closed_signal.notify();
        }
    };
    guard.expires_after(std::chrono::seconds(10));
    guard.async_wait([&](const std::error_code& error) {
        if (!error && idle_socket) {
            std::error_code ignored;
            idle_socket->close(ignored);
        }
    });
    asio::co_spawn(io, serve(), [&](std::exception_ptr failure) {
        peer_failure = failure;
        peer_finished = true;
        guard.cancel();
        peer_done.notify();
    });
    const std::string config_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(acceptor.local_endpoint().port())));
    auto config = local_http_client_config(config_url);
    config.connection_limits_.max_open_ = 1;
    config.idle_timeout_ = std::chrono::milliseconds(100);
    config.request_timeout_ = std::chrono::seconds(3);
    ruvia::http_client client(worker.attachment_.loop(), config);
    auto operation = [&]() -> ruvia::task<void> {
        const acceptor_closer closer{acceptor};
        for (const std::string_view expected : {std::string_view("first"), std::string_view("second")}) {
            auto response = co_await client.client().get("/idle").send();
            const auto bytes_value = co_await response.stream().read_all();
            EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(bytes_value.bytes().data()), bytes_value.size()), expected);
            while (!idle_closed) {
                co_await idle_closed_signal.wait();
            }
        }
        while (!peer_finished) {
            co_await peer_done.wait();
        }
        if (peer_failure) {
            std::rethrow_exception(peer_failure);
        }
        EXPECT_TRUE(idle_for < std::chrono::seconds(5));
        const auto stats = client.client().stats();
        EXPECT_EQ(stats.failed_requests_, std::size_t{0});
        EXPECT_EQ(stats.idle_connections_with_pending_data_, std::size_t{0});
        co_await client.shutdown();
    };
    run_operation(worker, io, operation);
    EXPECT_EQ(connections, 2U);
}

// Pingora's idle_poll: a pooled HTTP/1 connection closes as soon as its peer
// ends it or sends unsolicited bytes, even without idle_timeout_.
TEST(Ruvia, http1_client_closes_an_idle_connection_its_peer_ended_or_wrote_to) {
    for (const bool peer_writes : {false, true}) {
        using namespace http_client_pool_test;
        auto& io = ruvia::test::new_test_io_context();
        test_worker worker(io);
        scripted_origin origin(io, worker.handle_);
        std::optional<std::chrono::steady_clock::duration> closed_after;
        const auto script = [&]() -> asio::awaitable<void> {
            auto socket = co_await origin.acceptor().async_accept(asio::use_awaitable);
            origin.watch(socket.lowest_layer());
            asio::streambuf request;
            co_await asio::async_read_until(socket, request, "\r\n\r\n", asio::use_awaitable);
            constexpr std::string_view response = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nfirst";
            co_await asio::async_write(socket, asio::buffer(response), asio::use_awaitable);
            origin.advance();
            // Lets the client pool the connection first.
            asio::steady_timer pause(socket.get_executor(), std::chrono::milliseconds(50));
            co_await pause.async_wait(asio::use_awaitable);
            const auto acted = std::chrono::steady_clock::now();
            if (peer_writes) {
                co_await asio::async_write(socket, asio::buffer(std::string_view("X")), asio::use_awaitable);
            } else {
                socket.shutdown(asio::socket_base::shutdown_send);
            }
            std::array<char, 64> input{};
            std::error_code error;
            while (!error) {
                (void)co_await socket.async_read_some(asio::buffer(input), asio::redirect_error(asio::use_awaitable, error));
            }
            if (error != asio::error::operation_aborted && error != asio::error::bad_descriptor) {
                closed_after = std::chrono::steady_clock::now() - acted;
            }
        };
        origin.start(script());
        const std::string config_url = std::move(std::string("http://127.0.0.1:").append(std::to_string(origin.port())));
        auto config = local_http_client_config(config_url);
        config.connection_limits_.max_open_ = 1;
        config.idle_timeout_ = std::nullopt;
        config.request_timeout_ = std::chrono::seconds(3);
        ruvia::http_client client(worker.attachment_.loop(), config);
        ruvia::task_scope requests(worker.handle_);
        auto operation = [&]() -> ruvia::task<void> {
            co_await run_against(client, origin, requests, [&]() -> ruvia::task<void> {
                auto response = co_await client.client().get("/first").send();
                EXPECT_EQ(response_text(co_await response.stream().read_all()), "first");
                // The origin ends only once the client closed the connection;
                // its guard would close it after 10 seconds.
                co_await origin.join();
                EXPECT_TRUE(closed_after.has_value() && *closed_after < std::chrono::seconds(2));
                EXPECT_EQ(client.client().stats().idle_connections_with_pending_data_, peer_writes ? std::size_t{1} : std::size_t{0});
            });
        };
        run_operation(worker, io, operation);
    }
}
