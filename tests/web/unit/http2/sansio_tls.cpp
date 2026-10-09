#include <cstdint>
#include <memory>
#include <memory_resource>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

#include <asio/as_tuple.hpp>
#include <asio/awaitable.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read.hpp>
#include <asio/ssl.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/write.hpp>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>

#include "ruvia/core/asio_task.h"
#include "ruvia/core/memory/memory_pool.h"
#include "ruvia/http/hpack.h"
#include "ruvia/http/http2_framing.h"
#include "ruvia/web/context.h"

#include "http2/http2_sans_io_session.h"
#include "http2_sansio_session_fixture.h"
#include "router/router.h"
#include "router/router_impl.h"
#include "server/http_server_alpn.h"
#include "server/http_server_tls_verify.h"
#include "test_harness.h"
#include "test_io_context.h"
#include "test_tls_crypto.h"

namespace {

using asio::ip::tcp;
using ruvia::hpack_encoder;
using ruvia::http2_frame_type;

constexpr std::uint8_t end_stream = 0x1;
constexpr std::uint8_t end_headers = 0x4;

struct tls_connection_observation final {
    bool saw_plain_{false};
    bool saw_tls_{false};
    bool client_certificate_empty_{false};
    std::string_view remote_address_;
};

ruvia::task<ruvia::http_response> tls_pong_handler(void* state_value, ruvia::context& ctx) {
    auto& observation_value = *static_cast<tls_connection_observation*>(state_value);
    const auto info = ctx.conn();
    observation_value.saw_plain_ = info.plain() != nullptr;
    observation_value.saw_tls_ = info.tls() != nullptr;
    observation_value.client_certificate_empty_ =
        info.tls() != nullptr && info.tls()->client_certificate_subject().empty();
    observation_value.remote_address_ = info.remote().address();
    co_return ctx.text("tls-pong");
}

std::string frame(
    std::uint8_t type, std::uint8_t flags, std::uint32_t stream_id, std::string_view payload_value) {
    std::string bytes_value(ruvia::http2_frame_header_bytes, '\0');
    if (!ruvia::encode_http2_frame_header(std::span<char>(bytes_value.data(), bytes_value.size()),
            static_cast<std::uint32_t>(payload_value.size()), static_cast<http2_frame_type>(type), flags,
            stream_id)) {
        throw std::invalid_argument("invalid test HTTP/2 frame");
    }
    bytes_value.append(payload_value);
    return bytes_value;
}

// Generate an ephemeral RSA-2048 self-signed cert + key, PEM-encoded into memory.
struct self_signed_pem {
    std::string cert_;
    std::string key_;
};

self_signed_pem make_self_signed_pem() {
    EVP_PKEY* pkey = EVP_PKEY_Q_keygen(nullptr, nullptr, "RSA", std::size_t{2048});
    X509* x509 = X509_new_ex(nullptr, nullptr);
    ASN1_INTEGER_set(X509_get_serialNumber(x509), 1);
    X509_gmtime_adj(X509_getm_notBefore(x509), 0);
    X509_gmtime_adj(X509_getm_notAfter(x509), 60 * 60);  // 1 hour
    X509_set_pubkey(x509, pkey);
    const auto name = std::unique_ptr<X509_NAME, decltype(&X509_NAME_free)>(X509_NAME_new(), X509_NAME_free);
    X509_NAME_add_entry_by_txt(
        name.get(), "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char*>("localhost"), -1, -1, 0);
    X509_set_subject_name(x509, name.get());
    X509_set_issuer_name(x509, name.get());
    ruvia::test::sign_tls_certificate(x509, pkey);

    self_signed_pem out;
    BIO* cert_bio = BIO_new(BIO_s_mem());
    PEM_write_bio_X509(cert_bio, x509);
    char* cert_data = nullptr;
    const long cert_len = BIO_get_mem_data(cert_bio, &cert_data);
    out.cert_.assign(cert_data, static_cast<std::size_t>(cert_len));
    BIO* key_bio = BIO_new(BIO_s_mem());
    ruvia::test::write_tls_private_key(key_bio, pkey);
    char* key_data = nullptr;
    const long key_len = BIO_get_mem_data(key_bio, &key_data);
    out.key_.assign(key_data, static_cast<std::size_t>(key_len));

    BIO_free(cert_bio);
    BIO_free(key_bio);
    X509_free(x509);
    EVP_PKEY_free(pkey);
    return out;
}

// Server ALPN callback: advertise h2 (matching the production selector).
int server_alpn_select(SSL*, const unsigned char** out, unsigned char* outlen,
    const unsigned char* in, unsigned int inlen, void*) {
    static const unsigned char h2[] = {2, 'h', '2'};
    if (SSL_select_next_proto(const_cast<unsigned char**>(out), outlen, h2, sizeof(h2), in,
            inlen) != OPENSSL_NPN_NEGOTIATED) {
        return SSL_TLSEXT_ERR_NOACK;
    }
    return SSL_TLSEXT_ERR_OK;
}

}  // namespace

// End-to-end proof of the production TLS-ALPN h2 path: a real TLS handshake over
// loopback with the server advertising h2 via ALPN and the client offering it, then
// is_http2_alpn_selected() must be true and run_http2_sans_io_session over the TLS stream
// answers a GET. It exercises the same successful-handshake -> typed TLS transport
// -> ALPN HTTP/2 boundary used by HttpServerSessionEntry.inl, plus the production
// selector (http_server_alpn.h) that no other test exercises.
RUVIA_TEST(sansio_tls_alpn_h2_round_trip) {
    const auto pem = make_self_signed_pem();

    asio::io_context& io = ruvia::test::new_test_io_context();
    tcp::acceptor acceptor(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const std::uint16_t port = acceptor.local_endpoint().port();
    bool alpn_was_h2 = false;
    std::string body;
    tls_connection_observation connection_observation;

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            auto sock = co_await acceptor.async_accept(asio::use_awaitable);
            asio::ssl::context server_ctx(asio::ssl::context::tls_server);
            server_ctx.use_certificate(
                asio::buffer(pem.cert_.data(), pem.cert_.size()), asio::ssl::context::pem);
            server_ctx.use_private_key(
                asio::buffer(pem.key_.data(), pem.key_.size()), asio::ssl::context::pem);
            SSL_CTX_set_alpn_select_cb(server_ctx.native_handle(), &server_alpn_select, nullptr);

            asio::ssl::stream<tcp::socket&> tls(sock, server_ctx);
            auto [hs_ec] = co_await tls.async_handshake(
                asio::ssl::stream_base::server, asio::as_tuple(asio::use_awaitable));
            if (hs_ec) {
                co_return;
            }
            alpn_was_h2 = ruvia::detail::is_http2_alpn_selected(tls);

            ruvia::worker_memory worker;
            ruvia::detail::router router;
            auto& impl = ruvia::detail::router_impl::from(router);
            impl.register_route(ruvia::http_known_method::get,
                std::pmr::string("/ping", std::pmr::get_default_resource()),
                ruvia::detail::route_handler_type(&connection_observation, &tls_pong_handler),
                ruvia::detail::request_body_mode::buffered,
                std::span<const ruvia::detail::controller_middleware_descriptor>{},
                std::span<const ruvia::detail::controller_middleware_descriptor>{});
            impl.finalize();
            co_await ruvia::as_awaitable(ruvia::test::run_bare_tls_http2_sans_io_session(
                tls, impl.route_table(), worker, "127.0.0.1"));
        },
        asio::detached);

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            tcp::socket sock(io);
            co_await sock.async_connect(
                tcp::endpoint(asio::ip::make_address("127.0.0.1"), port), asio::use_awaitable);
            asio::ssl::context client_ctx(asio::ssl::context::tls_client);
            asio::ssl::stream<tcp::socket&> tls(sock, client_ctx);
            static const unsigned char alpn_h2[] = {2, 'h', '2'};
            SSL_set_alpn_protos(tls.native_handle(), alpn_h2, sizeof(alpn_h2));
            auto [hs_ec] = co_await tls.async_handshake(
                asio::ssl::stream_base::client, asio::as_tuple(asio::use_awaitable));
            if (hs_ec) {
                co_return;
            }

