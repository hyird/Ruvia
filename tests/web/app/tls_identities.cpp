#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <semaphore>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <variant>

#include <asio/buffer.hpp>
#include <asio/error.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/address.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/ssl/context.hpp>
#include <asio/ssl/stream.hpp>
#include <asio/write.hpp>
#include <openssl/objects.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/http/http1_client_request_writer.h"
#include "ruvia/http/http1_client_response_body_decoder.h"
#include "ruvia/web/app.h"
#include "ruvia/web/context.h"
#include "ruvia/web/controller.h"
#include "ruvia/web/http_server.h"

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

struct client_io_result final {
    asio::error_code error_;
    std::size_t size_{};
};

// Client contexts contain only fixture I/O. A deadline closes the transport,
// then drains the cancelled completion before its borrowed state can retire.
template <typename initiation_type>
client_io_result client_io(asio::ip::tcp::socket& socket, std::chrono::steady_clock::time_point deadline,
    initiation_type&& initiate) {
    auto& io = static_cast<asio::io_context&>(socket.get_executor().context());
    client_io_result result;
    bool done = false;
    initiate([&](const asio::error_code& error, auto... sizes) {
        result.error_ = error;
        ((result.size_ = sizes), ...);
        done = true;
    });
    io.restart();
    io.run_until(deadline);
    if (!done) {
        asio::error_code ignored;
        socket.close(ignored);
        io.restart();
        io.run();
        result.error_ = asio::error::timed_out;
    }
    return result;
}

// A completed handshake offering `server_name` (no SNI when null) and, when
// given, resuming `session`.
std::unique_ptr<tls_stream> connect_tls(asio::io_context& io, asio::ssl::context& client,
    const asio::ip::tcp::endpoint& endpoint, const char* server_name, SSL_SESSION* session = nullptr) {
    auto stream = std::make_unique<tls_stream>(io, client);
    // OpenSSL consumes TLS 1.3 tickets even when the attempted handshake fails.
    // Each attempt owns a copy, never the retained resumption snapshot.
    const std::unique_ptr<SSL_SESSION, decltype(&SSL_SESSION_free)> restored(
        session ? SSL_SESSION_dup(session) : nullptr, SSL_SESSION_free);
    if ((server_name != nullptr && SSL_set_tlsext_host_name(stream->native_handle(), server_name) != 1) ||
        (session != nullptr && (!restored || SSL_set_session(stream->native_handle(), restored.get()) != 1))) {
        throw std::runtime_error("cannot configure the TLS test connection");
    }
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    auto result = client_io(stream->next_layer(), deadline,
        [&](auto completion) { stream->next_layer().async_connect(endpoint, std::move(completion)); });
    if (!result.error_) {
        result = client_io(stream->next_layer(), deadline,
            [&](auto completion) { stream->async_handshake(asio::ssl::stream_base::client, std::move(completion)); });
    }
    if (result.error_) {
        throw std::system_error(result.error_, "TLS test connection failed");
    }
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
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    std::array<char, 1024> request;
    auto prepared = ruvia::http1_client_request_writer().prepare(
        ruvia::http_origin_view::https({.host_ = "localhost"}), {.method_ = "GET", .target_ = "/hello"}, request);
    auto& exchange = std::get<ruvia::prepared_http1_client_request>(prepared);
    ruvia::http1_client_response_parser parser(std::move(exchange).take_exchange_state());
    const auto written = client_io(stream.next_layer(), deadline,
        [&](auto completion) { asio::async_write(stream, asio::buffer(exchange.head()), std::move(completion)); });
    if (written.error_) {
        return false;
    }
    std::optional<ruvia::http1_client_response_body_decoder> decoder;
    std::string response, body;
    std::array<char, 1024> bytes, scratch;
    bool successful = false, eof = false;
    for (;;) {
        if (!decoder) {
            auto parsed = parser.parse(response);
            if (parsed.failure()) {
                return false;
            }
            if (const auto* head = parsed.parsed()) {
                if (head->plan().informational()) {
                    response.erase(0, head->consumed_bytes());
                    continue;
                }
                successful = head->head().status() == ruvia::http_status::ok;
                decoder.emplace(head->plan(), ruvia::process_resource());
                response.erase(0, head->consumed_bytes());
            }
        }
        if (decoder) {
            const auto decoded = eof ? decoder->finish_input(response, scratch) : decoder->decode(response, scratch);
            if (const auto* output = decoded.output()) {
                body.append(output->bytes());
            }
            response.erase(0, decoded.consumed_bytes());
            if (decoded.complete()) {
                return successful && body == "hello";
            }
            if (decoded.protocol_failure() || decoded.decoder_failure()) {
                return false;
            }
            if (!decoded.need_input()) {
                continue;
            }
        }
        if (eof) {
            return false;
        }
        const auto received = client_io(stream.next_layer(), deadline,
            [&](auto completion) { stream.async_read_some(asio::buffer(bytes), std::move(completion)); });
        response.append(bytes.data(), received.size_);
        if (received.error_) {
            if (received.error_ != asio::error::eof) {
                return false;
            }
            eof = true;
        }
    }
}

