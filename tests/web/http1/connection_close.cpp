#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <semaphore>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#include <asio/buffer.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/ssl/error.hpp>
#include <asio/ssl/stream.hpp>
#include <asio/write.hpp>
#include <openssl/bio.h>
#include <openssl/ssl.h>

#include "ruvia/core/event_loop.h"
#include "ruvia/core/stop_token.h"
#include "ruvia/core/task_scope.h"
#include "ruvia/core/timer.h"
#include "ruvia/web/app.h"
#include "ruvia/web/context.h"
#include "ruvia/web/controller.h"
#include "ruvia/web/http_client.h"
#include "ruvia/web/websocket.h"

#include "test_harness.h"
#include "test_tls_crypto.h"
#include "test_tls_identity.h"

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

// Reads until the server ends the stream. TLS reports a close_notify as eof
// and a TCP close without it as stream_truncated.
template <typename stream_type>
std::error_code read_to_end(stream_type& stream, std::string& received) {
    std::array<char, 16 * 1024> buffer;
    std::error_code error;
    while (!error) {
        const auto size = stream.read_some(asio::buffer(buffer), error);
        received.append(buffer.data(), size);
    }
    return error;
}

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
    ruvia::testing::test_context& ruvia_ctx, asio::io_context& io, const asio::ip::tcp::endpoint& endpoint) {
    asio::ip::tcp::socket socket(io);
    socket.open(endpoint.protocol());
    socket.set_option(asio::socket_base::receive_buffer_size(64 * 1024));
    socket.connect(endpoint);
    std::string request = "POST /close/large HTTP/1.1\r\nHost: localhost\r\nContent-Length: 1048576\r\n\r\n";
    request.append(std::size_t{64} * 1024, 'u');
    asio::write(socket, asio::buffer(request));
    std::this_thread::sleep_for(200ms);
    std::string response;
    const auto end = read_to_end(socket, response);
    const auto head_end = response.find("\r\n\r\n");
    RUVIA_CHECK(response.starts_with("HTTP/1.1 200 "));
    RUVIA_CHECK(head_end != std::string::npos);
    if (head_end != std::string::npos) {
        RUVIA_CHECK_EQ(response.size() - head_end - 4, large_response_bytes);
    }
    RUVIA_CHECK(end == asio::error::eof);
}

// A keep-alive connection that idles past idle_timeout ends with close_notify.
void idle_tls_connection_ends_with_close_notify(
    ruvia::testing::test_context& ruvia_ctx, asio::io_context& io, const asio::ip::tcp::endpoint& endpoint) {
    asio::ssl::context client(asio::ssl::context::tls_client);
    client.set_verify_mode(asio::ssl::verify_none);
    const auto stream = connect_tls(io, client, endpoint);
    // 服务端可在发送返回前开始空闲计时，所以先记录请求发送的起点。
    const auto started = std::chrono::steady_clock::now();
    asio::write(*stream, asio::buffer(std::string_view("GET /close/item HTTP/1.1\r\nHost: localhost\r\n\r\n")));
    std::string response;
    const auto end = read_to_end(*stream, response);
    RUVIA_CHECK(response.starts_with("HTTP/1.1 200 "));
    RUVIA_CHECK(response.ends_with("item"));
    RUVIA_CHECK(std::chrono::steady_clock::now() - started >= idle_timeout);
    RUVIA_CHECK(end == asio::error::eof);
}

