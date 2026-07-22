#include "test_io_context.h"
#include "test_harness.h"
#include "http2_sansio_session_fixture.h"

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

#include <cstdint>
#include <memory>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>

#include "ruvia/http/detail/http2/Http2FrameCodec.h"
#include "ruvia/web/detail/server/HttpServerTlsVerify.h"
#include "ruvia/http/detail/http2/Http2Hpack.h"
#include "ruvia/web/detail/server/HttpServerAlpn.h"
#include "ruvia/web/detail/server/Http2SansIoSession.h"
#include "ruvia/web/detail/router/RouterImpl.h"
#include "ruvia/core/detail/AsioAwait.h"
#include "ruvia/web/Context.h"
#include "ruvia/core/memory/MemoryPool.h"
#include "ruvia/web/Router.h"

namespace {

using asio::ip::tcp;
using ruvia::detail::Http2FrameType;
using ruvia::detail::HpackEncoder;

constexpr std::string_view kClientPreface = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";

struct TlsConnectionObservation final {
    bool sawPlain{false};
    bool sawTls{false};
    bool clientCertificateEmpty{false};
    std::string_view remoteAddress;
};

ruvia::Task<ruvia::HttpResponse> tlsPongHandler(
    void* state,
    ruvia::Context& ctx) {
    auto& observation = *static_cast<TlsConnectionObservation*>(state);
    const auto info = ruvia::getConnInfo(ctx);
    observation.sawPlain = info.plain() != nullptr;
    observation.sawTls = info.tls() != nullptr;
    observation.clientCertificateEmpty =
        info.tls() != nullptr &&
        info.tls()->clientCertificateSubject().empty();
    observation.remoteAddress = info.remote().address();
    co_return ctx.text("tls-pong");
}

std::string frame(std::uint8_t type, std::uint8_t flags, std::uint32_t streamId, std::string_view payload) {
    std::string bytes(ruvia::detail::kHttp2FrameHeaderBytes, '\0');
    ruvia::detail::http2WriteFrameHeader(
        bytes.data(), static_cast<std::uint32_t>(payload.size()),
        static_cast<Http2FrameType>(type), flags, streamId);
    bytes.append(payload);
    return bytes;
}

// Generate an ephemeral RSA-2048 self-signed cert + key, PEM-encoded into memory.
struct SelfSignedPem {
    std::string cert;
    std::string key;
};

SelfSignedPem makeSelfSignedPem() {
    EVP_PKEY* pkey = EVP_RSA_gen(2048);
    X509* x509 = X509_new();
    ASN1_INTEGER_set(X509_get_serialNumber(x509), 1);
    X509_gmtime_adj(X509_getm_notBefore(x509), 0);
    X509_gmtime_adj(X509_getm_notAfter(x509), 60 * 60);  // 1 hour
    X509_set_pubkey(x509, pkey);
    X509_NAME* name = X509_get_subject_name(x509);
    X509_NAME_add_entry_by_txt(
        name, "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char*>("localhost"), -1, -1, 0);
    X509_set_issuer_name(x509, name);
    X509_sign(x509, pkey, EVP_sha256());

    SelfSignedPem out;
    BIO* certBio = BIO_new(BIO_s_mem());
    PEM_write_bio_X509(certBio, x509);
    char* certData = nullptr;
    const long certLen = BIO_get_mem_data(certBio, &certData);
    out.cert.assign(certData, static_cast<std::size_t>(certLen));
    BIO* keyBio = BIO_new(BIO_s_mem());
    PEM_write_bio_PrivateKey(keyBio, pkey, nullptr, nullptr, 0, nullptr, nullptr);
    char* keyData = nullptr;
    const long keyLen = BIO_get_mem_data(keyBio, &keyData);
    out.key.assign(keyData, static_cast<std::size_t>(keyLen));

    BIO_free(certBio);
    BIO_free(keyBio);
    X509_free(x509);
    EVP_PKEY_free(pkey);
    return out;
}

// Server ALPN callback: advertise h2 (matching the production selector).
int serverAlpnSelect(
    SSL*, const unsigned char** out, unsigned char* outlen,
    const unsigned char* in, unsigned int inlen, void*) {
    static const unsigned char kH2[] = {2, 'h', '2'};
    if (SSL_select_next_proto(
            const_cast<unsigned char**>(out), outlen, kH2, sizeof(kH2), in, inlen) !=
        OPENSSL_NPN_NEGOTIATED) {
        return SSL_TLSEXT_ERR_NOACK;
    }
    return SSL_TLSEXT_ERR_OK;
}

}  // namespace

// End-to-end proof of the production TLS-ALPN h2 path: a real TLS handshake over
// loopback with the server advertising h2 via ALPN and the client offering it, then
// isHttp2AlpnSelected() must be true and runHttp2SansIoSession over the TLS stream
// answers a GET. It exercises the same successful-handshake -> typed TLS transport
// -> ALPN HTTP/2 boundary used by HttpServerSessionEntry.inl, plus the production
// selector (HttpServerAlpn.h) that no other test exercises.
RUVIA_TEST(sansio_tls_alpn_h2_round_trip) {
    const auto pem = makeSelfSignedPem();

    asio::io_context& io = ruvia::test::newTestIoContext();
    tcp::acceptor acceptor(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const std::uint16_t port = acceptor.local_endpoint().port();
    bool alpnWasH2 = false;
    std::string body;
    TlsConnectionObservation connectionObservation;

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            auto sock = co_await acceptor.async_accept(asio::use_awaitable);
            asio::ssl::context serverCtx(asio::ssl::context::tls_server);
            serverCtx.use_certificate(
                asio::buffer(pem.cert.data(), pem.cert.size()), asio::ssl::context::pem);
            serverCtx.use_private_key(
                asio::buffer(pem.key.data(), pem.key.size()), asio::ssl::context::pem);
            SSL_CTX_set_alpn_select_cb(serverCtx.native_handle(), &serverAlpnSelect, nullptr);

            asio::ssl::stream<tcp::socket&> tls(sock, serverCtx);
            auto [hsEc] = co_await tls.async_handshake(
                asio::ssl::stream_base::server, asio::as_tuple(asio::use_awaitable));
            if (hsEc) {
                co_return;
            }
            alpnWasH2 = ruvia::detail::isHttp2AlpnSelected(tls);

            ruvia::WorkerMemory worker;
            ruvia::Router router;
            auto& impl = ruvia::detail::RouterImpl::from(router);
            impl.registerRoute(
                ruvia::HttpKnownMethod::kGet,
                std::pmr::string("/ping", std::pmr::get_default_resource()),
                ruvia::detail::RouteHandler(
                    &connectionObservation,
                    &tlsPongHandler),
                ruvia::detail::RequestBodyMode::kBuffered,
                std::span<const ruvia::detail::ControllerMiddlewareDescriptor>{},
                std::span<const ruvia::detail::ControllerMiddlewareDescriptor>{});
            impl.finalize();
            co_await ruvia::detail::taskAsAwaitable(ruvia::test::runBareHttp2SansIoSession(
                tls,
                impl.routeTable(),
                worker,
                ruvia::detail::ContextServices{}.withTlsTransport(
                    "127.0.0.1")));
        },
        asio::detached);

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            tcp::socket sock(io);
            co_await sock.async_connect(
                tcp::endpoint(asio::ip::make_address("127.0.0.1"), port), asio::use_awaitable);
            asio::ssl::context clientCtx(asio::ssl::context::tls_client);
            asio::ssl::stream<tcp::socket&> tls(sock, clientCtx);
            static const unsigned char kAlpnH2[] = {2, 'h', '2'};
            SSL_set_alpn_protos(tls.native_handle(), kAlpnH2, sizeof(kAlpnH2));
            auto [hsEc] = co_await tls.async_handshake(
                asio::ssl::stream_base::client, asio::as_tuple(asio::use_awaitable));
            if (hsEc) {
                co_return;
            }