bool serves_hello(asio::io_context& io, asio::ssl::context& client, const asio::ip::tcp::endpoint& endpoint) {
    try {
        const auto stream = connect_tls(io, client, endpoint, "localhost");
        return serves_hello(*stream);
    } catch (const std::system_error&) {
        return false;
    }
}

template <typename stream_type>
std::string response_to_end(stream_type& stream, asio::ip::tcp::socket& socket,
    std::chrono::steady_clock::time_point deadline) {
    std::string response;
    std::array<char, 1024> buffer;
    for (;;) {
        const auto result = client_io(socket, deadline,
            [&](auto completion) { stream.async_read_some(asio::buffer(buffer), std::move(completion)); });
        response.append(buffer.data(), result.size_);
        if (result.error_ == asio::error::eof) {
            return response;
        }
        if (result.error_) {
            throw std::system_error(result.error_, "TLS fixture response failed");
        }
    }
}

// The complete response to GET /hello for `host` over a fresh connection.
std::string https_response(asio::io_context& io, asio::ssl::context& client, const asio::ip::tcp::endpoint& endpoint,
    const char* server_name, std::string_view host) {
    const auto stream = connect_tls(io, client, endpoint, server_name);
    const auto request = "GET /hello HTTP/1.1\r\nHost: " + std::string(host) + "\r\nConnection: close\r\n\r\n";
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    const auto result = client_io(stream->next_layer(), deadline,
        [&](auto completion) { asio::async_write(*stream, asio::buffer(request), std::move(completion)); });
    if (result.error_) {
        throw std::system_error(result.error_, "TLS fixture request failed");
    }
    return response_to_end(*stream, stream->next_layer(), deadline);
}

std::string plain_body(asio::io_context& io, const asio::ip::tcp::endpoint& endpoint, std::string_view target) {
    asio::ip::tcp::socket socket(io);
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    const auto connected = client_io(socket, deadline,
        [&](auto completion) { socket.async_connect(endpoint, std::move(completion)); });
    if (connected.error_) {
        throw std::system_error(connected.error_, "plain fixture connection failed");
    }
    const auto request = "GET " + std::string(target) + " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    const auto written = client_io(socket, deadline,
        [&](auto completion) { asio::async_write(socket, asio::buffer(request), std::move(completion)); });
    if (written.error_) {
        throw std::system_error(written.error_, "plain fixture request failed");
    }
    const auto response = response_to_end(socket, socket, deadline);
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
    std::shared_ptr<SSL_SESSION> session(SSL_SESSION_dup(SSL_get0_session(stream->native_handle())), SSL_SESSION_free);
    if (!session || SSL_SESSION_is_resumable(session.get()) != 1) {
        throw std::runtime_error("TLS session is not resumable");
    }
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

struct external_runtime final {
    asio::io_context first_io_, second_io_;
    ruvia::event_loop_attachment first_{ruvia::attach_event_loop(first_io_)};
    ruvia::event_loop_attachment second_{ruvia::attach_event_loop(second_io_)};
    std::array<ruvia::event_loop, 2> loops_{first_.loop(), second_.loop()};
    std::jthread first_thread_{[this] { first_.run(); }};
    std::jthread second_thread_{[this] { second_.run(); }};
    ~external_runtime() {
        first_.stop();
        second_.stop();
    }
};

struct selector_retirement_action final {
    std::function<void()> action_;
    std::exception_ptr& failure_;

    ~selector_retirement_action() {
        try {
            action_();
        } catch (...) {
            failure_ = std::current_exception();
        }
    }
};

