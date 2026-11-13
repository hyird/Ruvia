#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <exception>
#include <semaphore>
#include <string_view>
#include <system_error>
#include <thread>

#include <asio/buffer.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/write.hpp>

#include "ruvia/core/event_loop.h"
#include "ruvia/web/app.h"
#include "ruvia/web/context.h"

#include "http_server_fixture.h"
// A peer that resets its connection while a buffered handler is still running
// makes that handler's own transport write -- an interim head, or the 100
// Continue its body read sends -- fail. That is the connection ending, not the
// handler failing: the error handler does not see it, nothing is counted as a
// connection failure, and no 500 is attempted. A connection this server closes
// itself (here: idle_timeout during a stream handler's pause) ends just as
// normally for the handler's next write. This executable owns its application
// because the error handler is registered application-wide.

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <initializer_list>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <asio/read.hpp>
#include <asio/ssl/error.hpp>
#include <asio/ssl/stream.hpp>
#include <openssl/ssl.h>

#include "ruvia/web/middleware.h"

#if defined(RUVIA_ENABLE_HTTP2)
#include "ruvia/http/http2_connection.h"
#endif
#include <mutex>
#include <set>

#include <asio/ssl/context.hpp>

#if defined(RUVIA_ENABLE_HTTP3)
#include "ruvia/http/http3_connection.h"
#endif
#include "ruvia/http/http_request.h"

#if defined(RUVIA_ENABLE_HTTP2)
#include "http_server_fixture.h"
#endif
#if defined(RUVIA_ENABLE_HTTP2)

#include "ruvia/http/http2_connection.h"

#include "http_server_fixture.h"
#endif
#if defined(RUVIA_ENABLE_HTTP3)
#include <asio/ip/udp.hpp>

#include "http_server_fixture.h"
#endif

#include <algorithm>
#include <cctype>
#include <iostream>
#include <limits>
#include <new>
#include <variant>

#include <asio/bind_cancellation_slot.hpp>
#include <asio/cancellation_signal.hpp>
#include <asio/read_until.hpp>
#include <asio/socket_base.hpp>

#include "ruvia/http/http_response.h"
#include "ruvia/web/http_client.h"
#if defined(RUVIA_ENABLE_HTTP2)
#include "ruvia/http/http2_connection.h"
#endif
#if defined(RUVIA_ENABLE_HTTP2)
#include "http_server_fixture.h"
#endif
#include <asio/co_spawn.hpp>
#include <asio/executor_work_guard.hpp>
#include <asio/post.hpp>
#include <asio/use_awaitable.hpp>

#if defined(RUVIA_ENABLE_REDIS)
#include "ruvia/web/redis.h"
#endif
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <semaphore>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

#include <asio/buffer.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read.hpp>
#include <asio/write.hpp>

#include "ruvia/core/event_loop.h"
#include "ruvia/web/app.h"
#include "ruvia/web/context.h"
#include "ruvia/web/middleware.h"

#include "http_server_fixture.h"
namespace peer_reset_test {

using namespace std::chrono_literals;

// Far above the reset cases' handler waits, so it never closes those first.
constexpr auto idle_timeout = 1s;

std::atomic<bool> peer_closed{false};
std::atomic<std::size_t> error_handler_calls{0};
std::atomic<std::size_t> handlers_finished{0};
std::atomic<bool> head_write_failed{false};
std::atomic<bool> head_write_aborted{false};

ruvia::task<void> wait_for_peer_close(ruvia::context& c) {
    while (!peer_closed.load()) {
        (void)co_await ruvia::sleep_for(c.worker(), 10ms);
    }
    // The reset has reached this side by now; the next transport write
    // observes it.
    (void)co_await ruvia::sleep_for(c.worker(), 50ms);
}

class peer_reset_routes final {
    RUVIA_ROUTES_BEGIN(peer_reset_routes)
    RUVIA_GET("/peer-reset/inform", inform);
    RUVIA_POST("/peer-reset/body", body);
    RUVIA_GET("/peer-reset/idle-stream", idle_stream);
    RUVIA_GET("/peer-reset/head-only", head_only);
    RUVIA_ROUTES_END

    ruvia::task<ruvia::http_response> inform(ruvia::context& c) {
        co_await wait_for_peer_close(c);
        for (int attempt = 0; attempt < 4; ++attempt) {
            co_await c.inform(ruvia::http_interim_response_head(ruvia::http_status::early_hints));
            (void)co_await ruvia::sleep_for(c.worker(), 20ms);
        }
        handlers_finished.fetch_add(1);
        co_return c.text("informed");
    }

    ruvia::task<ruvia::http_response> body(ruvia::context& c) {
        co_await wait_for_peer_close(c);
        // The client expected 100 Continue and sent no body: the lazy read
        // writes the interim response first.
        (void)co_await c.req().text();
        handlers_finished.fetch_add(1);
        co_return c.text("read");
    }

    // Pauses past idle_timeout, so the server closes the connection before the
    // second write starts.
    ruvia::task<void> idle_stream(ruvia::context& c) {
        if (co_await c.stream_text().commit() == ruvia::stream_commit_result::completed) {
            co_return;
        }
        co_await c.stream().write("first");
        (void)co_await ruvia::sleep_for(c.worker(), idle_timeout + 500ms);
        co_await c.stream().write("second");
        handlers_finished.fetch_add(1);
    }

    ruvia::task<void> head_only(ruvia::context& c) {
        c.draft().status(ruvia::http_status::no_content);
        // A local timeout guarantees a closed socket; a peer RST alone need
        // not make the first small write fail on every supported transport.
        (void)co_await ruvia::sleep_for(c.worker(), idle_timeout + 500ms);
        auto& writer = c.stream();
        try {
            co_await writer.end();
        } catch (const std::exception&) {
            head_write_failed.store(true);
            head_write_aborted.store(writer.aborted());
            throw;
        }
        handlers_finished.fetch_add(1);
    }
};

ruvia::task<ruvia::http_response> count_error(ruvia::context& c, ruvia::http_error_info error) {
    error_handler_calls.fetch_add(1);
    c.draft().status(error.status());
    co_return c.text("error");
}

// Sends `request`, then closes with a zero linger so the close is a reset.
void send_and_reset(asio::io_context& io, const asio::ip::tcp::endpoint& endpoint, std::string_view request) {
    asio::ip::tcp::socket socket(io);
    socket.connect(endpoint);
    asio::write(socket, asio::buffer(request));
    // Let the server read the request before the reset follows it.
    std::this_thread::sleep_for(100ms);
    socket.set_option(asio::socket_base::linger(true, 0));
    std::error_code ignored;
    socket.close(ignored);
}

void wait_until_idle(ruvia::application& app) {
    ruvia::test::wait_until([&app] { return app.stats().active_connections_ == 0; }, 10s);
}

// Reads until the server closes the connection.
void send_and_read_to_close(asio::io_context& io, const asio::ip::tcp::endpoint& endpoint, std::string_view request) {
    (void)ruvia::test::exchange(io, endpoint, request);
}

}  // namespace peer_reset_test

TEST(Ruvia, a_peer_reset_during_a_handlers_transport_write_is_a_normal_connection_end) {
    using namespace peer_reset_test;
    ruvia::test::reset_app_callbacks(ruvia::app());
    asio::io_context io;
    ruvia::test::tcp_port_reservation reservation(io);
    const auto endpoint = reservation.endpoint();

    auto& app = ruvia::app();
    static std::binary_semaphore started{0};
    app.server({.worker_count_ = 1, .idle_timeout_ = idle_timeout, .connection_scan_interval_ = 5ms})
        .listen({.address_ = "127.0.0.1", .http_ = endpoint.port()})
        .on_error(&count_error)
        .on_start([] { started.release(); });
    ruvia::test::app_run runner(app);
    const bool ready = started.try_acquire_for(10s);
    EXPECT_TRUE(ready);
    if (ready) {
        send_and_reset(io, endpoint, "GET /peer-reset/inform HTTP/1.1\r\nHost: localhost\r\n\r\n");
        peer_closed.store(true);
        wait_until_idle(app);
        EXPECT_EQ(app.stats().active_connections_, std::size_t{0});

        peer_closed.store(false);
        send_and_reset(io, endpoint,
            "POST /peer-reset/body HTTP/1.1\r\nHost: localhost\r\nContent-Length: 4\r\nExpect: 100-continue\r\n\r\n");
        peer_closed.store(true);
        wait_until_idle(app);
        EXPECT_EQ(app.stats().active_connections_, std::size_t{0});

        send_and_read_to_close(io, endpoint, "GET /peer-reset/idle-stream HTTP/1.1\r\nHost: localhost\r\n\r\n");
        wait_until_idle(app);
        EXPECT_EQ(app.stats().active_connections_, std::size_t{0});

        send_and_read_to_close(io, endpoint, "GET /peer-reset/head-only HTTP/1.1\r\nHost: localhost\r\n\r\n");
        wait_until_idle(app);
        EXPECT_EQ(app.stats().active_connections_, std::size_t{0});
        EXPECT_TRUE(head_write_failed.load());
        EXPECT_TRUE(head_write_aborted.load());

        EXPECT_EQ(error_handler_calls.load(), std::size_t{0});
        EXPECT_EQ(app.stats().connection_failures_, std::size_t{0});
        // The failed write ended each handler; none ran to its end.
        EXPECT_EQ(handlers_finished.load(), std::size_t{0});
    }
    app.stop();
    runner.join();
    EXPECT_TRUE(runner.failure() == nullptr);
}

// How the server ends connections it closes itself. A final response that
// closes the connection must reach a peer that is still sending (RFC 9112
// §9.6: lingering close instead of a reset). Over TLS, a close after an idle
// timeout, after an HTTP/2 GOAWAY, or while draining sends close_notify
// (RFC 9112 §9.8). This executable owns its application because of its short
// idle timeout.

namespace connection_close_test {

using namespace std::chrono_literals;

constexpr auto idle_timeout = 300ms;
constexpr std::size_t large_response_bytes = std::size_t{4} * 1024 * 1024;

class connection_close_routes final {
    RUVIA_ROUTES_BEGIN(connection_close_routes)
    RUVIA_GET("/close/item", item);
    RUVIA_POST("/close/large", large);
    RUVIA_GET_WS("/close/ws", ws_close);
    RUVIA_GET_WS("/close/ws-read", ws_read);
    RUVIA_ROUTES_END

    ruvia::task<ruvia::http_response> item(ruvia::context& c) {
        co_return c.text("item");
    }

    // Answers without reading the request content, which closes the connection.
    ruvia::task<ruvia::http_response> large(ruvia::context& c) {
        co_return c.text(std::string(large_response_bytes, 'L'));
    }

    ruvia::task<void> ws_close(ruvia::context& c) {
        (void)c;
        co_return;
    }

    ruvia::task<void> ws_read(ruvia::context& c) {
        (void)co_await c.websocket().read();
        co_return;
    }
};

using tls_stream = asio::ssl::stream<asio::ip::tcp::socket>;

using ruvia::test::read_to_end;

std::unique_ptr<tls_stream> connect_tls(asio::io_context& io, asio::ssl::context& client,
    const asio::ip::tcp::endpoint& endpoint) {
    auto stream = std::make_unique<tls_stream>(io, client);
    stream->next_layer().connect(endpoint);
    stream->handshake(asio::ssl::stream_base::client);
    return stream;
}

// The server answers a request whose content it does not read with a large
// response and closes. The peer keeps unread request bytes queued at the
// server and reads slowly through a small receive window, so most of the
// response is still in the server's send queue when the server closes: an
// immediate close would reset the connection and discard it.
void final_response_survives_unread_request_content(
    asio::io_context& io, const asio::ip::tcp::endpoint& endpoint) {
    asio::ip::tcp::socket socket(io);
    socket.open(endpoint.protocol());
    socket.set_option(asio::socket_base::receive_buffer_size(64 * 1024));
    socket.connect(endpoint);
    std::string request("POST /close/large HTTP/1.1\r\nHost: localhost\r\nContent-Length: 1048576\r\n\r\n");
    request.append(std::size_t{64} * 1024, 'u');
    asio::write(socket, asio::buffer(request));
    std::this_thread::sleep_for(200ms);
    asio::error_code end;
    const auto response = read_to_end(socket, end);
    const auto head_end = response.find("\r\n\r\n");
    EXPECT_TRUE(response.starts_with("HTTP/1.1 200 "));
    EXPECT_TRUE(head_end != std::string::npos);
    if (head_end != std::string::npos) {
        EXPECT_EQ(response.size() - head_end - 4, large_response_bytes);
    }
    EXPECT_TRUE(end == asio::error::eof);
}

// A keep-alive connection that idles past idle_timeout ends with close_notify.
void idle_tls_connection_ends_with_close_notify(
    asio::io_context& io, const asio::ip::tcp::endpoint& endpoint) {
    asio::ssl::context client(asio::ssl::context::tls_client);
    client.set_verify_mode(asio::ssl::verify_none);
    const auto stream = connect_tls(io, client, endpoint);
    // 服务端可在发送返回前开始空闲计时，所以先记录请求发送的起点。
    const auto started = std::chrono::steady_clock::now();
    asio::write(*stream, asio::buffer(std::string_view("GET /close/item HTTP/1.1\r\nHost: localhost\r\n\r\n")));
    asio::error_code end;
    const auto response = read_to_end(*stream, end);
    EXPECT_TRUE(response.starts_with("HTTP/1.1 200 "));
    EXPECT_TRUE(response.ends_with("item"));
    EXPECT_TRUE(std::chrono::steady_clock::now() - started >= idle_timeout);
    EXPECT_TRUE(end == asio::error::eof);
}

#if defined(RUVIA_ENABLE_HTTP2)
// An HTTP/2 connection that idles past idle_timeout first sends GOAWAY, then
// close_notify (RFC 9113 §9.1). The peer acknowledges SETTINGS and PING so a
// graceful drain can complete.
void idle_http2_connection_sends_goaway_then_close_notify(
    asio::io_context& io, const asio::ip::tcp::endpoint& endpoint) {
    asio::ssl::context client(asio::ssl::context::tls_client);
    client.set_verify_mode(asio::ssl::verify_none);
    static constexpr unsigned char alpn[] = {2, 'h', '2'};
    EXPECT_EQ(SSL_CTX_set_alpn_protos(client.native_handle(), alpn, sizeof(alpn)), 0);
    const auto stream = connect_tls(io, client, endpoint);
    std::string opening(ruvia::http2_client_preface);
    ruvia::test::append_frame(opening, ruvia::http2_frame_type::settings, 0, 0, {});
    asio::write(*stream, asio::buffer(opening));

    std::string received{};
    std::size_t parsed = 0;
    bool goaway = false;
    std::array<char, 16 * 1024> buffer;
    std::error_code end;
    while (!end) {
        const auto size = stream->read_some(asio::buffer(buffer), end);
        received.append(buffer.data(), size);
        while (received.size() - parsed >= 9) {
            const auto* frame = reinterpret_cast<const unsigned char*>(received.data() + parsed);
            const std::size_t length = (std::size_t{frame[0]} << 16) | (std::size_t{frame[1]} << 8) | frame[2];
            if (received.size() - parsed < 9 + length) {
                break;
            }
            const auto type = frame[3];
            const auto flags = frame[4];
            std::error_code ignored;
            if (type == 4 && (flags & 1) == 0) {
                constexpr std::array<char, 9> settings_ack{0, 0, 0, 4, 1, 0, 0, 0, 0};
                asio::write(*stream, asio::buffer(settings_ack), ignored);
            } else if (type == 6 && (flags & 1) == 0 && length == 8) {
                std::string ack(received, parsed, 9 + length);
                ack[4] = 1;
                asio::write(*stream, asio::buffer(ack), ignored);
            } else if (type == 7) {
                goaway = true;
            }
            parsed += 9 + length;
        }
    }
    EXPECT_TRUE(goaway);
    EXPECT_TRUE(end == asio::error::eof);
}
#endif

// Draining closes a connection that awaits its first request with close_notify.
std::unique_ptr<tls_stream> open_unused_tls_connection(
    asio::io_context& io, asio::ssl::context& client, const asio::ip::tcp::endpoint& endpoint) {
    auto stream = connect_tls(io, client, endpoint);
    // Let the server finish its side of the handshake.
    std::this_thread::sleep_for(50ms);
    return stream;
}

// RFC 9846 §6.1 / RFC 6455 §7.1.1: after a clean WebSocket close handshake
// over TLS (wss://), the server sends close_notify before closing TCP.
void wss_clean_close_handshake_sends_close_notify(
    asio::io_context& io, const asio::ip::tcp::endpoint& endpoint) {
    asio::ssl::context client(asio::ssl::context::tls_client);
    client.set_verify_mode(asio::ssl::verify_none);
    const auto stream = connect_tls(io, client, endpoint);
    constexpr std::string_view upgrade =
        "GET /close/ws HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\n\r\n";
    asio::write(*stream, asio::buffer(upgrade));

    std::string received{};
    std::array<char, 1024> buffer{};
    std::error_code error;
    while (received.find("\x88") == std::string::npos && !error) {
        const auto count = stream->read_some(asio::buffer(buffer), error);
        received.append(buffer.data(), count);
    }
    EXPECT_TRUE(!error);
    // Reply with a masked client Close frame (code 1000, zero mask).
    constexpr std::array<unsigned char, 8> client_close{0x88, 0x82, 0x00, 0x00, 0x00, 0x00, 0x03, 0xe8};
    asio::write(*stream, asio::buffer(client_close), error);
    EXPECT_TRUE(!error);
    while (!error) {
        const auto count = stream->read_some(asio::buffer(buffer), error);
        received.append(buffer.data(), count);
    }
    EXPECT_TRUE(error == asio::error::eof);
}

// RFC 9846 §6.1 / RFC 6455 §7.1.7: when a wss:// peer truncates TLS / drops TCP
// mid-frame without sending a WebSocket Close frame or close_notify, the server
// aborts the connection without sending a clean TLS close_notify.
void wss_truncated_peer_eof_omits_close_notify(
    asio::io_context& io, const asio::ip::tcp::endpoint& endpoint) {
    asio::ssl::context client(asio::ssl::context::tls_client);
    client.set_verify_mode(asio::ssl::verify_none);
    const auto stream = connect_tls(io, client, endpoint);
    constexpr std::string_view upgrade =
        "GET /close/ws-read HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\n\r\n";
    asio::write(*stream, asio::buffer(upgrade));

    std::string head{};
    std::array<char, 1024> buffer{};
    std::error_code error;
    while (head.find("\r\n\r\n") == std::string::npos && !error) {
        const auto count = stream->read_some(asio::buffer(buffer), error);
        head.append(buffer.data(), count);
    }
    EXPECT_TRUE(!error);
    constexpr std::array<char, 1> partial_ws_frame{'\x81'};
    asio::write(*stream, asio::buffer(partial_ws_frame), error);
    EXPECT_TRUE(!error);
    stream->next_layer().shutdown(asio::ip::tcp::socket::shutdown_send, error);
    EXPECT_TRUE(!error);

    std::string rest{};
    while (!error) {
        const auto count = stream->read_some(asio::buffer(buffer), error);
        rest.append(buffer.data(), count);
    }
    EXPECT_TRUE(rest.empty());
    EXPECT_TRUE(error == asio::ssl::error::stream_truncated);
}
}  // namespace connection_close_test

TEST(Ruvia, server_closes_connections_gracefully) {
    using namespace connection_close_test;
    ruvia::test::reset_app_callbacks(ruvia::app());
    ruvia::test::tls_identity identity("localhost");
    const auto private_key = identity.private_key_file();

    asio::io_context io;
    ruvia::test::tcp_port_reservation plain_reservation(io);
    ruvia::test::tcp_port_reservation tls_reservation(io);
    const auto plain_endpoint = plain_reservation.endpoint();
    const auto tls_endpoint = tls_reservation.endpoint();

    auto& app = ruvia::app();
    static std::binary_semaphore started{0};
    app.server({.worker_count_ = 1, .idle_timeout_ = idle_timeout, .connection_scan_interval_ = 5ms})
        .listen({
            .address_ = "127.0.0.1",
            .http_ = plain_endpoint.port(),
            .https_ = tls_endpoint.port(),
            .tls_ = {.identity_ = {.certificate_chain_file_ = identity.ca_file_, .private_key_file_ = private_key}},
        })
        .on_start([] { started.release(); });
    ruvia::test::app_run runner(app);
    const bool ready = started.try_acquire_for(10s);
    EXPECT_TRUE(ready);
    if (ready) {
        final_response_survives_unread_request_content(io, plain_endpoint);
        idle_tls_connection_ends_with_close_notify(io, tls_endpoint);
#if defined(RUVIA_ENABLE_HTTP2)
        idle_http2_connection_sends_goaway_then_close_notify(io, tls_endpoint);
#endif
        wss_clean_close_handshake_sends_close_notify(io, tls_endpoint);
        wss_truncated_peer_eof_omits_close_notify(io, tls_endpoint);

        // A stop disconnects directly: an unused TLS connection is closed
        // without close_notify, so the peer sees a truncated stream.
        asio::ssl::context client(asio::ssl::context::tls_client);
        client.set_verify_mode(asio::ssl::verify_none);
        const auto unused = open_unused_tls_connection(io, client, tls_endpoint);
        std::thread stopper([&app] { app.stop(); });
        asio::error_code end;
        const auto received = read_to_end(*unused, end);
        stopper.join();
        EXPECT_TRUE(received.empty());
        EXPECT_TRUE(end == asio::ssl::error::stream_truncated || end == asio::error::connection_reset);
    } else {
        app.stop();
    }
    runner.join();
}

// A cleartext listener with auto_https_redirect_ answers every request with a
// permanent redirect to the HTTPS service and closes. A client that accepts
// no content coding cannot take the redirect's representation, so it gets a
// 406 from the application's error handler, called once. This executable owns
// its application, so its listeners and error handler cannot leak into other
// suites.

namespace auto_https_redirect_test {

using namespace std::chrono_literals;

std::atomic<int> error_handler_calls{0};

ruvia::task<ruvia::http_response> counted_error(ruvia::context& c, ruvia::http_error_info error) {
    ++error_handler_calls;
    c.draft().status(error.status());
    co_return c.text("rejected");
}

using ruvia::test::exchange;

}  // namespace auto_https_redirect_test

TEST(Ruvia, auto_https_redirect_points_cleartext_requests_at_the_https_service) {
    using namespace auto_https_redirect_test;
    ruvia::test::reset_app_callbacks(ruvia::app());
    ruvia::test::tls_identity identity("localhost");
    const auto private_key = identity.private_key_file();

    asio::io_context io;
    ruvia::test::tcp_port_reservation http_reservation(io);
    ruvia::test::tcp_port_reservation https_reservation(io);
    const auto http_endpoint = http_reservation.endpoint();
    const auto https_endpoint = https_reservation.endpoint();

    auto& app = ruvia::app();
    static std::binary_semaphore started{0};
    app.on_error(&counted_error);
    app.server({.worker_count_ = 1, .connection_scan_interval_ = 5ms})
        .listen({
            .address_ = "127.0.0.1",
            .http_ = http_endpoint.port(),
            .https_ = https_endpoint.port(),
            .tls_ = {.identity_ = {.certificate_chain_file_ = identity.ca_file_, .private_key_file_ = private_key}},
            .auto_https_redirect_ = true,
        })
        .on_start([] { started.release(); });
    ruvia::test::app_run runner(app);
    const bool ready = started.try_acquire_for(10s);
    EXPECT_TRUE(ready);
    if (ready) {
        const auto https_port = std::string(std::to_string(https_endpoint.port()));

        const auto redirected = exchange(io, http_endpoint,
            "GET /docs/page?lang=en HTTP/1.1\r\nHost: example.test:8080\r\n\r\n");
        EXPECT_TRUE(redirected.starts_with("HTTP/1.1 308 "));
        EXPECT_TRUE(redirected.find(std::string("\r\nLocation: https://example.test:") + https_port + "/docs/page?lang=en\r\n") !=
                    std::string::npos);

        // RFC 9112 §3.2.2: an absolute-form target names the host, so an
        // HTTP/1.0 request needs no Host field to be redirected.
        const auto absolute = exchange(io, http_endpoint, "GET http://example.com/x?y HTTP/1.0\r\n\r\n");
        EXPECT_TRUE(absolute.starts_with("HTTP/1.1 308 "));
        EXPECT_TRUE(absolute.find(std::string("\r\nLocation: https://example.com:") + https_port + "/x?y\r\n") != std::string::npos);

        error_handler_calls = 0;
        const auto refused = exchange(io, http_endpoint,
            "GET /docs/page HTTP/1.1\r\nHost: example.test\r\nAccept-Encoding: identity;q=0, *;q=0\r\n\r\n");
        EXPECT_TRUE(refused.starts_with("HTTP/1.1 406 "));
        EXPECT_TRUE(refused.find("Location:") == std::string::npos);
        EXPECT_TRUE(refused.ends_with("rejected"));
        EXPECT_EQ(error_handler_calls.load(), 1);
    }
    app.stop();
    runner.join();
}

// An interim head sent from a handler first writes the pipelined responses
// held before it; over TLS that write always completes asynchronously. Once
// it and the interim head are out, the handler is timed as a handler again
// (idle_timeout), not as a write. A response-stream handler likewise: only
// its writes are timed as writes, and the wait between them is governed by
// idle_timeout. This executable owns its application, so its short write
// timeout cannot leak into other suites.

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
        bulk_write_duration_ms.store(
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count(),
            std::memory_order_relaxed);
    }
};

// Sends `requests` in one TLS record, then reads until the server closes.
// A paced read drains a full 64 KiB before pausing: sleeping after each 16 KiB
// TLS record can leave a raw socket write pending for over 100 ms while the
// kernel drains its large send queue, which legitimately expires write_timeout.
// The receive window bounds read-ahead; it does not bound the server send queue.
std::string exchange_tls(asio::io_context& io, const asio::ip::tcp::endpoint& endpoint, std::string_view requests,
    std::chrono::milliseconds read_pause = 0ms, std::chrono::milliseconds initial_read_pause = 0ms) {
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
        const auto size = read_pause == 0ms ? stream.read_some(asio::buffer(buffer), error)
                                            : asio::read(stream, asio::buffer(buffer), error);
        response.append(buffer.data(), size);
        std::this_thread::sleep_for(read_pause);
    }
    return response;
}

}  // namespace interim_phase_timeout_test

TEST(Ruvia, a_handler_after_its_interim_head_is_timed_as_a_handler_not_a_write) {
    using namespace interim_phase_timeout_test;
    ruvia::test::reset_app_callbacks(ruvia::app());
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
        c.draft().status(error.status());
        co_return c.text("refused");
    });
    app.server({.worker_count_ = 1, .connection_scan_interval_ = 5ms, .write_timeout_ = write_timeout})
        .listen({
            .address_ = "127.0.0.1",
            .https_ = endpoint.port(),
            .tls_ = {.identity_ = {.certificate_chain_file_ = identity.ca_file_, .private_key_file_ = private_key}},
        })
        .on_start([] { started.release(); });
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

        const auto streamed = exchange_tls(io, endpoint,
            "GET /interim-phase/stream HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
        EXPECT_TRUE(streamed.find("first-chunk") != std::string::npos);
        EXPECT_TRUE(streamed.find("second-chunk") != std::string::npos);
        EXPECT_TRUE(streamed.ends_with("0\r\n\r\n"));

        // A steadily draining single write is not inactivity. Paced batches
        // keep each raw write below the timeout; the handler measurement proves
        // that the whole write still spans more than one inactivity budget.
        const auto bulk = exchange_tls(io, endpoint,
            "GET /interim-phase/bulk HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n", 2ms);
        EXPECT_TRUE(bulk.size() > bulk_bytes);
        EXPECT_TRUE(bulk.ends_with("0\r\n\r\n"));
        EXPECT_TRUE(bulk_write_duration_ms.load(std::memory_order_relaxed) > write_timeout.count());

        // The same response to a peer that accepts no data exhausts the send
        // queue and must time out, rather than treating a pending write as
        // progress. Only resume reading after several inactivity budgets.
        const auto stalled = exchange_tls(io, endpoint,
            "GET /interim-phase/bulk HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n", 0ms, handler_pause);
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
    app.stop();
    runner.join();
    EXPECT_TRUE(runner.failure() == nullptr);
}

