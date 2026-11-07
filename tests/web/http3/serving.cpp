#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <semaphore>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <variant>
#if !defined(_WIN32)
#include <sys/socket.h>
#endif

#include <asio/co_spawn.hpp>
#include <asio/connect.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/ip/udp.hpp>
#include <asio/read.hpp>
#include <asio/redirect_error.hpp>
#include <asio/ssl.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/write.hpp>
#include <openssl/bio.h>
#include <openssl/ssl.h>

#include "ruvia/core/timer.h"
#include "ruvia/http/http_datagram.h"
#include "ruvia/web/app.h"
#include "ruvia/web/context.h"
#include "ruvia/web/controller.h"
#include "ruvia/web/http_tunnel.h"
#include "ruvia/web/websocket.h"

#include "http3_test_client.h"
#include "test_harness.h"
#include "test_tls_crypto.h"
#include "test_tls_identity.h"
#include "udp_port_reservation.h"

// End to end over loopback UDP: a real application with an HTTPS listener and
// an HTTP/3 service on the same port number, four workers, and an ngtcp2 +
// nghttp3 client (http3_test_client.h).

namespace http3_serving_test {
using namespace std::chrono_literals;

constexpr auto client_timeout = 5s;
constexpr std::size_t echo_bytes = 1536 * 1024;
constexpr std::size_t file_bytes = 300 * 1024;
constexpr int stream_chunks = 8;

std::atomic<std::uint16_t> tcp_echo_port{};
std::atomic<std::uint16_t> udp_echo_port{};

std::span<const std::byte> byte_view(std::string_view value) {
    return std::as_bytes(std::span(value.data(), value.size()));
}

std::string udp_capsule(std::string_view payload) {
    std::array<char, 16> prefix{};
    const auto encoded = ruvia::encode_http_capsule_header(prefix, ruvia::http_datagram_capsule_type, payload.size() + 1);
    std::string result(prefix.data(), std::get<std::size_t>(encoded));
    result.push_back('\0');
    result.append(payload);
    return result;
}

std::string masked_frame(unsigned char opcode, std::string_view payload) {
    if (payload.size() > 125) {
        throw std::length_error("test frame exceeds short payload");
    }
    std::string frame;
    frame.push_back(static_cast<char>(0x80U | opcode));
    frame.push_back(static_cast<char>(0x80U | payload.size()));
    constexpr std::array<char, 4> mask{1, 2, 3, 4};
    frame.append(mask.data(), mask.size());
    for (std::size_t index = 0; index < payload.size(); ++index) {
        frame.push_back(static_cast<char>(payload[index] ^ mask[index % mask.size()]));
    }
    return frame;
}

std::string read_tunnel_bytes(ruvia::test::http3_test_client& client, std::int64_t stream, std::size_t size) {
    std::string bytes;
    while (bytes.size() < size) {
        auto chunk = client.read_body_chunk(stream, client_timeout);
        if (!chunk) {
            throw std::runtime_error("tunnel ended before echo");
        }
        bytes.append(*chunk);
    }
    return bytes;
}

ruvia::task<void> await_backend(ruvia::context& context, std::chrono::steady_clock::time_point deadline) {
    if (std::chrono::steady_clock::now() >= deadline ||
        co_await ruvia::sleep_for(context.worker(), 1ms, context.stop_token()) != ruvia::timer_sleep_result::elapsed) {
        throw std::runtime_error("echo backend deadline");
    }
}

// Real loopback TCP and UDP peers, independent of the application's workers.
class echo_backends final {
public:
    echo_backends()
        : tcp_(io_, {asio::ip::address_v4::loopback(), 0}),
          udp_(io_, {asio::ip::address_v4::loopback(), 0}) {
        tcp_echo_port = tcp_.local_endpoint().port();
        udp_echo_port = udp_.local_endpoint().port();
        asio::co_spawn(io_, serve_tcp(), [this](std::exception_ptr failure) { tcp_failure_ = failure; });
        asio::co_spawn(io_, serve_udp(), [this](std::exception_ptr failure) { udp_failure_ = failure; });
        thread_ = std::thread([this] { io_.run(); });
    }
    ~echo_backends() {
        io_.stop();
        if (thread_.joinable()) {
            thread_.join();
        }
    }
    void finish() {
        io_.stop();
        thread_.join();
        if (tcp_failure_) {
            std::rethrow_exception(tcp_failure_);
        }
        if (udp_failure_) {
            std::rethrow_exception(udp_failure_);
        }
    }

private:
    asio::awaitable<void> serve_tcp() {
        for (;;) {
            auto socket = co_await tcp_.async_accept(asio::use_awaitable);
            std::array<char, 16384> buffer{};
            std::error_code error;
            for (;;) {
                const auto count = co_await socket.async_read_some(asio::buffer(buffer),
                    asio::redirect_error(asio::use_awaitable, error));
                if (error == asio::error::eof) {
                    break;
                }
                if (error) {
                    throw std::system_error(error);
                }
                co_await asio::async_write(socket, asio::buffer(buffer.data(), count), asio::use_awaitable);
            }
            socket.shutdown(asio::ip::tcp::socket::shutdown_send);
        }
    }
    asio::awaitable<void> serve_udp() {
        std::array<char, 65536> buffer{};
        asio::ip::udp::endpoint sender;
        for (;;) {
            const auto count = co_await udp_.async_receive_from(asio::buffer(buffer), sender, asio::use_awaitable);
            co_await udp_.async_send_to(asio::buffer(buffer.data(), count), sender, asio::use_awaitable);
        }
    }
    asio::io_context io_;
    asio::ip::tcp::acceptor tcp_;
    asio::ip::udp::socket udp_;
    std::exception_ptr tcp_failure_;
    std::exception_ptr udp_failure_;
    std::thread thread_;
};

std::atomic<bool>& slow_entered() {
    static std::atomic<bool> value{false};
    return value;
}

std::filesystem::path& file_path() {
    static std::filesystem::path value;
    return value;
}

std::string file_content() {
    std::string content(file_bytes, '\0');
    for (std::size_t index = 0; index < content.size(); ++index) {
        content[index] = static_cast<char>('a' + index % 26);
    }
    return content;
}

class http3_routes final {
public:
    RUVIA_ROUTES_BEGIN(http3_routes)
    RUVIA_GET("/hello", hello);
    RUVIA_POST("/echo", echo);
    RUVIA_GET("/stream", stream);
    RUVIA_GET("/file", file);
    RUVIA_GET("/worker", worker);
    RUVIA_GET("/slow", slow);
    RUVIA_CONNECT("echo.test:443", tcp_tunnel);
    RUVIA_CONNECT_PROTOCOL("connect-udp", "/.well-known/masque/udp/echo.test/53/", udp_tunnel);
    RUVIA_GET_WS("/websocket", websocket);
    RUVIA_ROUTES_END