            auto writeAll = [&tls](std::string_view bytes) -> asio::awaitable<bool> {
                auto [ec, n] = co_await asio::async_write(
                    tls, asio::buffer(bytes.data(), bytes.size()), asio::as_tuple(asio::use_awaitable));
                (void)n;
                co_return !ec;
            };
            auto readExact = [&tls](void* data, std::size_t size) -> asio::awaitable<bool> {
                auto [ec, n] = co_await asio::async_read(
                    tls, asio::buffer(data, size), asio::as_tuple(asio::use_awaitable));
                co_return !ec && n == size;
            };

            if (!co_await writeAll(kClientPreface)) co_return;
            if (!co_await writeAll(frame(0x4, 0, 0, {}))) co_return;
            std::pmr::string headerBlock(std::pmr::get_default_resource());
            HpackEncoder::encodeHeader(headerBlock, ":method", "GET");
            HpackEncoder::encodeHeader(headerBlock, ":path", "/ping");
            HpackEncoder::encodeHeader(headerBlock, ":scheme", "https");
            HpackEncoder::encodeHeader(headerBlock, ":authority", "localhost");
            if (!co_await writeAll(frame(
                    0x1, ruvia::detail::kHttp2FlagEndStream | ruvia::detail::kHttp2FlagEndHeaders,
                    1, std::string_view(headerBlock.data(), headerBlock.size())))) {
                co_return;
            }

            for (;;) {
                char hb[ruvia::detail::kHttp2FrameHeaderBytes];
                if (!co_await readExact(hb, sizeof(hb))) break;
                const auto header = ruvia::detail::http2ParseFrameHeader(
                    std::string_view(hb, sizeof(hb)));
                std::string payload(header.length, '\0');
                if (header.length != 0 && !co_await readExact(payload.data(), payload.size())) break;
                if (header.type == static_cast<std::uint8_t>(Http2FrameType::kData) &&
                    header.streamId == 1 && !payload.empty()) {
                    body = payload;
                    break;
                }
            }
            std::error_code ignore;
            sock.shutdown(tcp::socket::shutdown_both, ignore);
        },
        asio::detached);

    io.run();
    RUVIA_CHECK(alpnWasH2);           // the production selector saw h2
    RUVIA_CHECK(body == "tls-pong");  // and the session answered over TLS
    RUVIA_CHECK(!connectionObservation.sawPlain);
    RUVIA_CHECK(connectionObservation.sawTls);
    RUVIA_CHECK(connectionObservation.clientCertificateEmpty);
    RUVIA_CHECK(
        connectionObservation.remoteAddress == std::string_view("127.0.0.1"));
}

// Optional mutual TLS verifies a presented certificate but admits a client that
// presents none; required mode adds fail-if-no-peer-cert so a missing
// certificate fails the handshake (mandatory mutual TLS).
RUVIA_TEST(http_server_tls_verify_mode_optional_vs_mandatory) {
    const auto optional = ruvia::detail::httpServerTlsVerifyMode(
        ruvia::TlsClientCertificateRequirement::kOptional);
    RUVIA_CHECK(optional == asio::ssl::verify_peer);
    RUVIA_CHECK((optional & asio::ssl::verify_fail_if_no_peer_cert) == 0);

    const auto mandatory = ruvia::detail::httpServerTlsVerifyMode(
        ruvia::TlsClientCertificateRequirement::kRequired);
    RUVIA_CHECK((mandatory & asio::ssl::verify_peer) != 0);
    RUVIA_CHECK((mandatory & asio::ssl::verify_fail_if_no_peer_cert) != 0);
}
