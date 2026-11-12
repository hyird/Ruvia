#include "ruvia/web/server_connection.h"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <mutex>
#include <optional>
#include <semaphore>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <asio/buffer.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read.hpp>
#include <asio/ssl/context.hpp>
#include <asio/ssl/stream.hpp>
#include <asio/write.hpp>
#include <openssl/ssl.h>

#include "ruvia/web/app.h"
#include "ruvia/web/context.h"
#include "ruvia/web/controller.h"

#include "app_run.h"
#include "raw_http_peer.h"
#include "tcp_port_reservation.h"
#include "test_tls_identity.h"

namespace server_connection_test {

using namespace std::chrono_literals;

class ping_controller final {
public:
    RUVIA_ROUTES_BEGIN(ping_controller)
    RUVIA_GET("/ping", ping);
    RUVIA_GET("/tls", tls);
    RUVIA_ROUTES_END

    ruvia::task<ruvia::http_response> ping(ruvia::context& context) {
        co_return context.text("pong");
    }

    // What the request's TLS handshake negotiated.
    ruvia::task<ruvia::http_response> tls(ruvia::context& context) {
        const auto* transport = context.conn().tls();
        co_return context.text(transport == nullptr
                                   ? std::string("plain")
                                   : std::string(transport->alpn()) + "|" + std::string(transport->server_name()));
    }
};

// What one handler saw of its connection, copied on its worker.
struct connection_view final {
    std::uint64_t id_{};
    std::string local_address_;
    std::uint16_t local_port_{};
    std::string remote_address_;
    std::uint16_t remote_port_{};
    bool tls_{};
    std::string alpn_;
    std::string server_name_;
    bool client_certificate_{};
    // The second read after the peer's end returned std::nullopt again.
    bool end_repeated_{};
    // The handler's pending read failed, and the stop token it saw then.
    bool read_failed_{};
    bool stop_requested_{};
};

class observed_connections final {
public:
    void add(const connection_view& view) {
        std::lock_guard lock(mutex_);
        views_.push_back(view);
    }

    [[nodiscard]] std::vector<connection_view> views() const {
        std::lock_guard lock(mutex_);
        return views_;
    }

    [[nodiscard]] std::size_t size() const {
        std::lock_guard lock(mutex_);
        return views_.size();
    }