    ruvia::task<ruvia::http_response> hello(ruvia::context& context) {
        co_return context.text("hello over http/3");
    }
    ruvia::task<ruvia::http_response> echo(ruvia::context& context) {
        const auto body = co_await context.req().text();
        co_return context.text(std::pmr::string(body.data(), body.size(), context.arena()));
    }
    ruvia::task<void> stream(ruvia::context& context) {
        auto& writer = context.stream_text();
        if (co_await writer.commit() == ruvia::stream_commit_result::completed) {
            co_return;
        }
        for (int chunk = 0; chunk < stream_chunks; ++chunk) {
            co_await writer.write("chunk-" + std::to_string(chunk) + ";");
            (void)co_await writer.sleep(2ms);
        }
        co_await writer.end();
    }
    ruvia::task<ruvia::http_response> file(ruvia::context& context) {
        co_return co_await context.file({.path_ = file_path()});
    }
    // Names the worker that serves the connection.
    ruvia::task<ruvia::http_response> worker(ruvia::context& context) {
        const auto id = std::to_string(context.worker().id());
        co_return context.text(std::pmr::string(id.data(), id.size(), context.arena()));
    }
    // Ends only on its stop token.
    ruvia::task<ruvia::http_response> slow(ruvia::context& context) {
        slow_entered() = true;
        (void)co_await ruvia::sleep_for(context.worker(), 1h, context.stop_token());
        co_return context.text("slow-done");
    }
    ruvia::task<void> tcp_tunnel(ruvia::context& context) {
        asio::io_context io;
        asio::ip::tcp::socket socket(io);
        socket.connect({asio::ip::address_v4::loopback(), tcp_echo_port.load()});
        socket.non_blocking(true);
        auto& tunnel = context.tunnel();
        std::array<char, 16384> buffer{};
        while (auto chunk = co_await tunnel.read()) {
            const auto deadline = std::chrono::steady_clock::now() + client_timeout;
            std::size_t sent = 0;
            while (sent < chunk->size()) {
                std::error_code error;
                sent += socket.write_some(asio::buffer(chunk->data() + sent, chunk->size() - sent), error);
                if (error == asio::error::would_block || error == asio::error::try_again) {
                    co_await await_backend(context, deadline);
                } else if (error) {
                    throw std::system_error(error);
                }
            }
            std::size_t received = 0;
            while (received < chunk->size()) {
                std::error_code error;
                const auto count = socket.read_some(asio::buffer(buffer.data(),
                                                        (std::min)(buffer.size(), chunk->size() - received)),
                    error);
                if (error == asio::error::would_block || error == asio::error::try_again) {
                    co_await await_backend(context, deadline);
                    continue;
                }
                if (error) {
                    throw std::system_error(error);
                }
                received += count;
                co_await tunnel.write(std::string_view(buffer.data(), count));
            }
        }
        socket.shutdown(asio::ip::tcp::socket::shutdown_send);
        co_await tunnel.finish();
    }
    ruvia::task<void> websocket(ruvia::context& context) {
        auto& socket = context.websocket();
        while (auto message = co_await socket.read()) {
            co_await socket.send(std::move(*message));
        }
    }
    ruvia::task<void> udp_tunnel(ruvia::context& context) {
        asio::io_context io;
        asio::ip::udp::socket socket(io, asio::ip::udp::v4());
        socket.connect({asio::ip::address_v4::loopback(), udp_echo_port.load()});
        socket.non_blocking(true);
        auto tunnel = context.tunnel().udp();
        std::array<char, 65536> buffer{};
        while (auto packet = co_await tunnel.read()) {
            const auto deadline = std::chrono::steady_clock::now() + client_timeout;
            for (;;) {
                std::error_code error;
                socket.send(asio::buffer(packet->text().data(), packet->text().size()), 0, error);
                if (!error) {
                    break;
                }
                if (error != asio::error::would_block && error != asio::error::try_again) {
                    throw std::system_error(error);
                }
                co_await await_backend(context, deadline);
            }
            for (;;) {
                std::error_code error;
                const auto count = socket.receive(asio::buffer(buffer), 0, error);
                if (!error) {
                    co_await tunnel.send(std::string_view(buffer.data(), count));
                    break;
                }
                if (error != asio::error::would_block && error != asio::error::try_again) {
                    throw std::system_error(error);
                }
                co_await await_backend(context, deadline);
            }
        }
        co_await tunnel.finish();
    }
};

// One loopback port number free for both TCP (HTTPS) and UDP (HTTP/3).
class shared_port_reservation final {
public:
    explicit shared_port_reservation(asio::io_context& io)
        : tcp_(io) {
        for (int attempt = 0; attempt < 64; ++attempt) {
            udp_.emplace(io);
            const auto port = udp_->endpoint().port();
            std::error_code error;
            tcp_.open(asio::ip::tcp::v4(), error);
            if (!error) {
                // Bound exclusively first, so no other socket holds the port;
                // then shared with the server's reuseport listeners.
                tcp_.bind({asio::ip::address_v4::loopback(), port}, error);
            }
            if (!error) {
                tcp_.set_option(asio::socket_base::reuse_address(true));
#if !defined(_WIN32)
                const int enabled = 1;
                if (::setsockopt(tcp_.native_handle(), SOL_SOCKET, SO_REUSEPORT, &enabled, sizeof(enabled)) != 0) {
                    throw std::system_error(errno, asio::system_category(), "cannot share the reserved TCP port");
                }
#else
                // Windows fixes sharing at bind time: the named UDP lease
                // keeps other fixtures off the number instead.
                tcp_.close();
#endif
                port_ = port;
                return;
            }
            tcp_.close(error);
        }
        throw std::runtime_error("no loopback port is free for both TCP and UDP");
    }

