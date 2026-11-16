#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <semaphore>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <asio/buffer.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read.hpp>
#include <asio/ssl/stream.hpp>
#include <asio/write.hpp>
#include <openssl/ssl.h>

#include "app.h"
#include "blocking_pool.h"
#include "context.h"
#include "controller.h"
#include "event_loop.h"
#include "http_response.h"
#include "http_server.h"
#include "http_server_fixture.h"
#include "request_stream.h"
#include "server_observation.h"
#include "streaming.h"
#include "task_scope.h"
#include "tls_config.h"
// An interim head sent from a handler first writes the pipelined responses
// held before it; over TLS that write always completes asynchronously. Once
// it and the interim head are out, the handler is timed as a handler again
// (idle_timeout), not as a write. A response-stream handler likewise: only
// its writes are timed as writes, and the wait between them is governed by
// idle_timeout. This executable owns its application, so its short write
// timeout cannot leak into other suites. A reader that keeps draining one
// buffered response slowly is still bounded by write_completion_timeout.

namespace interim_phase_timeout_test {

using namespace std::chrono_literals;

constexpr auto write_timeout = 100ms;
// Several write timeouts, far inside the default idle timeout.
constexpr auto handler_pause = 400ms;
// One stream write that takes many write timeouts to drain to a slow reader.
constexpr std::size_t bulk_bytes = std::size_t{8} * 1024 * 1024;
std::atomic<std::chrono::milliseconds::rep> bulk_write_duration_ms{0};

class interim_phase_routes final {
    RUVIA_ROUTES_BEGIN(interim_phase_routes)
    RUVIA_GET("/interim-phase/item", item);
    RUVIA_GET("/interim-phase/hinted", hinted);
    RUVIA_GET("/interim-phase/stream", stream);
    RUVIA_GET("/interim-phase/bulk", bulk);
    RUVIA_ROUTES_END

    ruvia::task<ruvia::http_response> item(ruvia::context& c) {
        co_return c.text("item");
    }

    ruvia::task<ruvia::http_response> hinted(ruvia::context& c) {
        co_await c.inform(ruvia::http_interim_response_head(ruvia::http_status::early_hints));
        (void)co_await ruvia::sleep_for(c.worker(), handler_pause);
        co_return c.text("hinted");
    }

    ruvia::task<void> stream(ruvia::context& c) {
        if (co_await c.stream().commit() == ruvia::stream_commit_result::completed) {
            co_return;
        }
        co_await c.stream().write("first-chunk");
        (void)co_await ruvia::sleep_for(c.worker(), handler_pause);
        co_await c.stream().write("second-chunk");
    }

    ruvia::task<void> bulk(ruvia::context& c) {
        const std::string bytes(bulk_bytes, 'b');
        const auto started = std::chrono::steady_clock::now();
        if (co_await c.stream().commit() == ruvia::stream_commit_result::completed) {
            co_return;
        }
        co_await c.stream().write(bytes);
        bulk_write_duration_ms.store(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count(), std::memory_order_relaxed);
    }
};

constexpr int completion_socket_buffer = 16 * 1024;
constexpr std::size_t completion_body_bytes = std::size_t{16} * 1024 * 1024;
constexpr auto write_completion_timeout = 300ms;
// Inactivity limit far beyond the whole exchange, so it cannot be what ends it.
constexpr auto patient_write_timeout = 20s;
// The reader trickles for several completion budgets before draining.
constexpr auto trickle_period = 1500ms;

ruvia::task<void> serve_buffered_response(ruvia::request_stream& stream) {
    ruvia::http_response response;
    response.owned_body(std::string(completion_body_bytes, 'w'));
    stream.respond(std::move(response));
    co_return;
}

ruvia::task<void> serve_slow_reader(ruvia::event_loop loop, asio::ip::tcp::socket accepted, std::uint16_t port, ruvia::tls_config tls, std::optional<ruvia::connection_close_reason>& close_reason) {
    ruvia::http_server server(loop, {.request_stream_ = &serve_buffered_response, .connection_close_ = [&close_reason](const ruvia::connection_event&, const std::shared_ptr<void>&, const ruvia::connection_close_info& info) noexcept {
        close_reason = info.reason_;
    }},
        {.connection_scan_interval_ = 5ms, .write_timeout_ = patient_write_timeout, .write_completion_timeout_ = write_completion_timeout});
    co_await server.add_connection_service({.address_ = "127.0.0.1", .https_ = port, .tls_ = tls, .dispatch_ = ruvia::listener_dispatch::request_stream});
    co_await server.start();
    const ruvia::listener_endpoint endpoint{.address_ = "127.0.0.1", .port_ = port};
    server.adopt(endpoint, accepted.release());
    co_await server.wait_connections_closed(endpoint, {.timeout_ = patient_write_timeout});
    co_await server.shutdown();
}

// Sends `requests` in one TLS record, then reads until the server closes.
// A paced read drains a full 64 KiB before pausing: sleeping after each 16 KiB
// TLS record can leave a raw socket write pending for over 100 ms while the
// kernel drains its large send queue, which legitimately expires write_timeout.
// The receive window bounds read-ahead; it does not bound the server send queue.
std::string exchange_tls(asio::io_context& io, const asio::ip::tcp::endpoint& endpoint, std::string_view requests, std::chrono::milliseconds read_pause = 0ms, std::chrono::milliseconds initial_read_pause = 0ms) {
    asio::ssl::context client(asio::ssl::context::tls_client);
    client.set_verify_mode(asio::ssl::verify_none);
    asio::ssl::stream<asio::ip::tcp::socket> stream(io, client);
    asio::error_code error;
    stream.next_layer().open(endpoint.protocol());
    stream.next_layer().set_option(asio::socket_base::receive_buffer_size(64 * 1024));
    stream.next_layer().connect(endpoint, error);
    if (!error) {
        stream.handshake(asio::ssl::stream_base::client, error);
    }
    if (!error) {
        asio::write(stream, asio::buffer(requests), error);
    }
    std::string response{};
    std::array<char, 64 * 1024> buffer;
    std::this_thread::sleep_for(initial_read_pause);
    while (!error) {
        const auto size = read_pause == 0ms ? stream.read_some(asio::buffer(buffer), error) : asio::read(stream, asio::buffer(buffer), error);
        response.append(buffer.data(), size);
        std::this_thread::sleep_for(read_pause);
    }
    return response;
}

}  // namespace interim_phase_timeout_test