// An HTTP/2 connection that idles past idle_timeout first sends GOAWAY, then
// close_notify (RFC 9113 §9.1). The peer acknowledges SETTINGS and PING so a
// graceful drain can complete.
void idle_http2_connection_sends_goaway_then_close_notify(
    ruvia::testing::test_context& ruvia_ctx, asio::io_context& io, const asio::ip::tcp::endpoint& endpoint) {
    asio::ssl::context client(asio::ssl::context::tls_client);
    client.set_verify_mode(asio::ssl::verify_none);
    static constexpr unsigned char alpn[] = {2, 'h', '2'};
    RUVIA_CHECK_EQ(SSL_CTX_set_alpn_protos(client.native_handle(), alpn, sizeof(alpn)), 0);
    const auto stream = connect_tls(io, client, endpoint);
    constexpr std::string_view preface = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
    constexpr std::array<char, 9> settings{0, 0, 0, 4, 0, 0, 0, 0, 0};
    std::string opening(preface);
    opening.append(settings.data(), settings.size());
    asio::write(*stream, asio::buffer(opening));

    std::string received;
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
    RUVIA_CHECK(goaway);
    RUVIA_CHECK(end == asio::error::eof);
}

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
    ruvia::testing::test_context& ruvia_ctx, asio::io_context& io, const asio::ip::tcp::endpoint& endpoint) {
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

    std::string received;
    std::array<char, 1024> buffer{};
    std::error_code error;
    while (received.find("\x88") == std::string::npos && !error) {
        const auto count = stream->read_some(asio::buffer(buffer), error);
        received.append(buffer.data(), count);
    }
    RUVIA_CHECK(!error);
    // Reply with a masked client Close frame (code 1000, zero mask).
    constexpr std::array<unsigned char, 8> client_close{0x88, 0x82, 0x00, 0x00, 0x00, 0x00, 0x03, 0xe8};
    asio::write(*stream, asio::buffer(client_close), error);
    RUVIA_CHECK(!error);
    while (!error) {
        const auto count = stream->read_some(asio::buffer(buffer), error);
        received.append(buffer.data(), count);
    }
    RUVIA_CHECK(error == asio::error::eof);
}

// RFC 9846 §6.1 / RFC 6455 §7.1.7: when a wss:// peer truncates TLS / drops TCP
// mid-frame without sending a WebSocket Close frame or close_notify, the server
// aborts the connection without sending a clean TLS close_notify.
void wss_truncated_peer_eof_omits_close_notify(
    ruvia::testing::test_context& ruvia_ctx, asio::io_context& io, const asio::ip::tcp::endpoint& endpoint) {
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

    std::string head;
    std::array<char, 1024> buffer{};
    std::error_code error;
    while (head.find("\r\n\r\n") == std::string::npos && !error) {
        const auto count = stream->read_some(asio::buffer(buffer), error);
        head.append(buffer.data(), count);
    }
    RUVIA_CHECK(!error);
    constexpr std::array<char, 1> partial_ws_frame{'\x81'};
    asio::write(*stream, asio::buffer(partial_ws_frame), error);
    RUVIA_CHECK(!error);
    stream->next_layer().shutdown(asio::ip::tcp::socket::shutdown_send, error);
    RUVIA_CHECK(!error);

    std::string rest;
    while (!error) {
        const auto count = stream->read_some(asio::buffer(buffer), error);
        rest.append(buffer.data(), count);
    }
    RUVIA_CHECK(rest.empty());
    RUVIA_CHECK(error == asio::ssl::error::stream_truncated);
}

// RFC 9112 §9.8 / RFC 9846 §6.1: cancelling a multi-record HTTPS request write
// after the first record is taken synchronously by the socket must close
// without sending a clean TLS close_notify.
void client_cancelled_multi_record_tls_write_omits_close_notify(
    ruvia::testing::test_context& ruvia_ctx, ruvia::test::tls_identity& identity) {
    asio::io_context server_io;
    asio::ip::tcp::acceptor acceptor(server_io, {asio::ip::address_v4::loopback(), 0});
    const auto endpoint = acceptor.local_endpoint();

    std::string server_received;
    std::error_code server_read_ec;
    std::thread server_thread([&] {
        tls_stream srv(server_io, identity.context_);
        acceptor.accept(srv.lowest_layer());
        std::error_code ec;
        srv.handshake(asio::ssl::stream_base::server, ec);
        std::array<char, 4096> buf{};
        std::string req1;
        while (req1.find("\r\n\r\n") == std::string::npos && !ec) {
            req1.append(buf.data(), srv.read_some(asio::buffer(buf), ec));
        }
        constexpr std::string_view resp1 = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok";
        asio::write(srv, asio::buffer(resp1), ec);

        while (!server_read_ec) {
            const auto n = srv.read_some(asio::buffer(buf), server_read_ec);
            server_received.append(buf.data(), n);
        }
    });

    asio::io_context client_io;
    auto attachment = ruvia::attach_event_loop(client_io);
    ruvia::http_client client(attachment.loop(), {
                                                     .scheme_ = ruvia::http_scheme::https,
                                                     .host_ = "127.0.0.1",
                                                     .port_ = endpoint.port(),
                                                     .protocol_ = ruvia::http_client_protocol::http1_only,
                                                     .tls_ = {.mode_ = ruvia::client_tls_mode::skip_verification},
                                                 });

    bool post_cancelled = false;
    auto run_client = [&]() -> ruvia::task<void> {
        auto h = client.handle();
        {
            auto r1 = co_await h.send({.method_ = "GET", .target_ = "/warmup"});
            (void)co_await r1.body().read_all();
        }
        ruvia::stop_source cancel_src;
        const std::string body(20480, 'B');
        asio::post(client_io, [&] {
            cancel_src.request_stop();
        });
        try {
            auto r2 = co_await h.with_options({.stop_token_ = cancel_src.token()})
                          .send({
                              .method_ = "POST",
                              .target_ = "/upload",
                              .content_ = ruvia::http_client_request_content_view::bytes(body),
                          });
            (void)co_await r2.body().read_all();
        } catch (const ruvia::http_client_error& ex) {
            post_cancelled = ex.code() == ruvia::http_client_error::code_type::cancelled;
        }
        co_await client.shutdown();
        attachment.stop();
    };
    auto root = attachment.loop().start(run_client());

    attachment.run();
    root.get();
    server_thread.join();

    RUVIA_CHECK(post_cancelled);
    RUVIA_CHECK(server_received.size() > 0 && server_received.size() < 20480);
    RUVIA_CHECK(server_read_ec == asio::ssl::error::stream_truncated);
}