    void clear() {
        std::lock_guard lock(mutex_);
        views_.clear();
    }

private:
    mutable std::mutex mutex_;
    std::vector<connection_view> views_;
};

observed_connections& observed() {
    static observed_connections value;
    return value;
}

std::binary_semaphore& started() {
    static std::binary_semaphore value{0};
    return value;
}

std::counting_semaphore<>& handler_entered() {
    static std::counting_semaphore<> value{0};
    return value;
}

struct failure_copy final {
    std::string remote_address_;
    std::string message_;
};

std::mutex& failures_mutex() {
    static std::mutex value;
    return value;
}

std::vector<failure_copy>& failures() {
    static std::vector<failure_copy> value;
    return value;
}

void record_failure(const ruvia::connection_failure_record& record) noexcept {
    std::string message;
    try {
        std::rethrow_exception(record.exception());
    } catch (const std::exception& error) {
        message = error.what();
    } catch (...) {
    }
    std::lock_guard lock(failures_mutex());
    failures().push_back({std::string(record.remote_address()), std::move(message)});
}

[[nodiscard]] connection_view describe(const ruvia::server_connection& connection) {
    connection_view view;
    view.id_ = connection.id();
    view.local_address_ = connection.local().address();
    view.local_port_ = connection.local().port();
    view.remote_address_ = connection.remote().address();
    view.remote_port_ = connection.remote().port();
    if (const auto* tls = connection.tls()) {
        view.tls_ = true;
        view.alpn_ = tls->alpn();
        view.server_name_ = tls->server_name();
        view.client_certificate_ = tls->client_certificate_subject().has_value();
    }
    return view;
}

[[nodiscard]] std::span<const std::byte> bytes_of(std::string_view text) {
    return std::as_bytes(std::span(text.data(), text.size()));
}

// Echoes every chunk until the peer ends its direction. A cleartext peer then
// gets "|eof" before the finish; a TLS peer's close_notify is answered by the
// finish alone. The end repeats, and returning closes the connection.
ruvia::task<void> echo(ruvia::server_connection& connection) {
    auto view = describe(connection);
    for (;;) {
        auto chunk = co_await connection.read();
        if (!chunk) {
            break;
        }
        co_await connection.write(*chunk);
    }
    if (connection.tls() == nullptr) {
        co_await connection.write(bytes_of("|eof"));
    }
    co_await connection.finish();
    view.end_repeated_ = !(co_await connection.read()).has_value();
    observed().add(view);
}

// Waits on input that never comes; the server ends the connection.
ruvia::task<void> wait_for_input(ruvia::server_connection& connection) {
    auto view = describe(connection);
    handler_entered().release();
    try {
        (void)co_await connection.read();
    } catch (const std::system_error&) {
        view.read_failed_ = true;
        view.stop_requested_ = connection.stop_token().stop_requested();
        observed().add(view);
        throw;
    }
    observed().add(view);
}

ruvia::task<void> fail_after_first_read(ruvia::server_connection& connection) {
    (void)co_await connection.read();
    throw std::runtime_error("raw handler failed");
}

[[nodiscard]] std::string read_until_closed(asio::ip::tcp::socket& socket) {
    std::string result;
    std::array<char, 1024> bytes;
    asio::error_code error;
    while (!error) {
        const auto count = socket.read_some(asio::buffer(bytes), error);
        result.append(bytes.data(), count);
    }
    return result;
}

[[nodiscard]] ruvia::listen_config connection_listener(const asio::ip::tcp::endpoint& http) {
    return ruvia::listen_config{
        .address_ = "127.0.0.1",
        .http_ = http.port(),
        .dispatch_ = ruvia::listener_dispatch::connection,
    };
}

}  // namespace server_connection_test

using namespace server_connection_test;