    [[nodiscard]] std::uint16_t port() const noexcept {
        return port_;
    }

private:
    std::optional<ruvia::test::udp_port_reservation> udp_;
    asio::ip::tcp::socket tcp_;
    std::uint16_t port_{0};
};

class app_run final {
public:
    explicit app_run(ruvia::application& app)
        : app_(app),
          thread_([this] {
              try {
                  app_.run();
              } catch (...) {
                  failure_ = std::current_exception();
              }
          }) {}
    ~app_run() {
        if (thread_.joinable()) {
            app_.stop();
            thread_.join();
        }
    }
    app_run(const app_run&) = delete;
    app_run& operator=(const app_run&) = delete;

    void join() {
        thread_.join();
        if (failure_) {
            std::rethrow_exception(failure_);
        }
    }

private:
    ruvia::application& app_;
    std::exception_ptr failure_;
    std::thread thread_;
};

// One HTTP/1.1 request over TLS on TCP; returns the response head.
std::string https_head(asio::io_context& io, std::uint16_t port, const std::filesystem::path& ca_file) {
    asio::ssl::context client(asio::ssl::context::tls_client);
    client.load_verify_file(ca_file.string());
    client.set_verify_mode(asio::ssl::verify_peer);
    static constexpr unsigned char alpn[] = {8, 'h', 't', 't', 'p', '/', '1', '.', '1'};
    if (SSL_CTX_set_alpn_protos(client.native_handle(), alpn, sizeof(alpn)) != 0) {
        throw std::runtime_error("cannot offer http/1.1");
    }
    asio::ssl::stream<asio::ip::tcp::socket> stream(io, client);
    if (SSL_set_tlsext_host_name(stream.native_handle(), "localhost") != 1) {
        throw std::runtime_error("cannot offer SNI");
    }
    stream.next_layer().connect({asio::ip::address_v4::loopback(), port});
    stream.handshake(asio::ssl::stream_base::client);
    const std::string request = "GET /hello HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    asio::write(stream, asio::buffer(request));
    std::string response;
    std::error_code error;
    std::array<char, 4096> buffer{};
    for (;;) {
        const auto count = stream.read_some(asio::buffer(buffer), error);
        response.append(buffer.data(), count);
        if (error || response.find("\r\n\r\n") != std::string::npos) {
            break;
        }
    }
    return response.substr(0, response.find("\r\n\r\n"));
}

ruvia::test::http3_test_client connect(std::uint16_t port, const ruvia::test::tls_identity& identity) {
    return ruvia::test::http3_test_client(
        {asio::ip::address_v4::loopback(), port}, identity.ca_file_, "localhost", client_timeout);
}

}  // namespace http3_serving_test

