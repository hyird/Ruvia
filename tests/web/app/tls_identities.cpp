#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <semaphore>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <asio/buffer.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/address.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read_until.hpp>
#include <asio/ssl/context.hpp>
#include <asio/ssl/stream.hpp>
#include <asio/write.hpp>
#include <openssl/objects.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include "ruvia/web/app.h"
#include "ruvia/web/context.h"
#include "ruvia/web/controller.h"

#include "app_run.h"
#include "tcp_port_reservation.h"
#include "test_tls_identity.h"

#if defined(RUVIA_ENABLE_HTTP3)
#include <asio/ip/udp.hpp>

#include "http3_test_client.h"
#include "udp_port_reservation.h"
#endif

namespace tls_identities_test {

using namespace std::chrono_literals;
using tls_stream = asio::ssl::stream<asio::ip::tcp::socket>;

std::binary_semaphore& started() {
    static std::binary_semaphore value{0};
    return value;
}

// The HTTPS port a route tries to replace identities on.
std::atomic<std::uint16_t>& replaced_port() {
    static std::atomic<std::uint16_t> value{0};
    return value;
}

// invalid_argument is a logic_error; the contract tells them apart.
template <typename fn_type>
std::string failure_of(fn_type&& fn) {
    try {
        fn();
        return "none";
    } catch (const std::invalid_argument&) {
        return "invalid_argument";
    } catch (const std::system_error&) {
        return "system_error";
    } catch (const std::logic_error&) {
        return "logic_error";
    }
}

class identity_controller final {
public:
    RUVIA_ROUTES_BEGIN(identity_controller)
    RUVIA_GET("/hello", hello);
    RUVIA_GET("/replace", replace);
    RUVIA_ROUTES_END

    ruvia::task<ruvia::http_response> hello(ruvia::context& context) {
        co_return context.text(std::string_view("hello"));
    }