// A file body that fails before its head is sent is answered with an error
// response, and only real faults reach the connection failure channel. The
// open a file response waits on, and every read after it, is bounded by
// write_timeout_: a pool that cannot run it (here: its single thread held by
// another request) does not pin the request. Without a blocking pool a file
// response -- context::file or a standalone static_root -- is a reported 503:
// nothing opens the file on the worker. This executable owns its application
// because the write timeout, the blocking pool and the failure hook are
// application-wide.

namespace file_response_failure_test {

using namespace std::chrono_literals;

constexpr auto write_timeout = 300ms;

[[nodiscard]] const std::filesystem::path& failure_dir() {
    static const ruvia::test::scoped_temp_directory directory("ruvia-file-response-failure");
    return directory.path();
}

void write_file(const std::filesystem::path& path, std::string_view bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

std::atomic<bool> pool_held{false};
std::atomic<bool> release_pool{false};
std::atomic<std::size_t> reported_failures{0};
std::atomic<std::size_t> reported_timeouts{0};
std::atomic<std::size_t> reported_missing_pools{0};
std::unique_ptr<ruvia::static_root> static_files;

class file_failure_routes final {
    RUVIA_ROUTES_BEGIN(file_failure_routes)
    RUVIA_GET("/file-failure/hold-pool", hold_pool);
    RUVIA_GET("/file-failure/file", file);
    RUVIA_GET("/file-failure/static", static_file);
    RUVIA_GET("/file-failure/changed", changed);
    RUVIA_ROUTES_END

    // Occupies the pool's only thread until the test releases it (bounded).
    ruvia::task<ruvia::http_response> hold_pool(ruvia::context& c) {
        auto held = co_await c.run_blocking([] {
            pool_held.store(true);
            for (int wait = 0; wait < 10000 && !release_pool.load(); ++wait) {
                std::this_thread::sleep_for(1ms);
            }
            return 0;
        });
        static_cast<void>(std::move(held).take_value());
        co_return c.text("released");
    }

    ruvia::task<ruvia::http_response> file(ruvia::context& c) {
        const auto path = failure_dir() / "body.bin";
        co_return co_await c.file({.path_ = path});
    }

    ruvia::task<ruvia::http_response> static_file(ruvia::context& c) {
        co_return co_await c.static_file(*static_files, {.relative_path_ = "asset.txt"});
    }

    // Framed from the file's snapshot, then truncated before the transport
    // reads it.
    ruvia::task<ruvia::http_response> changed(ruvia::context& c) {
        const auto path = failure_dir() / "changed.bin";
        write_file(path, std::string(4096, 'c'));
        auto response = co_await c.file({.path_ = path});
        write_file(path, "x");
        co_return response;
    }
};

void record_failure(const ruvia::connection_failure_record& record) noexcept {
    reported_failures.fetch_add(1);
    try {
        std::rethrow_exception(record.exception());
    } catch (const std::system_error& error) {
        if (error.code() == std::errc::timed_out) {
            reported_timeouts.fetch_add(1);
        } else if (error.code() == std::errc::operation_not_supported) {
            reported_missing_pools.fetch_add(1);
        }
    } catch (...) {
    }
}

std::string get(std::string_view target) {
    std::string request("GET ");
    request.append(target);
    request.append(" HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
    return request;
}

// Reads until the server closes the connection; nullopt when that takes
// longer than `limit` (the socket is then shut down to end the read).
[[nodiscard]] std::optional<std::string> read_to_close(asio::ip::tcp::socket& socket, std::chrono::milliseconds limit) {
    auto reader = std::async(std::launch::async, [&socket] {
        std::error_code error;
        return ruvia::test::read_to_end(socket, error);
    });
    if (reader.wait_for(limit) != std::future_status::ready) {
        std::error_code ignored;
        socket.shutdown(asio::ip::tcp::socket::shutdown_both, ignored);
        static_cast<void>(reader.get());
        return std::nullopt;
    }
    return reader.get();
}

}  // namespace file_response_failure_test

TEST(Ruvia, file_failures_before_the_head_get_an_error_response_and_stalled_opens_time_out) {
    using namespace file_response_failure_test;
    ruvia::test::reset_app_callbacks(ruvia::app());
    std::filesystem::create_directories(failure_dir() / "static");
    write_file(failure_dir() / "body.bin", std::string(4096, 'b'));
    write_file(failure_dir() / "static" / "asset.txt", std::string(4096, 's'));
    static_files = std::make_unique<ruvia::static_root>(failure_dir() / "static");

    asio::io_context io;
    ruvia::test::tcp_port_reservation reservation(io);
    const auto endpoint = reservation.endpoint();

    auto& app = ruvia::app();
    static std::binary_semaphore started{0};
    app.server({.worker_count_ = 1, .connection_scan_interval_ = 5ms, .write_timeout_ = write_timeout})
        .blocking_pool({.thread_count_ = 1, .queue_capacity_ = 4})
        .listen({.address_ = "127.0.0.1", .http_ = endpoint.port()})
        .on_connection_failure(&record_failure)
        .on_start([] { started.release(); });
    ruvia::test::app_run runner(app);
    const bool ready = started.try_acquire_for(10s);
    EXPECT_TRUE(ready);
    if (ready) {
        // A file changed after framing is an expected deployment race: the
        // client gets a 500 that closes the connection, and nothing is
        // reported.
        {
            asio::ip::tcp::socket socket(io);
            socket.connect(endpoint);
            asio::write(socket, asio::buffer(get("/file-failure/changed")));
            const auto wire = read_to_close(socket, 5s);
            EXPECT_TRUE(wire.has_value());
            if (wire.has_value()) {
                EXPECT_TRUE(wire->starts_with("HTTP/1.1 500 "));
                EXPECT_TRUE(wire->find(std::string(64, 'c')) == std::string::npos);
            }
            EXPECT_EQ(reported_failures.load(), std::size_t{0});
        }

        // The pool's only thread is held, so the file's open queues behind it.
        // The handler's wait still ends within write_timeout_ with a 503
        // before any head, and the stalled open is reported as a fault.
        asio::ip::tcp::socket holder(io);
        holder.connect(endpoint);
        asio::write(holder, asio::buffer(get("/file-failure/hold-pool")));
        ruvia::test::wait_until([] { return pool_held.load(); }, 5s);
        for (const std::string_view target : {"/file-failure/file", "/file-failure/static"}) {
            const auto timeouts_before = reported_timeouts.load();
            asio::ip::tcp::socket socket(io);
            socket.connect(endpoint);
            asio::write(socket, asio::buffer(get(target)));
            const auto wire = read_to_close(socket, 5s);
            EXPECT_TRUE(wire.has_value());
            if (wire.has_value()) {
                EXPECT_TRUE(wire->starts_with("HTTP/1.1 503 "));
                EXPECT_TRUE(wire->find("file_read_unavailable") != std::string::npos);
            }
            ruvia::test::wait_until([timeouts_before] { return reported_timeouts.load() > timeouts_before; }, 5s);
        }
        release_pool.store(true);
        const auto released = read_to_close(holder, 5s);
        EXPECT_TRUE(released.has_value() && released->starts_with("HTTP/1.1 200 "));

        // With the pool free again the same files are served.
        {
            asio::ip::tcp::socket socket(io);
            socket.connect(endpoint);
            asio::write(socket, asio::buffer(get("/file-failure/file")));
            const auto wire = read_to_close(socket, 5s);
            EXPECT_TRUE(wire.has_value() && wire->starts_with("HTTP/1.1 200 ") &&
                        wire->ends_with(std::string(4096, 'b')));
        }
        {
            asio::ip::tcp::socket socket(io);
            socket.connect(endpoint);
            asio::write(socket, asio::buffer(get("/file-failure/static")));
            const auto wire = read_to_close(socket, 5s);
            EXPECT_TRUE(wire.has_value() && wire->starts_with("HTTP/1.1 200 ") &&
                        wire->ends_with(std::string(4096, 's')));
        }
        ruvia::test::wait_until([&app] { return app.stats().active_connections_ == 0; }, 5s);
    }
    release_pool.store(true);
    app.stop();
    runner.join();
    EXPECT_TRUE(runner.failure() == nullptr);

    // Without a blocking pool nothing opens a file on the worker, on any
    // platform: context::file and a standalone static_root answer 503 before
    // any head, and each missing pool is reported as a fault.
    app.blocking_pool(nullptr);
    ruvia::test::app_run pool_less_runner(app);
    const bool pool_less_ready = started.try_acquire_for(10s);
    EXPECT_TRUE(pool_less_ready);
    if (pool_less_ready) {
        for (const std::string_view target : {"/file-failure/file", "/file-failure/static"}) {
            const auto missing_before = reported_missing_pools.load();
            asio::ip::tcp::socket socket(io);
            socket.connect(endpoint);
            asio::write(socket, asio::buffer(get(target)));
            const auto wire = read_to_close(socket, 5s);
            EXPECT_TRUE(wire.has_value());
            if (wire.has_value()) {
                EXPECT_TRUE(wire->starts_with("HTTP/1.1 503 "));
                EXPECT_TRUE(wire->find("file_read_unavailable") != std::string::npos);
            }
            ruvia::test::wait_until([missing_before] { return reported_missing_pools.load() > missing_before; }, 5s);
        }
    }
    app.stop();
    pool_less_runner.join();
    EXPECT_TRUE(pool_less_runner.failure() == nullptr);
    app.blocking_pool(ruvia::blocking_pool_options{});
    static_files.reset();
}

// RFC 9110 §7.4 / §15.5.20: an origin server rejects with 421 a request for
// an "https" resource unless the connection is secured by a certificate valid
// for the target's origin, and unless the peer is a trusted gateway. The
// listener's certificate names localhost, *.wild.test and 127.0.0.1 in its
// subjectAltName (and only localhost in its subject CN). This executable owns
// its application because the second run trusts the loopback peer.

namespace misdirected_request_test {

using namespace std::chrono_literals;

class origin_controller final {
public:
    RUVIA_ROUTES_BEGIN(origin_controller)
    RUVIA_GET("/origin", origin);
    RUVIA_ROUTES_END

    ruvia::task<ruvia::http_response> origin(ruvia::context& context) {
        co_return context.text(std::string_view("served"));
    }
};

using ruvia::test::read_to_end;

// The status line of `request` on a fresh cleartext connection.
std::string http_exchange(asio::io_context& io, const asio::ip::tcp::endpoint& endpoint, std::string_view request) {
    asio::ip::tcp::socket socket(io);
    asio::error_code error;
    socket.connect(endpoint, error);
    if (!error) {
        asio::write(socket, asio::buffer(request), error);
    }
    const auto response = error ? std::string{} : read_to_end(socket, error);
    return std::string(std::string_view(response).substr(0, response.find("\r\n")));
}

// The status line of `request` on a fresh TLS connection offering `sni`
// (none when null). The client authenticates nothing: the server decides.
std::string https_exchange(
    asio::io_context& io, const asio::ip::tcp::endpoint& endpoint, const char* sni, std::string_view request) {
    asio::ssl::context client(asio::ssl::context::tls_client);
    client.set_verify_mode(asio::ssl::verify_none);
    asio::ssl::stream<asio::ip::tcp::socket> stream(io, client);
    if (sni != nullptr && SSL_set_tlsext_host_name(stream.native_handle(), sni) != 1) {
        return std::string{};
    }
    asio::error_code error;
    stream.next_layer().connect(endpoint, error);
    if (!error) {
        stream.handshake(asio::ssl::stream_base::client, error);
    }
    if (!error) {
        asio::write(stream, asio::buffer(request), error);
    }
    const auto response = error ? std::string{} : read_to_end(stream, error);
    return std::string(std::string_view(response).substr(0, response.find("\r\n")));
}

[[nodiscard]] std::string get(std::string_view target, std::string_view host) {
    return std::string("GET ") + std::string(target) + " HTTP/1.1\r\nHost: " + std::string(host) +
           "\r\nConnection: close\r\n\r\n";
}

#if defined(RUVIA_ENABLE_HTTP2)
struct http2_target final {
    std::string_view scheme_;
    std::string_view authority_;
};

// Sends every request on one HTTP/2 connection over `stream` and returns the
// response status of each, in order; 0 when a stream ended without one.
template <typename stream_type>
std::vector<unsigned> http2_statuses(stream_type& stream, std::initializer_list<http2_target> targets) {
    auto peer = ruvia::http2_connection::client();
    std::map<std::uint32_t, std::size_t> pending{};
    std::vector<unsigned> statuses(targets.size(), 0);
    for (std::size_t i = 0; i < targets.size(); ++i) {
        const auto submitted = peer.submit_request_head(ruvia::http2_regular_request_head_view{
            .scheme_ = (targets.begin() + i)->scheme_, .authority_ = (targets.begin() + i)->authority_, .target_ = "/origin"});
        const auto* id = std::get_if<std::uint32_t>(&submitted);
        if (id == nullptr) {
            throw std::runtime_error("HTTP/2 test request rejected locally");
        }
        pending.emplace(*id, i);
    }
    std::string output;
    std::array<char, 16384> input{};
    while (!pending.empty()) {
        while (peer.wants_write()) {
            output.clear();
            (void)peer.take_output_batch(16384, output);
            asio::write(stream, asio::buffer(output));
        }
        asio::error_code error;
        const auto size = stream.read_some(asio::buffer(input), error);
        if (error || peer.feed(std::string_view(input.data(), size)) == ruvia::http2_feed_result::protocol_failure) {
            break;
        }
        while (auto event = peer.next_event()) {
            if (const auto* head = event->response_head()) {
                if (const auto found = pending.find(head->stream_id()); found != pending.end()) {
                    statuses[found->second] = head->head().status().value();
                    pending.erase(found);
                }
            }
        }
        if (peer.connection_error()) {
            break;
        }
    }
    return statuses;
}

std::vector<unsigned> h2_over_tls(
    asio::io_context& io, const asio::ip::tcp::endpoint& endpoint, std::initializer_list<http2_target> targets) {
    asio::ssl::context client(asio::ssl::context::tls_client);
    client.set_verify_mode(asio::ssl::verify_none);
    static constexpr unsigned char alpn[] = {2, 'h', '2'};
    if (SSL_CTX_set_alpn_protos(client.native_handle(), alpn, sizeof(alpn)) != 0) {
        throw std::runtime_error("cannot offer h2");
    }
    asio::ssl::stream<asio::ip::tcp::socket> stream(io, client);
    if (SSL_set_tlsext_host_name(stream.native_handle(), "localhost") != 1) {
        throw std::runtime_error("cannot offer SNI");
    }
    stream.next_layer().connect(endpoint);
    stream.handshake(asio::ssl::stream_base::client);
    return http2_statuses(stream, targets);
}

std::vector<unsigned> h2_cleartext(
    asio::io_context& io, const asio::ip::tcp::endpoint& endpoint, std::initializer_list<http2_target> targets) {
    asio::ip::tcp::socket socket(io);
    socket.connect(endpoint);
    return http2_statuses(socket, targets);
}
#endif

}  // namespace misdirected_request_test

TEST(Ruvia, requests_the_connection_cannot_carry_are_answered_421) {
    using namespace misdirected_request_test;
    ruvia::test::reset_app_callbacks(ruvia::app());
    ruvia::test::tls_identity identity("localhost", "DNS:localhost,DNS:*.wild.test,IP:127.0.0.1");
    const auto private_key = identity.private_key_file();

    asio::io_context io;
    ruvia::test::tcp_port_reservation http_reservation(io);
    ruvia::test::tcp_port_reservation https_reservation(io);
    const auto http_endpoint = http_reservation.endpoint();
    const auto https_endpoint = https_reservation.endpoint();

    auto& app = ruvia::app();
    static std::binary_semaphore started{0};
    app.server({.worker_count_ = 1, .connection_scan_interval_ = 5ms})
        .listen({
            .address_ = "127.0.0.1",
            .http_ = http_endpoint.port(),
            .https_ = https_endpoint.port(),
            .tls_ = {.identity_ = {.certificate_chain_file_ = identity.ca_file_, .private_key_file_ = private_key}},
        })
        .on_start([] { started.release(); });
    const auto ip_host = std::move(std::string("127.0.0.1:").append(std::to_string(https_endpoint.port())));
    for (const bool trusted : {false, true}) {
        if (trusted) {
            app.trusted_proxies({.cidrs_ = {"127.0.0.1/32"}});
        }
        ruvia::test::app_run runner(app);
        const bool ready = started.try_acquire_for(10s);
        EXPECT_TRUE(ready);
        if (ready && !trusted) {
            // Names the certificate covers: case-insensitively, as an absolute
            // domain name, through a whole-label wildcard, or as an IP SAN.
            EXPECT_EQ(https_exchange(io, https_endpoint, "localhost", get("/origin", "localhost")), "HTTP/1.1 200 OK");
            EXPECT_EQ(https_exchange(io, https_endpoint, "localhost", get("/origin", "LOCALHOST.")), "HTTP/1.1 200 OK");
            EXPECT_EQ(https_exchange(io, https_endpoint, nullptr, get("/origin", "a.wild.test")), "HTTP/1.1 200 OK");
            EXPECT_EQ(https_exchange(io, https_endpoint, nullptr, get("/origin", ip_host)), "HTTP/1.1 200 OK");
            EXPECT_EQ(https_exchange(io, https_endpoint, "localhost", get("/origin", "%6Cocalhost")), "HTTP/1.1 200 OK");
            EXPECT_EQ(https_exchange(io, https_endpoint, "localhost", get("https://%6Cocalhost/origin", "localhost")),
                "HTTP/1.1 200 OK");
            EXPECT_EQ(https_exchange(io, https_endpoint, "localhost", get("/origin", "localhost%2E%2E")),
                "HTTP/1.1 421 Misdirected Request");
            EXPECT_EQ(https_exchange(io, https_endpoint, nullptr, get("/origin", ".wild.test")),
                "HTTP/1.1 421 Misdirected Request");
            EXPECT_EQ(https_exchange(io, https_endpoint, nullptr, get("/origin", "%2Ewild.test")),
                "HTTP/1.1 421 Misdirected Request");
            EXPECT_EQ(https_exchange(io, https_endpoint, nullptr, get("/origin", "*.wild.test")),
                "HTTP/1.1 421 Misdirected Request");
            EXPECT_EQ(https_exchange(io, https_endpoint, nullptr, get("/origin", "127.0.0.1.")),
                "HTTP/1.1 421 Misdirected Request");
            // Names it does not cover, whatever SNI was offered.
            EXPECT_EQ(https_exchange(io, https_endpoint, "localhost", get("/origin", "attacker.example")),
                "HTTP/1.1 421 Misdirected Request");
            EXPECT_EQ(https_exchange(io, https_endpoint, "attacker.example", get("/origin", "attacker.example")),
                "HTTP/1.1 421 Misdirected Request");
            EXPECT_EQ(https_exchange(io, https_endpoint, nullptr, get("/origin", "attacker.example")),
                "HTTP/1.1 421 Misdirected Request");
            EXPECT_EQ(https_exchange(io, https_endpoint, nullptr, get("/origin", "a.b.wild.test")),
                "HTTP/1.1 421 Misdirected Request");
            EXPECT_EQ(https_exchange(io, https_endpoint, nullptr, get("/origin", "wild.test")),
                "HTTP/1.1 421 Misdirected Request");
            EXPECT_EQ(https_exchange(io, https_endpoint, nullptr, get("/origin", "[::1]")),
                "HTTP/1.1 421 Misdirected Request");
            EXPECT_EQ(https_exchange(io, https_endpoint, nullptr, get("https://attacker.example/origin", "localhost")),
                "HTTP/1.1 421 Misdirected Request");
#if defined(RUVIA_ENABLE_HTTP2)
            // HTTP/2 refuses only the misdirected stream; the connection
            // still serves the covered one.
            const auto h2 = h2_over_tls(io, https_endpoint,
                {{"https", "attacker.example"}, {"https", "localhost"}, {"https", "x.wild.test:443"}});
            EXPECT_EQ(h2.size(), std::size_t{3});
            if (h2.size() == 3) {
                EXPECT_EQ(h2[0], 421U);
                EXPECT_EQ(h2[1], 200U);
                EXPECT_EQ(h2[2], 200U);
            }
#endif

            // A cleartext connection carries no "https" target.
            EXPECT_EQ(http_exchange(io, http_endpoint, get("https://a.example/origin", "x")),
                "HTTP/1.1 421 Misdirected Request");
            EXPECT_EQ(http_exchange(io, http_endpoint, get("HTTPS://localhost:443/origin", "x")),
                "HTTP/1.1 421 Misdirected Request");
            EXPECT_EQ(http_exchange(io, http_endpoint, get("http://a.example/origin", "x")), "HTTP/1.1 200 OK");
            EXPECT_EQ(http_exchange(io, http_endpoint, get("/origin", "attacker.example")), "HTTP/1.1 200 OK");
#if defined(RUVIA_ENABLE_HTTP2)
            const auto h2c = h2_cleartext(io, http_endpoint, {{"https", "localhost"}, {"http", "localhost"}});
            EXPECT_EQ(h2c.size(), std::size_t{2});
            if (h2c.size() == 2) {
                EXPECT_EQ(h2c[0], 421U);
                EXPECT_EQ(h2c[1], 200U);
            }
#endif
        }
        if (ready && trusted) {
            // A trusted gateway vouches for the target itself.
            EXPECT_EQ(https_exchange(io, https_endpoint, "localhost", get("/origin", "attacker.example")), "HTTP/1.1 200 OK");
            EXPECT_EQ(http_exchange(io, http_endpoint, get("https://a.example/origin", "x")), "HTTP/1.1 200 OK");
#if defined(RUVIA_ENABLE_HTTP2)
            const auto h2 = h2_over_tls(io, https_endpoint, {{"https", "attacker.example"}});
            EXPECT_EQ(h2.size(), std::size_t{1});
            if (h2.size() == 1) {
                EXPECT_EQ(h2[0], 200U);
            }
#endif
        }
        app.stop();
        runner.join();
    }
    app.trusted_proxies({});
}

namespace server_observation_test {

using namespace std::chrono_literals;

class connection_id_controller final {
public:
    RUVIA_ROUTES_BEGIN(connection_id_controller)
    RUVIA_GET("/id", id);
    RUVIA_ROUTES_END

    ruvia::task<ruvia::http_response> id(ruvia::context& context) {
        co_return context.text(std::to_string(context.conn().connection_id()));
    }
};

struct client_hello_copy final {
    std::uint64_t connection_id_;
    std::string local_address_;
    std::uint16_t local_port_;
    std::string remote_address_;
    std::uint16_t remote_port_;
    bool quic_;
    std::string message_;
};

// What the observers saw, written on worker threads and read by the test.
class observed_records final {
public:
    void add(const ruvia::tls_client_hello_record& hello) {
        const auto message = hello.message();
        std::lock_guard lock(mutex_);
        client_hellos_.push_back({hello.connection_id(), std::string(hello.local().address().to_string()), hello.local().port(),
            std::string(hello.remote().address().to_string()), hello.remote().port(), hello.quic(),
            std::string(reinterpret_cast<const char*>(message.data()), message.size())});
    }

    void add(const ruvia::http2_input_record& input) {
        const auto bytes = input.bytes();
        std::lock_guard lock(mutex_);
        ++http2_reads_;
        http2_input_[input.connection_id()].append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    }

#if defined(RUVIA_ENABLE_HTTP3)
    void add(const ruvia::http3_frame_record& frame) {
        std::lock_guard lock(mutex_);
        http3_collectors_[frame.connection_id()].observe_frame(frame.metadata());
    }

    void add(const ruvia::http3_field_record& field) {
        std::lock_guard lock(mutex_);
        http3_collectors_[field.connection_id()].observe_field(field.metadata());
    }

    [[nodiscard]] std::string http3_fingerprint(std::uint64_t connection_id) const {
        std::lock_guard lock(mutex_);
        const auto found = http3_collectors_.find(connection_id);
        if (found == http3_collectors_.end() || !found->second.complete()) {
            return {};
        }
        return std::string(found->second.fingerprint());
    }
#endif
    [[nodiscard]] std::vector<client_hello_copy> client_hellos(std::uint64_t connection_id) const {
        std::lock_guard lock(mutex_);
        std::vector<client_hello_copy> result;
        for (const auto& hello : client_hellos_) {
            if (hello.connection_id_ == connection_id) {
                result.push_back(hello);
            }
        }
        return result;
    }

    [[nodiscard]] std::string http2_input(std::uint64_t connection_id) const {
        std::lock_guard lock(mutex_);
        const auto found = http2_input_.find(connection_id);
        return found == http2_input_.end() ? std::string() : found->second;
    }

    [[nodiscard]] std::pair<std::size_t, std::size_t> counts() const {
        std::lock_guard lock(mutex_);
        return {client_hellos_.size(), http2_reads_};
    }

private:
    mutable std::mutex mutex_;
    std::vector<client_hello_copy> client_hellos_;
    std::map<std::uint64_t, std::string> http2_input_;
    std::size_t http2_reads_{0};
#if defined(RUVIA_ENABLE_HTTP3)
    std::map<std::uint64_t, ruvia::http3_fingerprint_collector> http3_collectors_;
#endif
};

observed_records& records() {
    static observed_records value;
    return value;
}

std::binary_semaphore& started() {
    static std::binary_semaphore value{0};
    return value;
}

[[nodiscard]] std::uint64_t parse_connection_id(std::string_view text) {
    if (text.empty()) {
        throw std::runtime_error("connection id response is empty");
    }
    return std::stoull(std::string(text));
}

// The 24-bit handshake length (RFC 9846 §4) covers everything after the header.
[[nodiscard]] bool well_formed_client_hello(std::string_view message) {
    if (message.size() < 4 || static_cast<unsigned char>(message[0]) != 1) {
        return false;
    }
    const auto length = (std::size_t{static_cast<unsigned char>(message[1])} << 16) |
                        (std::size_t{static_cast<unsigned char>(message[2])} << 8) |
                        std::size_t{static_cast<unsigned char>(message[3])};
    return length == message.size() - 4;
}

void capture_sent_client_hello(
    int writing, int, int content_type, const void* bytes, std::size_t size, SSL*, void* argument) {
    if (writing != 0 && content_type == SSL3_RT_HANDSHAKE && size >= 4 &&
        *static_cast<const unsigned char*>(bytes) == SSL3_MT_CLIENT_HELLO) {
        static_cast<std::vector<std::string>*>(argument)->emplace_back(static_cast<const char*>(bytes), size);
    }
}

struct tls_exchange final {
    std::uint64_t connection_id_{};
    std::uint16_t client_port_{};
    // Every ClientHello the client wrote, as it wrote them.
    std::vector<std::string> client_hellos_;
};

// One HTTP/1.1 GET /id over TLS. `groups` with a first group the server does
// not support makes the server answer with a HelloRetryRequest.
tls_exchange https_connection_id(asio::io_context& io, const asio::ip::tcp::endpoint& endpoint, const char* groups) {
    asio::ssl::context client(asio::ssl::context::tls_client);
    client.set_verify_mode(asio::ssl::verify_none);
    tls_exchange result;
    std::vector<std::string> client_hellos;
    asio::ssl::stream<asio::ip::tcp::socket> stream(io, client);
    SSL_set_msg_callback(stream.native_handle(), capture_sent_client_hello);
    SSL_set_msg_callback_arg(stream.native_handle(), &client_hellos);
    static constexpr unsigned char alpn[] = {8, 'h', 't', 't', 'p', '/', '1', '.', '1'};
    if (SSL_set_alpn_protos(stream.native_handle(), alpn, sizeof(alpn)) != 0 ||
        SSL_set_tlsext_host_name(stream.native_handle(), "localhost") != 1 ||
        (groups != nullptr && SSL_set1_groups_list(stream.native_handle(), groups) != 1)) {
        throw std::runtime_error("cannot configure the TLS test client");
    }
    stream.next_layer().connect(endpoint);
    result.client_port_ = stream.next_layer().local_endpoint().port();
    stream.handshake(asio::ssl::stream_base::client);
    asio::write(stream, asio::buffer(std::string_view("GET /id HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n")));
    std::string response;
    std::array<char, 1024> buffer;
    asio::error_code error;
    while (!error) {
        const auto size = stream.read_some(asio::buffer(buffer), error);
        response.append(buffer.data(), size);
    }
    const auto body = response.find("\r\n\r\n");
    if (!response.starts_with("HTTP/1.1 200 ") || body == std::string::npos) {
        throw std::runtime_error("HTTPS connection id request failed");
    }
    result.connection_id_ = parse_connection_id(std::string_view(response).substr(body + 4));
    result.client_hellos_ = std::move(client_hellos);
    return result;
}

#if defined(RUVIA_ENABLE_HTTP2)
// The client connection preface, its SETTINGS and GET /id on stream 1.
std::string http2_opening(std::string_view scheme) {
    std::string block;
    ruvia::hpack_encoder::encode_header(block, ":method", "GET");
    ruvia::hpack_encoder::encode_header(block, ":scheme", scheme);
    ruvia::hpack_encoder::encode_header(block, ":authority", "localhost");
    ruvia::hpack_encoder::encode_header(block, ":path", "/id");
    std::string wire(ruvia::http2_client_preface);
    ruvia::test::append_frame(wire, ruvia::http2_frame_type::settings, 0, 0, {});
    // END_STREAM | END_HEADERS.
    ruvia::test::append_frame(wire, ruvia::http2_frame_type::headers, 0x5, 1, block);
    return std::string(wire);
}

// Sends the opening and returns the DATA of stream 1 up to END_STREAM.
template <typename stream_type>
std::uint64_t http2_connection_id(stream_type& stream, std::string_view opening) {
    asio::write(stream, asio::buffer(opening));
    std::string body;
    for (;;) {
        const auto frame = ruvia::test::read_frame(stream);
        if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::data) &&
            frame.header_.stream_id_ == 1) {
            body += frame.payload_;
            if ((frame.header_.flags_ & 0x1) != 0) {
                return parse_connection_id(body);
            }
        }
    }
}

struct http2_exchange final {
    std::uint64_t connection_id_;
    std::string opening_;
};

http2_exchange cleartext_http2_connection_id(asio::io_context& io, const asio::ip::tcp::endpoint& endpoint) {
    asio::ip::tcp::socket socket(io);
    socket.connect(endpoint);
    auto opening = http2_opening("http");
    const auto id = http2_connection_id(socket, opening);
    return {id, std::move(opening)};
}

http2_exchange tls_http2_connection_id(asio::io_context& io, const asio::ip::tcp::endpoint& endpoint) {
    asio::ssl::context client(asio::ssl::context::tls_client);
    client.set_verify_mode(asio::ssl::verify_none);
    asio::ssl::stream<asio::ip::tcp::socket> stream(io, client);
    static constexpr unsigned char alpn[] = {2, 'h', '2'};
    if (SSL_set_alpn_protos(stream.native_handle(), alpn, sizeof(alpn)) != 0 ||
        SSL_set_tlsext_host_name(stream.native_handle(), "localhost") != 1) {
        throw std::runtime_error("cannot configure the h2 test client");
    }
    stream.next_layer().connect(endpoint);
    stream.handshake(asio::ssl::stream_base::client);
    auto opening = http2_opening("https");
    const auto id = http2_connection_id(stream, opening);
    return {id, std::move(opening)};
}
#endif

#if defined(RUVIA_ENABLE_HTTP3)
std::uint64_t http3_connection_id(std::uint16_t port, const ruvia::test::tls_identity& identity) {
    ruvia::test::http3_test_client client({asio::ip::address_v4::loopback(), port}, identity.ca_file_, "localhost", 5s);
    const auto response = client.request("GET", "/id");
    if (response.status_ != 200) {
        throw std::runtime_error("HTTP/3 connection id request failed");
    }
    return parse_connection_id(response.body_);
}
#endif

}  // namespace server_observation_test

