#include <array>
#include <chrono>
#include <cstdint>
#include <exception>
#include <map>
#include <memory>
#include <semaphore>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <asio/buffer.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/ssl/stream.hpp>
#include <asio/write.hpp>
#include <openssl/bio.h>
#include <openssl/ssl.h>

#include "ruvia/http/http2_connection.h"
#include "ruvia/web/app.h"
#include "ruvia/web/context.h"
#include "ruvia/web/controller.h"

#include "test_harness.h"
#include "test_tls_crypto.h"
#include "test_tls_identity.h"

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

template <typename stream_type>
std::string read_to_end(stream_type& stream, asio::error_code& error) {
    std::string response;
    std::array<char, 1024> buffer;
    while (!error) {
        const auto size = stream.read_some(asio::buffer(buffer), error);
        response.append(buffer.data(), size);
    }
    return response;
}

// The status line of `request` on a fresh cleartext connection.
std::string http_exchange(asio::io_context& io, const asio::ip::tcp::endpoint& endpoint, std::string_view request) {
    asio::ip::tcp::socket socket(io);
    asio::error_code error;
    socket.connect(endpoint, error);
    if (!error) {
        asio::write(socket, asio::buffer(request), error);
    }
    const auto response = read_to_end(socket, error);
    return response.substr(0, response.find("\r\n"));
}

// The status line of `request` on a fresh TLS connection offering `sni`
// (none when null). The client authenticates nothing: the server decides.
std::string https_exchange(
    asio::io_context& io, const asio::ip::tcp::endpoint& endpoint, const char* sni, std::string_view request) {
    asio::ssl::context client(asio::ssl::context::tls_client);
    client.set_verify_mode(asio::ssl::verify_none);
    asio::ssl::stream<asio::ip::tcp::socket> stream(io, client);
    if (sni != nullptr && SSL_set_tlsext_host_name(stream.native_handle(), sni) != 1) {
        return {};
    }
    asio::error_code error;
    stream.next_layer().connect(endpoint, error);
    if (!error) {
        stream.handshake(asio::ssl::stream_base::client, error);
    }
    if (!error) {
        asio::write(stream, asio::buffer(request), error);
    }
    const auto response = read_to_end(stream, error);
    return response.substr(0, response.find("\r\n"));
}

[[nodiscard]] std::string get(std::string_view target, std::string_view host) {
    return std::string("GET ") + std::string(target) + " HTTP/1.1\r\nHost: " + std::string(host) +
           "\r\nConnection: close\r\n\r\n";
}

struct http2_target final {
    std::string_view scheme_;
    std::string_view authority_;
};

// Sends every request on one HTTP/2 connection over `stream` and returns the
// response status of each, in order; 0 when a stream ended without one.
template <typename stream_type>
std::vector<unsigned> http2_statuses(stream_type& stream, const std::vector<http2_target>& targets) {
    auto peer = ruvia::http2_connection::client();
    std::map<std::uint32_t, std::size_t> pending;
    std::vector<unsigned> statuses(targets.size(), 0);
    for (std::size_t i = 0; i < targets.size(); ++i) {
        const auto submitted = peer.submit_request_head(ruvia::http2_regular_request_head_view{
            .scheme_ = targets[i].scheme_, .authority_ = targets[i].authority_, .target_ = "/origin"});
        const auto* id = std::get_if<std::uint32_t>(&submitted);
        if (id == nullptr) {
            throw std::runtime_error("HTTP/2 test request rejected locally");
        }
        pending.emplace(*id, i);
    }
    std::pmr::string output;
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
    asio::io_context& io, const asio::ip::tcp::endpoint& endpoint, const std::vector<http2_target>& targets) {
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
    asio::io_context& io, const asio::ip::tcp::endpoint& endpoint, const std::vector<http2_target>& targets) {
    asio::ip::tcp::socket socket(io);
    socket.connect(endpoint);
    return http2_statuses(socket, targets);
}

}  // namespace misdirected_request_test

using namespace misdirected_request_test;

