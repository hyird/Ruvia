#include "ruvia/web/detail/server/HttpServer.h"
#include "ruvia/web/detail/server/request/RequestMemoryArena.h"
#include "ruvia/web/detail/server/request/HttpServerRequestState.h"
#include "ruvia/web/detail/server/tls/HttpServerAlpn.h"

#include <asio/ssl.hpp>
#include <openssl/bio.h>
#include <openssl/x509.h>
#include <array>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include "ruvia/http/detail/request/HttpRequestAccess.h"
#include "ruvia/web/detail/server/http1/Http1SessionRequestCompletion.h"
#include "ruvia/web/detail/server/http1/Http1ClosingRejection.h"
#include "ruvia/web/detail/server/response/HttpBufferedResponse.h"
#include "ruvia/web/detail/server/response/HttpResponseWriter.h"
#include "ruvia/web/detail/server/HttpServerAccessLog.h"
#include "ruvia/web/detail/server/tls/HttpServerAutoHttps.h"
#include "ruvia/web/detail/server/route/HttpServerBodyRouteCompletion.h"
#include "ruvia/web/detail/http2/CleartextUpgrade.h"
#include "ruvia/web/detail/server/session/HttpServerConnectionGuards.h"
#include "ruvia/web/detail/server/session/HttpServerIdleWorkSet.h"
#include "ruvia/web/detail/server/response/HttpServerResponseState.h"
#include "ruvia/web/detail/server/stream/HttpServerResponseStreamRoute.h"
#include "ruvia/web/detail/server/route/HttpServerStreamBodyRoute.h"
#include "ruvia/web/detail/server/route/HttpServerWebSocketRoute.h"
#include "ruvia/http/detail/http1/Http1ServerRequestParser.h"
#include "ruvia/http/detail/util/PmrString.h"
#include "ruvia/core/detail/io/AsioAwait.h"
#include "ruvia/core/detail/io/SocketUtils.h"

namespace ruvia::detail {

using TcpSocket = asio::ip::tcp::socket;

// Extracts the verified peer (client) certificate subject DN into `out`, or
// leaves it empty when no client certificate was presented. Used to surface
// mutual-TLS identity to handlers via getConnInfo(context).
inline void extractTlsClientCertificate(SSL* ssl, std::pmr::string& out) {
    out.clear();
    const auto certificate = std::unique_ptr<X509, decltype(&X509_free)>(
        SSL_get_peer_certificate(ssl),
        &X509_free);
    if (certificate == nullptr) {
        return;
    }
    X509_NAME* subject = X509_get_subject_name(certificate.get());
    if (subject == nullptr) {
        return;
    }
    // Render the DN in RFC 2253 form through a memory BIO. Unlike
    // X509_NAME_oneline into a fixed 256-byte buffer, this captures the full
    // subject without silent truncation and produces the unambiguous,
    // standard-parseable form recommended for authorization.
    const auto bio = std::unique_ptr<BIO, decltype(&BIO_free)>(
        BIO_new(BIO_s_mem()), &BIO_free);
    if (bio == nullptr) {
        return;
    }
    if (X509_NAME_print_ex(bio.get(), subject, 0, XN_FLAG_RFC2253) < 0) {
        return;
    }
    char* data = nullptr;
    const long length = BIO_get_mem_data(bio.get(), &data);
    if (data != nullptr && length > 0) {
        out.assign(data, static_cast<std::size_t>(length));
    }
}

struct TlsServerHandshakeInitiator final {
    asio::ssl::stream<TcpSocket&>* stream;

    template <typename Handler>
    void operator()(Handler handler) const {
        stream->async_handshake(asio::ssl::stream_base::server, std::move(handler));
    }
};

#include "ruvia/web/detail/server/session/HttpServerSessionEntry.inl"
#include "ruvia/web/detail/server/session/HttpServerStreamSession.inl"

}  // namespace ruvia::detail