            auto write_all = [&tls](std::string_view bytes_value) -> asio::awaitable<bool> {
                auto [ec, n] = co_await asio::async_write(tls,
                    asio::buffer(bytes_value.data(), bytes_value.size()), asio::as_tuple(asio::use_awaitable));
                (void)n;
                co_return !ec;
            };
            auto read_exact = [&tls](void* data, std::size_t size) -> asio::awaitable<bool> {
                auto [ec, n] = co_await asio::async_read(
                    tls, asio::buffer(data, size), asio::as_tuple(asio::use_awaitable));
                co_return !ec && n == size;
            };

            if (!co_await write_all(ruvia::http2_client_preface)) {
                co_return;
            }
            if (!co_await write_all(frame(0x4, 0, 0, {}))) {
                co_return;
            }
            std::pmr::string header_block(std::pmr::get_default_resource());
            hpack_encoder::encode_header(header_block, ":method", "GET");
            hpack_encoder::encode_header(header_block, ":path", "/ping");
            hpack_encoder::encode_header(header_block, ":scheme", "https");
            hpack_encoder::encode_header(header_block, ":authority", "localhost");
            if (!co_await write_all(frame(0x1,
                    end_stream | end_headers, 1,
                    std::string_view(header_block.data(), header_block.size())))) {
                co_return;
            }

