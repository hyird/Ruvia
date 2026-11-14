#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <exception>
#include <initializer_list>
#include <map>
#include <memory>
#include <semaphore>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <asio/buffer.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/ssl/stream.hpp>
#include <asio/write.hpp>
#include <openssl/ssl.h>

#include "app.h"
#include "context.h"
#include "controller.h"
#include "http2_connection.h"
#include "http_server_fixture.h"
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
std::string https_exchange(asio::io_context& io, const asio::ip::tcp::endpoint& endpoint, const char* sni, std::string_view request) {
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
    return std::string("GET ") + std::string(target) + " HTTP/1.1\r\nHost: " + std::string(host) + "\r\nConnection: close\r\n\r\n";
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
        const auto submitted = peer.submit_request_head(ruvia::http2_regular_request_head_view{.scheme_ = (targets.begin() + i)->scheme_, .authority_ = (targets.begin() + i)->authority_, .target_ = "/origin"});
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

std::vector<unsigned> h2_over_tls(asio::io_context& io, const asio::ip::tcp::endpoint& endpoint, std::initializer_list<http2_target> targets) {
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

std::vector<unsigned> h2_cleartext(asio::io_context& io, const asio::ip::tcp::endpoint& endpoint, std::initializer_list<http2_target> targets) {
    asio::ip::tcp::socket socket(io);
    socket.connect(endpoint);
    return http2_statuses(socket, targets);
}
#endif

}  // namespace misdirected_request_test

using namespace misdirected_request_test;

TEST(Ruvia, requests_the_connection_cannot_carry_are_answered_421) {
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
        .on_start([] {
        started.release();
    });
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
            EXPECT_EQ(https_exchange(io, https_endpoint, "localhost", get("https://%6Cocalhost/origin", "localhost")), "HTTP/1.1 200 OK");
            EXPECT_EQ(https_exchange(io, https_endpoint, "localhost", get("/origin", "localhost%2E%2E")), "HTTP/1.1 421 Misdirected Request");
            EXPECT_EQ(https_exchange(io, https_endpoint, nullptr, get("/origin", ".wild.test")), "HTTP/1.1 421 Misdirected Request");
            EXPECT_EQ(https_exchange(io, https_endpoint, nullptr, get("/origin", "%2Ewild.test")), "HTTP/1.1 421 Misdirected Request");
            EXPECT_EQ(https_exchange(io, https_endpoint, nullptr, get("/origin", "*.wild.test")), "HTTP/1.1 421 Misdirected Request");
            EXPECT_EQ(https_exchange(io, https_endpoint, nullptr, get("/origin", "127.0.0.1.")), "HTTP/1.1 421 Misdirected Request");
            // Names it does not cover, whatever SNI was offered.
            EXPECT_EQ(https_exchange(io, https_endpoint, "localhost", get("/origin", "attacker.example")), "HTTP/1.1 421 Misdirected Request");
            EXPECT_EQ(https_exchange(io, https_endpoint, "attacker.example", get("/origin", "attacker.example")), "HTTP/1.1 421 Misdirected Request");
            EXPECT_EQ(https_exchange(io, https_endpoint, nullptr, get("/origin", "attacker.example")), "HTTP/1.1 421 Misdirected Request");
            EXPECT_EQ(https_exchange(io, https_endpoint, nullptr, get("/origin", "a.b.wild.test")), "HTTP/1.1 421 Misdirected Request");
            EXPECT_EQ(https_exchange(io, https_endpoint, nullptr, get("/origin", "wild.test")), "HTTP/1.1 421 Misdirected Request");
            EXPECT_EQ(https_exchange(io, https_endpoint, nullptr, get("/origin", "[::1]")), "HTTP/1.1 421 Misdirected Request");
            EXPECT_EQ(https_exchange(io, https_endpoint, nullptr, get("https://attacker.example/origin", "localhost")), "HTTP/1.1 421 Misdirected Request");
#if defined(RUVIA_ENABLE_HTTP2)
            // HTTP/2 refuses only the misdirected stream; the connection
            // still serves the covered one.
            const auto h2 = h2_over_tls(io, https_endpoint, {{"https", "attacker.example"}, {"https", "localhost"}, {"https", "x.wild.test:443"}});
            EXPECT_EQ(h2.size(), std::size_t{3});
            if (h2.size() == 3) {
                EXPECT_EQ(h2[0], 421U);
                EXPECT_EQ(h2[1], 200U);
                EXPECT_EQ(h2[2], 200U);
            }
#endif

            // A cleartext connection carries no "https" target.
            EXPECT_EQ(http_exchange(io, http_endpoint, get("https://a.example/origin", "x")), "HTTP/1.1 421 Misdirected Request");
            EXPECT_EQ(http_exchange(io, http_endpoint, get("HTTPS://localhost:443/origin", "x")), "HTTP/1.1 421 Misdirected Request");
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