// One application with cleartext, HTTPS and HTTP/3 services on two workers.
// The first run observes: every ClientHello (TLS over TCP, including the
// second one after a HelloRetryRequest, and QUIC) arrives as the exact bytes
// the client sent, tagged with the conn_info::connection_id() its requests see, and the
// HTTP/2 input of each connection (TLS ALPN h2 and cleartext prior knowledge)
// concatenates to exactly what the client wrote, preface first. The second
// run, with both observers cleared, serves the same requests and records
// nothing.
TEST(Ruvia, server_observers_see_client_hellos_and_http2_input_by_connection_id) {
    using namespace server_observation_test;
    ruvia::test::reset_app_callbacks(ruvia::app());
    ruvia::test::tls_identity identity("localhost", "DNS:localhost,IP:127.0.0.1");
    asio::io_context io;
    ruvia::test::tcp_port_reservation http_reservation(io);
    ruvia::test::tcp_port_reservation https_reservation(io);
    const auto http_endpoint = http_reservation.endpoint();
    const auto https_endpoint = https_reservation.endpoint();
    ruvia::listen_config listener{
        .address_ = "127.0.0.1",
        .http_ = http_endpoint.port(),
        .https_ = https_endpoint.port(),
        .tls_ = {.identity_ = {.certificate_chain_file_ = identity.ca_file_,
                     .private_key_file_ = identity.private_key_file()}},
    };
#if defined(RUVIA_ENABLE_HTTP3)
    ruvia::test::udp_port_reservation udp_reservation(io);
    const auto http3_port = udp_reservation.endpoint().port();
    listener.http3_ = http3_port;
#endif

    auto& app = ruvia::app();
    app.server({.worker_count_ = 2})
        .listen(listener)
        .on_start([] { started().release(); })
        .on_tls_client_hello([](const ruvia::tls_client_hello_record& hello) noexcept { records().add(hello); })
        .on_http2_input([](const ruvia::http2_input_record& input) noexcept { records().add(input); });
#if defined(RUVIA_ENABLE_HTTP3)
    app.on_http3_frame([](const ruvia::http3_frame_record& frame) noexcept { records().add(frame); })
        .on_http3_field([](const ruvia::http3_field_record& field) noexcept { records().add(field); });
#endif

    std::set<std::uint64_t> connection_ids;
    {
        ruvia::test::app_run runner(app);
        ASSERT_TRUE(started().try_acquire_for(10s));
        EXPECT_THROW(app.on_tls_client_hello(nullptr), std::logic_error);
        EXPECT_THROW(app.on_http2_input(nullptr), std::logic_error);

        for (const char* groups : {static_cast<const char*>(nullptr), "brainpoolP256r1tls13:X25519"}) {
            const auto exchange = https_connection_id(io, https_endpoint, groups);
            EXPECT_TRUE(connection_ids.insert(exchange.connection_id_).second);
            EXPECT_EQ(exchange.client_hellos_.size(), groups == nullptr ? 1U : 2U);
            const auto hellos = records().client_hellos(exchange.connection_id_);
            ASSERT_EQ(hellos.size(), exchange.client_hellos_.size());
            for (std::size_t i = 0; i < hellos.size(); ++i) {
                EXPECT_TRUE(well_formed_client_hello(hellos[i].message_));
                EXPECT_EQ(hellos[i].message_, exchange.client_hellos_[i]);
                EXPECT_FALSE(hellos[i].quic_);
                EXPECT_EQ(hellos[i].local_address_, "127.0.0.1");
                EXPECT_EQ(hellos[i].local_port_, https_endpoint.port());
                EXPECT_EQ(hellos[i].remote_address_, "127.0.0.1");
                EXPECT_EQ(hellos[i].remote_port_, exchange.client_port_);
            }
        }

#if defined(RUVIA_ENABLE_HTTP2)
        for (const auto& exchange :
            {cleartext_http2_connection_id(io, http_endpoint), tls_http2_connection_id(io, https_endpoint)}) {
            EXPECT_TRUE(connection_ids.insert(exchange.connection_id_).second);
            const auto input = records().http2_input(exchange.connection_id_);
            EXPECT_TRUE(input.starts_with(ruvia::http2_client_preface));
            EXPECT_EQ(input, exchange.opening_);
        }
#endif

#if defined(RUVIA_ENABLE_HTTP3)
        const auto quic_id = http3_connection_id(http3_port, identity);
        EXPECT_TRUE(connection_ids.insert(quic_id).second);
        // QUIC carries the ClientHello in CRYPTO frames; the observer sees the
        // reassembled message once.
        const auto hellos = records().client_hellos(quic_id);
        ASSERT_EQ(hellos.size(), 1U);
        EXPECT_TRUE(hellos[0].quic_);
        EXPECT_TRUE(well_formed_client_hello(hellos[0].message_));
        EXPECT_EQ(hellos[0].local_address_, "127.0.0.1");
        EXPECT_EQ(hellos[0].local_port_, http3_port);
        EXPECT_EQ(hellos[0].remote_address_, "127.0.0.1");
        EXPECT_FALSE(records().http3_fingerprint(quic_id).empty());
#endif
        app.stop();
        runner.join();
    }
    EXPECT_EQ(connection_ids.count(0), 0U);

    const auto observed = records().counts();
    app.on_tls_client_hello(nullptr).on_http2_input(nullptr);
#if defined(RUVIA_ENABLE_HTTP3)
    app.on_http3_frame(nullptr).on_http3_field(nullptr);
#endif
    {
        ruvia::test::app_run runner(app);
        ASSERT_TRUE(started().try_acquire_for(10s));
        EXPECT_NE(https_connection_id(io, https_endpoint, nullptr).connection_id_, 0U);
#if defined(RUVIA_ENABLE_HTTP2)
        EXPECT_NE(cleartext_http2_connection_id(io, http_endpoint).connection_id_, 0U);
        EXPECT_NE(tls_http2_connection_id(io, https_endpoint).connection_id_, 0U);
#endif
#if defined(RUVIA_ENABLE_HTTP3)
        EXPECT_NE(http3_connection_id(http3_port, identity), 0U);
#endif
        app.stop();
        runner.join();
    }
    EXPECT_EQ(records().counts(), observed);
}

TEST(Ruvia, standalone_sliding_window_rate_limiter_and_address_key_canonicalization) {
    using namespace server_observation_test;
    std::array<char, ruvia::rate_limit_key_buffer_bytes> buf1{};
    std::array<char, ruvia::rate_limit_key_buffer_bytes> buf2{};
    const auto k1 = ruvia::format_rate_limit_address_key("2001:db8:1:2::1", buf1);
    const auto k2 = ruvia::format_rate_limit_address_key("2001:db8:1:2::ffff", buf2);
    EXPECT_EQ(k1, k2);

    const auto v4 = ruvia::format_rate_limit_address_key("192.0.2.9", buf1);
    const auto mapped = ruvia::format_rate_limit_address_key("::ffff:192.0.2.9", buf2);
    EXPECT_EQ(v4, mapped);

    ruvia::sliding_window_rate_limiter limiter({
        .rule_ = {.max_requests_ = 2, .window_ = std::chrono::seconds(10)},
        .capacity_per_worker_ = 64,
    });
    const auto t0 = std::chrono::steady_clock::now();
    const auto ip = *ruvia::ip_address::parse("203.0.113.5");
    const auto d1 = limiter.decide(ip, t0);
    EXPECT_TRUE(d1.allowed());
    EXPECT_EQ(d1.limit(), 2U);
    EXPECT_EQ(d1.remaining(), 1U);

    const auto d2 = limiter.decide(ip, t0);
    EXPECT_TRUE(d2.allowed());
    EXPECT_EQ(d2.remaining(), 0U);

    const auto d3 = limiter.decide(ip, t0);
    EXPECT_FALSE(d3.allowed());
    EXPECT_GE(d3.retry_after_seconds(), 1U);

    ruvia::http_response resp;
    ruvia::apply_rate_limit_headers(resp, d3);
    EXPECT_TRUE(resp.header("Retry-After").has_value());
    EXPECT_EQ(resp.header("X-RateLimit-Limit"), std::optional<std::string_view>("2"));
    EXPECT_EQ(resp.header("X-RateLimit-Remaining"), std::optional<std::string_view>("0"));
}

// Inbound buffer budgets on the streamed request body and CONNECT tunnel
// paths. This executable owns its application with small inbound budgets.

namespace http1_inbound_memory_test {

constexpr auto scan_interval = std::chrono::milliseconds(10);
// A tunnel's reused read window takes a little over 4 KiB: one fits the
// connection budget with room for a small chunk, two fit the worker budget,
// three do not. A streamed body's 8 KiB read buffer fits neither.
constexpr std::size_t connection_budget = 6000;
constexpr std::size_t worker_budget = 10000;

[[nodiscard]] ruvia::server_config inbound_memory_config() {
    return {
        .worker_count_ = 1,
        .connection_scan_interval_ = scan_interval,
        .max_inbound_buffer_bytes_per_worker_ = worker_budget,
        .max_inbound_buffer_bytes_per_connection_ = connection_budget,
    };
}

class inbound_memory_routes final {
    RUVIA_ROUTES_BEGIN(inbound_memory_routes)
    RUVIA_POST("/inbound-buffered", buffered_body);
    RUVIA_POST("/inbound-stream", stream_body, ruvia::stream_body);
    RUVIA_CONNECT("inbound-tunnel.test:443", tunnel);
    RUVIA_ROUTES_END

    ruvia::task<ruvia::http_response> buffered_body(ruvia::context& context_value) {
        const auto body = co_await context_value.req().text();
        co_return context_value.text(
            std::string(std::to_string(body.size())));
    }

    ruvia::task<ruvia::http_response> stream_body(ruvia::context& context_value) {
        auto& reader = context_value.req().body_reader();
        std::size_t received = 0;
        while (const auto chunk = co_await reader.read()) {
            received += chunk->size();
        }
        co_return context_value.text(std::string(std::to_string(received)));
    }

    // Acknowledges each borrowed chunk and reports inbound budget exhaustion.
    ruvia::task<void> tunnel(ruvia::context& context_value) {
        auto& tunnel_value = context_value.tunnel();
        bool limited = false;
        for (;;) {
            std::optional<std::span<const std::byte>> chunk;
            try {
                chunk = co_await tunnel_value.read();
            } catch (const std::bad_alloc&) {
                limited = true;
            }
            if (!chunk) {
                break;
            }
            co_await tunnel_value.write("ok\n");
        }
        co_await tunnel_value.write(limited ? "limited\n" : "end\n");
    }
};

class test_client final {
public:
    test_client(asio::io_context& io, const asio::ip::tcp::endpoint& server_endpoint)
        : io_(io),
          socket_(io) {
        socket_.connect(server_endpoint);
    }

    void send(std::string_view bytes) {
        asio::write(socket_, asio::buffer(bytes));
    }

    void finish_sending() {
        socket_.shutdown(asio::ip::tcp::socket::shutdown_send);
    }

    // Everything up to and including `delimiter`, or nullopt when the stream
    // ends or 5 s pass first.
    [[nodiscard]] std::optional<std::string> read_until(std::string_view delimiter) {
        std::optional<std::size_t> found;
        asio::async_read_until(socket_, asio::dynamic_buffer(input_), delimiter,
            [&found](const std::error_code& error, std::size_t bytes) {
                if (!error) {
                    found = bytes;
                }
            });
        run();
        if (!found) {
            return std::nullopt;
        }
        std::string result(std::string_view(input_).substr(0, *found));
        input_.erase(0, *found);
        return result;
    }

    // Everything until the server ends the stream (or 5 s pass).
    [[nodiscard]] std::string read_to_end() {
        asio::async_read(socket_, asio::dynamic_buffer(input_), [](const std::error_code&, std::size_t) {});
        run();
        return std::exchange(input_, std::string{});
    }

    void close() {
        std::error_code ignored;
        socket_.close(ignored);
    }

private:
    void run() {
        io_.restart();
        io_.run_for(std::chrono::seconds(5));
        if (!io_.stopped()) {
            std::error_code ignored;
            socket_.cancel(ignored);
            io_.restart();
            io_.run();
        }
    }

    asio::io_context& io_;
    asio::ip::tcp::socket socket_;
    std::string input_{};
};

constexpr std::string_view connect_request =
    "CONNECT inbound-tunnel.test:443 HTTP/1.1\r\nHost: inbound-tunnel.test:443\r\n\r\n";

[[nodiscard]] bool open_tunnel(test_client& client) {
    client.send(connect_request);
    const auto head = client.read_until("\r\n\r\n");
    return head && head->starts_with("HTTP/1.1 200 ");
}

}  // namespace http1_inbound_memory_test

TEST(Ruvia, streamed_request_body_reads_count_against_the_connection_inbound_budget) {
    using namespace http1_inbound_memory_test;
    ruvia::test::reset_app_callbacks(ruvia::app());
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io, inbound_memory_config());
    {
        // A body that arrives with its head needs no reader buffer.
        test_client client(io, server.endpoint());
        client.send(
            "POST /inbound-stream HTTP/1.1\r\nHost: localhost\r\nContent-Length: 16\r\nConnection: close\r\n\r\n"
            "0123456789abcdef");
        const auto response = client.read_to_end();
        EXPECT_TRUE(response.starts_with("HTTP/1.1 200 "));
        EXPECT_TRUE(response.ends_with("\r\n\r\n16"));
        client.close();
    }
    {
        // The reader's 8 KiB socket buffer exceeds the connection budget.
        test_client client(io, server.endpoint());
        client.send(
            "POST /inbound-stream HTTP/1.1\r\nHost: localhost\r\nContent-Length: 20000\r\nConnection: close\r\n\r\n" +
            std::string(20000, 'b'));
        const auto response = client.read_to_end();
        EXPECT_TRUE(response.starts_with("HTTP/1.1 503 "));
        EXPECT_TRUE(response.find("inbound_buffer_limit") != std::string::npos);
        client.close();
    }
    server.finish();
}

TEST(Ruvia, tunnel_reads_count_against_the_connection_inbound_budget) {
    using namespace http1_inbound_memory_test;
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io, inbound_memory_config());
    test_client client(io, server.endpoint());
    EXPECT_TRUE(open_tunnel(client));
    client.send(std::string(20000, 't'));
    client.finish_sending();
    const auto output = client.read_to_end();
    EXPECT_TRUE(output.ends_with("limited\n"));
    client.close();
    server.finish();
}

TEST(Ruvia, tunnel_reads_count_against_the_worker_inbound_budget) {
    using namespace http1_inbound_memory_test;
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io, inbound_memory_config());
    // Two tunnels each hold their read window, within their own connection
    // budgets; a third tunnel's first read exceeds the worker's.
    test_client first(io, server.endpoint());
    test_client second(io, server.endpoint());
    for (auto* const client : {&first, &second}) {
        EXPECT_TRUE(open_tunnel(*client));
        client->send("a");
        EXPECT_TRUE(client->read_until("\n") == std::optional<std::string>(std::in_place, "ok\n"));
    }
    test_client third(io, server.endpoint());
    EXPECT_TRUE(open_tunnel(third));
    EXPECT_TRUE(third.read_to_end() == "limited\n");
    third.close();
    for (auto* const client : {&first, &second}) {
        client->finish_sending();
        EXPECT_TRUE(client->read_to_end() == "end\n");
        client->close();
    }
    server.finish();
}

#if defined(RUVIA_ENABLE_HTTP2)
TEST(Ruvia, http2_request_body_inbound_budget_exhaustion_answers_with_503) {
    using namespace http1_inbound_memory_test;
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io, inbound_memory_config());

    auto& client_io = ruvia::test::new_test_io_context();
    auto attachment = ruvia::attach_event_loop(client_io);
    const auto url = "http://127.0.0.1:" + std::to_string(server.endpoint().port());

    const auto run = [&]() -> ruvia::task<void> {
        ruvia::http_client client(attachment.loop(),
            {.base_url_ = url, .versions_ = {.http1_ = false, .http2_ = true}});
        const std::string payload(12000, 'h');
        for (const std::string_view path : {"/inbound-buffered", "/inbound-stream"}) {
            auto response = co_await client.client().post(path).body(payload).send();
            EXPECT_EQ(response.status(), ruvia::http_status::service_unavailable);
            const auto body = co_await response.text();
            EXPECT_NE(body.text().find("inbound_buffer_limit"), std::string_view::npos);
        }
        co_await client.shutdown();
        attachment.stop();
    };
    auto root = attachment.loop().start(run());
    std::jthread driver([&] { attachment.run(); });
    driver.join();
    std::move(root).get();
    server.finish();
}
#endif

// Inline buffered-response compression on one worker, whose encoders are
// reused across responses. This executable owns its application, so the
// app-wide compression policy cannot change the bodies of other suites.