using namespace interim_phase_timeout_test;

TEST(Ruvia, a_handler_after_its_interim_head_is_timed_as_a_handler_not_a_write) {
    ruvia::test::tls_identity identity("localhost");
    const auto private_key = identity.private_key_file();

    asio::io_context io;
    ruvia::test::tcp_port_reservation reservation(io);
    const auto endpoint = reservation.endpoint();

    auto& app = ruvia::app();
    static std::binary_semaphore started{0};
    // Error handling runs while the response is prepared; it is handler work,
    // timed by idle_timeout, not by write_timeout.
    app.on_error([](ruvia::context& c, ruvia::http_error_info error) -> ruvia::task<ruvia::http_response> {
        (void)co_await ruvia::sleep_for(c.worker(), handler_pause);
        co_return c.text("refused", {.status_ = error.status()});
    });
    app.workers({.worker_count_ = 1}).server({.connection_scan_interval_ = 5ms, .write_timeout_ = write_timeout}).listen({
                                                                                                                             .address_ = "127.0.0.1",
                                                                                                                             .https_ = endpoint.port(),
                                                                                                                             .tls_ = {.identities_ = ruvia::tls_fixed_identities{.primary_ = {.certificate_chain_file_ = identity.ca_file_, .private_key_file_ = private_key}}},
                                                                                                                         })
        .on_start([] {
        started.release();
    });
    ruvia::test::app_run runner(app);
    const bool ready = started.try_acquire_for(10s);
    EXPECT_TRUE(ready);
    if (ready) {
        // The first response is held behind the second request, so the 103
        // has to flush it first.
        const auto output = exchange_tls(io, endpoint,
            "GET /interim-phase/item HTTP/1.1\r\nHost: localhost\r\n\r\n"
            "GET /interim-phase/hinted HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
        const auto item = output.find("item");
        const auto interim = output.find("HTTP/1.1 103 ");
        EXPECT_TRUE(item != std::string::npos && interim != std::string::npos && item < interim);
        EXPECT_TRUE(output.ends_with("hinted"));

        const auto streamed = exchange_tls(io, endpoint, "GET /interim-phase/stream HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
        EXPECT_TRUE(streamed.find("first-chunk") != std::string::npos);
        EXPECT_TRUE(streamed.find("second-chunk") != std::string::npos);
        EXPECT_TRUE(streamed.ends_with("0\r\n\r\n"));

        // A steadily draining single write is not inactivity. Paced batches
        // keep each raw write below the timeout; the handler measurement proves
        // that the whole write still spans more than one inactivity budget.
        const auto bulk = exchange_tls(io, endpoint, "GET /interim-phase/bulk HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n", 2ms);
        EXPECT_TRUE(bulk.size() > bulk_bytes);
        EXPECT_TRUE(bulk.ends_with("0\r\n\r\n"));
        EXPECT_TRUE(bulk_write_duration_ms.load(std::memory_order_relaxed) > write_timeout.count());

        // The same response to a peer that accepts no data exhausts the send
        // queue and must time out, rather than treating a pending write as
        // progress. Only resume reading after several inactivity budgets.
        const auto stalled = exchange_tls(io, endpoint, "GET /interim-phase/bulk HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n", 0ms, handler_pause);
        EXPECT_TRUE(stalled.starts_with("HTTP/1.1 200 "));
        EXPECT_TRUE(stalled.find("\r\n\r\n") != std::string::npos);
        EXPECT_TRUE(stalled.size() < bulk_bytes);
        EXPECT_TRUE(!stalled.ends_with("0\r\n\r\n"));

        // No acceptable coding: the 406 recovery runs on_error after the route.
        const auto refused = exchange_tls(io, endpoint,
            "GET /interim-phase/item HTTP/1.1\r\nHost: localhost\r\n"
            "Accept-Encoding: identity;q=0, *;q=0\r\nConnection: close\r\n\r\n");
        EXPECT_TRUE(refused.starts_with("HTTP/1.1 406 "));
        EXPECT_TRUE(refused.ends_with("refused"));
    }
    app.request_stop();
    runner.join();
    EXPECT_TRUE(runner.failure() == nullptr);
}

// A peer that keeps reading, so write_timeout never lapses, still cannot hold
// one buffered response write open: write_completion_timeout runs from the
// start of the write and progress does not renew it.
TEST(Ruvia, a_trickling_reader_cannot_hold_a_write_past_write_completion_timeout) {
    EXPECT_THROW(ruvia::app().server({.write_completion_timeout_ = 0ms}), std::invalid_argument);

    const ruvia::test::tls_identity identity("localhost");
    const auto private_key = identity.private_key_file();
    asio::io_context io;
    asio::ip::tcp::acceptor acceptor(io, {asio::ip::address_v4::loopback(), 0});
    const auto endpoint = acceptor.local_endpoint();
    asio::ssl::context tls(asio::ssl::context::tls_client);
    asio::ssl::stream<asio::ip::tcp::socket> client(io, tls);
    client.next_layer().open(endpoint.protocol());
    client.next_layer().set_option(asio::socket_base::receive_buffer_size(completion_socket_buffer));
    client.next_layer().connect(endpoint);
    auto accepted = acceptor.accept();
    // Bound queued TLS records so the slow reader leaves the response write pending.
    accepted.set_option(asio::socket_base::send_buffer_size(completion_socket_buffer));

    std::optional<ruvia::connection_close_reason> close_reason;
    ruvia::blocking_pool preparation({.thread_count_ = 1});
    ruvia::event_loop_pool loops({.loop_count_ = 1, .blocking_pool_ = &preparation});
    const auto loop = loops.loop(0);
    loops.start();
    auto server = loop.start(serve_slow_reader(loop, std::move(accepted), endpoint.port(), {.identities_ = ruvia::tls_fixed_identities{.primary_ = {.certificate_chain_file_ = identity.ca_file_, .private_key_file_ = private_key}}}, close_reason));
    std::string response;
    std::array<char, 4096> buffer;
    asio::error_code error;
    client.handshake(asio::ssl::stream_base::client, error);
    EXPECT_FALSE(error);
    const auto requested = std::chrono::steady_clock::now();
    if (!error) {
        asio::write(client, asio::buffer(std::string_view("GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n")), error);
        EXPECT_FALSE(error);
    }
    while (!error) {
        response.append(buffer.data(), client.read_some(asio::buffer(buffer), error));
        if (std::chrono::steady_clock::now() - requested < trickle_period) {
            std::this_thread::sleep_for(10ms);
        }
    }
    const auto elapsed = std::chrono::steady_clock::now() - requested;
    client.next_layer().close(error);
    std::move(server).get();
    loops.join();

    EXPECT_EQ(close_reason, ruvia::connection_close_reason::timeout);
    ASSERT_TRUE(response.starts_with("HTTP/1.1 200 "));
    const auto head_end = response.find("\r\n\r\n");
    ASSERT_NE(head_end, std::string::npos);
    const auto body_bytes = response.size() - (head_end + 4);
    EXPECT_GT(body_bytes, 0U);
    EXPECT_LT(body_bytes, completion_body_bytes);
    EXPECT_LT(elapsed, patient_write_timeout);
}