    // A business worker cannot wait for every worker, itself included.
    ruvia::task<ruvia::http_response> replace(ruvia::context& context) {
        co_return context.text(failure_of([] {
            ruvia::app().replace_tls_identities({.address_ = "127.0.0.1", .port_ = replaced_port().load()}, {});
        }));
    }
};

std::pmr::string read_text(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::ostringstream text;
    text << input.rdbuf();
    if (!input) {
        throw std::runtime_error("cannot read TLS test PEM");
    }
    return std::pmr::string(text.view());
}

ruvia::tls_identity_config file_identity(const ruvia::test::tls_identity& identity) {
    return {.certificate_chain_file_ = identity.ca_file_, .private_key_file_ = identity.private_key_file()};
}

ruvia::tls_identity_config pem_identity(const ruvia::test::tls_identity& identity) {
    return {.certificate_chain_pem_ = read_text(identity.ca_file_),
        .private_key_pem_ = read_text(identity.private_key_file())};
}

// Authenticates nothing: tests read the certificate they were served.
asio::ssl::context tls_client(int version = 0) {
    asio::ssl::context client(asio::ssl::context::tls_client);
    client.set_verify_mode(asio::ssl::verify_none);
    if (version != 0 && (SSL_CTX_set_min_proto_version(client.native_handle(), version) != 1 ||
                            SSL_CTX_set_max_proto_version(client.native_handle(), version) != 1)) {
        throw std::runtime_error("cannot configure the TLS test client version");
    }
    return client;
}

// A completed handshake offering `server_name` (no SNI when null) and, when
// given, resuming `session`.
std::unique_ptr<tls_stream> connect_tls(asio::io_context& io, asio::ssl::context& client,
    const asio::ip::tcp::endpoint& endpoint, const char* server_name, SSL_SESSION* session = nullptr) {
    auto stream = std::make_unique<tls_stream>(io, client);
    if ((server_name != nullptr && SSL_set_tlsext_host_name(stream->native_handle(), server_name) != 1) ||
        (session != nullptr && SSL_set_session(stream->native_handle(), session) != 1)) {
        throw std::runtime_error("cannot configure the TLS test connection");
    }
    stream->next_layer().connect(endpoint);
    stream->handshake(asio::ssl::stream_base::client);
    return stream;
}

// The subject CN of the certificate the connection was served.
std::string peer_common_name(SSL* ssl) {
    const std::unique_ptr<X509, decltype(&X509_free)> peer(SSL_get1_peer_certificate(ssl), X509_free);
    if (!peer) {
        return {};
    }
    std::array<char, 256> name{};
    const auto size = X509_NAME_get_text_by_NID(
        X509_get_subject_name(peer.get()), NID_commonName, name.data(), static_cast<int>(name.size()));
    return size < 0 ? std::string() : std::string(name.data(), static_cast<std::size_t>(size));
}

// The CN a fresh handshake is served; empty when it fails.
std::string served_name(asio::io_context& io, asio::ssl::context& client, const asio::ip::tcp::endpoint& endpoint,
    const char* server_name) {
    try {
        return peer_common_name(connect_tls(io, client, endpoint, server_name)->native_handle());
    } catch (const std::exception&) {
        return {};
    }
}

// One keep-alive GET /hello on an established connection.
bool serves_hello(tls_stream& stream) {
    asio::error_code error;
    asio::write(stream, asio::buffer(std::string_view("GET /hello HTTP/1.1\r\nHost: localhost\r\n\r\n")), error);
    std::string response;
    if (!error) {
        asio::read_until(stream, asio::dynamic_buffer(response), "\r\n\r\nhello", error);
    }
    return !error && response.starts_with("HTTP/1.1 200 ");
}

// The complete response to GET /hello for `host` over a fresh connection.
std::string https_response(asio::io_context& io, asio::ssl::context& client, const asio::ip::tcp::endpoint& endpoint,
    const char* server_name, std::string_view host) {
    const auto stream = connect_tls(io, client, endpoint, server_name);
    const auto request = "GET /hello HTTP/1.1\r\nHost: " + std::string(host) + "\r\nConnection: close\r\n\r\n";
    asio::error_code error;
    asio::write(*stream, asio::buffer(request), error);
    std::string response;
    std::array<char, 1024> buffer;
    while (!error) {
        const auto size = stream->read_some(asio::buffer(buffer), error);
        response.append(buffer.data(), size);
    }
    return response;
}

std::string plain_body(asio::io_context& io, const asio::ip::tcp::endpoint& endpoint, std::string_view target) {
    asio::ip::tcp::socket socket(io);
    socket.connect(endpoint);
    const auto request = "GET " + std::string(target) + " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    asio::write(socket, asio::buffer(request));
    std::string response;
    std::array<char, 1024> buffer;
    asio::error_code error;
    while (!error) {
        const auto size = socket.read_some(asio::buffer(buffer), error);
        response.append(buffer.data(), size);
    }
    const auto body = response.find("\r\n\r\n");
    return body == std::string::npos ? std::string() : response.substr(body + 4);
}

// A session of a completed exchange, kept resumable.
std::shared_ptr<SSL_SESSION> resumable_session(
    asio::io_context& io, asio::ssl::context& client, const asio::ip::tcp::endpoint& endpoint) {
    const auto stream = connect_tls(io, client, endpoint, "localhost");
    // TLS 1.3 tickets follow the handshake; reading a response processes them.
    if (!serves_hello(*stream)) {
        throw std::runtime_error("TLS session exchange failed");
    }
    std::shared_ptr<SSL_SESSION> session(SSL_get1_session(stream->native_handle()), SSL_SESSION_free);
    if (!session || SSL_SESSION_is_resumable(session.get()) != 1) {
        throw std::runtime_error("TLS session is not resumable");
    }
    // An SSL freed without a local shutdown marks its session unresumable.
    SSL_set_shutdown(stream->native_handle(), SSL_SENT_SHUTDOWN);
    return session;
}

bool resumes(asio::io_context& io, asio::ssl::context& client, const asio::ip::tcp::endpoint& endpoint,
    const std::shared_ptr<SSL_SESSION>& session) {
    const auto stream = connect_tls(io, client, endpoint, "localhost", session.get());
    return SSL_session_reused(stream->native_handle()) == 1 && serves_hello(*stream);
}

#if defined(RUVIA_ENABLE_HTTP3)
bool serves_hello(ruvia::test::http3_test_client& client) {
    const auto response = client.request("GET", "/hello");
    return response.status_ == 200 && response.body_ == "hello";
}

// Whether a fresh HTTP/3 connection verifying the server against `ca_file`
// for `server_name` (an IP literal sends no SNI) serves GET /hello.
bool http3_serves(std::uint16_t port, const std::filesystem::path& ca_file, const std::string& server_name) {
    try {
        ruvia::test::http3_test_client client({asio::ip::address_v4::loopback(), port}, ca_file, server_name, 3s);
        return serves_hello(client);
    } catch (const std::exception&) {
        return false;
    }
}
#endif

}  // namespace tls_identities_test