namespace http1_response_compression_test {

// Every fixture passes the same configuration: whichever case runs first
// fixes the shared application's server_config.
[[nodiscard]] ruvia::server_config compressing_server() {
    return {.worker_count_ = 1,
        .connection_scan_interval_ = std::chrono::milliseconds(5),
        .compression_ = ruvia::compression_config{.min_bytes_ = 16, .sync_bytes_ = 64 * 1024}};
}

[[nodiscard]] std::string make_body(std::size_t size, unsigned seed) {
    std::string body(size, '\0');
    unsigned state_value = seed * 2654435761U + 3U;
    for (std::size_t i = 0; i < size; ++i) {
        state_value = state_value * 1103515245U + 12345U;
        body[i] = static_cast<char>('a' + ((state_value >> 16U) % 6U) + ((i / 48U) % 5U));
    }
    return body;
}

// Cover both sides of the configured inline ceiling without changing policy between cases.
const std::string small_body(make_body(3'000, 1));
const std::string large_body(make_body(60'000, 2));
const std::string offloaded_body(make_body(128 * 1024, 3));

[[nodiscard]] const std::filesystem::path& file_dir() {
    static const ruvia::test::scoped_temp_directory directory("ruvia-response-compression");
    return directory.path();
}

std::unique_ptr<ruvia::static_root> resident_root;
std::unique_ptr<ruvia::static_root> coded_root;

class response_compression_routes final {
    RUVIA_ROUTES_BEGIN(response_compression_routes)
    RUVIA_GET("/compression-small", small);
    RUVIA_GET("/compression-large", large);
    RUVIA_GET("/compression-offloaded", offloaded);
    RUVIA_GET("/compression-tiny", tiny);
    RUVIA_GET("/compression-digest", digest);
    RUVIA_GET("/compression-file", file);
    RUVIA_GET("/compression-static", static_file);
    RUVIA_GET("/compression-coded-static", coded_static_file);
    RUVIA_GET("/compression-head", small);
    RUVIA_HEAD("/compression-head", head_declared);
    RUVIA_GET("/compression-head-bare", small);
    RUVIA_HEAD("/compression-head-bare", head_bare);
    RUVIA_GET("/compression-empty-get", empty_get);
    RUVIA_GET("/compression-redirect", redirect_get);
    RUVIA_GET("/compression-zero-file", zero_file);
    RUVIA_GET("/compression-zero-static", zero_static_file);
    RUVIA_GET("/compression-head-comma", small);
    RUVIA_HEAD("/compression-head-comma", head_comma_declared);
    RUVIA_GET("/compression-head-zero", empty_get);
    RUVIA_HEAD("/compression-head-zero", head_zero_declared);
    RUVIA_GET("/compression-custom-coding", custom_coding);
    RUVIA_GET("/compression-mixed-rejected", mixed_rejected_coding);
    RUVIA_POST("/compression-echo-decoded", echo_decoded);
    RUVIA_GET("/compression-zero-declared", head_zero_declared);
    RUVIA_GET("/compression-stream", stream);
    RUVIA_GET("/compression-stream-held", hold_stream);
    RUVIA_GET("/compression-stream-custom", stream_custom_coding);
    RUVIA_GET("/compression-stream-mixed", stream_mixed_coding);
    RUVIA_GET("/compression-stream-204", stream_no_content);
    RUVIA_GET("/compression-stream-304", stream_not_modified);
    RUVIA_GET("/compression-validators", validators);
    RUVIA_GET("/compression-media", media);
    RUVIA_ROUTES_END

    ruvia::task<ruvia::http_response> small(ruvia::context& context_value) {
        co_return context_value.text(std::string_view(small_body));
    }

    ruvia::task<ruvia::http_response> large(ruvia::context& context_value) {
        co_return context_value.text(std::string_view(large_body));
    }

    ruvia::task<ruvia::http_response> offloaded(ruvia::context& context_value) {
        co_return context_value.text(std::string_view(offloaded_body));
    }

    ruvia::task<ruvia::http_response> head_declared(ruvia::context& context_value) {
        context_value.draft().header("Content-Length", std::to_string(small_body.size()));
        co_return context_value.text("");
    }
    ruvia::task<ruvia::http_response> head_bare(ruvia::context& context_value) {
        co_return context_value.text("");
    }
    ruvia::task<ruvia::http_response> empty_get(ruvia::context& context_value) {
        context_value.draft().header("ETag", "\"empty-v1\"");
        co_return context_value.text("");
    }
    ruvia::task<ruvia::http_response> redirect_get(ruvia::context& context_value) {
        co_return context_value.redirect({.location_ = "/compression-small"});
    }
    ruvia::task<ruvia::http_response> zero_file(ruvia::context& context_value) {
        const auto path = file_dir() / "zero.txt";
        co_return co_await context_value.file({.path_ = path});
    }
    ruvia::task<ruvia::http_response> zero_static_file(ruvia::context& context_value) {
        co_return co_await context_value.static_file(*resident_root, {.relative_path_ = "zero.txt"});
    }
    ruvia::task<ruvia::http_response> head_comma_declared(ruvia::context& context_value) {
        const auto len = std::string(std::to_string(small_body.size()));
        context_value.draft().header("Content-Length", std::string(len) + ", " + len);
        co_return context_value.text("");
    }
    ruvia::task<ruvia::http_response> head_zero_declared(ruvia::context& context_value) {
        context_value.draft().header("ETag", "\"empty-v1\"");
        context_value.draft().header("Content-Length", "0");
        co_return context_value.text("");
    }
    ruvia::task<ruvia::http_response> custom_coding(ruvia::context& context_value) {
        auto resp = context_value.text("custom-encoded-payload");
        ruvia::http_accepted_encoding_quality quality;
        quality.update(context_value.req().header("Accept-Encoding").value_or(""), "x-custom");
        if (quality.accepts(false)) {
            resp.header("Content-Encoding", "x-custom");
        }
        co_return resp;
    }
    ruvia::task<ruvia::http_response> mixed_rejected_coding(ruvia::context& context_value) {
        auto resp = context_value.text("mixed-encoded-payload");
        resp.header("Content-Encoding", "br, x-custom");
        co_return resp;
    }
    ruvia::task<ruvia::http_response> echo_decoded(ruvia::context& context_value) {
        const auto body = co_await context_value.req().text();
        co_return context_value.text(body);
    }
    ruvia::task<void> stream(ruvia::context& context_value) {
        auto& writer = context_value.stream_text();
        if (co_await writer.commit() == ruvia::stream_commit_result::completed) {
            co_return;
        }
        const std::string_view body(small_body);
        co_await writer.write(body.substr(0, body.size() / 2));
        co_await writer.write(body.substr(body.size() / 2));
        co_await writer.end();
    }
    ruvia::task<void> hold_stream(ruvia::context& context_value) {
        auto& writer = context_value.stream_text();
        if (co_await writer.commit() == ruvia::stream_commit_result::completed) {
            co_return;
        }
        co_await writer.write(std::string_view(small_body));
        static_cast<void>(co_await writer.sleep(std::chrono::hours(1)));
    }

    ruvia::task<void> stream_custom_coding(ruvia::context& context_value) {
        context_value.draft().header("Content-Encoding", "x-custom");
        auto& writer = context_value.stream_text();
        if (co_await writer.commit() == ruvia::stream_commit_result::completed) {
            co_return;
        }
        co_await writer.write("custom-stream-payload");
        co_await writer.end();
    }
    ruvia::task<void> stream_mixed_coding(ruvia::context& context_value) {
        context_value.draft().header("Content-Encoding", "br, x-custom");
        auto& writer = context_value.stream_text();
        if (co_await writer.commit() == ruvia::stream_commit_result::completed) {
            co_return;
        }
        co_await writer.write("mixed-stream-payload");
        co_await writer.end();
    }
    ruvia::task<void> stream_no_content(ruvia::context& context_value) {
        context_value.draft().status(ruvia::http_status::no_content);
        co_await context_value.stream_text().end();
    }
    ruvia::task<void> stream_not_modified(ruvia::context& context_value) {
        context_value.draft().status(ruvia::http_status::not_modified);
        co_await context_value.stream_text().end();
    }
    ruvia::task<ruvia::http_response> tiny(ruvia::context& context_value) {
        co_return context_value.text("hello");
    }

    ruvia::task<ruvia::http_response> validators(ruvia::context& context_value) {
        auto resp = context_value.text(std::string_view(small_body));
        resp.header("ETag", context_value.req().query("etag").value_or(""));
        resp.header("Accept-Ranges", "bytes");
        co_return resp;
    }

    ruvia::task<ruvia::http_response> media(ruvia::context& context_value) {
        auto resp = context_value.text(std::string_view(small_body));
        const auto type = context_value.req().query("type").value_or("");
        if (type.empty()) {
            resp.remove_header("Content-Type");
        } else {
            resp.header("Content-Type", type);
        }
        co_return resp;
    }

    ruvia::task<ruvia::http_response> digest(ruvia::context& context_value) {
        const auto field = context_value.req().query("field").value_or("");
        context_value.draft().header(field, "sha-256=:AAAA:");
        co_return context_value.text(std::string_view(small_body));
    }

    ruvia::task<ruvia::http_response> file(ruvia::context& context_value) {
        static const std::filesystem::path path = file_dir() / "file.txt";
        co_return co_await context_value.file({.path_ = path});
    }

    ruvia::task<ruvia::http_response> static_file(ruvia::context& context_value) {
        co_return co_await context_value.static_file(*resident_root, {.relative_path_ = "resident.txt"});
    }

    ruvia::task<ruvia::http_response> coded_static_file(ruvia::context& context_value) {
        co_return co_await context_value.static_file(*coded_root, {.relative_path_ = "asset.txt"});
    }
};

// Sends one request and returns every byte the server wrote before closing,
// or nullopt when the connection is still open after 5 s.
[[nodiscard]] std::optional<std::string> fetch_response(
    const asio::ip::tcp::endpoint& server_endpoint, std::string_view request) {
    auto& io = ruvia::test::new_test_io_context();
    asio::ip::tcp::socket socket(io);
    socket.connect(server_endpoint);
    asio::write(socket, asio::buffer(request));
    std::string output{};
    bool closed = false;
    asio::async_read(socket, asio::dynamic_buffer(output),
        [&closed](const std::error_code&, std::size_t) { closed = true; });
    io.run_for(std::chrono::seconds(5));
    if (!closed) {
        return std::nullopt;
    }
    return output;
}

[[nodiscard]] std::string lower_head(std::string_view response) {
    std::string head(response.substr(0, response.find("\r\n\r\n")));
    std::ranges::transform(head, head.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return head;
}

// The response head's field lines without Date, which may tick between two
// requests.
[[nodiscard]] std::vector<std::string> head_fields(std::string_view response) {
    std::vector<std::string> fields{};
    const auto head = lower_head(response);
    for (auto begin = head.find("\r\n"); begin != std::string::npos;) {
        begin += 2;
        const auto end = head.find("\r\n", begin);
        const auto line = std::string_view(head).substr(begin, end == std::string::npos ? std::string::npos : end - begin);
        if (!line.starts_with("date:")) {
            fields.emplace_back(line);
        }
        begin = end;
    }
    std::ranges::sort(fields);
    return fields;
}

[[nodiscard]] bool has_field(const std::vector<std::string>& fields, std::string_view prefix) {
    return std::ranges::any_of(fields, [prefix](const std::string& line) { return line.starts_with(prefix); });
}

[[nodiscard]] std::string expected_encoding(ruvia::http_content_coding coding, std::string_view body) {
    const auto result_value = ruvia::encode_http_content(coding, body, {.max_encoded_bytes_ = body.size() - 1});
    return result_value.has_value() ? std::string(*result_value) : std::string();
}

[[nodiscard]] std::string_view response_body(std::string_view response) {
    const auto separator = response.find("\r\n\r\n");
    return separator == std::string_view::npos ? std::string_view{} : response.substr(separator + 4);
}

[[nodiscard]] std::string head_request(std::string_view target, std::string_view fields) {
    return "HEAD " + std::string(target) + " HTTP/1.1\r\nHost: localhost\r\n" + std::string(fields) +
           "Connection: close\r\n\r\n";
}

[[nodiscard]] std::string get_request(std::string_view target, std::string_view fields) {
    return "GET " + std::string(target) + " HTTP/1.1\r\nHost: localhost\r\n" + std::string(fields) +
           "Connection: close\r\n\r\n";
}

}  // namespace http1_response_compression_test

TEST(Ruvia, http1_streaming_head_preserves_coding_before_following_encoded_get) {
    using namespace http1_response_compression_test;
    ruvia::test::reset_app_callbacks(ruvia::app());
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io, compressing_server());
    constexpr ruvia::http_content_coding codings[]{ruvia::http_content_coding::identity,
        ruvia::http_content_coding::gzip, ruvia::http_content_coding::deflate,
        ruvia::http_content_coding::brotli, ruvia::http_content_coding::zstd};
    for (const auto coding : codings) {
        const auto token = ruvia::http_content_coding_token(coding);
        std::string fields("Accept-Encoding: ");
        fields.append(token).append("\r\n");
        const auto check_coding = [coding, token](std::string_view response) {
            const auto headers = head_fields(response);
            if (coding == ruvia::http_content_coding::identity) {
                EXPECT_FALSE(has_field(headers, "content-encoding:"));
            } else {
                std::string expected("content-encoding: ");
                expected.append(token);
                EXPECT_TRUE(has_field(headers, expected));
            }
        };
        const auto head = fetch_response(server.endpoint(), head_request("/compression-stream", fields));
        ASSERT_TRUE(head.has_value());
        EXPECT_TRUE(head->starts_with("HTTP/1.1 200 "));
        EXPECT_TRUE(response_body(*head).empty());
        check_coding(*head);

        // HTTP/1.0 close delimiting isolates content encoding from chunk framing.
        std::string request("GET /compression-stream HTTP/1.0\r\nHost: localhost\r\n");
        request.append(fields).append("\r\n");
        const auto get = fetch_response(server.endpoint(), request);
        ASSERT_TRUE(get.has_value());
        EXPECT_NE(std::string_view(*get).substr(0, get->find("\r\n")).find(" 200 "), std::string_view::npos);
        check_coding(*get);
        if (coding == ruvia::http_content_coding::identity) {
            EXPECT_EQ(response_body(*get), std::string_view(small_body));
        } else {
            const auto decoded = ruvia::decode_http_content(std::array{coding}, response_body(*get),
                {.max_decoded_bytes_ = small_body.size()});
            const auto* body = std::get_if<std::string>(&decoded);
            ASSERT_NE(body, nullptr);
            EXPECT_EQ(*body, small_body);
        }
    }
    server.finish();
}

TEST(Ruvia, http1_compressed_stream_peer_reset_allows_following_response) {
    using namespace http1_response_compression_test;
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io, compressing_server());
    const auto failures_before = ruvia::app().stats().connection_failures_;
    {
        asio::ip::tcp::socket client(io);
        client.connect(server.endpoint());
        constexpr std::string_view request =
            "GET /compression-stream-held HTTP/1.0\r\nHost: localhost\r\nAccept-Encoding: gzip\r\n\r\n";
        asio::write(client, asio::buffer(request));
        std::string prefix;
        asio::read_until(client, asio::dynamic_buffer(prefix), "\r\n\r\n");
        ASSERT_TRUE(has_field(head_fields(prefix), "content-encoding: gzip"));
        if (prefix.size() == prefix.find("\r\n\r\n") + 4) {
            char byte;
            asio::read(client, asio::buffer(&byte, 1));
        }
        client.set_option(asio::socket_base::linger(true, 0));
        client.close();
    }
    server.finish();

    const auto response = fetch_response(server.endpoint(),
        "GET /compression-stream HTTP/1.0\r\nHost: localhost\r\nAccept-Encoding: gzip\r\n\r\n");
    ASSERT_TRUE(response.has_value());
    EXPECT_TRUE(has_field(head_fields(*response), "content-encoding: gzip"));
    const auto decoded = ruvia::decode_http_content(
        std::array{ruvia::http_content_coding::gzip}, response_body(*response),
        {.max_decoded_bytes_ = small_body.size()});
    const auto* body = std::get_if<std::string>(&decoded);
    ASSERT_NE(body, nullptr);
    EXPECT_EQ(*body, small_body);
    server.finish();
    EXPECT_EQ(ruvia::app().stats().connection_failures_, failures_before);
}

TEST(Ruvia, http1_inline_compression_reuses_worker_encoders_with_identical_bytes) {
    using namespace http1_response_compression_test;
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io, compressing_server());
    constexpr ruvia::http_content_coding codings[]{ruvia::http_content_coding::gzip,
        ruvia::http_content_coding::deflate, ruvia::http_content_coding::brotli, ruvia::http_content_coding::zstd};
    // Each encoder serves several responses of alternating sizes in a row.
    for (int round = 0; round < 2; ++round) {
        for (const auto coding : codings) {
            for (const bool large : {true, false, true}) {
                const std::string_view path = large ? "/compression-large" : "/compression-small";
                const std::string_view body = large ? large_body : small_body;
                const auto token = ruvia::http_content_coding_token(coding);
                const auto output = fetch_response(server.endpoint(),
                    "GET " + std::string(path) + " HTTP/1.1\r\nHost: localhost\r\nAccept-Encoding: " +
                        std::string(token) + "\r\nConnection: close\r\n\r\n");
                EXPECT_TRUE(output.has_value());
                const auto text = (output ? std::string_view(*output) : std::string_view{});
                EXPECT_TRUE(text.starts_with("HTTP/1.1 200 "));
                EXPECT_TRUE(lower_head(text).find("\r\ncontent-encoding: " + std::string(token) + "\r\n") !=
                            std::string::npos);
                const auto separator = text.find("\r\n\r\n");
                EXPECT_TRUE(separator != std::string::npos);
                if (separator != std::string::npos) {
                    const auto expected = expected_encoding(coding, body);
                    EXPECT_TRUE(!expected.empty());
                    EXPECT_TRUE(std::string_view(text).substr(separator + 4) == expected);
                }
            }
        }
    }
    server.finish();
}

// context::file and static_file serve fixed file representations: a small
// file's bytes travel in memory on GET but still are not runtime-compressed,
// so GET and HEAD carry the same identity fields (RFC 9110 9.3.2).
TEST(Ruvia, http1_file_representations_are_not_runtime_compressed) {
    using namespace http1_response_compression_test;
    namespace fs = std::filesystem;
    std::error_code ignored;
    fs::remove_all(file_dir(), ignored);
    fs::create_directories(file_dir());
    for (const auto* name : {"file.txt", "resident.txt"}) {
        std::ofstream(file_dir() / name, std::ios::binary) << small_body;
    }
    resident_root = std::make_unique<ruvia::static_root>(file_dir(),
        ruvia::static_root_options{
            .file_types_ = {.kind_ = ruvia::static_file_type_policy::kind::all},
            .resident_max_bytes_ = 16 * 1024,
            .resident_budget_bytes_ = 16 * 1024,
        });

    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io, compressing_server());
    for (const std::string_view path : {"/compression-file", "/compression-static"}) {
        const auto fetch = [&](std::string_view method) {
            return fetch_response(server.endpoint(), std::string(method) + " " + std::string(path) +
                                                         " HTTP/1.1\r\nHost: localhost\r\nAccept-Encoding: "
                                                         "gzip\r\nConnection: close\r\n\r\n")
                .value_or(std::string{});
        };
        const auto get = fetch("GET");
        const auto head = fetch("HEAD");
        EXPECT_TRUE(get.starts_with("HTTP/1.1 200 "));
        EXPECT_TRUE(head.starts_with("HTTP/1.1 200 "));
        const auto get_fields = head_fields(get);
        EXPECT_TRUE(get_fields == head_fields(head));
        EXPECT_TRUE(!has_field(get_fields, "content-encoding:"));
        EXPECT_TRUE(has_field(get_fields, "content-length: " + std::string(std::to_string(small_body.size()))));
        EXPECT_TRUE(has_field(get_fields, "etag: \""));
        // static_file negotiates indexed precompressed variants; context::file
        // has no coding dimension at all.
        EXPECT_EQ(has_field(get_fields, "vary: accept-encoding"), path == "/compression-static");
        const auto separator = get.find("\r\n\r\n");
        EXPECT_TRUE(separator != std::string::npos);
        if (separator != std::string::npos) {
            EXPECT_TRUE(std::string_view(get).substr(separator + 4) == small_body);
        }
    }
    server.finish();
    resident_root.reset();
    fs::remove_all(file_dir(), ignored);
}

// RFC 9530 §2/§3: Content-Digest and Repr-Digest are computed over the content
// as sent, including its content coding, so the framework must not encode a
// body whose integrity fields the application already computed.
TEST(Ruvia, http1_compression_keeps_responses_with_integrity_fields_identity) {
    using namespace http1_response_compression_test;
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io, compressing_server());
    for (const std::string_view field : {"Content-Digest", "Repr-Digest", "Content-MD5"}) {
        const auto text = fetch_response(server.endpoint(),
            get_request("/compression-digest?field=" + std::string(field), "Accept-Encoding: gzip\r\n"))
                              .value_or(std::string{});
        EXPECT_TRUE(text.starts_with("HTTP/1.1 200 "));
        const auto fields = head_fields(text);
        EXPECT_TRUE(!has_field(fields, "content-encoding:"));
        EXPECT_TRUE(has_field(fields, "content-length: " + std::string(std::to_string(small_body.size()))));
        EXPECT_TRUE(response_body(text) == small_body);
    }
    server.finish();
}

// RFC 9110 §12.5.3: a client that refuses identity accepts a listed coding, so a
// body below min_bytes_ is encoded instead of answered with 406.
TEST(Ruvia, http1_compression_encodes_small_bodies_when_identity_is_refused) {
    using namespace http1_response_compression_test;
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io, compressing_server());
    const auto refused = fetch_response(server.endpoint(),
        get_request("/compression-tiny", "Accept-Encoding: gzip, identity;q=0\r\n"))
                             .value_or(std::string{});
    EXPECT_TRUE(refused.starts_with("HTTP/1.1 200 "));
    EXPECT_TRUE(has_field(head_fields(refused), "content-encoding: gzip"));
    const auto decoded = ruvia::decode_http_content(std::array{ruvia::http_content_coding::gzip}, response_body(refused),
        {.max_decoded_bytes_ = 64});
    const auto* plain = std::get_if<std::string>(&decoded);
    EXPECT_TRUE(plain != nullptr && std::string_view(*plain) == "hello");

    // With identity acceptable, the size threshold still keeps it identity.
    const auto accepted =
        fetch_response(server.endpoint(), get_request("/compression-tiny", "Accept-Encoding: gzip\r\n")).value_or(std::string{});
    EXPECT_TRUE(accepted.starts_with("HTTP/1.1 200 "));
    EXPECT_TRUE(!has_field(head_fields(accepted), "content-encoding:"));
    EXPECT_TRUE(response_body(accepted) == "hello");
    server.finish();
}

// RFC 9110 §8.8.3 / §14.3: a coded representation weakens a strong ETag, drops
// a value that is not an entity-tag, and stops advertising identity ranges.
TEST(Ruvia, http1_compression_rewrites_validators_and_range_support) {
    using namespace http1_response_compression_test;
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io, compressing_server());
    const struct {
        std::string_view query_;
        std::optional<std::string_view> expected_;
    } cases[] = {
        {"%22v1%22", "etag: w/\"v1\""},
        {"W%2F%22v2%22", "etag: w/\"v2\""},
        {"abc", std::nullopt},
        {"%22unterminated", std::nullopt},
    };
    for (const auto& test : cases) {
        const auto text = fetch_response(server.endpoint(),
            get_request("/compression-validators?etag=" + std::string(test.query_), "Accept-Encoding: gzip\r\n"))
                              .value_or(std::string{});
        EXPECT_TRUE(text.starts_with("HTTP/1.1 200 "));
        const auto fields = head_fields(text);
        EXPECT_TRUE(has_field(fields, "content-encoding: gzip"));
        EXPECT_TRUE(!has_field(fields, "accept-ranges:"));
        if (test.expected_.has_value()) {
            EXPECT_TRUE(has_field(fields, *test.expected_));
        } else {
            EXPECT_TRUE(!has_field(fields, "etag:"));
        }
    }

    // Identity keeps both fields untouched.
    const auto identity = fetch_response(server.endpoint(),
        get_request("/compression-validators?etag=%22v1%22", "Accept-Encoding: identity\r\n"))
                              .value_or(std::string{});
    EXPECT_TRUE(has_field(head_fields(identity), "etag: \"v1\""));
    EXPECT_TRUE(has_field(head_fields(identity), "accept-ranges: bytes"));
    server.finish();
}

// One allowlist decides compressibility: already-compressed and unknown media
// types, and a response without Content-Type, stay identity.
TEST(Ruvia, http1_compression_encodes_only_compressible_media_types) {
    using namespace http1_response_compression_test;
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io, compressing_server());
    const struct {
        std::string_view query_;
        bool encoded_;
    } cases[] = {
        {"application%2Fjson", true},
        {"application%2Fld%2Bjson", true},
        {"image%2Fsvg%2Bxml", true},
        {"font%2Fwoff2", false},
        {"application%2Fepub%2Bzip", false},
        {"application%2Fx-7z-compressed", false},
        {"application%2Fx-unknown", false},
        {"", false},
    };
    for (const auto& test : cases) {
        const auto text = fetch_response(server.endpoint(),
            get_request("/compression-media?type=" + std::string(test.query_), "Accept-Encoding: gzip\r\n"))
                              .value_or(std::string{});
        EXPECT_TRUE(text.starts_with("HTTP/1.1 200 "));
        EXPECT_EQ(has_field(head_fields(text), "content-encoding: gzip"), test.encoded_);
        if (!test.encoded_) {
            EXPECT_TRUE(response_body(text) == small_body);
        }
    }
    server.finish();
}

// RFC 9110 §12.5.5: an application-encoded body is sent or refused (406)
// depending on Accept-Encoding, so the 200 varies on it like a framework coding.
TEST(Ruvia, http1_application_encoded_responses_vary_on_accept_encoding) {
    using namespace http1_response_compression_test;
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io, compressing_server());
    for (const std::string_view path :
        {std::string_view("/compression-custom-coding"), std::string_view("/compression-stream-custom")}) {
        const auto text = fetch_response(server.endpoint(), get_request(path, "Accept-Encoding: x-custom\r\n"))
                              .value_or(std::string{});
        EXPECT_TRUE(text.starts_with("HTTP/1.1 200 "));
        const auto fields = head_fields(text);
        EXPECT_TRUE(has_field(fields, "content-encoding: x-custom"));
        EXPECT_TRUE(has_field(fields, "vary: accept-encoding"));
    }
    server.finish();
}

// RFC 9110 §8.4 / §14.6: a multipart/byteranges body cannot carry the coding of
// the representation it slices, so several ranges of a precompressed variant
// are answered with the full coded representation; one range stays a 206.
TEST(Ruvia, http1_precompressed_variant_answers_multiple_ranges_in_full) {
    using namespace http1_response_compression_test;
    namespace fs = std::filesystem;
    const ruvia::test::scoped_temp_directory directory("ruvia-response-compression-coded");
    const auto& dir = directory.path();
    const std::string sidecar("gz-sidecar-bytes");
    std::ofstream(dir / "asset.txt", std::ios::binary) << "plain-asset-body";
    std::ofstream(dir / "asset.txt.gz", std::ios::binary) << sidecar;
    coded_root = std::make_unique<ruvia::static_root>(dir,
        ruvia::static_root_options{
            .file_types_ = {.kind_ = ruvia::static_file_type_policy::kind::all},
            .range_requests_ = ruvia::static_range_request_policy::honor,
        });

    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io, compressing_server());
    const auto multi = fetch_response(server.endpoint(),
        get_request("/compression-coded-static", "Accept-Encoding: gzip\r\nRange: bytes=0-1,4-5\r\n"))
                           .value_or(std::string{});
    EXPECT_TRUE(multi.starts_with("HTTP/1.1 200 "));
    const auto multi_fields = head_fields(multi);
    EXPECT_TRUE(has_field(multi_fields, "content-encoding: gzip"));
    EXPECT_TRUE(!has_field(multi_fields, "content-type: multipart/byteranges"));
    EXPECT_TRUE(response_body(multi) == sidecar);

    const auto single = fetch_response(server.endpoint(),
        get_request("/compression-coded-static", "Accept-Encoding: gzip\r\nRange: bytes=0-1\r\n"))
                            .value_or(std::string{});
    EXPECT_TRUE(single.starts_with("HTTP/1.1 206 "));
    const auto single_fields = head_fields(single);
    EXPECT_TRUE(has_field(single_fields, "content-encoding: gzip"));
    EXPECT_TRUE(has_field(single_fields, "content-range: bytes 0-1/" + std::string(std::to_string(sidecar.size()))));
    EXPECT_TRUE(response_body(single) == "gz");

    // RFC 9110 §15.5.17: the empty 416 does not describe coded or typed content.
    for (const std::string_view coding : {"gzip", "gzip, identity;q=0"}) {
        const auto unsatisfied = fetch_response(server.endpoint(),
            get_request("/compression-coded-static",
                "Accept-Encoding: " + std::string(coding) + "\r\nRange: bytes=100-200\r\n"))
                                     .value_or(std::string{});
        EXPECT_TRUE(unsatisfied.starts_with("HTTP/1.1 416 "));
        const auto unsatisfied_fields = head_fields(unsatisfied);
        EXPECT_TRUE(!has_field(unsatisfied_fields, "content-encoding:"));
        EXPECT_TRUE(!has_field(unsatisfied_fields, "content-type:"));
        EXPECT_TRUE(has_field(unsatisfied_fields, "content-range: bytes */" + std::string(std::to_string(sidecar.size()))));
        EXPECT_TRUE(has_field(unsatisfied_fields, "accept-ranges: bytes"));
        EXPECT_TRUE(has_field(unsatisfied_fields, "etag:"));
        EXPECT_TRUE(has_field(unsatisfied_fields, "vary: accept-encoding"));
    }

    // The identity representation still answers several ranges with multipart.
    const auto identity = fetch_response(server.endpoint(),
        get_request("/compression-coded-static", "Accept-Encoding: identity\r\nRange: bytes=0-1,4-5\r\n"))
                              .value_or(std::string{});
    EXPECT_TRUE(identity.starts_with("HTTP/1.1 206 "));
    EXPECT_TRUE(has_field(head_fields(identity), "content-type: multipart/byteranges"));
    EXPECT_TRUE(identity.find("\r\nContent-Encoding:") == std::string::npos);
    server.finish();
    coded_root.reset();
}

// RFC 9110 §8.6: a HEAD response's Content-Length equals the GET content length
// or is absent. An explicit HEAD route that only declares the GET length must
// get GET's coding without the encoded length of its own empty body.
TEST(Ruvia, http1_explicit_head_route_matches_get_coding_without_false_length) {
    using namespace http1_response_compression_test;
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io, compressing_server());
    for (const std::string_view fields : {"Accept-Encoding: gzip\r\n", "Accept-Encoding: gzip, identity;q=0\r\n"}) {
        const auto get = fetch_response(server.endpoint(), get_request("/compression-head", fields)).value_or(std::string{});
        EXPECT_TRUE(has_field(head_fields(get), "content-encoding: gzip"));
        const auto head = fetch_response(server.endpoint(), head_request("/compression-head", fields)).value_or(std::string{});
        EXPECT_TRUE(head.starts_with("HTTP/1.1 200 "));
        const auto head_line_fields = head_fields(head);
        EXPECT_TRUE(has_field(head_line_fields, "content-encoding: gzip"));
        EXPECT_TRUE(!has_field(head_line_fields, "content-length:"));
        EXPECT_TRUE(response_body(head).empty());
    }
    // Without a declared length, identity stays and no length is claimed.
    const auto bare =
        fetch_response(server.endpoint(), head_request("/compression-head-bare", "Accept-Encoding: gzip\r\n"))
            .value_or(std::string{});
    EXPECT_TRUE(bare.starts_with("HTTP/1.1 200 "));
    EXPECT_TRUE(!has_field(head_fields(bare), "content-encoding:"));
    EXPECT_TRUE(!has_field(head_fields(bare), "content-length:"));
    const auto refused = fetch_response(server.endpoint(),
        head_request("/compression-head-bare", "Accept-Encoding: gzip, identity;q=0\r\n"))
                             .value_or(std::string{});
    EXPECT_TRUE(refused.starts_with("HTTP/1.1 200 "));
    EXPECT_TRUE(has_field(head_fields(refused), "content-encoding: gzip"));
    EXPECT_TRUE(!has_field(head_fields(refused), "content-length:"));
    // Identity accepted with an identity GET keeps the declared length.
    const auto identity =
        fetch_response(server.endpoint(), head_request("/compression-head", "Accept-Encoding: identity\r\n"))
            .value_or(std::string{});
    EXPECT_TRUE(has_field(head_fields(identity), "content-length: " + std::string(std::to_string(small_body.size()))));
    server.finish();
}