std::shared_ptr<const ruvia::tls_certificate_selector> retiring_selector(
    std::function<void()> action, std::exception_ptr& failure) {
    auto retirement = std::make_unique<selector_retirement_action>(std::move(action), failure);
    return std::make_shared<const ruvia::tls_certificate_selector>(
        [retirement = std::move(retirement)](const ruvia::tls_certificate_selection&)
            -> std::optional<ruvia::tls_certificate_id> { return 1; });
}

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

TEST(Ruvia, certificate_library_parses_only_changed_identity_and_failed_updates_are_atomic) {
    ruvia::test::tls_identity first("first"), second("second");
    ruvia::tls_certificate_store store;
    store.insert_or_assign(1, file_identity(first));
    store.insert_or_assign(2, pem_identity(second));
    EXPECT_EQ(store.size(), 2u);
    // Existing identities are already parsed, not re-read during any mutation.
    std::filesystem::remove(first.private_key_file());
    store.insert_or_assign(2, pem_identity(second));
    EXPECT_TRUE(store.contains(1));
    EXPECT_THROW(store.insert_or_assign(1, file_identity(first)), std::system_error);
    EXPECT_THROW(store.insert_or_assign(0, pem_identity(second)), std::invalid_argument);
    EXPECT_EQ(store.size(), 2u);
    std::array<std::jthread, 4> writers;
    for (std::size_t i = 0; i < writers.size(); ++i) {
        writers[i] = std::jthread([&, id = i + 10] {
            for (int n = 0; n < 8; ++n) {
                store.insert_or_assign(id, pem_identity(second));
                EXPECT_TRUE(store.contains(id));
                EXPECT_TRUE(store.erase(id));
            }
        });
    }
    for (auto& writer : writers) {
        writer.join();
    }
    EXPECT_EQ(store.size(), 2u);
    EXPECT_TRUE(store.erase(1));
    EXPECT_FALSE(store.erase(1));
}