// One application serves byte streams on a cleartext and a TLS connection
// listener beside an ordinary HTTP listener. The server parses nothing on the
// connection listeners: an HTTP request is echoed back verbatim. The handler
// sees the connection's id and endpoints, the TLS listener's own ALPN choice
// (in server preference) and the client's server_name; read() ends with
// std::nullopt at the peer's FIN or close_notify and keeps returning it, and
// finish() ends the send direction with a FIN or close_notify. A client
// offering ALPN with none of the listener's protocols fails the handshake.
TEST(Ruvia, connection_listeners_serve_byte_streams_beside_routes) {
    ruvia::test::tls_identity identity("localhost", "DNS:localhost,IP:127.0.0.1");
    asio::io_context io;
    ruvia::test::tcp_port_reservation raw_reservation(io);
    ruvia::test::tcp_port_reservation raw_tls_reservation(io);
    ruvia::test::tcp_port_reservation routes_reservation(io);
    ruvia::test::tcp_port_reservation routes_tls_reservation(io);
    const auto raw_endpoint = raw_reservation.endpoint();
    const auto raw_tls_endpoint = raw_tls_reservation.endpoint();
    const auto routes_endpoint = routes_reservation.endpoint();
    const auto routes_tls_endpoint = routes_tls_reservation.endpoint();
    const ruvia::tls_identity_config server_identity{
        .certificate_chain_file_ = identity.ca_file_, .private_key_file_ = identity.private_key_file()};
    auto raw_listener = connection_listener(raw_endpoint);
    raw_listener.https_ = raw_tls_endpoint.port();
    raw_listener.tls_ = {.identity_ = server_identity, .alpn_ = {"echo/1", "h2"}};
    const ruvia::listen_config routes_listener{.address_ = "127.0.0.1",
        .http_ = routes_endpoint.port(),
        .https_ = routes_tls_endpoint.port(),
        .tls_ = {.identity_ = server_identity}};

    observed().clear();
    auto& app = ruvia::app();
    app.server({.worker_count_ = 2})
        .listen({raw_listener, routes_listener})
        .on_start(nullptr)
        .on_start([] { started().release(); })
        .on_connection(&echo);

    ruvia::test::app_run runner(app);
    ASSERT_TRUE(started().try_acquire_for(10s));
    EXPECT_THROW(app.on_connection(nullptr), std::logic_error);

    constexpr std::string_view payload = "GET /ping HTTP/1.1\r\nHost: localhost\r\n\r\n";
    std::uint16_t plain_client_port = 0;
    {
        asio::ip::tcp::socket socket(io);
        socket.connect(raw_endpoint);
        plain_client_port = socket.local_endpoint().port();
        asio::write(socket, asio::buffer(payload));
        socket.shutdown(asio::ip::tcp::socket::shutdown_send);
        EXPECT_EQ(read_until_closed(socket), std::string(payload) + "|eof");
    }

    std::uint16_t tls_client_port = 0;
    {
        asio::ssl::context client(asio::ssl::context::tls_client);
        client.set_verify_mode(asio::ssl::verify_none);
        asio::ssl::stream<asio::ip::tcp::socket> stream(io, client);
        static constexpr unsigned char alpn[] = {2, 'h', '2', 6, 'e', 'c', 'h', 'o', '/', '1'};
        ASSERT_EQ(SSL_set_alpn_protos(stream.native_handle(), alpn, sizeof(alpn)), 0);
        ASSERT_EQ(SSL_set_tlsext_host_name(stream.native_handle(), "localhost"), 1);
        stream.next_layer().connect(raw_tls_endpoint);
        tls_client_port = stream.next_layer().local_endpoint().port();
        stream.handshake(asio::ssl::stream_base::client);
        asio::write(stream, asio::buffer(payload));
        std::string echoed(payload.size(), '\0');
        asio::read(stream, asio::buffer(echoed));
        EXPECT_EQ(echoed, payload);
        // Sends close_notify and waits for the server's: the handler's read
        // ends, and its finish() answers.
        asio::error_code shutdown_error;
        stream.shutdown(shutdown_error);
        EXPECT_FALSE(shutdown_error) << shutdown_error.message();
    }

    {
        asio::ssl::context client(asio::ssl::context::tls_client);
        client.set_verify_mode(asio::ssl::verify_none);
        asio::ssl::stream<asio::ip::tcp::socket> stream(io, client);
        static constexpr unsigned char alpn[] = {8, 'h', 't', 't', 'p', '/', '1', '.', '1'};
        ASSERT_EQ(SSL_set_alpn_protos(stream.native_handle(), alpn, sizeof(alpn)), 0);
        stream.next_layer().connect(raw_tls_endpoint);
        asio::error_code handshake_error;
        stream.handshake(asio::ssl::stream_base::client, handshake_error);
        EXPECT_TRUE(handshake_error);
    }

    const auto pong = ruvia::test::exchange(
        io, routes_endpoint, "GET /ping HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
    EXPECT_TRUE(pong.starts_with("HTTP/1.1 200 "));
    EXPECT_TRUE(pong.ends_with("pong"));

    // Routed HTTPS requests see the same negotiated values through conn_info.
    {
        asio::ssl::context client(asio::ssl::context::tls_client);
        client.set_verify_mode(asio::ssl::verify_none);
        asio::ssl::stream<asio::ip::tcp::socket> stream(io, client);
        static constexpr unsigned char alpn[] = {8, 'h', 't', 't', 'p', '/', '1', '.', '1'};
        ASSERT_EQ(SSL_set_alpn_protos(stream.native_handle(), alpn, sizeof(alpn)), 0);
        ASSERT_EQ(SSL_set_tlsext_host_name(stream.native_handle(), "localhost"), 1);
        stream.next_layer().connect(routes_tls_endpoint);
        stream.handshake(asio::ssl::stream_base::client);
        asio::write(stream, asio::buffer(std::string_view("GET /tls HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n")));
        const auto response = ruvia::test::read_to_end(stream);
        EXPECT_TRUE(response.starts_with("HTTP/1.1 200 "));
        EXPECT_TRUE(response.ends_with("http/1.1|localhost"));
    }

    ruvia::test::wait_until([] { return observed().size() == 2; }, 10s);
    app.stop();
    runner.join();

    const auto views = observed().views();
    ASSERT_EQ(views.size(), 2U);
    const auto& plain = views[0].tls_ ? views[1] : views[0];
    const auto& tls = views[0].tls_ ? views[0] : views[1];
    EXPECT_NE(plain.id_, 0U);
    EXPECT_NE(tls.id_, 0U);
    EXPECT_NE(plain.id_, tls.id_);
    EXPECT_FALSE(plain.tls_);
    EXPECT_EQ(plain.local_address_, "127.0.0.1");
    EXPECT_EQ(plain.local_port_, raw_endpoint.port());
    EXPECT_EQ(plain.remote_address_, "127.0.0.1");
    EXPECT_EQ(plain.remote_port_, plain_client_port);
    EXPECT_TRUE(plain.end_repeated_);
    EXPECT_TRUE(tls.tls_);
    EXPECT_EQ(tls.local_port_, raw_tls_endpoint.port());
    EXPECT_EQ(tls.remote_port_, tls_client_port);
    EXPECT_EQ(tls.alpn_, "echo/1");
    EXPECT_EQ(tls.server_name_, "localhost");
    EXPECT_FALSE(tls.client_certificate_);
    EXPECT_TRUE(tls.end_repeated_);
}

// idle_timeout_ closes a connection without I/O; its handler's pending read
// fails, which is a normal end rather than a connection failure.
TEST(Ruvia, connection_idle_timeout_closes_raw_connection) {
    asio::io_context io;
    ruvia::test::tcp_port_reservation reservation(io);
    const auto endpoint = reservation.endpoint();
    observed().clear();
    auto& app = ruvia::app();
    app.server({.worker_count_ = 1, .idle_timeout_ = 300ms, .connection_scan_interval_ = 50ms})
        .listen(connection_listener(endpoint))
        .on_start(nullptr)
        .on_start([] { started().release(); })
        .on_connection(&wait_for_input);

    ruvia::test::app_run runner(app);
    ASSERT_TRUE(started().try_acquire_for(10s));
    asio::ip::tcp::socket socket(io);
    socket.connect(endpoint);
    ASSERT_TRUE(handler_entered().try_acquire_for(10s));
    const auto begin = std::chrono::steady_clock::now();
    EXPECT_EQ(read_until_closed(socket), "");
    const auto elapsed = std::chrono::steady_clock::now() - begin;
    EXPECT_GE(elapsed, 200ms);
    EXPECT_LT(elapsed, 5s);
    ruvia::test::wait_until([] { return observed().size() == 1; }, 10s);
    EXPECT_EQ(app.http_stats().connection_failures_, 0U);
    app.stop();
    runner.join();
    const auto views = observed().views();
    ASSERT_EQ(views.size(), 1U);
    EXPECT_TRUE(views[0].read_failed_);
    EXPECT_FALSE(views[0].stop_requested_);
}

// stop() closes every raw connection at once and requests the handler's stop
// token; run() returns once the handlers ended.
TEST(Ruvia, stop_disconnects_raw_connections) {
    asio::io_context io;
    ruvia::test::tcp_port_reservation reservation(io);
    const auto endpoint = reservation.endpoint();
    observed().clear();
    auto& app = ruvia::app();
    app.server({.worker_count_ = 2})
        .listen(connection_listener(endpoint))
        .on_start(nullptr)
        .on_start([] { started().release(); })
        .on_connection(&wait_for_input);

    ruvia::test::app_run runner(app);
    ASSERT_TRUE(started().try_acquire_for(10s));
    std::array<asio::ip::tcp::socket, 3> sockets{
        asio::ip::tcp::socket(io), asio::ip::tcp::socket(io), asio::ip::tcp::socket(io)};
    for (auto& socket : sockets) {
        socket.connect(endpoint);
        ASSERT_TRUE(handler_entered().try_acquire_for(10s));
    }
    EXPECT_EQ(app.http_stats().active_connections_, sockets.size());
    app.stop();
    runner.join();
    for (auto& socket : sockets) {
        EXPECT_EQ(read_until_closed(socket), "");
    }
    const auto views = observed().views();
    ASSERT_EQ(views.size(), sockets.size());
    for (const auto& view : views) {
        EXPECT_TRUE(view.read_failed_);
        EXPECT_TRUE(view.stop_requested_);
    }
}

// An exception escaping the handler aborts the connection and reaches
// on_connection_failure, counted in connection_failures_.
TEST(Ruvia, raw_handler_failure_reaches_connection_failure_hook) {
    asio::io_context io;
    ruvia::test::tcp_port_reservation reservation(io);
    const auto endpoint = reservation.endpoint();
    auto& app = ruvia::app();
    app.server({.worker_count_ = 1})
        .listen(connection_listener(endpoint))
        .on_start(nullptr)
        .on_start([] { started().release(); })
        .on_connection(&fail_after_first_read)
        .on_connection_failure(&record_failure);

    ruvia::test::app_run runner(app);
    ASSERT_TRUE(started().try_acquire_for(10s));
    asio::ip::tcp::socket socket(io);
    socket.connect(endpoint);
    asio::write(socket, asio::buffer(std::string_view("x")));
    EXPECT_EQ(read_until_closed(socket), "");
    ruvia::test::wait_until([&app] { return app.http_stats().connection_failures_ == 1; }, 10s);
    app.stop();
    runner.join();
    app.on_connection_failure(nullptr);
    std::lock_guard lock(failures_mutex());
    ASSERT_EQ(failures().size(), 1U);
    EXPECT_EQ(failures()[0].remote_address_, "127.0.0.1");
    EXPECT_EQ(failures()[0].message_, "raw handler failed");
}

// A connection listener needs on_connection at run(); listen() rejects
// what such a listener cannot serve, and an ALPN list on a routes listener.
TEST(Ruvia, connection_dispatch_configuration_is_validated) {
    asio::io_context io;
    ruvia::test::tcp_port_reservation reservation(io);
    ruvia::test::tcp_port_reservation tls_reservation(io);
    const auto endpoint = reservation.endpoint();
    const auto tls_port = tls_reservation.endpoint().port();
    ruvia::test::tls_identity identity("localhost");
    const ruvia::tls_config tls{.identity_ = {.certificate_chain_file_ = identity.ca_file_,
                                    .private_key_file_ = identity.private_key_file()}};
    auto& app = ruvia::app();
    app.server({.worker_count_ = 1}).listen(connection_listener(endpoint)).on_start(nullptr).on_connection(nullptr);
    EXPECT_THROW(app.run(), std::invalid_argument);

    auto with_http3 = connection_listener(endpoint);
    with_http3.https_ = tls_port;
    with_http3.http3_ = with_http3.https_;
    with_http3.tls_ = tls;
    EXPECT_THROW(app.listen(with_http3), std::invalid_argument);

    auto redirecting = connection_listener(endpoint);
    redirecting.https_ = tls_port;
    redirecting.tls_ = tls;
    redirecting.auto_https_redirect_ = true;
    EXPECT_THROW(app.listen(redirecting), std::invalid_argument);

    auto empty_protocol = connection_listener(endpoint);
    empty_protocol.https_ = tls_port;
    empty_protocol.tls_ = tls;
    empty_protocol.tls_.alpn_ = {"h2", ""};
    EXPECT_THROW(app.listen(empty_protocol), std::invalid_argument);

    auto cleartext_protocols = connection_listener(endpoint);
    cleartext_protocols.tls_.alpn_ = {"h2"};
    EXPECT_THROW(app.listen(cleartext_protocols), std::invalid_argument);

    auto routes_protocols = ruvia::listen_config{.address_ = "127.0.0.1",
        .https_ = endpoint.port(),
        .tls_ = tls};
    routes_protocols.tls_.alpn_ = {"h2"};
    EXPECT_THROW(app.listen(routes_protocols), std::invalid_argument);
}