TEST(Ruvia, http1_head_fallback_on_empty_get_files_and_redirects_matches_get_headers) {
    using namespace http1_response_compression_test;
    namespace fs = std::filesystem;
    std::error_code ignored;
    fs::remove_all(file_dir(), ignored);
    fs::create_directories(file_dir());
    {
        std::ofstream(file_dir() / "zero.txt", std::ios::binary);
        std::ofstream(file_dir() / "file.txt", std::ios::binary) << small_body;
    }
    resident_root = std::make_unique<ruvia::static_root>(file_dir(),
        ruvia::static_root_options{
            .file_types_ = {.kind_ = ruvia::static_file_type_policy::kind::all},
            .resident_max_bytes_ = 16 * 1024,
            .resident_budget_bytes_ = 16 * 1024,
        });

    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io, compressing_server());

    // 1. HEAD fallback on 0-byte GET route matches GET under both identity and refused identity.
    for (const std::string_view fields :
        {std::string_view("Accept-Encoding: identity\r\n"),
            std::string_view("Accept-Encoding: gzip, identity;q=0\r\n")}) {
        const auto get = fetch_response(server.endpoint(), get_request("/compression-empty-get", fields)).value_or(std::string{});
        const auto head = fetch_response(server.endpoint(), head_request("/compression-empty-get", fields)).value_or(std::string{});
        EXPECT_TRUE(get.starts_with("HTTP/1.1 200 "));
        EXPECT_TRUE(head.starts_with("HTTP/1.1 200 "));
        EXPECT_TRUE(head_fields(get) == head_fields(head));
        EXPECT_TRUE(response_body(head).empty());
    }

    // 2. 0-byte file, 0-byte static_file, and redirect emit Content-Length: 0 on both GET and HEAD.
    for (const std::string_view path :
        {std::string_view("/compression-zero-file"), std::string_view("/compression-zero-static"),
            std::string_view("/compression-redirect")}) {
        const auto get = fetch_response(server.endpoint(), get_request(path, "")).value_or(std::string{});
        const auto head = fetch_response(server.endpoint(), head_request(path, "")).value_or(std::string{});
        EXPECT_TRUE(head_fields(get) == head_fields(head));
        EXPECT_TRUE(has_field(head_fields(head), "content-length: 0"));
        EXPECT_TRUE(response_body(head).empty());
    }

    // 3. Explicit HEAD route with comma-separated identical Content-Length ("3000, 3000").
    const auto comma_head = fetch_response(
        server.endpoint(), head_request("/compression-head-comma", "Accept-Encoding: gzip\r\n"))
                                .value_or(std::string{});
    EXPECT_TRUE(comma_head.starts_with("HTTP/1.1 200 "));
    EXPECT_TRUE(has_field(head_fields(comma_head), "content-encoding: gzip"));
    EXPECT_TRUE(!has_field(head_fields(comma_head), "content-length:"));

    // 4. Explicit HEAD route with Content-Length: 0 keeps Content-Length: 0 when identity is accepted.
    const auto zero_declared_get = fetch_response(
        server.endpoint(), get_request("/compression-head-zero", "Accept-Encoding: gzip\r\n"))
                                       .value_or(std::string{});
    const auto zero_declared_head = fetch_response(
        server.endpoint(), head_request("/compression-head-zero", "Accept-Encoding: gzip\r\n"))
                                        .value_or(std::string{});
    EXPECT_TRUE(head_fields(zero_declared_get) == head_fields(zero_declared_head));
    EXPECT_TRUE(has_field(head_fields(zero_declared_head), "content-length: 0"));
    for (const std::string_view path :
        {std::string_view("/compression-head-zero"), std::string_view("/compression-zero-declared")}) {
        const auto refused_zero_get = fetch_response(
            server.endpoint(), get_request(path, "Accept-Encoding: gzip, identity;q=0\r\n"))
                                          .value_or(std::string{});
        const auto refused_zero_head = fetch_response(
            server.endpoint(), head_request(path, "Accept-Encoding: gzip, identity;q=0\r\n"))
                                           .value_or(std::string{});
        EXPECT_TRUE(head_fields(refused_zero_get) == head_fields(refused_zero_head));
        EXPECT_TRUE(has_field(head_fields(refused_zero_head), "content-encoding: gzip"));
        EXPECT_TRUE(has_field(head_fields(refused_zero_head), "content-length: 20"));
    }

    // 5. Custom Content-Encoding negotiation and mixed rejected coding stacks.
    const auto custom_ok = fetch_response(server.endpoint(),
        get_request("/compression-custom-coding", "Accept-Encoding: x-custom, identity;q=0\r\n"))
                               .value_or(std::string{});
    EXPECT_TRUE(custom_ok.starts_with("HTTP/1.1 200 "));
    EXPECT_TRUE(has_field(head_fields(custom_ok), "content-encoding: x-custom"));

    const auto mixed_rejected = fetch_response(
        server.endpoint(), get_request("/compression-mixed-rejected", "Accept-Encoding: br;q=0\r\n"))
                                    .value_or(std::string{});
    EXPECT_TRUE(mixed_rejected.starts_with("HTTP/1.1 406 "));
    const auto stream_custom_ok = fetch_response(server.endpoint(),
        get_request("/compression-stream-custom", "Accept-Encoding: x-custom, identity;q=0\r\n"))
                                      .value_or(std::string{});
    EXPECT_TRUE(stream_custom_ok.starts_with("HTTP/1.1 200 "));
    EXPECT_TRUE(has_field(head_fields(stream_custom_ok), "content-encoding: x-custom"));

    const auto stream_custom_rejected = fetch_response(server.endpoint(),
        get_request("/compression-stream-custom", "Accept-Encoding: gzip\r\n"))
                                            .value_or(std::string{});
    EXPECT_TRUE(stream_custom_rejected.starts_with("HTTP/1.1 406 "));

    const auto stream_mixed_rejected = fetch_response(server.endpoint(),
        get_request("/compression-stream-mixed", "Accept-Encoding: br, x-custom;q=0\r\n"))
                                           .value_or(std::string{});
    EXPECT_TRUE(stream_mixed_rejected.starts_with("HTTP/1.1 406 "));

    const auto stream_204 = fetch_response(server.endpoint(),
        get_request("/compression-stream-204", "Accept-Encoding: *;q=0\r\n"))
                                .value_or(std::string{});
    EXPECT_TRUE(stream_204.starts_with("HTTP/1.1 204 "));

    const auto stream_304 = fetch_response(server.endpoint(),
        get_request("/compression-stream-304", "Accept-Encoding: *;q=0\r\n"))
                                .value_or(std::string{});
    EXPECT_TRUE(stream_304.starts_with("HTTP/1.1 304 "));

    const auto range_416_custom = fetch_response(server.endpoint(),
        get_request("/compression-file",
            "Range: bytes=10000-20000\r\nAccept-Encoding: x-custom, identity;q=0\r\n"))
                                      .value_or(std::string{});
    EXPECT_TRUE(range_416_custom.starts_with("HTTP/1.1 416 "));

    // 6. Request Content-Encoding with >8 identity tokens does not fail with 415.
    const auto id9 = fetch_response(server.endpoint(),
        "POST /compression-echo-decoded HTTP/1.1\r\nHost: localhost\r\n"
        "Content-Encoding: identity, identity, identity, identity, identity, identity, identity, identity, identity\r\n"
        "Content-Length: 5\r\nConnection: close\r\n\r\nhello")
                         .value_or(std::string{});
    EXPECT_TRUE(id9.starts_with("HTTP/1.1 200 "));
    EXPECT_TRUE(response_body(id9) == "hello");

    const auto gz_hello = *ruvia::encode_http_content(
        ruvia::http_content_coding::gzip, "hello", {.max_encoded_bytes_ = 1024});
    const auto id8_gz = fetch_response(server.endpoint(),
        "POST /compression-echo-decoded HTTP/1.1\r\nHost: localhost\r\n"
        "Content-Encoding: identity, identity, identity, identity, identity, identity, identity, identity, gzip\r\n"
        "Content-Length: " +
            std::string(std::to_string(gz_hello.size())) + "\r\nConnection: close\r\n\r\n" + std::string(gz_hello))
                            .value_or(std::string{});
    EXPECT_TRUE(id8_gz.starts_with("HTTP/1.1 200 "));
    EXPECT_TRUE(response_body(id8_gz) == "hello");

    server.finish();
    resident_root.reset();
    fs::remove_all(file_dir(), ignored);
}

// Application-wide response policy (CORS, access log, session commit) on the
// heads that do not pass through the buffered response tail: response streams
// and CONNECT/WebSocket handshakes. This executable owns its application, so
// the app-wide CORS policy and access-log hook cannot leak into other suites.

namespace http1_response_policy_test {

struct access_entry final {
    std::string method_{};
    std::string path_{};
    ruvia::http_status_code status_;
};

std::mutex access_mutex;
std::vector<access_entry> access_entries{};

// Every fixture passes the same configuration: whichever case runs first
// fixes the shared application's server_config.
[[nodiscard]] ruvia::server_config cors_server() {
    return {.worker_count_ = 1,
        .connection_scan_interval_ = std::chrono::milliseconds(5),
        .cors_ = ruvia::http_cors_policy_config{}};
}

void install_policy_access_logger() {
    ruvia::app().on_access(nullptr).on_access([](const ruvia::access_log_record& record) noexcept {
        try {
            std::lock_guard lock(access_mutex);
            access_entries.push_back(
                {std::string(record.method()), std::string(record.path()), record.status()});
        } catch (...) {
        }
    });
}

[[nodiscard]] std::vector<access_entry> take_access_entries() {
    std::lock_guard lock(access_mutex);
    return std::exchange(access_entries, std::vector<access_entry>{});
}

// Rejects the CONNECT before the tunnel handshake is committed.
class connect_denying_middleware final {
public:
    ruvia::task<void> invoke(ruvia::context&, ruvia::next&) {
        throw ruvia::http_error({.status_ = ruvia::http_status::forbidden, .message_ = "denied"});
        co_return;
    }
};

#ifdef RUVIA_ENABLE_REDIS
// Clears the session bound by the enclosing session_middleware, then lets the
// route continue. No cookie was presented, so the commit touches no Redis key.
class session_clearing_middleware final {
public:
    ruvia::task<void> invoke(ruvia::context& c, ruvia::next& next_value) {
        c.session().clear();
        co_await next_value();
    }
};
#endif

std::atomic<int> head_loop_writes{0};
std::atomic<bool> head_loop_aborted{false};
std::atomic<bool> head_loop_sleep_rejected{false};
std::atomic<bool> head_loop_token_stopped{false};
std::atomic<bool> head_loop_completed{false};
std::atomic<bool> naive_loop_failed{false};
std::atomic<bool> naive_loop_finished{false};
std::atomic<bool> read_after_write_failed{false};
std::atomic<bool> read_after_write_finished{false};
std::mutex read_after_write_mutex;
std::string read_after_write_body{};

// Turns every exception from the rest of the chain into a 500, as
// error-wrapping middleware does.
class exception_converting_middleware final {
public:
    ruvia::task<void> invoke(ruvia::context&, ruvia::next& next_value) {
        try {
            co_await next_value();
        } catch (...) {
            throw ruvia::http_error({.status_ = ruvia::http_status::internal_server_error, .message_ = "converted"});
        }
    }
};

class response_policy_routes final {
    RUVIA_ROUTES_BEGIN(response_policy_routes)
    RUVIA_GET("/policy-stream", stream);
    RUVIA_GET("/policy-secure-stream", stream, ruvia::security_headers_middleware);
    RUVIA_GET("/policy-trailer-stream", trailer_stream);
    RUVIA_GET("/policy-plain", plain);
    RUVIA_GET("/policy-head-loop", head_loop);
    RUVIA_GET("/policy-head-naive", head_naive);
    RUVIA_GET("/policy-head-read-after-write", head_read_after_write);
    RUVIA_GET("/policy-head-convert", stream, exception_converting_middleware);
    RUVIA_GET("/policy-stream-fail-after-commit", stream_fail_after_commit, exception_converting_middleware);
    RUVIA_GET_WS("/policy-websocket", websocket);
    RUVIA_CONNECT("policy-target.test:443", tunnel);
    RUVIA_CONNECT("denied-target.test:443", tunnel, connect_denying_middleware);
#ifdef RUVIA_ENABLE_REDIS
    RUVIA_CONNECT("session-target.test:443", tunnel, (ruvia::http_tunnel_route_config{}),
        ruvia::session_middleware, session_clearing_middleware);
#endif
    RUVIA_ROUTES_END

    ruvia::task<void> stream(ruvia::context& context_value) {
        if (context_value.req().query("fail").has_value()) {
            context_value.draft().header("Content-Range", "bytes 0-2/10");
            throw ruvia::http_error({.status_ = ruvia::http_status::conflict});
        }
        if (co_await context_value.stream().commit() == ruvia::stream_commit_result::completed) {
            co_return;
        }
        co_await context_value.stream().write("chunk");
    }

    ruvia::task<void> stream_fail_after_commit(ruvia::context& context_value) {
        auto& output = context_value.stream_text();
        if (co_await output.commit() == ruvia::stream_commit_result::completed) {
            co_return;
        }
        if (!context_value.committed() || !output.committed()) {
            co_return;
        }
        co_await output.write("partial");
        throw std::runtime_error("failed after stream commit");
    }

    // 完成态不是取消：不启动正文生产，误用 write/sleep 必须明确拒绝。
    ruvia::task<void> head_loop(ruvia::context& context_value) {
        if (context_value.req().query("not-modified").has_value()) {
            context_value.draft().status(ruvia::http_status::not_modified);
        }
        auto& output = context_value.stream_text();
        const auto token = context_value.stop_token();
        const auto phase = co_await output.commit();
        if (phase == ruvia::stream_commit_result::body) {
            co_await output.write("tick");
            head_loop_writes.store(1);
        } else {
            head_loop_writes.store(-1);
            try {
                co_await output.write("late");
            } catch (const std::logic_error&) {
                head_loop_writes.store(0);
            }
            try {
                static_cast<void>(co_await output.sleep(std::chrono::seconds(10)));
            } catch (const std::logic_error&) {
                head_loop_sleep_rejected.store(true);
            }
        }
        head_loop_aborted.store(output.aborted());
        head_loop_token_stopped.store(token.stop_requested());
        co_await output.end();
        head_loop_completed.store(true);
    }

    // 即使没有轮询 aborted，显式提交结果也在循环开始前结束 HEAD。
    ruvia::task<void> head_naive(ruvia::context& context_value) {
        auto& output = context_value.stream_text();
        if (co_await output.commit() == ruvia::stream_commit_result::completed) {
            naive_loop_finished.store(true);
            co_return;
        }
        try {
            for (int i = 0; i < 1000; ++i) {
                co_await output.write("tick");
                static_cast<void>(co_await ruvia::sleep_for(context_value.worker(), std::chrono::milliseconds(10)));
            }
        } catch (...) {
            naive_loop_failed.store(true);
            naive_loop_finished.store(true);
            throw;
        }
        naive_loop_finished.store(true);
    }

    ruvia::task<void> head_read_after_write(ruvia::context& context_value) {
        auto& output = context_value.stream_text();
        try {
            const auto phase = co_await output.commit();
            if (phase == ruvia::stream_commit_result::body) {
                co_await output.write("first");
            }
            const auto body = co_await context_value.req().text();
            {
                std::lock_guard lock(read_after_write_mutex);
                read_after_write_body.assign(body);
            }
            if (phase == ruvia::stream_commit_result::body) {
                co_await output.write("tick");
            }
        } catch (...) {
            read_after_write_failed.store(true);
            read_after_write_finished.store(true);
            throw;
        }
        read_after_write_finished.store(true);
    }

    ruvia::task<void> trailer_stream(ruvia::context& context_value) {
        auto& output = context_value.stream_text();
        context_value.draft().header("Trailer", "X-Complete");
        if (co_await output.commit() == ruvia::stream_commit_result::completed) {
            co_return;
        }
        co_await output.write("body");
        const std::array<ruvia::http_header_view, 1> trailers{{{"X-Complete", "yes"}}};
        co_await output.end(trailers);
    }

    ruvia::task<ruvia::http_response> plain(ruvia::context& context_value) {
        co_return context_value.text("plain");
    }

    ruvia::task<void> websocket(ruvia::context&) {
        co_return;
    }

    ruvia::task<void> tunnel(ruvia::context& context_value) {
        co_await context_value.tunnel().finish();
    }
};

// Sends `request` and returns every byte the server wrote before closing, or
// nullopt when the connection is still open after 5 s. Responses end with the
// connection (Connection: close, HTTP/1.0, or a failed CONNECT). Only an
// upgraded exchange -- a 101 or a CONNECT 2xx -- half-closes once its head has
// arrived, so that the tunnel or WebSocket ends; any other early FIN would
// read as the client leaving a streamed response still in flight.
[[nodiscard]] std::optional<std::string> exchange(
    const asio::ip::tcp::endpoint& server_endpoint, std::string_view request) {
    auto& io = ruvia::test::new_test_io_context();
    asio::ip::tcp::socket socket(io);
    socket.connect(server_endpoint);
    asio::write(socket, asio::buffer(request));
    std::string output{};
    bool closed = false;
    asio::async_read_until(
        socket, asio::dynamic_buffer(output), "\r\n\r\n", [&](const std::error_code& error, std::size_t) {
            if (error) {
                closed = true;
                return;
            }
            const bool upgraded = output.starts_with("HTTP/1.1 101 ") ||
                                  (request.starts_with("CONNECT ") && output.starts_with("HTTP/1.1 2"));
            if (upgraded) {
                std::error_code ignored;
                socket.shutdown(asio::ip::tcp::socket::shutdown_send, ignored);
            }
            asio::async_read(socket, asio::dynamic_buffer(output),
                [&closed](const std::error_code&, std::size_t) { closed = true; });
        });
    io.run_for(std::chrono::seconds(5));
    if (!closed) {
        return std::nullopt;
    }
    return output;
}

// Field names are case-insensitive; compare the head in lower case.
[[nodiscard]] std::string lower_head(std::string_view response) {
    std::string head(response.substr(0, response.find("\r\n\r\n")));
    std::ranges::transform(head, head.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return head;
}

}  // namespace http1_response_policy_test

TEST(Ruvia, http1_response_stream_head_carries_the_application_cors_policy) {
    using namespace http1_response_policy_test;
    ruvia::test::reset_app_callbacks(ruvia::app());
    install_policy_access_logger();
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io, cors_server());
    const auto response = http1_response_policy_test::exchange(server.endpoint(),
        "GET /policy-stream HTTP/1.1\r\nHost: localhost\r\nOrigin: https://client.test\r\nConnection: close\r\n\r\n");
    server.finish();
    EXPECT_TRUE(response.has_value());
    const auto output = (response ? std::string_view(*response) : std::string_view{});
    EXPECT_TRUE(output.starts_with("HTTP/1.1 200 "));
    EXPECT_TRUE(lower_head(output).find("\r\naccess-control-allow-origin: *") != std::string::npos);
    EXPECT_TRUE(output.find("chunk") != std::string::npos);
}
TEST(Ruvia, http1_cors_reflection_preserves_many_requested_names_in_one_field) {
    using namespace http1_response_policy_test;
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io, cors_server());
    std::string names{};
    for (unsigned i = 0; i != 128; ++i) {
        if (!names.empty()) {
            names.append(", ");
        }
        names.append("x-name-").append(std::to_string(i));
    }
    const auto response = http1_response_policy_test::exchange(server.endpoint(),
        std::string("OPTIONS /policy-stream HTTP/1.1\r\nHost: localhost\r\n"
                    "Origin: https://client.test\r\nAccess-Control-Request-Method: GET\r\n"
                    "Access-Control-Request-Headers: ") +
            names +
            "\r\nAccess-Control-Request-Headers: x-final\r\nConnection: close\r\n\r\n");
    server.finish();
    EXPECT_TRUE(response.has_value());
    const auto output = (response ? std::string_view(*response) : std::string_view{});
    EXPECT_TRUE(output.starts_with("HTTP/1.1 204 "));
    const auto head = lower_head(output);
    const auto expected = std::string("\r\naccess-control-allow-headers: ") + names + ", x-final\r\n";
    EXPECT_TRUE(head.find(expected) != std::string::npos);
    const auto first = head.find("\r\naccess-control-allow-headers:");
    EXPECT_TRUE(first != std::string::npos &&
                head.find("\r\naccess-control-allow-headers:", first + 1) == std::string::npos);
}

TEST(Ruvia, http1_cors_requires_options_origin_and_requested_method_for_preflight) {
    using namespace http1_response_policy_test;
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io, cors_server());
    for (const auto& [method, fields] : {
             std::array<std::string_view, 2>{"OPTIONS", "Origin: https://client.test\r\n"},
             {"OPTIONS", "Access-Control-Request-Method: GET\r\n"},
             {"GET", "Origin: https://client.test\r\nAccess-Control-Request-Method: GET\r\n"}}) {
        const auto response = http1_response_policy_test::exchange(server.endpoint(),
            std::string(method) + " /policy-stream HTTP/1.1\r\nHost: localhost\r\n" +
                std::string(fields) + "Access-Control-Request-Headers: x-token\r\nConnection: close\r\n\r\n");
        ASSERT_TRUE(response.has_value());
        const auto& output = *response;
        EXPECT_TRUE(output.starts_with(method == "OPTIONS" ? "HTTP/1.1 204 " : "HTTP/1.1 200 "));
        const auto head = lower_head(output);
        EXPECT_TRUE(head.find("\r\naccess-control-allow-origin: *\r\n") != std::string::npos) << output;
        EXPECT_TRUE(head.find("\r\naccess-control-allow-methods:") == std::string::npos) << output;
        EXPECT_TRUE(head.find("\r\naccess-control-allow-headers:") == std::string::npos) << output;
    }
    server.finish();
}

TEST(Ruvia, http1_cors_invalid_metadata_never_reflects_a_valid_prefix) {
    using namespace http1_response_policy_test;
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io, cors_server());
    for (const auto& [origin, method, names] : {
             std::array<std::string_view, 3>{"https://client.test/path", "GET", "x-good"},
             {"https://client.test", "BAD METHOD", "x-good"},
             {"https://client.test", "GET", "x-good, bad name"}}) {
        std::string request("OPTIONS /policy-stream HTTP/1.1\r\nHost: localhost\r\nOrigin: ");
        request.append(origin).append("\r\nAccess-Control-Request-Method: ").append(method).append("\r\nAccess-Control-Request-Headers: ").append(names).append("\r\nConnection: close\r\n\r\n");
        const auto response = http1_response_policy_test::exchange(server.endpoint(), request);
        EXPECT_TRUE(response.has_value());
        const auto output = (response ? std::string_view(*response) : std::string_view{});
        EXPECT_TRUE(output.starts_with("HTTP/1.1 204 "));
        const auto head = lower_head(output);
        EXPECT_TRUE(head.find("\r\naccess-control-allow-origin:") == std::string::npos);
        EXPECT_TRUE(head.find("\r\naccess-control-allow-methods:") == std::string::npos);
        EXPECT_TRUE(head.find("\r\naccess-control-allow-headers:") == std::string::npos);
    }
    server.finish();
}

// Trailers are optional metadata: an HTTP/1.0 close-delimited stream drops the
// Trailer field and the trailer section instead of failing with 500.
TEST(Ruvia, http1_response_stream_discards_trailers_it_cannot_frame) {
    using namespace http1_response_policy_test;
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io, cors_server());
    const auto http10 = http1_response_policy_test::exchange(server.endpoint(), "GET /policy-trailer-stream HTTP/1.0\r\nHost: localhost\r\n\r\n");
    const auto http11 = http1_response_policy_test::exchange(server.endpoint(),
        "GET /policy-trailer-stream HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
    server.finish();
    EXPECT_TRUE(http10.has_value());
    const auto http10_output = (http10 ? std::string_view(*http10) : std::string_view{});
    EXPECT_TRUE(http10_output.starts_with("HTTP/1.1 200 "));
    EXPECT_TRUE(lower_head(http10_output).find("\r\ntrailer:") == std::string::npos);
    EXPECT_TRUE(http10_output.ends_with("\r\n\r\nbody"));
    EXPECT_TRUE(http11.has_value());
    const auto http11_output = (http11 ? std::string_view(*http11) : std::string_view{});
    EXPECT_TRUE(lower_head(http11_output).find("\r\ntrailer: x-complete") != std::string::npos);
    EXPECT_TRUE(http11_output.ends_with("0\r\nX-Complete: yes\r\n\r\n"));
}

// RFC 9110 9.3.2: HEAD on a response-stream GET route answers like GET
// without content, and the connection stays framed for the next request. A
// websocket GET route is an upgrade, not a representation, so HEAD stays 405.
TEST(Ruvia, http1_head_falls_back_to_response_stream_get_routes) {
    using namespace http1_response_policy_test;
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io, cors_server());
    const auto pipelined = http1_response_policy_test::exchange(server.endpoint(),
        "HEAD /policy-stream HTTP/1.1\r\nHost: localhost\r\nOrigin: https://client.test\r\n\r\n"
        "HEAD /policy-trailer-stream HTTP/1.1\r\nHost: localhost\r\n\r\n"
        "GET /policy-plain HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
    const auto websocket = http1_response_policy_test::exchange(server.endpoint(),
        "HEAD /policy-websocket HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
    server.finish();

    EXPECT_TRUE(pipelined.has_value());
    const auto output = (pipelined ? std::string_view(*pipelined) : std::string_view{});
    EXPECT_TRUE(output.starts_with("HTTP/1.1 200 "));
    EXPECT_TRUE(lower_head(output).find("\r\naccess-control-allow-origin: *") != std::string::npos);
    EXPECT_TRUE(output.find("chunk") == std::string::npos);
    EXPECT_TRUE(output.find("body") == std::string::npos);
    const auto second = output.find("HTTP/1.1 200 ", 1);
    EXPECT_TRUE(second != std::string::npos);
    const auto third = output.find("HTTP/1.1 200 ", second == std::string::npos ? output.size() : second + 1);
    EXPECT_TRUE(third != std::string::npos);
    EXPECT_TRUE(output.ends_with("\r\n\r\nplain"));

    EXPECT_TRUE(websocket.has_value());
    const auto websocket_output = (websocket ? std::string_view(*websocket) : std::string_view{});
    EXPECT_TRUE(websocket_output.starts_with("HTTP/1.1 405 "));
    EXPECT_TRUE(lower_head(websocket_output).find("\r\nallow: get, options") != std::string::npos);
}

// 完成头后不生产正文；非法写/等待明确失败，正常完成仍不是 abort。
TEST(Ruvia, http1_head_only_stream_completes_without_cancellation) {
    using namespace http1_response_policy_test;
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io, cors_server());
    const auto output = http1_response_policy_test::exchange(server.endpoint(),
        "HEAD /policy-head-loop HTTP/1.1\r\nHost: localhost\r\n\r\n"
        "GET /policy-plain HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
    server.finish();

    EXPECT_TRUE(output.has_value());
    const auto text = (output ? std::string_view(*output) : std::string_view{});
    EXPECT_TRUE(text.starts_with("HTTP/1.1 200 "));
    EXPECT_TRUE(text.find("tick") == std::string::npos);
    EXPECT_TRUE(text.ends_with("\r\n\r\nplain"));
    EXPECT_TRUE(text.find("late") == std::string::npos);
    EXPECT_EQ(head_loop_writes.load(), 0);
    EXPECT_TRUE(!head_loop_aborted.load());
    EXPECT_TRUE(!head_loop_token_stopped.load());
    EXPECT_TRUE(head_loop_sleep_rejected.load());
    EXPECT_TRUE(head_loop_completed.load());
}

TEST(Ruvia, http1_not_modified_stream_commit_completes_without_abort) {
    using namespace http1_response_policy_test;
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io, cors_server());
    const auto output = http1_response_policy_test::exchange(server.endpoint(),
        "GET /policy-head-loop?not-modified HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
    server.finish();
    EXPECT_TRUE(output.has_value());
    EXPECT_TRUE((output ? std::string_view(*output) : std::string_view{}).starts_with("HTTP/1.1 304 "));
    EXPECT_TRUE((output ? std::string_view(*output) : std::string_view{}).ends_with("\r\n\r\n"));
    EXPECT_EQ(head_loop_writes.load(), 0);
    EXPECT_TRUE(!head_loop_aborted.load());
    EXPECT_TRUE(!head_loop_token_stopped.load());
}

TEST(Ruvia, http1_security_policy_covers_stream_head_and_precommit_error) {
    using namespace http1_response_policy_test;
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io, cors_server());
    for (const auto request : {
             "GET /policy-secure-stream HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n",
             "HEAD /policy-secure-stream HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n",
             "GET /policy-secure-stream?fail HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n"}) {
        const auto output = http1_response_policy_test::exchange(server.endpoint(), request);
        EXPECT_TRUE(output.has_value());
        EXPECT_TRUE((output ? std::string_view(*output) : std::string_view{}).find("X-Frame-Options: DENY\r\n") != std::string::npos);
        EXPECT_TRUE((output ? std::string_view(*output) : std::string_view{}).find("Content-Range:") == std::string::npos);
    }
    server.finish();
}