            for (;;) {
                char hb[ruvia::http2_frame_header_bytes];
                if (!co_await read_exact(hb, sizeof(hb))) {
                    break;
                }
                const auto header_value = ruvia::parse_http2_frame_header(std::span<const char>(hb));
                if (!header_value.has_value()) {
                    break;
                }
                std::string payload_value(header_value->length_, '\0');
                if (header_value->length_ != 0 && !co_await read_exact(payload_value.data(), payload_value.size())) {
                    break;
                }
                if (header_value->type_ == static_cast<std::uint8_t>(http2_frame_type::data) &&
                    header_value->stream_id_ == 1 && !payload_value.empty()) {
                    body = payload_value;
                    break;
                }
            }
            std::error_code ignore;
            sock.shutdown(tcp::socket::shutdown_both, ignore);
        },
        asio::detached);

    io.run();
    RUVIA_CHECK(alpn_was_h2);         // the production selector saw h2
    RUVIA_CHECK(body == "tls-pong");  // and the session answered over TLS
    RUVIA_CHECK(!connection_observation.saw_plain_);
    RUVIA_CHECK(connection_observation.saw_tls_);
    RUVIA_CHECK(connection_observation.client_certificate_empty_);
    RUVIA_CHECK(connection_observation.remote_address_ == std::string_view("127.0.0.1"));
}

// Optional mutual TLS verifies a presented certificate but admits a client that
// presents none; required mode adds fail-if-no-peer-cert so a missing
// certificate fails the handshake (mandatory mutual TLS).
RUVIA_TEST(http_server_tls_verify_mode_optional_vs_mandatory) {
    const auto optional =
        ruvia::detail::http_server_tls_verify_mode(ruvia::tls_client_certificate_requirement::optional);
    RUVIA_CHECK(optional == asio::ssl::verify_peer);
    RUVIA_CHECK((optional & asio::ssl::verify_fail_if_no_peer_cert) == 0);

    const auto mandatory =
        ruvia::detail::http_server_tls_verify_mode(ruvia::tls_client_certificate_requirement::required);
    RUVIA_CHECK((mandatory & asio::ssl::verify_peer) != 0);
    RUVIA_CHECK((mandatory & asio::ssl::verify_fail_if_no_peer_cert) != 0);
}