// A write_timeout_ firing while a speculative TLS record write completion is
// queued via asio::post must abort the write immediately instead of hanging on
// the reactor on the next record.
void client_tls_write_timeout_during_speculative_post_does_not_hang(
    ruvia::testing::test_context& ruvia_ctx, ruvia::test::tls_identity& identity) {
    asio::io_context server_io;
    asio::ip::tcp::acceptor acceptor(server_io, {asio::ip::address_v4::loopback(), 0});
    const auto endpoint = acceptor.local_endpoint();

    std::binary_semaphore read_32k{0};
    std::binary_semaphore read_32k_done{0};
    std::atomic<bool> release_server{false};
    std::thread server_thread([&] {
        tls_stream srv(server_io, identity.context_);
        acceptor.accept(srv.lowest_layer());
        std::error_code ec;
        srv.lowest_layer().set_option(asio::socket_base::receive_buffer_size(4096), ec);
        srv.handshake(asio::ssl::stream_base::server, ec);
        std::array<char, 4096> buf{};
        std::string req1;
        while (req1.find("\r\n\r\n") == std::string::npos && !ec) {
            req1.append(buf.data(), srv.read_some(asio::buffer(buf), ec));
        }
        constexpr std::string_view resp1 = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok";
        asio::write(srv, asio::buffer(resp1), ec);

        read_32k.acquire();
        std::vector<char> raw(32768);
        (void)srv.next_layer().read_some(asio::buffer(raw), ec);
        read_32k_done.release();

        while (!release_server.load()) {
            std::this_thread::sleep_for(10ms);
        }
    });

    asio::io_context client_io;
    auto attachment = ruvia::attach_event_loop(client_io);
    ruvia::http_client client(attachment.loop(), {
                                                     .scheme_ = ruvia::http_scheme::https,
                                                     .host_ = "127.0.0.1",
                                                     .port_ = endpoint.port(),
                                                     .write_timeout_ = 40ms,
                                                     .request_timeout_ = std::nullopt,
                                                     .protocol_ = ruvia::http_client_protocol::http1_only,
                                                     .tls_ = {.mode_ = ruvia::client_tls_mode::skip_verification},
                                                 });

    std::atomic<bool> post_completed{false};
    bool timed_out = false;
    auto run_client = [&]() -> ruvia::task<void> {
        auto h = client.handle();
        {
            auto r1 = co_await h.send({.method_ = "GET", .target_ = "/warmup"});
            (void)co_await r1.body().read_all();
        }
        std::string big_body(8 * 1024 * 1024, 'C');
        const auto worker = attachment.loop().handle();
        auto trigger = [&]() -> ruvia::task<void> {
            (void)co_await ruvia::sleep_for(worker, 5ms);
            read_32k.release();
            read_32k_done.acquire();
            std::this_thread::sleep_for(50ms);
        };
        ruvia::task_scope tasks(worker);
        tasks.spawn(trigger());
        try {
            auto r2 = co_await h.send({
                .method_ = "POST",
                .target_ = "/hang",
                .content_ = ruvia::http_client_request_content_view::bytes(big_body),
            });
            (void)co_await r2.body().read_all();
        } catch (const ruvia::http_client_error& ex) {
            timed_out = ex.code() == ruvia::http_client_error::code_type::timeout;
        }
        post_completed.store(true);
        co_await tasks.join();
        co_await client.shutdown();
        attachment.stop();
    };
    auto root = attachment.loop().start(run_client());

    bool watchdog_fired = false;
    std::thread watchdog([&] {
        for (int i = 0; i < 40; ++i) {
            if (post_completed.load()) {
                return;
            }
            std::this_thread::sleep_for(25ms);
        }
        watchdog_fired = true;
        release_server.store(true);
        asio::post(client_io, [&] {
            client.close();
        });
    });

    attachment.run();
    root.get();
    release_server.store(true);
    watchdog.join();
    server_thread.join();

    RUVIA_CHECK(!watchdog_fired);
    RUVIA_CHECK(timed_out);
}
}  // namespace connection_close_test