TEST(Ruvia, certificate_library_selector_uses_capabilities_rejects_and_preserves_inflight_snapshots) {
    const ruvia::test::tls_identity rsa("rsa", "DNS:localhost,IP:127.0.0.1");
    const ruvia::test::tls_identity replacement("replacement", "DNS:localhost,IP:127.0.0.1");
    const ruvia::test::tls_identity ec("ec", "DNS:localhost,DNS:deny.test,DNS:unknown.test,DNS:throw.test,IP:127.0.0.1", true);
    const ruvia::test::tls_identity next_ec("next-ec", "DNS:localhost,DNS:deny.test,DNS:unknown.test,DNS:throw.test,IP:127.0.0.1", true);
    const auto store = std::make_shared<ruvia::tls_certificate_store>();
    store->insert_or_assign(1, pem_identity(rsa));
    store->insert_or_assign(2, pem_identity(ec));
    asio::io_context io;
    ruvia::test::tcp_port_reservation reservation(io);
    const auto endpoint = reservation.endpoint();
    std::binary_semaphore selecting{0}, release{0};
    std::atomic<unsigned int> invalid_metadata{0}, calls{0};
    const auto selector = std::make_shared<const ruvia::tls_certificate_selector>(
        [&](const ruvia::tls_certificate_selection& input) -> std::optional<ruvia::tls_certificate_id> {
            ++calls;
            if (input.connection_id_ == 0 || input.remote_address_ != "127.0.0.1" ||
                input.remote_port_ == 0 || !input.local_address_ || !input.local_address_->is_loopback() ||
                input.local_port_ == 0 || input.cipher_suites_.empty()) {
                ++invalid_metadata;
            }
            if (input.server_name_ == "deny.test") {
                return std::nullopt;
            }
            if (input.server_name_ == "unknown.test") {
                return 999;
            }
            if (input.server_name_ == "throw.test") {
                throw std::runtime_error("policy failed");
            }
            if (input.server_name_ == "hold.test") {
                selecting.release();
                if (!release.try_acquire_for(10s)) {
                    throw std::runtime_error("TLS snapshot selection was not released");
                }
                return 1;
            }
            return input.offers_signature_algorithm(0x0403) ? 2 : 1;
        });
    ruvia::listen_config listener{
        .address_ = "127.0.0.1", .https_ = endpoint.port(), .tls_ = {.certificate_store_ = store, .select_certificate_ = selector}};
#if defined(RUVIA_ENABLE_HTTP3)
    ruvia::test::udp_port_reservation udp(io);
    listener.http3_ = udp.endpoint().port();
#endif
    auto& app = ruvia::app();
    app.server({.worker_count_ = 2}).listen(listener).on_start(nullptr).on_start([] { started().release(); });
    ruvia::test::app_run runner(app);
    ASSERT_TRUE(started().try_acquire_for(10s));
    for (const int version : {TLS1_2_VERSION, TLS1_3_VERSION}) {
        auto client = tls_client(version);
        ASSERT_EQ(SSL_CTX_set1_sigalgs_list(client.native_handle(), "rsa_pss_rsae_sha256"), 1);
        EXPECT_EQ(served_name(io, client, endpoint, "localhost"), "rsa");
        auto ec_client = tls_client(version);
        ASSERT_EQ(SSL_CTX_set1_sigalgs_list(ec_client.native_handle(), "ecdsa_secp256r1_sha256"), 1);
        EXPECT_EQ(served_name(io, ec_client, endpoint, "localhost"), "ec");
        EXPECT_TRUE(served_name(io, client, endpoint, "deny.test").empty());
        EXPECT_TRUE(served_name(io, client, endpoint, "unknown.test").empty());
        EXPECT_TRUE(served_name(io, client, endpoint, "throw.test").empty());
        EXPECT_TRUE(https_response(io, ec_client, endpoint, "localhost", "other.test").starts_with("HTTP/1.1 421 "));
    }
    auto rsa12 = tls_client(TLS1_2_VERSION), rsa13 = tls_client(TLS1_3_VERSION);
    ASSERT_EQ(SSL_CTX_set1_sigalgs_list(rsa12.native_handle(), "rsa_pss_rsae_sha256"), 1);
    ASSERT_EQ(SSL_CTX_set1_sigalgs_list(rsa13.native_handle(), "rsa_pss_rsae_sha256"), 1);
    const auto session12 = resumable_session(io, rsa12, endpoint);
    const auto session13 = resumable_session(io, rsa13, endpoint);
    const auto established = connect_tls(io, rsa13, endpoint, "localhost");
    auto held = std::async(std::launch::async, [&] {
        asio::io_context held_io;
        auto client = tls_client();
        return served_name(held_io, client, endpoint, "hold.test");
    });
    const bool entered = selecting.try_acquire_for(10s);
    EXPECT_TRUE(entered);
    store->insert_or_assign(1, pem_identity(replacement));
    EXPECT_TRUE(store->erase(1));
    release.release();
    EXPECT_EQ(held.get(), "rsa");
    EXPECT_TRUE(served_name(io, rsa13, endpoint, "localhost").empty());
    EXPECT_THROW(connect_tls(io, rsa13, endpoint, "localhost", session13.get()), std::system_error);
    EXPECT_TRUE(serves_hello(*established));
    EXPECT_EQ(peer_common_name(established->native_handle()), "rsa");
    store->insert_or_assign(1, pem_identity(replacement));
    EXPECT_EQ(served_name(io, rsa13, endpoint, "localhost"), "replacement");
    EXPECT_TRUE(resumes(io, rsa12, endpoint, session12));
    EXPECT_TRUE(resumes(io, rsa13, endpoint, session13));
#if defined(RUVIA_ENABLE_HTTP3)
    const asio::ip::udp::endpoint h3_endpoint(asio::ip::address_v4::loopback(), udp.endpoint().port());
    ruvia::test::http3_test_client h3(h3_endpoint, ec.ca_file_, "localhost", 5s);
    EXPECT_TRUE(serves_hello(h3));
    const auto ticket = [&] {
        ruvia::test::http3_test_client client(h3_endpoint, ec.ca_file_, "localhost", 5s);
        return client.capture_resumption();
    }();
    store->insert_or_assign(2, pem_identity(next_ec));
    EXPECT_TRUE(serves_hello(h3));
    EXPECT_TRUE(http3_serves(udp.endpoint().port(), next_ec.ca_file_, "localhost"));
    {
        ruvia::test::http3_test_client resumed(h3_endpoint, ec.ca_file_, "localhost", 5s, &ticket);
        EXPECT_TRUE(resumed.resumed());
        EXPECT_TRUE(serves_hello(resumed));
    }
    EXPECT_FALSE(http3_serves(udp.endpoint().port(), next_ec.ca_file_, "deny.test"));
    EXPECT_FALSE(http3_serves(udp.endpoint().port(), next_ec.ca_file_, "unknown.test"));
    EXPECT_FALSE(http3_serves(udp.endpoint().port(), next_ec.ca_file_, "throw.test"));
#endif
    EXPECT_EQ(invalid_metadata.load(), 0u);
    EXPECT_GT(calls.load(), 10u);
    app.stop();
    runner.join();
}