// HEAD 在提交时完成，不等待客户端断开，也不启动无限正文循环。
TEST(Ruvia, http1_head_only_producer_finishes_at_commit) {
    using namespace http1_response_policy_test;
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io, cors_server());
    for (const std::string_view request : {
             std::string_view("HEAD /policy-head-naive HTTP/1.1\r\nHost: localhost\r\n\r\n"),
             std::string_view("HEAD /policy-head-naive HTTP/1.1\r\nHost: localhost\r\nContent-Length: 5\r\n\r\nhello"),
         }) {
        naive_loop_failed.store(false);
        naive_loop_finished.store(false);
        {
            asio::ip::tcp::socket socket(io);
            socket.connect(server.endpoint());
            asio::write(socket, asio::buffer(request));
            std::string head{};
            asio::read_until(socket, asio::dynamic_buffer(head), "\r\n\r\n");
            EXPECT_TRUE(head.starts_with("HTTP/1.1 200 "));
            socket.close();
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (!naive_loop_finished.load() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        EXPECT_TRUE(naive_loop_finished.load());
        EXPECT_TRUE(!naive_loop_failed.load());
    }

    read_after_write_failed.store(false);
    read_after_write_finished.store(false);
    {
        std::lock_guard lock(read_after_write_mutex);
        read_after_write_body.clear();
    }
    {
        asio::ip::tcp::socket socket(io);
        socket.connect(server.endpoint());
        asio::write(socket,
            asio::buffer(std::string_view(
                "HEAD /policy-head-read-after-write HTTP/1.1\r\nHost: localhost\r\nContent-Length: 5\r\n\r\n")));
        std::string head{};
        asio::read_until(socket, asio::dynamic_buffer(head), "\r\n\r\n");
        EXPECT_TRUE(head.starts_with("HTTP/1.1 200 "));
        asio::write(socket, asio::buffer(std::string_view("hello")));
        socket.close();
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!read_after_write_finished.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    EXPECT_TRUE(read_after_write_finished.load());
    EXPECT_TRUE(!read_after_write_failed.load());
    {
        std::lock_guard lock(read_after_write_mutex);
        EXPECT_EQ(read_after_write_body, "hello");
    }
    server.finish();
}

// Head-only completion raises nothing: middleware converting every exception
// into a 500 sees a normal success, so HEAD on a stream route completes and
// the connection stays usable for the pipelined GET.
TEST(Ruvia, http1_head_only_stream_is_a_success_for_error_converting_middleware) {
    using namespace http1_response_policy_test;
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io, cors_server());
    const auto output = http1_response_policy_test::exchange(server.endpoint(),
        "HEAD /policy-head-convert HTTP/1.1\r\nHost: localhost\r\n\r\n"
        "GET /policy-plain HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
    server.finish();

    EXPECT_TRUE(output.has_value());
    const auto text = (output ? std::string_view(*output) : std::string_view{});
    EXPECT_TRUE(text.starts_with("HTTP/1.1 200 "));
    EXPECT_TRUE(text.find("chunk") == std::string::npos);
    EXPECT_TRUE(text.find("HTTP/1.1 500 ") == std::string::npos);
    EXPECT_TRUE(text.ends_with("\r\n\r\nplain"));
}

TEST(Ruvia, http1_stream_failure_after_commit_is_not_converted_to_buffered_response_by_middleware) {
    using namespace http1_response_policy_test;
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io, cors_server());
    const auto output = http1_response_policy_test::exchange(server.endpoint(),
        "GET /policy-stream-fail-after-commit HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
    server.finish();
    EXPECT_TRUE(output.has_value());
    const auto text = (output ? std::string_view(*output) : std::string_view{});
    EXPECT_TRUE(text.starts_with("HTTP/1.1 200 "));
    EXPECT_TRUE(text.find("partial") != std::string::npos);
    EXPECT_TRUE(text.find("0\r\n\r\n") == std::string::npos);
}

// A failed CONNECT closes the connection: bytes the client already sent for
// the tunnel are never parsed as another request (RFC 9110 9.3.6).
TEST(Ruvia, http1_failed_connect_closes_instead_of_parsing_tunnel_bytes) {
    using namespace http1_response_policy_test;
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io, cors_server());
    for (const std::string_view authority : {"unrouted-target.test:443", "denied-target.test:443"}) {
        const std::string request(std::string("CONNECT ") + std::string(authority) + " HTTP/1.1\r\nHost: " +
                                  std::string(authority) + "\r\n\r\nGET /policy-plain HTTP/1.1\r\nHost: smuggled\r\n\r\n");
        const auto output = http1_response_policy_test::exchange(server.endpoint(), request);
        EXPECT_TRUE(output.has_value());
        const auto text = (output ? std::string_view(*output) : std::string_view{});
        EXPECT_TRUE(!text.starts_with("HTTP/1.1 2"));
        EXPECT_TRUE(lower_head(text).find("\r\nconnection: close") != std::string::npos);
        EXPECT_TRUE(text.find("HTTP/1.1 ", 1) == std::string::npos);
    }
    server.finish();
}

// TE without the TE connection option only fails to advertise trailers; the
// request itself is served.
TEST(Ruvia, http1_te_without_connection_option_is_ignored) {
    using namespace http1_response_policy_test;
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io, cors_server());
    const auto output = http1_response_policy_test::exchange(server.endpoint(),
        "GET /policy-plain HTTP/1.1\r\nTE: trailers\r\nHost: localhost\r\nConnection: close\r\n\r\n");
    server.finish();
    EXPECT_TRUE(output.has_value());
    EXPECT_TRUE((output ? std::string_view(*output) : std::string_view{}).starts_with("HTTP/1.1 200 "));
}

TEST(Ruvia, http1_successful_upgrades_are_access_logged_once_with_their_handshake_status) {
    using namespace http1_response_policy_test;
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io, cors_server());
    static_cast<void>(take_access_entries());

    const auto tunnel = http1_response_policy_test::exchange(server.endpoint(),
        "CONNECT policy-target.test:443 HTTP/1.1\r\nHost: policy-target.test:443\r\n\r\n");
    EXPECT_TRUE(tunnel.has_value());
    EXPECT_TRUE((tunnel ? std::string_view(*tunnel) : std::string_view{}).starts_with("HTTP/1.1 200 "));

    const auto websocket = http1_response_policy_test::exchange(server.endpoint(),
        "GET /policy-websocket HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Connection: Upgrade\r\n"
        "Upgrade: websocket\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n");
    EXPECT_TRUE(websocket.has_value());
    EXPECT_TRUE((websocket ? std::string_view(*websocket) : std::string_view{}).starts_with("HTTP/1.1 101 "));
    server.finish();

    const auto entries = take_access_entries();
    EXPECT_EQ(entries.size(), std::size_t{2});
    if (entries.size() == 2) {
        EXPECT_EQ(entries[0].method_, std::string("CONNECT"));
        EXPECT_TRUE(entries[0].status_ == ruvia::http_status::ok);
        EXPECT_EQ(entries[1].method_, std::string("GET"));
        EXPECT_EQ(entries[1].path_, std::string("/policy-websocket"));
        EXPECT_TRUE(entries[1].status_ == ruvia::http_status::switching_protocols);
    }
}

// Pipelined responses may leave in one coalesced write, after the requests
// that produced them were already replaced by later ones; each still gets
// exactly one record, in request order, with its own method, path and status.
TEST(Ruvia, http1_pipelined_responses_are_access_logged_in_order) {
    using namespace http1_response_policy_test;
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io, cors_server());
    static_cast<void>(take_access_entries());

    const auto output = http1_response_policy_test::exchange(server.endpoint(),
        "GET /policy-plain HTTP/1.1\r\nHost: localhost\r\n\r\n"
        "HEAD /policy-plain HTTP/1.1\r\nHost: localhost\r\n\r\n"
        "GET /policy-missing HTTP/1.1\r\nHost: localhost\r\n\r\n"
        "GET /policy-plain HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
    server.finish();
    EXPECT_TRUE(output.has_value());
    const auto text = (output ? std::string_view(*output) : std::string_view{});
    EXPECT_TRUE(text.starts_with("HTTP/1.1 200 "));
    EXPECT_TRUE(text.ends_with("plain"));

    const auto entries = take_access_entries();
    EXPECT_EQ(entries.size(), std::size_t{4});
    if (entries.size() == 4) {
        EXPECT_EQ(entries[0].method_, std::string("GET"));
        EXPECT_EQ(entries[0].path_, std::string("/policy-plain"));
        EXPECT_TRUE(entries[0].status_ == ruvia::http_status::ok);
        EXPECT_EQ(entries[1].method_, std::string("HEAD"));
        EXPECT_EQ(entries[1].path_, std::string("/policy-plain"));
        EXPECT_TRUE(entries[1].status_ == ruvia::http_status::ok);
        EXPECT_EQ(entries[2].method_, std::string("GET"));
        EXPECT_EQ(entries[2].path_, std::string("/policy-missing"));
        EXPECT_TRUE(entries[2].status_ == ruvia::http_status::not_found);
        EXPECT_EQ(entries[3].path_, std::string("/policy-plain"));
        EXPECT_TRUE(entries[3].status_ == ruvia::http_status::ok);
    }
}

#ifdef RUVIA_ENABLE_REDIS
TEST(Ruvia, http1_connect_handshake_carries_the_committed_session_cookie) {
    using namespace http1_response_policy_test;
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io, cors_server());
    const auto tunnel = http1_response_policy_test::exchange(server.endpoint(),
        "CONNECT session-target.test:443 HTTP/1.1\r\nHost: session-target.test:443\r\n\r\n");
    server.finish();
    EXPECT_TRUE(tunnel.has_value());
    const auto output = (tunnel ? std::string_view(*tunnel) : std::string_view{});
    EXPECT_TRUE(output.starts_with("HTTP/1.1 200 "));
    // The cleared session's expiring cookie is part of the tunnel's only head.
    EXPECT_TRUE(lower_head(output).find("\r\nset-cookie: sid=;") != std::string::npos);
}
#endif

TEST(Ruvia, http_server_fixture_reservations_stay_bound_without_accepting_clients) {
    using namespace http1_response_policy_test;
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io, cors_server());
    ruvia::test::tcp_port_reservation first(io);
    ruvia::test::tcp_port_reservation second(io);
    EXPECT_TRUE(first.endpoint() != second.endpoint());
    EXPECT_TRUE(first.endpoint() != server.endpoint());
    EXPECT_TRUE(second.endpoint() != server.endpoint());

    std::error_code error;
#if !defined(_WIN32)
    // POSIX: a reservation stays bound, including before the real server starts.
    // Windows keeps only a named lease so the server's exclusive bind can succeed.
    asio::ip::tcp::acceptor contender(io);
    contender.open(asio::ip::tcp::v4());
    contender.bind(first.endpoint(), error);
    EXPECT_TRUE(error == asio::error::address_in_use);
#endif

    // It must not be a listener that can steal a client's connection.
    asio::ip::tcp::socket client(io);
    client.connect(first.endpoint(), error);
    EXPECT_TRUE(error == asio::error::connection_refused);

    const auto response = http1_response_policy_test::exchange(server.endpoint(),
        "GET /policy-plain HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
    server.finish();
    EXPECT_TRUE(response.has_value());
    EXPECT_TRUE((response ? std::string_view(*response) : std::string_view{}).ends_with("\r\n\r\nplain"));
}

namespace stream_disconnect_test {
using namespace std::chrono_literals;
using clock_type = std::chrono::steady_clock;
std::atomic<std::size_t> finished{}, cancelled{}, errors{}, context_cancelled{}, body_cancelled{}, body_complete{};
std::atomic<std::size_t> pipeline_cancelled{};
std::atomic<std::size_t> write_entered{};
// Upgraded writes to a peer that stops reading, and how many of them failed.
std::atomic<std::size_t> stalled_upgrade_writes{}, stalled_upgrade_failures{};
// More than loopback socket buffers absorb, so the write stays blocked.
constexpr std::size_t stalled_upgrade_bytes = std::size_t{16} << 20;
ruvia::stop_source explicit_stop;

struct completion final {
    ~completion() {
        finished.fetch_add(1);
    }
};

class routes final {
    RUVIA_ROUTES_BEGIN(routes)
    RUVIA_GET("/disconnect/events", events);
    RUVIA_GET("/disconnect/pipeline", pipeline);
    RUVIA_GET("/disconnect/pipeline-wait", pipeline_wait);
    RUVIA_GET("/disconnect/body", body);
    RUVIA_POST("/disconnect/echo", echo);
    RUVIA_POST("/disconnect/post-sse", post_sse);
    RUVIA_GET("/disconnect/health", health);
    RUVIA_GET("/disconnect/slow", slow);
    RUVIA_GET("/disconnect/h2-bulk", h2_bulk);
    RUVIA_GET_WS("/disconnect/websocket-stall", websocket_stall);
    RUVIA_CONNECT("stall.test:443", tunnel_stall);
    RUVIA_ROUTES_END

    ruvia::task<void> events(ruvia::context& c) {
        completion done;
        const auto mode = c.req().query("mode").value_or("token");
        const auto original = c.stop_token();
        auto writer = c.stream_sse();
        if (co_await writer.commit() == ruvia::stream_commit_result::completed) {
            co_return;
        }
        const ruvia::stop_group wait_stop(original, mode == "explicit" ? explicit_stop.token() : ruvia::stop_token{});
        const auto token = wait_stop.token();
        co_await writer.write({.data_ = "first"});
        if (mode == "write" || mode == "timeout") {
            // 大块写入让关闭、写完成和写超时竞争同一个连接生命周期。
            const std::string bulk(8 * 1024 * 1024, 'x');
            write_entered.fetch_add(1);
            co_await c.stream().write(bulk);
            co_return;
        }
        // 超大无符号等待不得窄化成负值后立即完成；客户端断开仍须唤醒它。
        const auto huge = std::chrono::duration<std::uint64_t, std::milli>{
            (std::numeric_limits<std::uint64_t>::max)()};
        const auto result = mode == "writer" ? co_await writer.sleep(huge)
                                             : co_await ruvia::sleep_for(c.worker(), 10s, token);
        if (result == ruvia::timer_sleep_result::stop_requested) {
            cancelled.fetch_add(1);
        }
        if (original.stop_requested()) {
            context_cancelled.fetch_add(1);
        }
    }

    ruvia::task<void> pipeline(ruvia::context& c) {
        auto& writer = c.stream();
        if (co_await writer.commit() == ruvia::stream_commit_result::completed) {
            co_return;
        }
        co_await writer.write("pipeline-first");
        if (co_await ruvia::sleep_for(c.worker(), 100ms, c.stop_token()) ==
            ruvia::timer_sleep_result::stop_requested) {
            pipeline_cancelled.fetch_add(1);
        }
        co_await writer.write("pipeline-last");
    }

    // A streamed request pipelined behind another: an idle wait on its stop.
    ruvia::task<void> pipeline_wait(ruvia::context& c) {
        auto& writer = c.stream();
        if (co_await writer.commit() == ruvia::stream_commit_result::completed) {
            co_return;
        }
        co_await writer.write("wait-first");
        if (co_await ruvia::sleep_for(c.worker(), 150ms, c.stop_token()) ==
            ruvia::timer_sleep_result::stop_requested) {
            pipeline_cancelled.fetch_add(1);
        }
        co_await writer.write("wait-last");
    }

    // The request body completes only after the handler starts; detection
    // begins with the read that completes it, early commit or not.
    ruvia::task<void> body(ruvia::context& c) {
        auto& writer = c.stream();
        const bool early = c.req().query("early").has_value();
        if (early) {
            if (co_await writer.commit() == ruvia::stream_commit_result::completed) {
                static_cast<void>(co_await c.req().text());
                co_return;
            }
            co_await writer.write("early");
        }
        const auto received = co_await c.req().text();
        body_complete.fetch_add(1);
        if (!early && co_await writer.commit() == ruvia::stream_commit_result::completed) {
            co_return;
        }
        co_await writer.write(received);
        if (c.req().query("wait").has_value() &&
            co_await ruvia::sleep_for(c.worker(), 10s, c.stop_token()) == ruvia::timer_sleep_result::stop_requested) {
            body_cancelled.fetch_add(1);
        }
    }

    ruvia::task<void> post_sse(ruvia::context& c) {
        const auto received = co_await c.req().text();
        auto writer = c.stream_sse();
        if (co_await writer.commit() == ruvia::stream_commit_result::completed) {
            co_return;
        }
        co_await writer.write({.data_ = received});
    }

    ruvia::task<ruvia::http_response> echo(ruvia::context& c) {
        const auto text = co_await c.req().text();
        co_return c.text(text);
    }

    ruvia::task<ruvia::http_response> health(ruvia::context& c) {
        co_return c.text("healthy");
    }

    // A buffered response that leaves only after 100ms, so a client can end
    // its request direction before the server writes anything.
    ruvia::task<ruvia::http_response> slow(ruvia::context& c) {
        (void)co_await ruvia::sleep_for(c.worker(), 100ms);
        co_return c.text("slow-done");
    }

    ruvia::task<ruvia::http_response> h2_bulk(ruvia::context& c) {
        co_return c.text(std::string(stalled_upgrade_bytes, 'b'));
    }

    // The WebSocket route keeps its default lifecycle: no heartbeat.
    ruvia::task<void> websocket_stall(ruvia::context& c) {
        stalled_upgrade_writes.fetch_add(1);
        try {
            co_await c.websocket().binary(std::string(stalled_upgrade_bytes, 'w'));
        } catch (const std::system_error&) {
            stalled_upgrade_failures.fetch_add(1);
        }
    }

    ruvia::task<void> tunnel_stall(ruvia::context& c) {
        stalled_upgrade_writes.fetch_add(1);
        try {
            co_await c.tunnel().write(std::string(stalled_upgrade_bytes, 't'));
        } catch (const std::system_error&) {
            stalled_upgrade_failures.fetch_add(1);
        }
    }
};

#if defined(RUVIA_ENABLE_HTTP2)
using ruvia::test::append_frame;

void append_u32(std::string& out, std::uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) {
        out.push_back(static_cast<char>((value >> shift) & 0xff));
    }
}

// A prior-knowledge HTTP/2 connection that opens every flow-control window to
// its maximum, so the server's output is limited only by the socket, then
// sends one request on stream 1: a large buffered GET or a CONNECT tunnel.
std::string http2_request_without_reading(bool tunnel) {
    constexpr std::uint32_t max_window = 0x7fffffffU;
    std::string wire(ruvia::http2_client_preface);
    std::string settings({0x00, 0x04});
    append_u32(settings, max_window);
    append_frame(wire, ruvia::http2_frame_type::settings, 0, 0, settings);
    std::string increment{};
    append_u32(increment, max_window - 65535U);
    append_frame(wire, ruvia::http2_frame_type::window_update, 0, 0, increment);
    std::string block;
    if (tunnel) {
        ruvia::hpack_encoder::encode_header(block, ":method", "CONNECT");
        ruvia::hpack_encoder::encode_header(block, ":authority", "stall.test:443");
        append_frame(wire, ruvia::http2_frame_type::headers, 0x4, 1, block);
    } else {
        ruvia::hpack_encoder::encode_header(block, ":method", "GET");
        ruvia::hpack_encoder::encode_header(block, ":scheme", "http");
        ruvia::hpack_encoder::encode_header(block, ":authority", "localhost");
        ruvia::hpack_encoder::encode_header(block, ":path", "/disconnect/h2-bulk");
        append_frame(wire, ruvia::http2_frame_type::headers, 0x5, 1, block);
    }
    return wire;
}

#endif

// 异步客户端给每次 I/O 设置独立截止时间，回归失败不会挂住测试进程。
class peer final {
public:
    peer(asio::io_context& io, const asio::ip::tcp::endpoint& endpoint, bool tls)
        : io_(io),
          context_(asio::ssl::context::tls_client),
          stream_(io, context_),
          tls_(tls) {
        context_.set_verify_mode(asio::ssl::verify_none);
        bool connected = false;
        asio::error_code connect_error;
        stream_.next_layer().async_connect(endpoint, [&](const asio::error_code& ec) {
            connect_error = ec;
            connected = true;
        });
        if (!wait_for_completion(connected, 2s)) {
            throw std::runtime_error("disconnect test connect timed out");
        }
        if (connect_error) {
            throw std::system_error(connect_error);
        }
        if (tls_) {
            bool handshaken = false;
            asio::error_code handshake_error;
            stream_.async_handshake(asio::ssl::stream_base::client, [&](asio::error_code ec) {
                handshake_error = ec;
                handshaken = true;
            });
            if (!wait_for_completion(handshaken, 2s)) {
                throw std::runtime_error("disconnect test TLS handshake timed out");
            }
            if (handshake_error) {
                throw std::system_error(handshake_error);
            }
        }
    }
    void send(std::string_view bytes) {
        bool written = false;
        asio::error_code error;
        const auto complete = [&](asio::error_code ec, std::size_t) {
            error = ec;
            written = true;
        };
        if (tls_) {
            asio::async_write(stream_, asio::buffer(bytes), complete);
        } else {
            asio::async_write(stream_.next_layer(), asio::buffer(bytes), complete);
        }
        if (!wait_for_completion(written, 2s)) {
            throw std::runtime_error("disconnect test write timed out");
        }
        if (error) {
            throw std::system_error(error);
        }
    }
    std::string read_until(std::string_view marker, std::chrono::milliseconds timeout = 2s) {
        std::string output{};
        std::array<char, 16384> bytes;
        const auto deadline = clock_type::now() + timeout;
        while (clock_type::now() < deadline) {
            asio::error_code ec;
            std::size_t n = 0;
            bool read = false;
            const auto complete = [&](asio::error_code error, std::size_t count) {
                ec = error;
                n = count;
                read = true;
            };
            if (tls_) {
                stream_.async_read_some(asio::buffer(bytes), complete);
            } else {
                stream_.next_layer().async_read_some(asio::buffer(bytes), complete);
            }
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - clock_type::now());
            const bool completed = wait_for_completion(read, std::max(remaining, 0ms));
            output.append(bytes.data(), n);
            if (!marker.empty() && output.find(marker) != std::string::npos) {
                return output;
            }
            if (!completed || ec) {
                return output;
            }
        }
        return output;
    }
    void half_close() {
        asio::error_code ignored;
        stream_.next_layer().shutdown(asio::ip::tcp::socket::shutdown_send, ignored);
    }
    // TLS 1.3 half-close: close_notify ends only the sending direction. The
    // shutdown then waits for the server's close_notify; cancel that read so
    // the response stays readable. Callers send it while the server stays
    // quiet for far longer than the 20ms the close_notify write needs.
    void send_close_notify() {
        asio::cancellation_signal cancel;
        bool completed = false;
        asio::error_code error;
        stream_.async_shutdown(asio::bind_cancellation_slot(cancel.slot(), [&](asio::error_code ec) {
            error = ec;
            completed = true;
        }));
        io_.restart();
        io_.run_for(20ms);
        if (!completed) {
            cancel.emit(asio::cancellation_type::terminal);
            wait_for_completion(completed, 2s);
        }
        if ((SSL_get_shutdown(stream_.native_handle()) & SSL_SENT_SHUTDOWN) == 0) {
            throw std::runtime_error("TLS close_notify was not sent");
        }
        // The server may close first: answering with its close_notify, or,
        // having abandoned a lone request, without one.
        if (error && error != asio::error::operation_aborted && error != asio::error::eof &&
            error != asio::ssl::error::stream_truncated) {
            throw std::system_error(error);
        }
    }
    // A clean end of the request direction: TCP FIN, or TLS close_notify. A
    // bare TCP FIN under TLS is a truncation instead (half_close()).
    void end_requests() {
        if (tls_) {
            send_close_notify();
        } else {
            half_close();
        }
    }
    void request_key_update() {
        if (SSL_key_update(stream_.native_handle(), SSL_KEY_UPDATE_REQUESTED) != 1) {
            throw std::runtime_error("TLS key update failed");
        }
    }
    void close(bool notify = false, bool reset = false) {
        if (notify && tls_) {
            asio::error_code ec;
            bool shutdown = false;
            stream_.async_shutdown([&](asio::error_code error) {
                ec = error;
                shutdown = true;
            });
            const bool completed = wait_for_completion(shutdown, 2s);
            if ((SSL_get_shutdown(stream_.native_handle()) & SSL_SENT_SHUTDOWN) == 0) {
                throw std::runtime_error("TLS close_notify was not sent");
            }
            if (completed && ec && ec != asio::error::eof && ec != asio::ssl::error::stream_truncated) {
                throw std::system_error(ec);
            }
        }
        asio::error_code ignored;
        if (reset) {
            stream_.next_layer().set_option(asio::socket_base::linger(true, 0), ignored);
        }
        stream_.next_layer().close(ignored);
    }

private:
    bool wait_for_completion(bool& completed, std::chrono::milliseconds timeout) {
        io_.restart();
        io_.run_for(timeout);
        if (completed) {
            return true;
        }
        // 关闭后排完组合操作，回调和借用缓冲都先于本次调用返回而退役。
        asio::error_code ignored;
        stream_.next_layer().close(ignored);
        io_.restart();
        io_.run();
        return false;
    }
    asio::io_context& io_;
    asio::ssl::context context_;
    asio::ssl::stream<asio::ip::tcp::socket> stream_;
    bool tls_;
};

void check_pipeline_upload(asio::io_context& io,
    const asio::ip::tcp::endpoint& endpoint, bool tls, std::size_t size, bool defer_body) {
    peer client(io, endpoint, tls);
    client.send("GET /disconnect/pipeline HTTP/1.1\r\nHost: localhost\r\n\r\n");
    EXPECT_TRUE(client.read_until("pipeline-first").find("pipeline-first") != std::string::npos);
    if (tls) {
        client.request_key_update();
    }
    const std::string payload(size, 'p');
    const auto prefix = defer_body ? size / 2 : size;
    std::string request("POST /disconnect/echo HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\nContent-Length: ");
    request.append(std::to_string(size));
    request.append("\r\n\r\n");
    request.append(payload.data(), prefix);
    client.send(request);
    std::string output{};
    if (defer_body) {
        // 前一响应结束后，EOF 监测器必须 join；剩余上传由后续请求接管。
        output = client.read_until("pipeline-last");
        client.send(std::string_view(payload).substr(prefix));
    }
    output += client.read_until({}, 3s);
    if (!output.ends_with(payload)) {
        std::cerr << "stream_disconnect pipeline: tls=" << tls << " size=" << size << " deferred=" << defer_body
                  << " received=" << output.size() << " response=" << std::string_view(output).substr(0, 256) << '\n';
    }
    EXPECT_TRUE(output.find("pipeline-last") != std::string::npos);
    EXPECT_TRUE(output.ends_with(payload));
    EXPECT_TRUE(output.find("HTTP/1.1 200 ") != std::string::npos);
}