RUVIA_TEST(requests_the_connection_cannot_carry_are_answered_421) {
    ruvia::test::tls_identity identity("localhost", "DNS:localhost,DNS:*.wild.test,IP:127.0.0.1");
    const auto private_key = identity.ca_file_.parent_path() / "key.pem";
    {
        std::unique_ptr<BIO, decltype(&BIO_free)> output(BIO_new_file(private_key.string().c_str(), "w"), BIO_free);
        if (!output || ruvia::test::write_tls_private_key(
                           output.get(), SSL_CTX_get0_privatekey(identity.context_.native_handle())) != 1) {
            throw std::runtime_error("cannot write misdirected-request test private key");
        }
    }

    asio::io_context io;
    // Both reservations are held at once so the two ports differ.
    asio::ip::tcp::acceptor http_reservation(io, {asio::ip::address_v4::loopback(), 0});
    asio::ip::tcp::acceptor https_reservation(io, {asio::ip::address_v4::loopback(), 0});
    const auto http_endpoint = http_reservation.local_endpoint();
    const auto https_endpoint = https_reservation.local_endpoint();
    http_reservation.close();
    https_reservation.close();

    auto& app = ruvia::app();
    std::binary_semaphore started{0};
    app.server({.worker_count_ = 1, .connection_scan_interval_ = 5ms})
        .listen({
            .address_ = "127.0.0.1",
            .http_ = http_endpoint.port(),
            .https_ = https_endpoint.port(),
            .tls_ = {.certificate_chain_file_ = identity.ca_file_, .private_key_file_ = private_key},
        })
        .on_start([&started] { started.release(); });
    const auto ip_host = "127.0.0.1:" + std::to_string(https_endpoint.port());
    for (const bool trusted : {false, true}) {
        if (trusted) {
            app.trusted_proxies({.cidrs_ = {"127.0.0.1/32"}});
        }
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
        if (ready && !trusted) {
            // Names the certificate covers: case-insensitively, as an absolute
            // domain name, through a whole-label wildcard, or as an IP SAN.
            RUVIA_CHECK_EQ(https_exchange(io, https_endpoint, "localhost", get("/origin", "localhost")), "HTTP/1.1 200 OK");
            RUVIA_CHECK_EQ(https_exchange(io, https_endpoint, "localhost", get("/origin", "LOCALHOST.")), "HTTP/1.1 200 OK");
            RUVIA_CHECK_EQ(https_exchange(io, https_endpoint, nullptr, get("/origin", "a.wild.test")), "HTTP/1.1 200 OK");
            RUVIA_CHECK_EQ(https_exchange(io, https_endpoint, nullptr, get("/origin", ip_host)), "HTTP/1.1 200 OK");
            RUVIA_CHECK_EQ(https_exchange(io, https_endpoint, "localhost", get("/origin", "%6Cocalhost")), "HTTP/1.1 200 OK");
            RUVIA_CHECK_EQ(https_exchange(io, https_endpoint, "localhost", get("https://%6Cocalhost/origin", "localhost")),
                "HTTP/1.1 200 OK");
            RUVIA_CHECK_EQ(https_exchange(io, https_endpoint, "localhost", get("/origin", "localhost%2E%2E")),
                "HTTP/1.1 421 Misdirected Request");
            // Names it does not cover, whatever SNI was offered.
            RUVIA_CHECK_EQ(https_exchange(io, https_endpoint, "localhost", get("/origin", "attacker.example")),
                "HTTP/1.1 421 Misdirected Request");
            RUVIA_CHECK_EQ(https_exchange(io, https_endpoint, "attacker.example", get("/origin", "attacker.example")),
                "HTTP/1.1 421 Misdirected Request");
            RUVIA_CHECK_EQ(https_exchange(io, https_endpoint, nullptr, get("/origin", "attacker.example")),
                "HTTP/1.1 421 Misdirected Request");
            RUVIA_CHECK_EQ(https_exchange(io, https_endpoint, nullptr, get("/origin", "a.b.wild.test")),
                "HTTP/1.1 421 Misdirected Request");
            RUVIA_CHECK_EQ(https_exchange(io, https_endpoint, nullptr, get("/origin", "wild.test")),
                "HTTP/1.1 421 Misdirected Request");
            RUVIA_CHECK_EQ(https_exchange(io, https_endpoint, nullptr, get("/origin", "[::1]")),
                "HTTP/1.1 421 Misdirected Request");
            RUVIA_CHECK_EQ(https_exchange(io, https_endpoint, nullptr, get("https://attacker.example/origin", "localhost")),
                "HTTP/1.1 421 Misdirected Request");
            // HTTP/2 refuses only the misdirected stream; the connection
            // still serves the covered one.
            const auto h2 = h2_over_tls(io, https_endpoint,
                {{"https", "attacker.example"}, {"https", "localhost"}, {"https", "x.wild.test:443"}});
            RUVIA_CHECK_EQ(h2.size(), std::size_t{3});
            if (h2.size() == 3) {
                RUVIA_CHECK_EQ(h2[0], 421U);
                RUVIA_CHECK_EQ(h2[1], 200U);
                RUVIA_CHECK_EQ(h2[2], 200U);
            }

            // A cleartext connection carries no "https" target.
            RUVIA_CHECK_EQ(http_exchange(io, http_endpoint, get("https://a.example/origin", "x")),
                "HTTP/1.1 421 Misdirected Request");
            RUVIA_CHECK_EQ(http_exchange(io, http_endpoint, get("HTTPS://localhost:443/origin", "x")),
                "HTTP/1.1 421 Misdirected Request");
            RUVIA_CHECK_EQ(http_exchange(io, http_endpoint, get("http://a.example/origin", "x")), "HTTP/1.1 200 OK");
            RUVIA_CHECK_EQ(http_exchange(io, http_endpoint, get("/origin", "attacker.example")), "HTTP/1.1 200 OK");
            const auto h2c = h2_cleartext(io, http_endpoint, {{"https", "localhost"}, {"http", "localhost"}});
            RUVIA_CHECK_EQ(h2c.size(), std::size_t{2});
            if (h2c.size() == 2) {
                RUVIA_CHECK_EQ(h2c[0], 421U);
                RUVIA_CHECK_EQ(h2c[1], 200U);
            }
        }
        if (ready && trusted) {
            // A trusted gateway vouches for the target itself.
            RUVIA_CHECK_EQ(https_exchange(io, https_endpoint, "localhost", get("/origin", "attacker.example")), "HTTP/1.1 200 OK");
            RUVIA_CHECK_EQ(http_exchange(io, http_endpoint, get("https://a.example/origin", "x")), "HTTP/1.1 200 OK");
            const auto h2 = h2_over_tls(io, https_endpoint, {{"https", "attacker.example"}});
            RUVIA_CHECK_EQ(h2.size(), std::size_t{1});
            if (h2.size() == 1) {
                RUVIA_CHECK_EQ(h2[0], 200U);
            }
        }
        app.stop();
        runner.join();
        if (failure) {
            std::rethrow_exception(failure);
        }
    }
    app.trusted_proxies(nullptr);
}