using namespace tls_identities_test;

// One HTTPS listener on the wildcard address chooses each handshake's identity
// by precedence: an exact SNI host, a wildcard SNI host covering exactly the
// left-most label (ASCII case-insensitively), the address the client
// connected to, else the primary identity. Identities load from files and
// from in-memory PEM alike, and RFC 9110 §7.4 checks each request against the
// certificate its own connection was served.
TEST(Ruvia, https_selects_identity_by_exact_and_wildcard_sni_then_local_address) {
    const ruvia::test::tls_identity primary("primary", "DNS:localhost,IP:127.0.0.1");
    const ruvia::test::tls_identity exact("exact", "DNS:api.example.test");
    const ruvia::test::tls_identity wildcard("wildcard", "DNS:*.example.test");
    const ruvia::test::tls_identity local("local", "IP:127.0.0.2");
    asio::io_context io;
    ruvia::test::tcp_port_reservation reservation(io);
    const auto port = reservation.endpoint().port();
    const asio::ip::tcp::endpoint first(asio::ip::make_address("127.0.0.1"), port);
    const asio::ip::tcp::endpoint second(asio::ip::make_address("127.0.0.2"), port);

    auto& app = ruvia::app();
    app.server({.worker_count_ = 2})
        .listen({
            .address_ = "0.0.0.0",
            .https_ = port,
            .tls_ =
                {
                    .identity_ = pem_identity(primary),
                    .sni_ = {{.host_ = "*.example.test", .identity_ = pem_identity(wildcard)},
                        {.host_ = "api.example.test", .identity_ = file_identity(exact)}},
                    .local_addresses_ = {{.address_ = "127.0.0.2", .identity_ = file_identity(local)}},
                },
        })
        .on_start(nullptr)
        .on_start([] { started().release(); });
    ruvia::test::app_run runner(app);
    ASSERT_TRUE(started().try_acquire_for(10s));

    for (const int version : {TLS1_2_VERSION, TLS1_3_VERSION}) {
        auto client = tls_client(version);
        EXPECT_EQ(served_name(io, client, first, nullptr), "primary");
        EXPECT_EQ(served_name(io, client, second, nullptr), "local");
        EXPECT_EQ(served_name(io, client, first, "api.example.test"), "exact");
        EXPECT_EQ(served_name(io, client, second, "API.Example.Test"), "exact");
        EXPECT_EQ(served_name(io, client, first, "www.example.test"), "wildcard");
        EXPECT_EQ(served_name(io, client, second, "WWW.example.TEST"), "wildcard");
        EXPECT_EQ(served_name(io, client, first, "example.test"), "primary");
        EXPECT_EQ(served_name(io, client, first, "a.b.example.test"), "primary");
        EXPECT_EQ(served_name(io, client, second, "a.b.example.test"), "local");
        EXPECT_EQ(served_name(io, client, second, "other.test"), "local");
    }
    auto client = tls_client();
    EXPECT_TRUE(https_response(io, client, first, "www.example.test", "www.example.test").starts_with("HTTP/1.1 200 "));
    EXPECT_TRUE(https_response(io, client, first, "www.example.test", "localhost").starts_with("HTTP/1.1 421 "));
    EXPECT_TRUE(https_response(io, client, second, nullptr, "127.0.0.2").starts_with("HTTP/1.1 200 "));
    EXPECT_TRUE(https_response(io, client, second, nullptr, "localhost").starts_with("HTTP/1.1 421 "));
    EXPECT_TRUE(https_response(io, client, first, nullptr, "localhost").starts_with("HTTP/1.1 200 "));
    app.stop();
    runner.join();
}

