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

#include "worker_task_fixture.h"
#if defined(RUVIA_ENABLE_HTTP2)
#include "ruvia/http/http2_connection.h"
#endif
#include <mutex>
#include <set>

#include <asio/ssl/context.hpp>

#include "ruvia/http/http3_connection.h"
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