TEST(Ruvia, certificate_library_is_shared_by_independent_single_and_multiworker_hosts) {
    const ruvia::test::tls_identity first("first", "DNS:localhost,IP:127.0.0.1");
    const ruvia::test::tls_identity second("second", "DNS:localhost,IP:127.0.0.1");
    const auto store = std::make_shared<ruvia::tls_certificate_store>();
    store->insert_or_assign(1, file_identity(first));
    external_runtime runtime;
    auto& loops = runtime.loops_;
    asio::io_context client_io;
    // Ensure the external loops are driven before a cross-worker operation.
    const auto ready = []() -> ruvia::task<void> { co_return; };
    loops[0].start(ready()).get();
    loops[1].start(ready()).get();
    ruvia::test::tcp_port_reservation single_port(client_io), group_port(client_io), mtls_port(client_io), limited_port(client_io);
#if defined(RUVIA_ENABLE_HTTP3)
    ruvia::test::udp_port_reservation single_udp(client_io), group_udp(client_io);
#endif
    const auto answer = [](ruvia::request_stream& stream) -> ruvia::task<void> {
        if (co_await stream.response().commit() == ruvia::stream_commit_result::body) {
            co_await stream.response().write("hello");
            co_await stream.response().end();
        }
    };
    const auto selector = std::make_shared<const ruvia::tls_certificate_selector>(
        [](const ruvia::tls_certificate_selection& input) -> std::optional<ruvia::tls_certificate_id> {
            if (input.connection_id_ == 0 || input.remote_port_ == 0 || input.local_port_ == 0) {
                return std::nullopt;
            }
            return 1;
        });
    const auto configuration = [&](std::uint16_t port) {
        return ruvia::listen_config{
            .address_ = "127.0.0.1", .https_ = port, .tls_ = {.certificate_store_ = store, .select_certificate_ = selector}, .dispatch_ = ruvia::listener_dispatch::request_stream};
    };
    ruvia::http_server single(loops[0], {.request_stream_ = answer});
    ruvia::http_server_group group(loops, {.request_stream_ = answer});
    auto group_configuration = configuration(group_port.endpoint().port());
#if defined(RUVIA_ENABLE_HTTP3)
    group_configuration.http3_ = group_udp.endpoint().port();
#endif
    group.listen(group_configuration);
    const auto configure_single = [&]() -> ruvia::task<void> {
        auto single_configuration = configuration(single_port.endpoint().port());
#if defined(RUVIA_ENABLE_HTTP3)
        single_configuration.http3_ = single_udp.endpoint().port();
#endif
        single.listen(single_configuration);
        auto mtls = configuration(mtls_port.endpoint().port());
        mtls.tls_.select_certificate_.reset();
        mtls.tls_.default_certificate_ = 1;
        mtls.tls_.client_certificates_ = {
            .ca_file_ = first.ca_file_, .requirement_ = ruvia::tls_client_certificate_requirement::required};
        single.listen(mtls);
        auto limited = configuration(limited_port.endpoint().port());
        limited.tls_.max_outgoing_records_ = 1;
        single.listen(limited);
        co_await single.start();
    };
    loops[0].start(configure_single()).get();
    group.start();
    std::array clients{tls_client(TLS1_2_VERSION), tls_client(TLS1_3_VERSION)};
    std::array authenticated{tls_client(TLS1_2_VERSION), tls_client(TLS1_3_VERSION)};
    std::array<std::shared_ptr<SSL_SESSION>, 2> single_tickets, group_tickets;
    for (std::size_t i = 0; i < clients.size(); ++i) {
        SCOPED_TRACE(i == 0 ? "TLS 1.2" : "TLS 1.3");
        EXPECT_EQ(served_name(client_io, clients[i], single_port.endpoint(), "localhost"), "first");
        EXPECT_EQ(served_name(client_io, clients[i], group_port.endpoint(), "localhost"), "first");
        single_tickets[i] = resumable_session(client_io, clients[i], single_port.endpoint());
        group_tickets[i] = resumable_session(client_io, clients[i], group_port.endpoint());
        EXPECT_FALSE(serves_hello(client_io, clients[i], mtls_port.endpoint()));
        EXPECT_FALSE(serves_hello(client_io, clients[i], limited_port.endpoint()));
        authenticated[i].use_certificate_chain_file(first.ca_file_.string());
        authenticated[i].use_private_key_file(first.private_key_file().string(), asio::ssl::context::pem);
        EXPECT_EQ(served_name(client_io, authenticated[i], mtls_port.endpoint(), "localhost"), "first");
        EXPECT_TRUE(serves_hello(client_io, authenticated[i], mtls_port.endpoint()));
    }
#if defined(RUVIA_ENABLE_HTTP3)
    EXPECT_TRUE(http3_serves(single_udp.endpoint().port(), first.ca_file_, "localhost"));
    EXPECT_TRUE(http3_serves(group_udp.endpoint().port(), first.ca_file_, "localhost"));
#endif
    store->insert_or_assign(1, pem_identity(second));
    for (std::size_t i = 0; i < clients.size(); ++i) {
        SCOPED_TRACE(i == 0 ? "TLS 1.2" : "TLS 1.3");
        EXPECT_EQ(served_name(client_io, clients[i], single_port.endpoint(), "localhost"), "second");
        EXPECT_EQ(served_name(client_io, clients[i], group_port.endpoint(), "localhost"), "second");
        EXPECT_EQ(served_name(client_io, authenticated[i], mtls_port.endpoint(), "localhost"), "second");
        EXPECT_TRUE(serves_hello(client_io, authenticated[i], mtls_port.endpoint()));
        EXPECT_TRUE(resumes(client_io, clients[i], single_port.endpoint(), single_tickets[i]));
        EXPECT_TRUE(resumes(client_io, clients[i], group_port.endpoint(), group_tickets[i]));
    }
#if defined(RUVIA_ENABLE_HTTP3)
    EXPECT_TRUE(http3_serves(single_udp.endpoint().port(), second.ca_file_, "localhost"));
    EXPECT_TRUE(http3_serves(group_udp.endpoint().port(), second.ca_file_, "localhost"));
#endif
    EXPECT_TRUE(store->erase(1));
    for (auto& client : clients) {
        EXPECT_TRUE(served_name(client_io, client, single_port.endpoint(), "localhost").empty());
        EXPECT_TRUE(served_name(client_io, client, group_port.endpoint(), "localhost").empty());
    }
    group.shutdown();
    loops[0].start(single.shutdown()).get();
}