using namespace http3_serving_test;

RUVIA_TEST(a_multi_worker_app_serves_http3_and_closes_it_on_stop) {
    ruvia::test::tls_identity identity("localhost", "DNS:localhost,IP:127.0.0.1");
    const auto private_key = identity.ca_file_.parent_path() / "key.pem";
    {
        std::unique_ptr<BIO, decltype(&BIO_free)> output(BIO_new_file(private_key.string().c_str(), "w"), BIO_free);
        if (!output || ruvia::test::write_tls_private_key(
                           output.get(), SSL_CTX_get0_privatekey(identity.context_.native_handle())) != 1) {
            throw std::runtime_error("cannot write the HTTP/3 test private key");
        }
    }
    file_path() = identity.ca_file_.parent_path() / "body.txt";
    {
        std::ofstream output(file_path(), std::ios::binary);
        const auto content = file_content();
        output.write(content.data(), static_cast<std::streamsize>(content.size()));
        if (!output) {
            throw std::runtime_error("cannot write the HTTP/3 test file");
        }
    }

    asio::io_context io;
    shared_port_reservation reservation(io);
    const auto port = reservation.port();
    echo_backends backends;

    auto& app = ruvia::app();
    std::binary_semaphore started{0};
    app.server({.worker_count_ = 4, .max_buffered_body_bytes_ = 4 * 1024 * 1024})
        .listen({
            .address_ = "127.0.0.1",
            .https_ = port,
            .http3_ = port,
            .tls_ = {.identity_ = {.certificate_chain_file_ = identity.ca_file_, .private_key_file_ = private_key}},
        })
        .on_start([&started] { started.release(); });
    app_run run(app);
    RUVIA_CHECK(started.try_acquire_for(10s));

    {
        auto client = connect(port, identity);

        // A buffered body.
        const auto hello = client.request("GET", "/hello");
        RUVIA_CHECK_EQ(hello.status_, 200);
        RUVIA_CHECK_EQ(hello.body_, "hello over http/3");
        RUVIA_CHECK(!hello.header("alt-svc").has_value());

        // Request content larger than every flow-control window, echoed back.
        std::string upload(echo_bytes, '\0');
        for (std::size_t index = 0; index < upload.size(); ++index) {
            upload[index] = static_cast<char>('0' + index % 10);
        }
        const auto echoed = client.request(
            "POST", "/echo", {{"content-length", std::to_string(upload.size())}}, upload);
        RUVIA_CHECK_EQ(echoed.status_, 200);
        RUVIA_CHECK_EQ(echoed.body_.size(), upload.size());
        RUVIA_CHECK(echoed.body_ == upload);

        // A response stream, read chunk by chunk.
        const auto stream_id = client.start_request("GET", "/stream");
        const auto& head = client.wait_for_response_head(stream_id);
        RUVIA_CHECK_EQ(head.status_, 200);
        std::string streamed;
        while (auto chunk = client.read_body_chunk(stream_id, client_timeout)) {
            streamed += *chunk;
        }
        const auto stream_end = client.finish_response(stream_id);
        streamed += stream_end.body_;
        std::string expected_stream;
        for (int chunk = 0; chunk < stream_chunks; ++chunk) {
            expected_stream += "chunk-" + std::to_string(chunk) + ";";
        }
        RUVIA_CHECK_EQ(streamed, expected_stream);
        RUVIA_CHECK(!stream_end.reset_error_.has_value());

        // A file body larger than the stream's output budget.
        const auto file = client.request("GET", "/file");
        RUVIA_CHECK_EQ(file.status_, 200);
        RUVIA_CHECK_EQ(file.header("content-length").value_or(""), std::to_string(file_bytes));
        RUVIA_CHECK(file.body_ == file_content());

        const auto missing = client.request("GET", "/missing");
        RUVIA_CHECK_EQ(missing.status_, 404);

        // RFC 9114 §4.3.1: :authority and Host must agree. The malformed
        // request is a stream error of type H3_MESSAGE_ERROR (§4.1.2); the
        // connection keeps serving.
        const auto malformed = client.request("GET", "/hello", {{"host", "attacker.example"}});
        RUVIA_CHECK_EQ(malformed.reset_error_.value_or(0), std::uint64_t{NGHTTP3_H3_MESSAGE_ERROR});
        RUVIA_CHECK_EQ(malformed.status_, 0);
        RUVIA_CHECK_EQ(client.request("GET", "/hello").status_, 200);
    }

    {
        auto client = connect(port, identity);
        RUVIA_CHECK(client.peer_settings().enable_connect_protocol_);
        RUVIA_CHECK(client.peer_settings().h3_datagram_);
        const auto tunnel = client.start_tunnel({{":method", "CONNECT"}, {":authority", "echo.test:443"}});
        RUVIA_CHECK_EQ(client.wait_for_response_head(tunnel).status_, 200);
        const std::string payload(128 * 1024, 't');
        client.send_stream_data(tunnel, byte_view(payload), true);
        const auto echo = client.finish_response(tunnel);
        RUVIA_CHECK_EQ(echo.body_, payload);
        RUVIA_CHECK(!echo.reset_error_);
        RUVIA_CHECK_EQ(client.request("GET", "/hello").status_, 200);
        const auto cancelled = client.start_tunnel({{":method", "CONNECT"}, {":authority", "echo.test:443"}});
        RUVIA_CHECK_EQ(client.wait_for_response_head(cancelled).status_, 200);
        client.reset_request(cancelled, NGHTTP3_H3_REQUEST_CANCELLED);
        RUVIA_CHECK_EQ(client.request("GET", "/hello").status_, 200);

        const auto websocket = client.start_tunnel({{":method", "CONNECT"}, {":protocol", "websocket"},
            {":scheme", "https"}, {":authority", "localhost"}, {":path", "/websocket"}, {"sec-websocket-version", "13"}});
        const auto& head = client.wait_for_response_head(websocket);
        RUVIA_CHECK_EQ(head.status_, 200);
        RUVIA_CHECK(!head.header("sec-websocket-accept"));
        RUVIA_CHECK(!head.header("upgrade"));
        const auto frame = masked_frame(1, "websocket-echo");
        client.send_stream_data(websocket, byte_view(frame));
        const auto echoed = read_tunnel_bytes(client, websocket, 16);
        RUVIA_CHECK_EQ(echoed, std::string("\x81\x0e"
                                           "websocket-echo",
                                   16));
        const auto close = masked_frame(8, std::string("\x03\xe8", 2));
        client.send_stream_data(websocket, byte_view(close), true);
        const auto closed = client.finish_response(websocket);
        RUVIA_CHECK_EQ(closed.body_, std::string("\x88\x02\x03\xe8", 4));
        RUVIA_CHECK(!closed.reset_error_);
    }

    const ruvia::test::http3_test_fields udp_head{{":method", "CONNECT"}, {":protocol", "connect-udp"},
        {":scheme", "https"}, {":authority", "localhost"}, {":path", "/.well-known/masque/udp/echo.test/53/"},
        {"capsule-protocol", "?1"}};
    {
        auto client = connect(port, identity);
        const auto tunnel = client.start_tunnel(udp_head);
        const auto& head = client.wait_for_response_head(tunnel);
        RUVIA_CHECK_EQ(head.status_, 200);
        RUVIA_CHECK_EQ(head.header("capsule-protocol").value_or(""), "?1");
        const std::string unknown("\x01ignored", 8);
        client.send_datagram(tunnel, byte_view(unknown));

        // A native packet may interrupt an incomplete reliable capsule.
        const auto capsule = udp_capsule("capsule");
        client.send_stream_data(tunnel, byte_view(std::string_view(capsule).substr(0, 1)));
        const std::string native("\0native", 7);
        client.send_datagram(tunnel, byte_view(native));
        RUVIA_CHECK_EQ(client.receive_datagram(tunnel), native);
        client.send_stream_data(tunnel, byte_view(std::string_view(capsule).substr(1)));
        RUVIA_CHECK_EQ(client.receive_datagram(tunnel), std::string("\0capsule", 8));
        const std::string empty(1, '\0');
        client.send_datagram(tunnel, byte_view(empty));
        RUVIA_CHECK_EQ(client.receive_datagram(tunnel), empty);

        // Oversize UDP packets use reliable capsules, never truncated QUIC DATAGRAMs.
        const std::string large(4096, 'u');
        const auto large_capsule = udp_capsule(large);
        client.send_stream_data(tunnel, byte_view(large_capsule));
        RUVIA_CHECK_EQ(read_tunnel_bytes(client, tunnel, large_capsule.size()), large_capsule);
        client.send_stream_data(tunnel, {}, true);
        RUVIA_CHECK(!client.finish_response(tunnel).reset_error_);
        RUVIA_CHECK_EQ(client.request("GET", "/hello").status_, 200);
        const auto malformed = client.start_tunnel(udp_head);
        RUVIA_CHECK_EQ(client.wait_for_response_head(malformed).status_, 200);
        const std::string truncated_context(1, static_cast<char>(0xff));
        client.send_datagram(malformed, byte_view(truncated_context));
        RUVIA_CHECK(client.finish_response(malformed).reset_error_.has_value());
        const auto cancelled = client.start_tunnel(udp_head);
        RUVIA_CHECK_EQ(client.wait_for_response_head(cancelled).status_, 200);
        client.reset_request(cancelled, NGHTTP3_H3_REQUEST_CANCELLED);
        RUVIA_CHECK_EQ(client.request("GET", "/hello").status_, 200);
    }
    {
        ruvia::test::http3_test_client client({asio::ip::address_v4::loopback(), port},
            identity.ca_file_, "localhost", client_timeout, nullptr, false, false);
        const auto tunnel = client.start_tunnel(udp_head);
        RUVIA_CHECK_EQ(client.wait_for_response_head(tunnel).status_, 200);
        const auto capsule = udp_capsule("capsule-only");
        client.send_stream_data(tunnel, byte_view(capsule), true);
        const auto echo = client.finish_response(tunnel);
        RUVIA_CHECK_EQ(echo.body_, capsule);
        RUVIA_CHECK(!echo.reset_error_);
    }

    {
        // Keep one connection through consecutive active migrations, then NAT
        // rebindings. ngtcp2 1.25.0 could retire the fallback CID when the next
        // migration began after new-path validation but before old-path probing
        // ended. Do not wait for those probes to expire between path changes.
        auto client = connect(port, identity);
        const auto* connection = client.native_handle();
        auto previous_stream = client.start_request("GET", "/worker");
        const auto initial = client.finish_response(previous_stream);
        RUVIA_CHECK_EQ(initial.status_, 200);
        RUVIA_CHECK(!initial.reset_error_);
        RUVIA_CHECK(!initial.body_.empty());
        for (const bool active_migration : {true, false}) {
            for (int change = 0; change < 16; ++change) {
                const auto previous_port = client.local_port();
                if (active_migration) {
                    client.migrate();
                } else {
                    client.rebind();
                }
                RUVIA_CHECK(client.local_port() != previous_port);
                const auto stream = client.start_request("GET", "/worker");
                RUVIA_CHECK_EQ(stream, previous_stream + 4);
                const auto response = client.finish_response(stream);
                RUVIA_CHECK_EQ(response.status_, 200);
                RUVIA_CHECK(!response.reset_error_);
                RUVIA_CHECK_EQ(response.body_, initial.body_);
                RUVIA_CHECK(client.native_handle() == connection);
                RUVIA_CHECK(!client.closed());
                previous_stream = stream;
            }
        }
    }

    // nghttp3 treats a request it cannot frame (here a :path that is no
    // origin-form target) as a connection error of type H3_MESSAGE_ERROR, as
    // RFC 9114 §8 permits.
    {
        auto client = connect(port, identity);
        (void)client.start_request("GET", "hello");
        RUVIA_CHECK(client.wait_until([&client] { return client.closed(); }, client_timeout));
        const auto* close_error = ngtcp2_conn_get_ccerr2(client.native_handle());
        RUVIA_CHECK(close_error != nullptr && close_error->error_code == NGHTTP3_H3_MESSAGE_ERROR);
    }

    // The HTTPS listener advertises the HTTP/3 service (RFC 7838 §3).
    const auto https = https_head(io, port, identity.ca_file_);
    RUVIA_CHECK(https.starts_with("HTTP/1.1 200 OK"));
    RUVIA_CHECK(https.find("\r\nalt-svc: h3=\":" + std::to_string(port) + "\"; ma=86400") != std::string::npos ||
                https.find("\r\nAlt-Svc: h3=\":" + std::to_string(port) + "\"; ma=86400") != std::string::npos);

    // Fresh connections from fresh client ports: the kernel (POSIX) or the
    // acceptor (Windows) spreads them over the four workers, and each
    // connection's packets reach the worker that owns it.
    std::set<std::string> workers;
    for (int connection = 0; connection < 50; ++connection) {
        auto client = connect(port, identity);
        const auto served = client.request("GET", "/worker");
        RUVIA_CHECK_EQ(served.status_, 200);
        workers.insert(served.body_);
        client.close();
    }
    RUVIA_CHECK(workers.size() > 1);

    // Direct disconnect: the stop closes the connection at once with
    // CONNECTION_CLOSE(H3_NO_ERROR) while its request is still in flight,
    // without GOAWAY.
    {
        auto client = connect(port, identity);
        (void)client.start_request("GET", "/slow");
        // The stop begins while the request's handler runs.
        RUVIA_CHECK(client.wait_until([] { return slow_entered().load(); }, client_timeout));
        std::thread stopper([&app] { app.stop(); });
        RUVIA_CHECK(client.wait_until([&client] { return client.closed(); }, client_timeout));
        stopper.join();
        RUVIA_CHECK(client.goaway_ids().empty());
        const auto* close_error = ngtcp2_conn_get_ccerr2(client.native_handle());
        RUVIA_CHECK(close_error != nullptr);
        if (close_error != nullptr) {
            RUVIA_CHECK(close_error->type == NGTCP2_CCERR_TYPE_APPLICATION);
            RUVIA_CHECK_EQ(close_error->error_code, std::uint64_t{NGHTTP3_H3_NO_ERROR});
        }
    }
    run.join();
    backends.finish();

    // A connection that reaches max_requests_per_connection_ still ends
    // gracefully (RFC 9114 §5.2): its requests are answered, the shutdown
    // notice and the final GOAWAY follow, then CONNECTION_CLOSE(H3_NO_ERROR).
    app.server({.worker_count_ = 4, .max_requests_per_connection_ = 2});
    app_run capped(app);
    RUVIA_CHECK(started.try_acquire_for(10s));
    {
        auto client = connect(port, identity);
        RUVIA_CHECK_EQ(client.request("GET", "/hello").status_, 200);
        const auto last = client.start_request("GET", "/hello");
        const auto response = client.finish_response(last);
        RUVIA_CHECK_EQ(response.status_, 200);
        RUVIA_CHECK_EQ(response.body_, "hello over http/3");
        RUVIA_CHECK(client.wait_until([&client] { return client.closed(); }, client_timeout));
        const auto& goaways = client.goaway_ids();
        RUVIA_CHECK_EQ(goaways.size(), std::size_t{2});
        if (goaways.size() == 2) {
            RUVIA_CHECK_EQ(goaways.front(), std::int64_t{NGHTTP3_SHUTDOWN_NOTICE_STREAM_ID});
            RUVIA_CHECK(goaways.back() > last);
        }
        const auto* close_error = ngtcp2_conn_get_ccerr2(client.native_handle());
        RUVIA_CHECK(close_error != nullptr);
        if (close_error != nullptr) {
            RUVIA_CHECK(close_error->type == NGTCP2_CCERR_TYPE_APPLICATION);
            RUVIA_CHECK_EQ(close_error->error_code, std::uint64_t{NGHTTP3_H3_NO_ERROR});
        }
    }
    app.stop();
    capped.join();
}