using namespace connection_close_test;

RUVIA_TEST(server_closes_connections_gracefully) {
    ruvia::test::tls_identity identity("localhost");
    const auto private_key = identity.ca_file_.parent_path() / "key.pem";
    {
        std::unique_ptr<BIO, decltype(&BIO_free)> output(BIO_new_file(private_key.string().c_str(), "w"), BIO_free);
        if (!output || ruvia::test::write_tls_private_key(
                           output.get(), SSL_CTX_get0_privatekey(identity.context_.native_handle())) != 1) {
            throw std::runtime_error("cannot write connection close test private key");
        }
    }

    asio::io_context io;
    asio::ip::tcp::acceptor plain_reservation(io, {asio::ip::address_v4::loopback(), 0});
    asio::ip::tcp::acceptor tls_reservation(io, {asio::ip::address_v4::loopback(), 0});
    const auto plain_endpoint = plain_reservation.local_endpoint();
    const auto tls_endpoint = tls_reservation.local_endpoint();
    plain_reservation.close();
    tls_reservation.close();

    auto& app = ruvia::app();
    std::binary_semaphore started{0};
    app.server({.worker_count_ = 1, .idle_timeout_ = idle_timeout, .connection_scan_interval_ = 5ms})
        .listen({
            .address_ = "127.0.0.1",
            .http_ = plain_endpoint.port(),
            .https_ = tls_endpoint.port(),
            .tls_ = {.certificate_chain_file_ = identity.ca_file_, .private_key_file_ = private_key},
        })
        .on_start([&started] { started.release(); });
    std::exception_ptr failure;
    std::thread runner([&app, &failure] {
        try {
            app.run();
        } catch (...) {
            failure = std::current_exception();
        }
    });
    const bool ready = started.try_acquire_for(10s);
    RUVIA_CHECK(ready);
    if (ready) {
        final_response_survives_unread_request_content(ruvia_ctx, io, plain_endpoint);
        idle_tls_connection_ends_with_close_notify(ruvia_ctx, io, tls_endpoint);
        idle_http2_connection_sends_goaway_then_close_notify(ruvia_ctx, io, tls_endpoint);
        wss_clean_close_handshake_sends_close_notify(ruvia_ctx, io, tls_endpoint);
        wss_truncated_peer_eof_omits_close_notify(ruvia_ctx, io, tls_endpoint);
        client_cancelled_multi_record_tls_write_omits_close_notify(ruvia_ctx, identity);
        client_tls_write_timeout_during_speculative_post_does_not_hang(ruvia_ctx, identity);

        asio::ssl::context client(asio::ssl::context::tls_client);
        client.set_verify_mode(asio::ssl::verify_none);
        const auto unused = open_unused_tls_connection(io, client, tls_endpoint);
        std::thread stopper([&app] { app.stop(); });
        std::string received;
        const auto end = read_to_end(*unused, received);
        stopper.join();
        RUVIA_CHECK(received.empty());
        RUVIA_CHECK(end == asio::error::eof);
    } else {
        app.stop();
    }
    runner.join();
    if (failure) {
        std::rethrow_exception(failure);
    }
}