// RFC 9112 section 9.6: a FIN behind pipelined requests closes only the
// request direction. The stream in progress and every request received before
// the FIN -- a streamed one idling on its stop included -- complete; then the
// server closes without reading that end again as abandonment.
void check_pipeline_half_close(asio::io_context& io,
    const asio::ip::tcp::endpoint& endpoint, bool tls, bool trailing_buffered) {
    peer client(io, endpoint, tls);
    const auto cancelled_before = pipeline_cancelled.load();
    client.send("GET /disconnect/pipeline HTTP/1.1\r\nHost: localhost\r\n\r\n");
    EXPECT_TRUE(client.read_until("pipeline-first").find("pipeline-first") != std::string::npos);
    // The streamed request is either the last one before the FIN, with no byte
    // behind it, or followed by a buffered one.
    client.send(std::string("GET /disconnect/pipeline-wait HTTP/1.1\r\nHost: localhost\r\n\r\n") +
                (trailing_buffered ? "GET /disconnect/health HTTP/1.1\r\nHost: localhost\r\n\r\n" : ""));
    // Under TLS the TLS engine then reports the end to every later read at
    // once, with no transport read: a pipelined stream must not re-read it.
    client.end_requests();
    // Read to the server's close: no Connection: close was requested, so only
    // the request direction's end can close it before the 3s read deadline.
    const auto begin = clock_type::now();
    const auto output = client.read_until({}, 3s);
    EXPECT_TRUE(clock_type::now() - begin < 2s);
    const auto first_end = output.find("pipeline-last");
    const auto waited_end = output.find("wait-last");
    const auto health = output.find("healthy");
    EXPECT_TRUE(first_end != std::string::npos);
    EXPECT_TRUE(waited_end != std::string::npos && waited_end > first_end);
    EXPECT_TRUE(output.find("0\r\n\r\n", waited_end) != std::string::npos);
    if (trailing_buffered) {
        EXPECT_TRUE(health != std::string::npos && health > waited_end);
    } else {
        EXPECT_TRUE(health == std::string::npos);
    }
    EXPECT_EQ(pipeline_cancelled.load(), cancelled_before);
}

std::string request(std::string_view mode) {
    return "GET /disconnect/events?mode=" + std::string(mode) + " HTTP/1.1\r\nHost: localhost\r\n\r\n";
}

// How the client ends a pipelined burst whose last request is a streamed
// response idling on its stop.
enum class pipeline_end { immediate,
    after_last_started,
    truncated };

// A request received behind another one -- whatever route answered that one,
// and whenever the end arrives -- treats a clean end of the request direction
// as a half-close: it completes, then the server closes. A TLS truncation
// (TCP FIN without close_notify) still ends it as abandonment.
void check_pipelined_stream_end(asio::io_context& io,
    const asio::ip::tcp::endpoint& endpoint, bool tls, std::string_view leading_requests,
    std::string_view leading_marker, pipeline_end end) {
    peer client(io, endpoint, tls);
    const auto cancelled_before = pipeline_cancelled.load();
    client.send(std::string(leading_requests) + "GET /disconnect/pipeline-wait HTTP/1.1\r\nHost: localhost\r\n\r\n");
    std::string output{};
    if (end == pipeline_end::after_last_started) {
        // The streamed request already runs, its predecessors done: the end
        // arrives with nothing buffered behind any request.
        output = client.read_until("wait-first");
        EXPECT_TRUE(output.find("wait-first") != std::string::npos);
        client.end_requests();
    } else if (end == pipeline_end::truncated) {
        client.half_close();
    } else {
        client.end_requests();
    }
    const auto begin = clock_type::now();
    output += client.read_until({}, 3s);
    EXPECT_TRUE(clock_type::now() - begin < 2s);
    const auto leading_end = output.find(leading_marker);
    EXPECT_TRUE(leading_end != std::string::npos);
    const auto waited_end = output.find("wait-last");
    if (end == pipeline_end::truncated) {
        // The stream is abandoned: stopped in its wait, or failing its first
        // write when the truncation is seen before it.
        EXPECT_TRUE(waited_end == std::string::npos);
        return;
    }
    EXPECT_TRUE(waited_end != std::string::npos && waited_end > leading_end);
    EXPECT_TRUE(output.find("0\r\n\r\n", waited_end) != std::string::npos);
    EXPECT_EQ(pipeline_cancelled.load(), cancelled_before);
}

// A FIN (or close_notify) with no request before or behind the stream is the
// client leaving (Go net/http, Pingora, nginx): the request stops and the
// stream truncates.
void check_single_stream_end(asio::io_context& io,
    const asio::ip::tcp::endpoint& endpoint, bool tls) {
    peer client(io, endpoint, tls);
    const auto before = cancelled.load();
    const auto context_before = context_cancelled.load();
    client.send(request("token"));
    EXPECT_TRUE(client.read_until("data: first").find("data: first") != std::string::npos);
    client.end_requests();
    ruvia::test::wait_until([&] { return cancelled.load() == before + 1; }, 2s);
    EXPECT_EQ(context_cancelled.load(), context_before + 1);
    EXPECT_TRUE(client.read_until("0\r\n\r\n", 500ms).find("0\r\n\r\n") == std::string::npos);
    client.close();
}

std::size_t fd_count() {
#ifdef __linux__
    std::error_code ec;
    std::size_t count = 0;
    for (std::filesystem::directory_iterator it("/proc/self/fd", ec), end; !ec && it != end; it.increment(ec)) {
        ++count;
    }
    return count;
#else
    return 0;
#endif
}
}  // namespace stream_disconnect_test

using namespace stream_disconnect_test;

TEST(Ruvia, http1_stream_peer_close_stops_request_and_recycles_connections) {
    using namespace stream_disconnect_test;
    ruvia::test::reset_app_callbacks(ruvia::app());
    ruvia::test::tls_identity identity("localhost");
    const auto private_key = identity.private_key_file();
    asio::io_context io;
    ruvia::test::tcp_port_reservation plain_reservation(io);
    ruvia::test::tcp_port_reservation tls_reservation(io);
    const auto plain_endpoint = plain_reservation.endpoint();
    const auto tls_endpoint = tls_reservation.endpoint();
    auto& app = ruvia::app();
    static std::binary_semaphore started{0};
    app.server({.worker_count_ = 2, .idle_timeout_ = 15s, .connection_scan_interval_ = 5ms, .write_timeout_ = 150ms})
        .listen({.address_ = "127.0.0.1", .http_ = plain_endpoint.port(), .https_ = tls_endpoint.port(), .tls_ = {.identity_ = {.certificate_chain_file_ = identity.ca_file_, .private_key_file_ = private_key}}})
        .on_error([](ruvia::context& c, ruvia::http_error_info error) -> ruvia::task<ruvia::http_response> {
            errors.fetch_add(1);
            std::cerr << "stream_disconnect error: " << error.code() << ": " << error.message() << '\n';
            c.draft().status(error.status());
            co_return c.text("unexpected-error");
        })
        .on_start([] { started.release(); });
    ruvia::test::app_run runner(app);
    std::exception_ptr client_failure;
    const bool ready = started.try_acquire_for(10s);
    EXPECT_TRUE(ready);
    try {
        if (ready) {
            for (const bool tls : {false, true}) {
                const auto endpoint = tls ? tls_endpoint : plain_endpoint;
                // An idle SSE (no write, no writer.sleep()) observes the client
                // leaving through context::stop_token(): FIN, TLS close_notify, or RST.
                struct close_kind final {
                    bool notify_;
                    bool reset_;
                };
                for (const auto mode : {"token", "writer"}) {
                    for (const auto kind : {close_kind{false, false}, close_kind{true, false}, close_kind{false, true}}) {
                        peer client(io, endpoint, tls);
                        const auto before = cancelled.load();
                        const auto context_before = context_cancelled.load();
                        client.send(request(mode));
                        EXPECT_TRUE(client.read_until("data: first").find("data: first") != std::string::npos);
                        const auto begin = clock_type::now();
                        client.close(kind.notify_, kind.reset_);
                        ruvia::test::wait_until([&] { return cancelled.load() == before + 1; }, 2s);
                        EXPECT_TRUE(clock_type::now() - begin < 2s);
                        EXPECT_EQ(context_cancelled.load(), context_before + 1);
                        ruvia::test::wait_until([&] { return app.stats().active_connections_ == 0; }, 2s);
                    }
                }
                peer body_client(io, endpoint, tls);
                const auto complete_before = body_complete.load();
                body_client.send("GET /disconnect/body HTTP/1.1\r\nHost: localhost\r\nContent-Length: 11\r\nConnection: close\r\n\r\n");
                std::this_thread::sleep_for(20ms);
                body_client.send("hello-world");
                EXPECT_TRUE(body_client.read_until("0\r\n\r\n").find("hello-world") != std::string::npos);
                EXPECT_EQ(body_complete.load(), complete_before + 1);
                // Committed before its body arrived: the read completing the
                // body starts detection, so the idle wait still ends on close.
                peer waiting_body_client(io, endpoint, tls);
                const auto body_cancelled_before = body_cancelled.load();
                waiting_body_client.send("GET /disconnect/body?early=1&wait=1 HTTP/1.1\r\nHost: localhost\r\nContent-Length: 11\r\n\r\n");
                EXPECT_TRUE(waiting_body_client.read_until("early").find("early") != std::string::npos);
                waiting_body_client.send("hello-world");
                EXPECT_TRUE(waiting_body_client.read_until("hello-world").find("hello-world") != std::string::npos);
                const auto body_close_begin = clock_type::now();
                waiting_body_client.close();
                ruvia::test::wait_until([&] { return body_cancelled.load() == body_cancelled_before + 1; }, 2s);
                EXPECT_TRUE(clock_type::now() - body_close_begin < 2s);
                peer post_sse_client(io, endpoint, tls);
                post_sse_client.send("POST /disconnect/post-sse HTTP/1.1\r\nHost: localhost\r\nContent-Length: 10\r\nConnection: close\r\n\r\n{\"id\":\"1\"}");
                const auto post_sse_output = post_sse_client.read_until("0\r\n\r\n");
                EXPECT_TRUE(post_sse_output.find("text/event-stream") != std::string::npos);
                EXPECT_TRUE(post_sse_output.find("data: {\"id\":\"1\"}") != std::string::npos);
                peer early_client(io, endpoint, tls);
                early_client.send("GET /disconnect/body?early=1 HTTP/1.1\r\nHost: localhost\r\nContent-Length: 11\r\n\r\n");
                EXPECT_TRUE(early_client.read_until("early").find("early") != std::string::npos);
                early_client.send("hello-worldGET /disconnect/health HTTP/1.1\r\nHost: localhost\r\n\r\n");
                const auto early_output = early_client.read_until({});
                EXPECT_TRUE(early_output.find("hello-world") != std::string::npos);
                EXPECT_TRUE(early_output.find("healthy") == std::string::npos);
                ruvia::test::wait_until([&] { return app.stats().active_connections_ == 0; }, 2s);

                const auto pipeline_cancelled_before = pipeline_cancelled.load();
                for (const std::size_t size : {std::size_t{31}, std::size_t{256 * 1024}}) {
                    check_pipeline_upload(io, endpoint, tls, size, false);
                    if (tls) {
                        check_pipeline_upload(io, endpoint, true, size, true);
                    }
                }
                // Pipelined bytes arriving during the stream are retained, not
                // read as the client leaving.
                EXPECT_EQ(pipeline_cancelled.load(), pipeline_cancelled_before);
                for (const bool trailing_buffered : {false, true}) {
                    check_pipeline_half_close(io, endpoint, tls, trailing_buffered);
                }
                constexpr std::string_view slow_request = "GET /disconnect/slow HTTP/1.1\r\nHost: localhost\r\n\r\n";
                const std::string two_buffered(std::string(slow_request) + "GET /disconnect/health HTTP/1.1\r\nHost: localhost\r\n\r\n");
                // [buffered, streamed] and [buffered, buffered, streamed]: no
                // monitor ran before the stream, which still completes.
                check_pipelined_stream_end(io, endpoint, tls, slow_request, "slow-done", pipeline_end::immediate);
                check_pipelined_stream_end(io, endpoint, tls, two_buffered, "healthy", pipeline_end::immediate);
                // [streamed, streamed] with the end after the first completed.
                check_pipelined_stream_end(io, endpoint, tls,
                    "GET /disconnect/pipeline HTTP/1.1\r\nHost: localhost\r\n\r\n", "pipeline-last",
                    pipeline_end::after_last_started);
                if (tls) {
                    check_pipelined_stream_end(io, endpoint, tls, slow_request, "slow-done", pipeline_end::truncated);
                }
                check_single_stream_end(io, endpoint, tls);
                ruvia::test::wait_until([&] { return app.stats().active_connections_ == 0; }, 2s);
                for (const auto mode : {"write", "timeout"}) {
                    peer racing(io, endpoint, tls);
                    const auto writes_before = write_entered.load();
                    racing.send(request(mode));
                    EXPECT_TRUE(racing.read_until("data: first").find("data: first") != std::string::npos);
                    ruvia::test::wait_until([&] { return write_entered.load() == writes_before + 1; }, 2s);
                    if (std::string_view(mode) == "timeout") {
                        ruvia::test::wait_until([&] { return app.stats().active_connections_ == 0; }, 2s);
                    }
                    racing.close(false, true);
                    ruvia::test::wait_until([&] { return app.stats().active_connections_ == 0; }, 2s);
                }
                // write_timeout_ bounds every write of an upgraded connection
                // as well: a WebSocket without heartbeat or a CONNECT tunnel
                // whose peer stops reading is closed once its write makes no
                // progress, long before idle_timeout_.
                for (const auto upgrade : {std::string_view("GET /disconnect/websocket-stall HTTP/1.1\r\nHost: localhost\r\n"
                                                            "Connection: Upgrade\r\nUpgrade: websocket\r\nSec-WebSocket-Version: 13\r\n"
                                                            "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n"),
                         std::string_view("CONNECT stall.test:443 HTTP/1.1\r\nHost: stall.test:443\r\n\r\n")}) {
                    peer stalled(io, endpoint, tls);
                    const auto writes_before = stalled_upgrade_writes.load();
                    const auto failures_before = stalled_upgrade_failures.load();
                    stalled.send(upgrade);
                    const auto head = stalled.read_until("\r\n\r\n");
                    EXPECT_TRUE(head.starts_with(upgrade.starts_with("GET") ? "HTTP/1.1 101 " : "HTTP/1.1 200 "));
                    ruvia::test::wait_until([&] { return stalled_upgrade_writes.load() == writes_before + 1; }, 2s);
                    ruvia::test::wait_until([&] { return app.stats().active_connections_ == 0; }, 5s);
                    ruvia::test::wait_until([&] { return stalled_upgrade_failures.load() == failures_before + 1; }, 2s);
                    stalled.close(false, true);
                }
            }
#if defined(RUVIA_ENABLE_HTTP2)
            // The HTTP/2 reader keeps its connection idle (or long_lived for
            // a tunnel) while the writer is blocked; write_timeout_ still
            // closes a connection whose peer stopped reading.
            for (const bool tunnel : {false, true}) {
                peer stalled(io, plain_endpoint, false);
                const auto writes_before = stalled_upgrade_writes.load();
                const auto failures_before = stalled_upgrade_failures.load();
                stalled.send(http2_request_without_reading(tunnel));
                if (tunnel) {
                    ruvia::test::wait_until([&] { return stalled_upgrade_writes.load() == writes_before + 1; }, 2s);
                }
                ruvia::test::wait_until([&] { return app.stats().active_connections_ == 0; }, 5s);
                if (tunnel) {
                    ruvia::test::wait_until([&] { return stalled_upgrade_failures.load() == failures_before + 1; }, 2s);
                }
                stalled.close(false, true);
            }
#endif
            peer explicit_peer(io, plain_endpoint, false);
            const auto explicit_before = cancelled.load();
            const auto explicit_context_before = context_cancelled.load();
            explicit_peer.send(request("explicit"));
            EXPECT_TRUE(explicit_peer.read_until("data: first").find("data: first") != std::string::npos);
            explicit_stop.request_stop();
            ruvia::test::wait_until([&] { return cancelled.load() == explicit_before + 1; }, 2s);
            EXPECT_EQ(context_cancelled.load(), explicit_context_before);
            explicit_peer.close();
            ruvia::test::wait_until([&] { return app.stats().active_connections_ == 0; }, 2s);

            const auto fd_baseline = fd_count();
            std::size_t fd_peak = fd_baseline, active_peak = 0;
            std::chrono::milliseconds recycle_max{0};
            constexpr std::size_t batch_size = 64, batches = 36;
            for (std::size_t batch = 0; batch < batches; ++batch) {
                std::vector<std::unique_ptr<peer>> clients{};
                for (std::size_t index = 0; index < batch_size; ++index) {
                    auto client = std::make_unique<peer>(io, plain_endpoint, false);
                    client->send(request("token"));
                    EXPECT_TRUE(client->read_until("data: first").find("data: first") != std::string::npos);
                    clients.push_back(std::move(client));
                }
                active_peak = std::max(active_peak, app.stats().active_connections_);
                fd_peak = std::max(fd_peak, fd_count());
                const auto before = cancelled.load();
                const auto context_before = context_cancelled.load();
                const auto begin = clock_type::now();
                for (auto& client : clients) {
                    client->close();
                }
                ruvia::test::wait_until([&] { return app.stats().active_connections_ == 0; }, 2s);
                EXPECT_EQ(cancelled.load(), before + batch_size);
                EXPECT_EQ(context_cancelled.load(), context_before + batch_size);
                recycle_max = std::max(recycle_max, std::chrono::duration_cast<std::chrono::milliseconds>(clock_type::now() - begin));
            }
            const auto fd_final = fd_count();
            EXPECT_EQ(active_peak, batch_size);
#ifdef __linux__
            EXPECT_TRUE(fd_final <= fd_baseline);
#endif
            peer recovery(io, plain_endpoint, false);
            recovery.send("GET /disconnect/health HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
            EXPECT_TRUE(recovery.read_until("healthy").find("healthy") != std::string::npos);
            recovery.close();
            ruvia::test::wait_until([&] { return app.stats().active_connections_ == 0; }, 2s);
            std::cout << "stream_disconnect metrics: total=" << batch_size * batches << " peak_active=" << active_peak
                      << " fd_baseline=" << fd_baseline << " fd_peak=" << fd_peak << " fd_final=" << fd_final
                      << " max_recycle_ms=" << recycle_max.count() << " errors=" << errors.load()
                      << " connection_failures=" << app.stats().connection_failures_
                      << " refused=" << app.stats().connections_refused_
                      << " max_connections_per_worker=" << *ruvia::server_config{}.max_connections_per_worker_ << '\n';
            EXPECT_EQ(app.stats().connections_refused_, std::size_t{0});

            // 在同一个有限等待上竞争 worker stop 和 peer EOF。
            peer stopping(io, plain_endpoint, false);
            const auto before_stop = finished.load();
            stopping.send(request("token"));
            EXPECT_TRUE(stopping.read_until("data: first").find("data: first") != std::string::npos);
            app.stop();
            stopping.close();
            ruvia::test::wait_until([&] { return finished.load() == before_stop + 1; }, 2s);
        }
    } catch (...) {
        client_failure = std::current_exception();
        app.stop();
    }
    runner.join();
    EXPECT_EQ(app.stats().connection_failures_, std::size_t{0});
    EXPECT_EQ(app.stats().accept_failures_, std::size_t{0});
    EXPECT_EQ(app.stats().worker_failures_, std::size_t{0});
    if (client_failure) {
        std::rethrow_exception(client_failure);
    }
}

#ifdef RUVIA_ENABLE_REDIS

TEST(Ruvia, session_middleware_rejects_invalid_config_before_use) {
    ruvia::test::reset_app_callbacks(ruvia::app());
    const auto rejected = [](const ruvia::session_config& config) {
        try {
            const ruvia::session_middleware middleware(config);
        } catch (const std::invalid_argument&) {
            return true;
        }
        return false;
    };

    auto config = ruvia::session_config{};
    config.redis_alias_ = "";
    EXPECT_TRUE(rejected(config));

    config = ruvia::session_config{};
    config.cookie_name_ = "bad cookie";
    EXPECT_TRUE(rejected(config));

    config = ruvia::session_config{};
    config.key_prefix_ = "";
    EXPECT_TRUE(rejected(config));

    config = ruvia::session_config{};
    config.ttl_ = std::chrono::seconds::zero();
    EXPECT_TRUE(rejected(config));

    // A new session is written with a millisecond expiry.
    constexpr auto max_ttl = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::milliseconds::max());
    config.ttl_ = max_ttl + std::chrono::seconds(1);
    EXPECT_TRUE(rejected(config));
    config.ttl_ = std::chrono::seconds::max();
    EXPECT_TRUE(rejected(config));
}

namespace session_test {

// Writes a cookie under the session's name before clearing the session, so the
// commit stage finds a same-name Set-Cookie already in the response. The
// session middleware only runs on matched requests, so this needs a route.
class same_name_cookie_controller final {
public:
    RUVIA_CONTROLLER_GROUP("/session-test")
    RUVIA_ROUTES_BEGIN(same_name_cookie_controller)
    RUVIA_GET("/same-name-cookie", same_name_cookie);
    RUVIA_ROUTES_END

private:
    ruvia::task<ruvia::http_response> same_name_cookie(ruvia::context& c) {
        c.draft().set_cookie({.name_ = "ruvia_session", .value_ = "handler"});
        c.session().clear();
        co_return c.text("ok");
    }
};

class stored_session_controller final {
public:
    RUVIA_ROUTES_BEGIN(stored_session_controller)
    RUVIA_GET("/session-ids/:space/read", read);
    RUVIA_GET("/session-ids/:space/set", set);
    RUVIA_GET("/session-ids/:space/clear", clear);
    RUVIA_GET("/session-ids/:space/rotate", rotate);
    RUVIA_GET("/session-ids/b/stale", stale);
    RUVIA_GET("/session-store", stored);
    RUVIA_ROUTES_END

    ruvia::task<ruvia::http_response> read(ruvia::context& c) {
        auto value = c.session();
        co_return c.text(value.data().empty() ? "empty" : value.data());
    }

    ruvia::task<ruvia::http_response> set(ruvia::context& c) {
        c.session().set("private");
        co_return c.text("set");
    }

    ruvia::task<ruvia::http_response> clear(ruvia::context& c) {
        c.session().clear();
        co_return c.text("clear");
    }

    ruvia::task<ruvia::http_response> rotate(ruvia::context& c) {
        c.session().regenerate();
        co_return c.text("rotate");
    }

    ruvia::task<ruvia::http_response> stale(ruvia::context& c) {
        // Revoke after middleware loaded the session but before response commit.
        const auto id = c.req().cookie("sid_b");
        if (id) {
            const std::string key = "sess:a" + std::string(*id);
            (void)co_await c.redis().del(key);
        }
        c.session().regenerate();
        co_return c.text("stale");
    }

    ruvia::task<ruvia::http_response> stored(ruvia::context& c) {
        const auto id = c.req().query("id");
        const std::string key = "sess:" + std::string(id.value_or(""));
        const auto value = co_await c.redis().get(key);
        co_return c.text(value ? std::string_view(*value) : "empty");
    }
};

struct session_response final {
    unsigned status_;
    std::string body_{};
    std::string cookie_{};
};

[[nodiscard]] session_response request(asio::io_context& io, const asio::ip::tcp::endpoint& endpoint,
    std::string_view target, std::string_view cookie = {}) {
    asio::ip::tcp::socket socket(io);
    socket.connect(endpoint);
    const std::string wire = "GET " + std::string(target) + " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\nCookie: " + std::string(cookie) + "\r\n\r\n";
    asio::write(socket, asio::buffer(wire));
    socket.non_blocking(true);
    std::string response{};
    std::array<char, 4096> buffer;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    for (;;) {
        std::error_code error;
        const auto size = socket.read_some(asio::buffer(buffer), error);
        response.append(buffer.data(), size);
        if (error == asio::error::eof) {
            break;
        }
        if (error && error != asio::error::would_block && error != asio::error::try_again) {
            throw std::system_error(error);
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            throw std::runtime_error("session HTTP response timed out");
        }
        if (error) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    const auto head_end = response.find("\r\n\r\n");
    if (!response.starts_with("HTTP/1.1 ") || head_end == std::string::npos) {
        throw std::runtime_error("invalid session HTTP response");
    }
    session_response result{static_cast<unsigned>(std::get<0>(ruvia::parse_integer<unsigned>(std::string_view(response).substr(9, 3)))), std::string(std::string_view(response).substr(head_end + 4)), std::string{}};
    const auto cookie_start = response.find("\r\nSet-Cookie: ");
    if (cookie_start != std::string::npos && cookie_start < head_end) {
        const auto start = cookie_start + std::string_view("\r\nSet-Cookie: ").size();
        result.cookie_.assign(std::string_view(response).substr(start, response.find(';', start) - start));
    }
    return result;
}

}  // namespace session_test

TEST(Ruvia, session_cookie_replaces_a_same_name_cookie_set_by_the_handler) {
    using namespace session_test;
    ruvia::test_app app;
    {
        ruvia::session_config config;
        config.cookie_name_ = "ruvia_session";
        app.use<ruvia::session_middleware>(config);
    }

    const auto response = app.request(ruvia::test_request::get("/session-test/same-name-cookie"));
    EXPECT_EQ(response.status(), ruvia::http_status::ok);
    std::size_t session_cookies = 0;
    bool expired = false;
    for (const auto& header : response.headers()) {
        if (header.name() != std::string_view("Set-Cookie") || !header.value().starts_with("ruvia_session=")) {
            continue;
        }
        ++session_cookies;
        expired = expired || (header.value().starts_with("ruvia_session=;") &&
                                 header.value().find("; Expires=Thu, 01 Jan 1970 00:00:00 GMT") != std::string_view::npos &&
                                 header.value().find("Max-Age") == std::string_view::npos);
    }
    EXPECT_EQ(session_cookies, std::size_t{1});
    EXPECT_TRUE(expired);
}

TEST(Ruvia, session_ids_isolate_prefixes_and_preserve_fixed_length_lifecycle) {
    using namespace session_test;
    // Construct before the shared HTTP runtime so the server retires its Redis
    // client before this peer is destroyed at process exit.
    static ruvia::test::redis_session_store_peer peer([] {
        std::map<std::string, std::string> store;
        for (const auto size : {47, 48, 49, 128}) {
            store.emplace("sess:" + std::string(size, 'b'), "existing");
        }
        store.emplace("sess:" + std::string(48, 'B'), "existing");
        store.emplace("sess:" + std::string(48, 'g'), "existing");
        return store;
    }());
    ruvia::app()
        .redis({.config_ = {.host_ = "127.0.0.1", .port_ = peer.port(), .tls_ = {.mode_ = ruvia::client_tls_mode::disabled}, .pool_size_per_worker_ = 1, .connect_timeout_ = std::chrono::seconds(2), .command_timeout_ = std::chrono::seconds(2)}})
        .use_at<ruvia::session_middleware>({.prefix_ = "/session-ids/a"},
            ruvia::session_config{.cookie_name_ = "sid_a", .key_prefix_ = "sess:"})
        .use_at<ruvia::session_middleware>({.prefix_ = "/session-ids/b"},
            ruvia::session_config{.cookie_name_ = "sid_b", .key_prefix_ = "sess:a"});
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io);
    const auto request = [&](std::string_view path, std::string_view cookie = {}) {
        return session_test::request(io, server.endpoint(), path, cookie);
    };
    const auto expect = [&](std::string_view path, std::string_view cookie, std::string_view body) {
        const auto response = request(path, cookie);
        EXPECT_EQ(response.status_, 200U);
        EXPECT_EQ(response.body_, body);
        return response;
    };