TEST(Ruvia, certificate_selector_last_release_reenters_application_replacement_outside_control_locks) {
    const ruvia::test::tls_identity first("first", "DNS:localhost");
    const ruvia::test::tls_identity second("second", "DNS:localhost");
    const ruvia::test::tls_identity third("third", "DNS:localhost");
    const auto store = std::make_shared<ruvia::tls_certificate_store>();
    store->insert_or_assign(1, pem_identity(first));
    store->insert_or_assign(2, pem_identity(second));
    store->insert_or_assign(3, pem_identity(third));
    asio::io_context io;
    ruvia::test::tcp_port_reservation port(io);
    const ruvia::listener_endpoint endpoint{.address_ = "127.0.0.1", .port_ = port.endpoint().port()};
    std::exception_ptr retirement_failure;
    unsigned int retirements = 0;
    auto& app = ruvia::app();
    app.server({.worker_count_ = 2})
        .listen({.address_ = "127.0.0.1", .https_ = port.endpoint().port(), .tls_ = {.certificate_store_ = store, .select_certificate_ = retiring_selector([&] {
                                                                                         ++retirements;
                                                                                         app.replace_tls_identities(endpoint, {.certificate_store_ = store, .default_certificate_ = 3});
                                                                                     },
                                                                                                                      retirement_failure)}})
        .on_start(nullptr)
        .on_start([] { started().release(); });
    ruvia::test::app_run runner(app);
    ASSERT_TRUE(started().try_acquire_for(10s));
    auto client = tls_client();
    EXPECT_EQ(served_name(io, client, port.endpoint(), "localhost"), "first");
    app.replace_tls_identities(endpoint, {.certificate_store_ = store, .default_certificate_ = 2});
    EXPECT_EQ(retirements, 1u);
    EXPECT_FALSE(static_cast<bool>(retirement_failure));
    // The destructor's nested replacement must win over the outer operation.
    EXPECT_EQ(served_name(io, client, port.endpoint(), "localhost"), "third");
    app.stop();
    runner.join();
}

