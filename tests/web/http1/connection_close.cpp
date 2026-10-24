#include <array>
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

#include <asio/buffer.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/ssl/error.hpp>
#include <asio/ssl/stream.hpp>
#include <asio/write.hpp>
#include <openssl/bio.h>
#include <openssl/ssl.h>

#include "ruvia/web/app.h"
#include "ruvia/web/context.h"
#include "ruvia/web/controller.h"

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
    RUVIA_ROUTES_END

    ruvia::task<ruvia::http_response> item(ruvia::context& c) {
        co_return c.text("item");
    }

    // Answers without reading the request content, which closes the connection.
    ruvia::task<ruvia::http_response> large(ruvia::context& c) {
        co_return c.text(std::string(large_response_bytes, 'L'));
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
    asio::write(*stream, asio::buffer(std::string_view("GET /close/item HTTP/1.1\r\nHost: localhost\r\n\r\n")));
    std::string response;
    const auto started = std::chrono::steady_clock::now();
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