    // These IDs have actual stored values. Rejection must hide the data and
    // leave it unchanged across both clear and regenerate, not merely avoid a
    // backend error. Uppercase and non-hex IDs still remain invalid at length 48.
    for (const auto& id : {std::string(47, 'b'), std::string(49, 'b'), std::string(128, 'b'),
             std::string(48, 'B'), std::string(48, 'g')}) {
        const auto cookie = std::string("sid_a=") + id;
        expect("/session-ids/a/read", cookie, "empty");
        expect("/session-ids/a/clear", cookie, "clear");
        const auto rotated = expect("/session-ids/a/rotate", cookie, "rotate");
        EXPECT_TRUE(rotated.cookie_.empty());
        expect(std::string("/session-store?id=") + id, {}, "existing");
    }

    // A freshly minted B session must not be loadable or mutable through A's
    // shorter prefix, even though raw prefix + presented ID names the same key.
    for (const auto action : {"read", "clear", "rotate"}) {
        const auto created = expect("/session-ids/b/set", {}, "set");
        EXPECT_TRUE(created.cookie_.starts_with("sid_b="));
        EXPECT_EQ(created.cookie_.size(), std::size_t{6 + 48});
        const auto alias = "sid_a=a" + std::string(std::string_view(created.cookie_).substr(6));
        const auto response = expect(std::string("/session-ids/a/") + action, alias,
            std::string_view(action) == "read" ? "empty" : action);
        if (std::string_view(action) != "clear") {
            EXPECT_TRUE(response.cookie_.empty());
        }
        expect("/session-ids/b/read", created.cookie_, "private");
        expect("/session-ids/b/clear", created.cookie_, "clear");
        expect("/session-ids/b/read", created.cookie_, "empty");
    }

    // An already-stored 48-character ID retains the same raw key format.
    const auto existing = "sid_a=" + std::string(48, 'b');
    const auto loaded = expect("/session-ids/a/read", existing, "existing");
    EXPECT_TRUE(loaded.cookie_.empty());
    const auto rotated = expect("/session-ids/a/rotate", existing, "rotate");
    EXPECT_EQ(rotated.cookie_.size(), std::size_t{6 + 48});
    EXPECT_TRUE(rotated.cookie_ != existing);
    expect("/session-ids/a/read", existing, "empty");
    expect("/session-ids/a/read", rotated.cookie_, "existing");
    expect("/session-ids/a/clear", rotated.cookie_, "clear");
    expect("/session-ids/a/read", rotated.cookie_, "empty");

    const auto created = expect("/session-ids/b/set", {}, "set");
    const auto stale = request("/session-ids/b/stale", created.cookie_);
    EXPECT_EQ(stale.status_, 409U);
    EXPECT_TRUE(stale.cookie_.empty());
    expect("/session-ids/b/read", created.cookie_, "empty");
    server.finish();
    ruvia::test::reset_app_callbacks(ruvia::app());
}

#endif

// A document root shares paths with a guarded wildcard route. This executable
// owns its application, so the document root cannot leak into other suites.

namespace document_root_test {

namespace fs = std::filesystem;

class document_root_guard final {
public:
    ruvia::task<void> invoke(ruvia::context& c, ruvia::next& next_value) {
        if (!c.req().header("X-Test-Credential").has_value()) {
            throw ruvia::http_error({.status_ = ruvia::http_status::unauthorized});
        }
        co_await next_value();
    }
};

class document_root_routes final {
    RUVIA_ROUTES_BEGIN(document_root_routes)
    RUVIA_GET("/private/*", private_file, document_root_guard);
    RUVIA_GET("/admin/panel/*", private_file, document_root_guard);
    // Makes PROPFIND a method this server knows, for the 405-vs-501 split.
    RUVIA_METHOD("PROPFIND", "/dav", private_file);
    RUVIA_POST("/encoded/:slug", post_slug);
    RUVIA_GET("/a%20b.txt", get_space_route);
    RUVIA_ROUTES_END

    ruvia::task<ruvia::http_response> private_file(ruvia::context& c) {
        co_return c.text("guarded");
    }

    ruvia::task<ruvia::http_response> post_slug(ruvia::context& c) {
        co_return c.text("posted:" + std::string(c.req().param("slug").value_or("")));
    }

    ruvia::task<ruvia::http_response> get_space_route(ruvia::context& c) {
        co_return c.text("from-route");
    }
};

void populate_document_root(const fs::path& dir) {
    fs::create_directories(dir / "private");
    fs::create_directories(dir / "admin" / "panel");
    fs::create_directories(dir / "encoded");
    std::ofstream(dir / "private" / "file.txt") << "secret";
    std::ofstream(dir / "admin" / "panel" / "secret.txt") << "secret";
    std::ofstream(dir / "public.txt") << "public";
    std::ofstream(dir / "my report.txt") << "spaced";
    std::ofstream(dir / "encoded" / "readme.txt") << "readme-from-disk";
    std::ofstream(dir / "encoded" / "hello world.txt") << "hello-world-from-disk";
    std::ofstream(dir / "a%20b.txt") << "literal-percent";
}

// Sends one request on a fresh connection and returns the complete response.
[[nodiscard]] std::string exchange(asio::io_context& io, const asio::ip::tcp::endpoint& endpoint,
    std::string_view target, std::string_view method = "GET") {
    std::string request(method);
    request.append(" ");
    request.append(target);
    request.append(" HTTP/1.1\r\nHost: a\r\nConnection: close\r\nContent-Length: 0\r\n\r\n");
    return ruvia::test::exchange(io, endpoint, request);
}

[[nodiscard]] bool has_status(std::string_view response, std::string_view status) {
    return response.starts_with("HTTP/1.1 ") && response.substr(9).starts_with(status);
}

}  // namespace document_root_test

TEST(Ruvia, document_root_never_serves_targets_rejected_by_routing) {
    using namespace document_root_test;
    ruvia::test::reset_app_callbacks(ruvia::app());
    const ruvia::test::scoped_temp_directory directory("ruvia-document-root");
    const auto& dir = directory.path();
    populate_document_root(dir);
    ruvia::document_root_config config;
    config.root_ = dir;
    config.static_options_.file_types_ =
        ruvia::static_file_type_policy{.kind_ = ruvia::static_file_type_policy::kind::all};
    ruvia::app().document_root(std::move(config));

    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io);

    // The document root still answers paths no route claims.
    const auto public_file = exchange(io, server.endpoint(), "/public.txt");
    EXPECT_TRUE(has_status(public_file, "200"));
    EXPECT_TRUE(public_file.ends_with("public"));

    // The guarded route owns its prefix, under every equivalent spelling
    // (RFC 3986 6.2.2.2): an encoded unreserved character reaches the same
    // route and its guard.
    for (const std::string_view target : {"/private/file.txt", "/private/file.txt/", "/admin/panel/secret.txt",
             "/%70rivate/file.txt", "/admin/%70anel/secret.txt"}) {
        EXPECT_TRUE(has_status(exchange(io, server.endpoint(), target), "401"));
    }

    // Dot-segment and empty-segment ("//", trailing '/') spellings of guarded
    // routes must not fall back to the document root.
    for (const std::string_view target :
        {"/private/./file.txt", "/private/%2E/file.txt", "/private/%2e/file.txt", "/private/x/../file.txt",
            "//private/file.txt", "/admin//panel/secret.txt", "//public.txt", "/public.txt/"}) {
        const auto response = exchange(io, server.endpoint(), target);
        EXPECT_TRUE(has_status(response, "404"));
        EXPECT_TRUE(response.find("secret") == std::string::npos);
        EXPECT_TRUE(response.find("public") == std::string::npos);
    }
    // Another method on an encoded spelling answers for the route, never the file.
    const auto encoded_post = exchange(io, server.endpoint(), "/%70rivate/file.txt", "POST");
    EXPECT_TRUE(has_status(encoded_post, "405"));
    EXPECT_TRUE(encoded_post.find("secret") == std::string::npos);
    EXPECT_TRUE(has_status(exchange(io, server.endpoint(), "/%70rivate/file.txt", "OPTIONS"), "204"));

    // The request path is decoded exactly once into the file name, and no
    // escape may reveal a separator routing never split on.
    const auto spaced = exchange(io, server.endpoint(), "/my%20report.txt");
    EXPECT_TRUE(has_status(spaced, "200"));
    EXPECT_TRUE(spaced.ends_with("spaced"));
    for (const std::string_view target : {"/my%2520report.txt", "/private%2Ffile.txt", "/private%5Cfile.txt"}) {
        const auto response = exchange(io, server.endpoint(), target);
        EXPECT_TRUE(has_status(response, "404"));
        EXPECT_TRUE(response.find("secret") == std::string::npos);
    }

    // RFC 9110 15.5.6 and 9.3.7: a document-root file exists for every method,
    // so OPTIONS describes it and other methods are 405 with Allow, while a
    // missing file stays 404 and an unregistered token stays 501.
    const auto options = exchange(io, server.endpoint(), "/public.txt", "OPTIONS");
    EXPECT_TRUE(has_status(options, "204"));
    EXPECT_TRUE(options.find("\r\nAllow: GET, HEAD, OPTIONS\r\n") != std::string::npos);
    const auto post = exchange(io, server.endpoint(), "/my%20report.txt", "POST");
    EXPECT_TRUE(has_status(post, "405"));
    EXPECT_TRUE(post.find("\r\nAllow: GET, HEAD, OPTIONS\r\n") != std::string::npos);
    EXPECT_TRUE(has_status(exchange(io, server.endpoint(), "/missing.txt", "POST"), "404"));
    EXPECT_TRUE(has_status(exchange(io, server.endpoint(), "/missing.txt", "OPTIONS"), "404"));
    EXPECT_TRUE(has_status(exchange(io, server.endpoint(), "/public.txt", "BREW"), "501"));
    // A registered extension token is known here, so the file answers 405 too.
    const auto propfind = exchange(io, server.endpoint(), "/public.txt", "PROPFIND");
    EXPECT_TRUE(has_status(propfind, "405"));
    EXPECT_TRUE(propfind.find("\r\nAllow: GET, HEAD, OPTIONS\r\n") != std::string::npos);

    // Percent-encoded document-root paths alongside a sibling method route on the same path pattern.
    const auto encoded_readme = exchange(io, server.endpoint(), "/encoded/%72eadme.txt", "GET");
    EXPECT_TRUE(has_status(encoded_readme, "200"));
    EXPECT_TRUE(encoded_readme.ends_with("readme-from-disk"));
    const auto encoded_space = exchange(io, server.endpoint(), "/encoded/hello%20world.txt", "GET");
    EXPECT_TRUE(has_status(encoded_space, "200"));
    EXPECT_TRUE(encoded_space.ends_with("hello-world-from-disk"));
    const auto encoded_options = exchange(io, server.endpoint(), "/encoded/%72eadme.txt", "OPTIONS");
    EXPECT_TRUE(has_status(encoded_options, "204"));
    EXPECT_TRUE(encoded_options.find("\r\nAllow: GET, POST, HEAD, OPTIONS\r\n") != std::string::npos);
    const auto delete_encoded = exchange(io, server.endpoint(), "/encoded/%72eadme.txt", "DELETE");
    EXPECT_TRUE(has_status(delete_encoded, "405"));
    EXPECT_TRUE(delete_encoded.find("\r\nAllow: GET, POST, HEAD, OPTIONS\r\n") != std::string::npos);
    const auto literal_percent = exchange(io, server.endpoint(), "/a%2520b.txt", "GET");
    EXPECT_TRUE(has_status(literal_percent, "200"));
    EXPECT_TRUE(literal_percent.ends_with("literal-percent"));
    server.finish();
}

// A multi-worker application serves a document root that changes while it
// runs. This executable owns that application, so the root cannot leak into
// other suites.

namespace document_root_refresh_test {

namespace fs = std::filesystem;
using std::chrono::steady_clock;

constexpr std::size_t worker_count = 3;
constexpr std::chrono::milliseconds refresh_interval{20};
constexpr std::chrono::seconds observation_timeout{10};

std::atomic<std::size_t> next_worker_id{0};
std::atomic<bool> held_request_started{false};
std::atomic<bool> release_held_request{true};

struct root_worker_state final {
    root_worker_state() {
        label_.append(std::to_string(next_worker_id.fetch_add(1, std::memory_order_relaxed)));
    }
    std::string label_{};
};

class root_request_observation final {
public:
    static constexpr bool ruvia_runs_on_unmatched_requests = true;

    ruvia::task<void> invoke(ruvia::context& context_value, ruvia::next& next_value) {
        if (context_value.req().header("X-Hold-Root").has_value()) {
            held_request_started.store(true, std::memory_order_release);
            while (!release_held_request.load(std::memory_order_acquire)) {
                if (co_await ruvia::sleep_for(context_value.worker(), std::chrono::milliseconds(1),
                        context_value.stop_token()) == ruvia::timer_sleep_result::stop_requested) {
                    co_return;
                }
            }
        }
        co_await next_value();
        context_value.edit_response().header(
            "X-Document-Root-Worker", context_value.worker_state<root_worker_state>().label_);
    }
};

[[nodiscard]] const fs::path& test_directory() {
    static const ruvia::test::scoped_temp_directory directory("ruvia-document-root-refresh");
    return directory.path();
}

[[nodiscard]] fs::path document_root_dir() {
    return test_directory() / "root";
}

[[nodiscard]] fs::path retired_root_dir() {
    return test_directory() / "retired";
}

// The application is configured through this symlink where the platform
// creates one unprivileged, as an atomic-deploy layout would.
[[nodiscard]] fs::path configured_root() {
#ifdef _WIN32
    return document_root_dir();
#else
    return test_directory() / "link";
#endif
}

[[nodiscard]] fs::path version_file() {
    return document_root_dir() / "version.txt";
}

[[nodiscard]] ruvia::server_config multi_worker_config() {
    return {.worker_count_ = worker_count, .connection_scan_interval_ = std::chrono::milliseconds(5)};
}

template <typename fn_type>
[[nodiscard]] bool retry_until_deadline(fn_type&& attempt) {
    ruvia::test::wait_until(std::forward<fn_type>(attempt), observation_timeout);
    return true;
}

// Publish each filesystem mutation atomically. A truncate or remove_all could
// otherwise expose a valid intermediate tree that a refresh may publish.
// Windows can briefly refuse a rename while a refresh pass inspects the tree.
[[nodiscard]] bool write_version(std::string_view body) {
    return retry_until_deadline([body] {
        std::error_code ec;
        fs::create_directories(document_root_dir(), ec);
        if (ec) {
            return false;
        }
        const auto staged = document_root_dir() / ".version.txt.next";
        std::ofstream output(staged, std::ios::binary | std::ios::trunc);
        output << body;
        output.close();
        if (output.fail()) {
            return false;
        }
        fs::rename(staged, version_file(), ec);
        return !ec;
    });
}

[[nodiscard]] bool remove_document_root() {
    return retry_until_deadline([] {
        std::error_code ec;
        fs::rename(document_root_dir(), retired_root_dir(), ec);
        return !ec;
    });
}

// The application is shared by every case in this executable. It is configured
// once, with the root populated: startup builds the first index synchronously.
[[nodiscard]] steady_clock::time_point configure_application() {
    static const auto configured_at = [] {
        std::error_code ignored;
        fs::remove_all(document_root_dir(), ignored);
        fs::remove_all(retired_root_dir(), ignored);
        fs::create_directories(document_root_dir());
        std::ofstream(version_file(), std::ios::binary) << "initial";
#ifndef _WIN32
        fs::remove(configured_root(), ignored);
        fs::create_directory_symlink(document_root_dir(), configured_root());
#endif

        ruvia::document_root_config config;
        config.root_ = configured_root();
        config.static_options_ =
            {.file_types_ = {.kind_ = ruvia::static_file_type_policy::kind::all}};
        config.refresh_interval_ = refresh_interval;
        ruvia::app().use_worker_state<root_worker_state>().use<root_request_observation>();
        ruvia::app().document_root(std::move(config));
        return steady_clock::now();
    }();
    return configured_at;
}

// Sends one request on a fresh connection and returns the complete response.
[[nodiscard]] std::string exchange(
    asio::io_context& io, const asio::ip::tcp::endpoint& endpoint, std::string_view target) {
    std::string request("GET ");
    request.append(target);
    request.append(" HTTP/1.1\r\nHost: a\r\nConnection: close\r\n\r\n");
    return ruvia::test::exchange(io, endpoint, request);
}

// Observe distinct workers rather than whichever one accepts first. Windows
// routes fresh connections away from a worker that holds a long-lived request.
[[nodiscard]] bool eventually_serves(
    asio::io_context& io, const asio::ip::tcp::endpoint& endpoint, std::string_view body,
    std::size_t required_workers = worker_count) {
    std::set<std::string> observed_workers{};
    return retry_until_deadline([&] {
        const auto response = exchange(io, endpoint, "/version.txt");
        if (!response.starts_with("HTTP/1.1 200") || !response.ends_with(body)) {
            return false;
        }
        constexpr std::string_view worker_header = "\r\nX-Document-Root-Worker: ";
        const auto header = response.find(worker_header);
        if (header == std::string::npos) {
            return false;
        }
        const auto label = header + worker_header.size();
        const auto end = response.find("\r\n", label);
        if (end == std::string::npos) {
            return false;
        }
        observed_workers.emplace(std::string_view(response).substr(label, end - label));
        return observed_workers.size() >= required_workers;
    });
}

[[nodiscard]] std::size_t refresh_failures() {
    return ruvia::app().stats().document_root_refresh_failures_;
}

}  // namespace document_root_refresh_test

TEST(Ruvia, document_root_refresh_updates_multi_worker_application) {
    using namespace document_root_test;
    using namespace document_root_refresh_test;
    ruvia::test::reset_app_callbacks(ruvia::app());
    static_cast<void>(configure_application());
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io, multi_worker_config());
    EXPECT_EQ(ruvia::app().workers().size(), worker_count);

    // Every version has a distinct size, so a change is indexed even where the
    // filesystem's modification time is coarse.
    EXPECT_TRUE(write_version("first version"));
    EXPECT_TRUE(eventually_serves(io, server.endpoint(), "first version"));

    EXPECT_TRUE(write_version("second, longer version"));
    EXPECT_TRUE(eventually_serves(io, server.endpoint(), "second, longer version"));
    server.finish();
}

TEST(Ruvia, document_root_refresh_keeps_old_requests_across_multiple_publications) {
    using namespace document_root_test;
    using namespace document_root_refresh_test;
    static_cast<void>(configure_application());
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io, multi_worker_config());

    EXPECT_TRUE(write_version("old leased snapshot"));
    EXPECT_TRUE(eventually_serves(io, server.endpoint(), "old leased snapshot"));
    held_request_started.store(false, std::memory_order_relaxed);
    release_held_request.store(false, std::memory_order_release);

    asio::ip::tcp::socket held(io);
    held.connect(server.endpoint());
    constexpr std::string_view request =
        "GET /version.txt HTTP/1.1\r\nHost: a\r\nX-Hold-Root: yes\r\nConnection: close\r\n\r\n";
    asio::write(held, asio::buffer(request));
    EXPECT_TRUE(retry_until_deadline([] {
        return held_request_started.load(std::memory_order_acquire);
    }));

    EXPECT_TRUE(write_version("new snapshot with a request still held"));
    EXPECT_TRUE(eventually_serves(io, server.endpoint(), "new snapshot with a request still held", worker_count - 1));
    EXPECT_TRUE(write_version("another publication before the old request finishes"));
    EXPECT_TRUE(eventually_serves(io, server.endpoint(), "another publication before the old request finishes", worker_count - 1));

    release_held_request.store(true, std::memory_order_release);
    std::string response{};
    std::error_code error;
    asio::read(held, asio::dynamic_buffer(response), error);
    EXPECT_TRUE(response.starts_with("HTTP/1.1 200"));
    EXPECT_TRUE(response.ends_with("old leased snapshot"));
    held.close();
    EXPECT_TRUE(eventually_serves(io, server.endpoint(), "another publication before the old request finishes"));
    server.finish();
}

#ifndef _WIN32
TEST(Ruvia, document_root_refresh_follows_a_retargeted_symlink_root) {
    using namespace document_root_test;
    using namespace document_root_refresh_test;
    static_cast<void>(configure_application());
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io, multi_worker_config());

    const auto release = test_directory() / "release";
    const auto staged_link = test_directory() / "link.next";
    std::error_code ignored;
    fs::remove_all(release, ignored);
    fs::remove(staged_link, ignored);
    fs::create_directories(release);
    std::ofstream(release / "version.txt", std::ios::binary) << "retargeted release";

    // Swap the link atomically, as `ln -sfn` deploys do.
    const auto retarget = [&](const fs::path& target) {
        fs::create_directory_symlink(target, staged_link);
        fs::rename(staged_link, configured_root());
    };
    retarget(release);
    EXPECT_TRUE(eventually_serves(io, server.endpoint(), "retargeted release"));

    retarget(document_root_dir());
    EXPECT_TRUE(write_version("back on the original root"));
    EXPECT_TRUE(eventually_serves(io, server.endpoint(), "back on the original root"));
    fs::remove_all(release, ignored);
    server.finish();
}
#endif

TEST(Ruvia, document_root_refresh_failures_count_once_per_application) {
    using namespace document_root_test;
    using namespace document_root_refresh_test;
    static_cast<void>(configure_application());
    auto& io = ruvia::test::new_test_io_context();
    ruvia::test::http_server_fixture server(io, multi_worker_config());

    EXPECT_TRUE(write_version("cached before removal"));
    EXPECT_TRUE(eventually_serves(io, server.endpoint(), "cached before removal"));
    const auto failures_before = refresh_failures();
    const auto removed_at = steady_clock::now();
    EXPECT_TRUE(remove_document_root());

    // Observe a window of several intervals with the root missing. The
    // application's one refresher sleeps a full interval before every pass, so
    // it can fail at most once per interval in that window plus one pass that
    // was already sleeping, however many workers serve the root.
    std::size_t failures = 0;
    EXPECT_TRUE(retry_until_deadline([&] {
        failures = refresh_failures() - failures_before;
        return failures != 0 && steady_clock::now() - removed_at >= 10 * refresh_interval;
    }));
    const auto window_intervals =
        static_cast<std::size_t>((steady_clock::now() - removed_at) / refresh_interval);
    EXPECT_TRUE(failures >= 1);
    EXPECT_TRUE(failures <= window_intervals + 1);
    // Configuring static_options_ without resident limits still keeps the
    // application's small-file snapshots. Failed refreshes must not turn
    // the last complete snapshot into an unreadable file-backed response.
    EXPECT_TRUE(eventually_serves(io, server.endpoint(), "cached before removal"));

    // The refresher keeps running after failures and recovers once the root
    // is back.
    EXPECT_TRUE(write_version("restored"));
    EXPECT_TRUE(eventually_serves(io, server.endpoint(), "restored"));
    EXPECT_TRUE(retry_until_deadline([] {
        std::error_code ec;
        fs::remove_all(retired_root_dir(), ec);
        return !ec;
    }));
    server.finish();
}

namespace document_root_shutdown_test {

using namespace std::chrono_literals;

std::atomic<bool> held_request_started{false};
std::atomic<bool> held_request_cancelled{false};

class hold_root_request final {
public:
    static constexpr bool ruvia_runs_on_unmatched_requests = true;

    ruvia::task<void> invoke(ruvia::context& context_value, ruvia::next& next_value) {
        if (context_value.req().header("X-Hold-Root").has_value()) {
            held_request_started.store(true, std::memory_order_release);
            if (co_await ruvia::sleep_for(context_value.worker(), 1h, context_value.stop_token()) ==
                ruvia::timer_sleep_result::stop_requested) {
                held_request_cancelled.store(true, std::memory_order_release);
                context_value.respond(context_value.text("cancelled"));
                co_return;
            }
        }
        co_await next_value();
    }
};

void replace_version(const std::filesystem::path& root, std::string_view bytes) {
    ruvia::test::wait_until([&] {
        const auto staged = root / ".version.next";
        std::ofstream output(staged, std::ios::binary | std::ios::trunc);
        output << bytes;
        output.close();
        if (output.fail()) {
            return false;
        }
        std::error_code error;
        std::filesystem::rename(staged, root / "version.txt", error);
        return !error;
    },
        5s);
}

}  // namespace document_root_shutdown_test

TEST(Ruvia, document_root_shutdown_cancels_old_snapshot_lease_and_restarts_refresh) {
    using namespace document_root_test;
    using namespace document_root_shutdown_test;
    ruvia::test::reset_app_callbacks(ruvia::app());
    using namespace document_root_shutdown_test;
    const ruvia::test::scoped_temp_directory directory("ruvia-document-root-shutdown");
    replace_version(directory.path(), "old leased snapshot");
    asio::io_context io;
    ruvia::test::tcp_port_reservation reservation(io);
    const auto endpoint = reservation.endpoint();
    static std::binary_semaphore started{0};
    auto& app = ruvia::app();
    ruvia::document_root_config config;
    config.root_ = directory.path();
    config.refresh_interval_ = 20ms;
    app.server({.worker_count_ = 3, .connection_scan_interval_ = 5ms})
        .listen({.address_ = "127.0.0.1", .http_ = endpoint.port()})
        .use<hold_root_request>()
        .document_root(std::move(config))
        .on_start([] { started.release(); });
    constexpr std::string_view get_version =
        "GET /version.txt HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    {
        ruvia::test::app_run runner(app);
        ASSERT_TRUE(started.try_acquire_for(5s));
        asio::ip::tcp::socket held(io);
        held.connect(endpoint);
        constexpr std::string_view request =
            "GET /version.txt HTTP/1.1\r\nHost: localhost\r\nX-Hold-Root: yes\r\nConnection: close\r\n\r\n";
        asio::write(held, asio::buffer(request));
        ruvia::test::wait_until([] {
            return held_request_started.load(std::memory_order_acquire);
        },
            5s);
        replace_version(directory.path(), "published while the old request remains held");
        ruvia::test::wait_until([&] {
            const auto response = ruvia::test::exchange(io, endpoint, get_version);
            return response.starts_with("HTTP/1.1 200") &&
                   response.ends_with("published while the old request remains held");
        },
            5s);
        app.stop();
        runner.join();
        EXPECT_TRUE(held_request_cancelled.load(std::memory_order_acquire));
        std::error_code error;
        const auto response = ruvia::test::read_to_end(held, error);
        EXPECT_TRUE(response.empty());
        EXPECT_TRUE(error == asio::error::eof || error == asio::error::connection_reset);
    }
    {
        ruvia::test::app_run restarted(app);
        ASSERT_TRUE(started.try_acquire_for(5s));
        replace_version(directory.path(), "refreshed after cancellation");
        ruvia::test::wait_until([&] {
            const auto response = ruvia::test::exchange(io, endpoint, get_version);
            return response.starts_with("HTTP/1.1 200") && response.ends_with("refreshed after cancellation");
        },
            5s);
        app.stop();
        restarted.join();
    }
}