TEST(Ruvia, certificate_selector_last_release_reenters_independent_host_and_group_controls) {
    const ruvia::test::tls_identity first("first", "DNS:localhost");
    const ruvia::test::tls_identity second("second", "DNS:localhost");
    const ruvia::test::tls_identity third("third", "DNS:localhost");
    const auto store = std::make_shared<ruvia::tls_certificate_store>();
    store->insert_or_assign(1, pem_identity(first));
    store->insert_or_assign(2, pem_identity(second));
    store->insert_or_assign(3, pem_identity(third));
    external_runtime runtime;
    const auto ready = []() -> ruvia::task<void> { co_return; };
    runtime.loops_[0].start(ready()).get();
    runtime.loops_[1].start(ready()).get();
    asio::io_context io;
    ruvia::test::tcp_port_reservation single_port(io), group_port(io);
    const ruvia::listener_endpoint single_endpoint{.address_ = "127.0.0.1", .port_ = single_port.endpoint().port()};
    const ruvia::listener_endpoint group_endpoint{.address_ = "127.0.0.1", .port_ = group_port.endpoint().port()};
    const auto answer = [](ruvia::request_stream& stream) -> ruvia::task<void> {
        if (co_await stream.response().commit() == ruvia::stream_commit_result::body) {
            co_await stream.response().write("hello");
            co_await stream.response().end();
        }
    };
    std::exception_ptr single_failure, group_failure;
    unsigned int single_retirements = 0, group_retirements = 0;
    ruvia::http_server single(runtime.loops_[0], {.request_stream_ = answer});
    ruvia::http_server_group group(runtime.loops_, {.request_stream_ = answer});
    const auto start_single = [&]() -> ruvia::task<void> {
        single.listen({.address_ = "127.0.0.1", .https_ = single_port.endpoint().port(), .tls_ = {.certificate_store_ = store, .select_certificate_ = retiring_selector([&] {
                                                                                                      ++single_retirements;
                                                                                                      single.replace_tls_identities(single_endpoint, {.certificate_store_ = store, .default_certificate_ = 3});
                                                                                                  },
                                                                                                                                   single_failure)},
            .dispatch_ = ruvia::listener_dispatch::request_stream});
        co_await single.start();
    };
    runtime.loops_[0].start(start_single()).get();
    group.listen({.address_ = "127.0.0.1", .https_ = group_port.endpoint().port(), .tls_ = {.certificate_store_ = store, .select_certificate_ = retiring_selector([&] {
                                                                                                ++group_retirements;
                                                                                                group.replace_tls_identities(group_endpoint, {.certificate_store_ = store, .default_certificate_ = 3});
                                                                                            },
                                                                                                                             group_failure)},
        .dispatch_ = ruvia::listener_dispatch::request_stream});
    group.start();
    auto client = tls_client();
    EXPECT_EQ(served_name(io, client, single_port.endpoint(), "localhost"), "first");
    EXPECT_EQ(served_name(io, client, group_port.endpoint(), "localhost"), "first");
    const auto replace_single = [&]() -> ruvia::task<void> {
        single.replace_tls_identities(single_endpoint, {.certificate_store_ = store, .default_certificate_ = 2});
        EXPECT_EQ(single_retirements, 1u);
        EXPECT_FALSE(static_cast<bool>(single_failure));
        co_return;
    };
    runtime.loops_[0].start(replace_single()).get();
    group.replace_tls_identities(group_endpoint, {.certificate_store_ = store, .default_certificate_ = 2});
    EXPECT_EQ(group_retirements, 1u);
    EXPECT_FALSE(static_cast<bool>(group_failure));
    EXPECT_EQ(served_name(io, client, single_port.endpoint(), "localhost"), "third");
    EXPECT_EQ(served_name(io, client, group_port.endpoint(), "localhost"), "third");
    group.shutdown();
    runtime.loops_[0].start(single.shutdown()).get();
}
