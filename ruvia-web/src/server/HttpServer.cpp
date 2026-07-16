#include "ruvia/web/detail/server/HttpServer.h"

#include <asio/ssl.hpp>
#include <array>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <type_traits>

#include "ruvia/core/detail/ConnectionScanner.h"
#include "ruvia/http/detail/HttpRequestInternal.h"
#include "ruvia/web/detail/http/HttpProtocolErrorInfo.h"
#include "ruvia/web/detail/server/Http1SessionRequestCompletion.h"
#include "ruvia/web/detail/server/Http1ClosingRejection.h"
#include "ruvia/web/detail/server/HttpBufferedResponse.h"
#include "ruvia/web/detail/server/HttpConnectionState.h"
#include "ruvia/web/detail/server/HttpResponseWriter.h"
#include "ruvia/web/detail/server/HttpServerAccessLog.h"
#include "ruvia/web/detail/server/HttpServerAlpn.h"
#include "ruvia/web/detail/server/HttpServerAutoHttps.h"
#include "ruvia/web/detail/server/HttpServerBodyRouteCompletion.h"
#include "ruvia/web/detail/server/HttpServerCleartextHttp2.h"
#include "ruvia/web/detail/server/HttpServerConnectionGuards.h"
#include "ruvia/web/detail/server/HttpServerIdleWorkSet.h"
#include "ruvia/web/detail/server/HttpServerRequestState.h"
#include "ruvia/web/detail/server/HttpServerResponseState.h"
#include "ruvia/web/detail/server/HttpServerResponseStreamRoute.h"
#include "ruvia/web/detail/server/HttpServerStreamBodyRoute.h"
#include "ruvia/web/detail/server/HttpServerWebSocketRoute.h"
#include "ruvia/http/detail/http1/Http1ServerRequestParser.h"
#include "ruvia/web/Error.h"
#include "ruvia/http/detail/PmrString.h"
#include "ruvia/web/detail/router/RouteTable.h"
#include "ruvia/core/detail/AsioAwait.h"
#include "ruvia/core/detail/SocketUtils.h"

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
    char buffer[256];
    X509_NAME* subject = X509_get_subject_name(certificate.get());
    if (subject != nullptr && X509_NAME_oneline(subject, buffer, sizeof(buffer)) != nullptr) {
        out.assign(buffer);
    }
}

struct TlsServerHandshakeInitiator final {
    asio::ssl::stream<TcpSocket&>* stream;

    template <typename Handler>
    void operator()(Handler handler) const {
        stream->async_handshake(asio::ssl::stream_base::server, std::move(handler));
    }
};

#include "ruvia/web/detail/server/HttpServerSessionEntry.inl"
#include "ruvia/web/detail/server/HttpServerStreamSession.inl"

}  // namespace ruvia::detail
