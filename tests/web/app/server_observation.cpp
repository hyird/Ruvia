#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <semaphore>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <asio/buffer.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/ssl/context.hpp>
#include <asio/ssl/stream.hpp>
#include <asio/write.hpp>
#include <openssl/ssl.h>

#include "app.h"
#include "context.h"
#include "controller.h"
#include "http2_connection.h"
#include "http3_connection.h"
#include "http_request.h"
#include "http_server.h"
#include "http_server_fixture.h"
#include "middleware.h"
#if defined(RUVIA_ENABLE_HTTP2)

#include "http2_connection.h"
#include "http_server_fixture.h"
#endif

#if defined(RUVIA_ENABLE_HTTP3)
#include <asio/ip/udp.hpp>

#include "http3_test_client.h"
#endif

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
        client_hellos_.push_back({hello.connection_id(), std::string(hello.local().address().to_string()), hello.local().port(), std::string(hello.remote().address().to_string()), hello.remote().port(), hello.quic(), std::string(reinterpret_cast<const char*>(message.data()), message.size())});
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
    const auto length = (std::size_t{static_cast<unsigned char>(message[1])} << 16) | (std::size_t{static_cast<unsigned char>(message[2])} << 8) | std::size_t{static_cast<unsigned char>(message[3])};
    return length == message.size() - 4;
}

void capture_sent_client_hello(int writing, int, int content_type, const void* bytes, std::size_t size, SSL*, void* argument) {
    if (writing != 0 && content_type == SSL3_RT_HANDSHAKE && size >= 4 && *static_cast<const unsigned char*>(bytes) == SSL3_MT_CLIENT_HELLO) {
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
    if (SSL_set_alpn_protos(stream.native_handle(), alpn, sizeof(alpn)) != 0 || SSL_set_tlsext_host_name(stream.native_handle(), "localhost") != 1 || (groups != nullptr && SSL_set1_groups_list(stream.native_handle(), groups) != 1)) {
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
        if (frame.header_.type_ == static_cast<std::uint8_t>(ruvia::http2_frame_type::data) && frame.header_.stream_id_ == 1) {
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
    if (SSL_set_alpn_protos(stream.native_handle(), alpn, sizeof(alpn)) != 0 || SSL_set_tlsext_host_name(stream.native_handle(), "localhost") != 1) {
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

using namespace server_observation_test;

// One application with cleartext, HTTPS and HTTP/3 services on two workers.
// The first run observes: every ClientHello (TLS over TCP, including the
// second one after a HelloRetryRequest, and QUIC) arrives as the exact bytes
// the client sent, tagged with the conn_info::connection_id() its requests see, and the
// HTTP/2 input of each connection (TLS ALPN h2 and cleartext prior knowledge)
// concatenates to exactly what the client wrote, preface first. The second
// run, with both observers cleared, serves the same requests and records
// nothing.
TEST(Ruvia, server_observers_see_client_hellos_and_http2_input_by_connection_id) {
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
        .tls_ = {.identity_ = {.certificate_chain_file_ = identity.ca_file_, .private_key_file_ = identity.private_key_file()}},
    };
#if defined(RUVIA_ENABLE_HTTP3)
    ruvia::test::udp_port_reservation udp_reservation(io);
    const auto http3_port = udp_reservation.endpoint().port();
    listener.http3_ = http3_port;
#endif

    auto& app = ruvia::app();
    app.server({.worker_count_ = 2})
        .listen(listener)
        .on_start([] {
        started().release();
    })
        .on_tls_client_hello([](const ruvia::tls_client_hello_record& hello) noexcept {
        records().add(hello);
    }).on_http2_input([](const ruvia::http2_input_record& input) noexcept {
        records().add(input);
    });
#if defined(RUVIA_ENABLE_HTTP3)
    app.on_http3_frame([](const ruvia::http3_frame_record& frame) noexcept {
        records().add(frame);
    }).on_http3_field([](const ruvia::http3_field_record& field) noexcept {
        records().add(field);
    });
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
        for (const auto& exchange : {cleartext_http2_connection_id(io, http_endpoint), tls_http2_connection_id(io, https_endpoint)}) {
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