// replace_tls_identities() swaps a running listener's identities for every
// later handshake over TCP and HTTP/3, while established connections keep
// theirs and sessions keep resuming across the swap. Invalid input changes
// nothing; a stopped app and a business worker cannot replace; and the
// listener keeps the last set for its next run.
TEST(Ruvia, https_identities_are_replaced_at_runtime_for_new_handshakes) {
    const ruvia::test::tls_identity first("first", "DNS:localhost,IP:127.0.0.1");
    const ruvia::test::tls_identity second("second", "DNS:localhost,IP:127.0.0.1");
    const ruvia::test::tls_identity exact("exact", "DNS:api.example.test");
    const ruvia::test::tls_identity local("local", "DNS:localhost,IP:127.0.0.1");
    asio::io_context io;
    ruvia::test::tcp_port_reservation http_reservation(io);
    ruvia::test::tcp_port_reservation https_reservation(io);
    const auto http_endpoint = http_reservation.endpoint();
    const auto https_endpoint = https_reservation.endpoint();
    replaced_port() = https_endpoint.port();
    const ruvia::listener_endpoint https{.address_ = "127.0.0.1", .port_ = https_endpoint.port()};
    ruvia::listen_config listener{
        .address_ = "127.0.0.1",
        .http_ = http_endpoint.port(),
        .https_ = https_endpoint.port(),
        .tls_ = {.identity_ = file_identity(first)},
    };
#if defined(RUVIA_ENABLE_HTTP3)
    ruvia::test::udp_port_reservation udp_reservation(io);
    const auto http3_port = udp_reservation.endpoint().port();
    listener.http3_ = http3_port;
#endif
    auto& app = ruvia::app();
    app.server({.worker_count_ = 2}).listen(listener).on_start(nullptr).on_start([] { started().release(); });
    EXPECT_EQ(failure_of([&] { app.replace_tls_identities(https, {.identity_ = file_identity(second)}); }),
        "logic_error");

    auto client = tls_client();
    {
        ruvia::test::app_run runner(app);
        ASSERT_TRUE(started().try_acquire_for(10s));
        const auto established = connect_tls(io, client, https_endpoint, "localhost");
        ASSERT_TRUE(serves_hello(*established));
        EXPECT_EQ(peer_common_name(established->native_handle()), "first");
        auto tls12 = tls_client(TLS1_2_VERSION);
        auto tls13 = tls_client(TLS1_3_VERSION);
        const auto tls12_session = resumable_session(io, tls12, https_endpoint);
        const auto tls13_session = resumable_session(io, tls13, https_endpoint);
#if defined(RUVIA_ENABLE_HTTP3)
        const auto loopback = asio::ip::udp::endpoint(asio::ip::address_v4::loopback(), http3_port);
        ruvia::test::http3_test_client established_http3(loopback, first.ca_file_, "localhost", 5s);
        ASSERT_TRUE(serves_hello(established_http3));
        const auto http3_ticket = [&] {
            ruvia::test::http3_test_client ticket_client(loopback, first.ca_file_, "localhost", 5s);
            return ticket_client.capture_resumption();
        }();
#endif

        EXPECT_EQ(plain_body(io, http_endpoint, "/replace"), "logic_error");
        const auto replace = [&](const ruvia::listener_endpoint& endpoint, ruvia::tls_config config) {
            return failure_of([&] { app.replace_tls_identities(endpoint, std::move(config)); });
        };
        const auto replacement = [&] { return ruvia::tls_config{.identity_ = file_identity(second)}; };
        EXPECT_EQ(replace({.address_ = "127.0.0.2", .port_ = https_endpoint.port()}, replacement()), "invalid_argument");
        EXPECT_EQ(replace({.address_ = "localhost", .port_ = https_endpoint.port()}, replacement()), "invalid_argument");
        EXPECT_EQ(replace({.address_ = "127.0.0.1", .port_ = http_endpoint.port()}, replacement()), "invalid_argument");
        EXPECT_EQ(replace(https, {.identity_ = file_identity(second), .max_outgoing_records_ = 32}), "invalid_argument");
        EXPECT_EQ(replace(https, {.identity_ = file_identity(second), .client_certificates_ = {.ca_file_ = first.ca_file_}}),
            "invalid_argument");
        EXPECT_EQ(replace(https, {.identity_ = file_identity(second), .sni_ = {{.host_ = "*", .identity_ = file_identity(exact)}}}),
            "invalid_argument");
        EXPECT_EQ(replace(https, {.identity_ = {.certificate_chain_pem_ = read_text(second.ca_file_)}}), "invalid_argument");
        EXPECT_EQ(replace(https, {.identity_ = {.certificate_chain_file_ = first.ca_file_.parent_path() / "missing.pem",
                                      .private_key_file_ = first.private_key_file()}}),
            "system_error");
        EXPECT_EQ(replace(https, {.identity_ = {.certificate_chain_pem_ = read_text(second.ca_file_),
                                      .private_key_pem_ = read_text(first.private_key_file())}}),
            "system_error");
        EXPECT_EQ(served_name(io, client, https_endpoint, "localhost"), "first");
#if defined(RUVIA_ENABLE_HTTP3)
        EXPECT_TRUE(http3_serves(http3_port, first.ca_file_, "localhost"));
#endif

        EXPECT_EQ(replace(https,
                      {.identity_ = pem_identity(second),
                          .sni_ = {{.host_ = "api.example.test", .identity_ = file_identity(exact)}}}),
            "none");
        EXPECT_EQ(served_name(io, client, https_endpoint, "localhost"), "second");
        EXPECT_EQ(served_name(io, client, https_endpoint, nullptr), "second");
        EXPECT_EQ(served_name(io, client, https_endpoint, "api.example.test"), "exact");
        EXPECT_TRUE(serves_hello(*established));
        EXPECT_EQ(peer_common_name(established->native_handle()), "first");
        EXPECT_TRUE(resumes(io, tls12, https_endpoint, tls12_session));
        EXPECT_TRUE(resumes(io, tls13, https_endpoint, tls13_session));
#if defined(RUVIA_ENABLE_HTTP3)
        EXPECT_TRUE(serves_hello(established_http3));
        EXPECT_TRUE(http3_serves(http3_port, second.ca_file_, "localhost"));
        EXPECT_FALSE(http3_serves(http3_port, first.ca_file_, "localhost"));
        {
            ruvia::test::http3_test_client resumed(loopback, first.ca_file_, "localhost", 5s, &http3_ticket);
            EXPECT_TRUE(resumed.resumed());
            EXPECT_TRUE(serves_hello(resumed));
        }
#endif

        // A connection without SNI (HTTP/3 to an IP literal) arrives at
        // 127.0.0.1, which now names its own identity.
        EXPECT_EQ(replace(https,
                      {.identity_ = file_identity(second),
                          .local_addresses_ = {{.address_ = "127.0.0.1", .identity_ = pem_identity(local)}}}),
            "none");
        EXPECT_EQ(served_name(io, client, https_endpoint, nullptr), "local");
        EXPECT_EQ(served_name(io, client, https_endpoint, "localhost"), "local");
#if defined(RUVIA_ENABLE_HTTP3)
        EXPECT_TRUE(http3_serves(http3_port, local.ca_file_, "127.0.0.1"));
#endif
        app.stop();
        runner.join();
    }
    EXPECT_EQ(failure_of([&] { app.replace_tls_identities(https, {.identity_ = file_identity(second)}); }),
        "logic_error");

    ruvia::test::app_run runner(app);
    ASSERT_TRUE(started().try_acquire_for(10s));
    EXPECT_EQ(served_name(io, client, https_endpoint, nullptr), "local");
    app.stop();
    runner.join();
}
