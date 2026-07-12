#include "ruvia/web/detail/server/HttpServer.h"

#include <asio/ssl.hpp>
#include <stdexcept>
#include <type_traits>

#include "ruvia/core/detail/ConnectionScanner.h"
#include "ruvia/http/detail/HttpRequestInternal.h"
#include "ruvia/web/detail/server/Http1SessionRequestCompletion.h"
#include "ruvia/web/detail/server/HttpBufferedResponse.h"
#include "ruvia/web/detail/server/HttpConnectionState.h"
#include "ruvia/web/detail/server/HttpResponseWriter.h"
#include "ruvia/web/detail/server/HttpServerAccessLog.h"
#include "ruvia/web/detail/server/HttpServerBufferedRoute.h"
#include "ruvia/web/detail/server/HttpServerSessionUtils.h"
#include "ruvia/http/detail/http1/Http1ServerRequestParser.h"
#include "ruvia/web/Error.h"
#include "ruvia/http/detail/PmrString.h"
#include "ruvia/web/detail/router/RouteTable.h"
#include "ruvia/core/detail/AsioAwait.h"

namespace ruvia::detail {

using TcpSocket = asio::ip::tcp::socket;

// Extracts the verified peer (client) certificate subject DN into `out`, or
// leaves it empty when no client certificate was presented. Used to surface
// mutual-TLS identity to handlers via getConnInfo(context).
inline void extractTlsClientCertificate(SSL* ssl, std::pmr::string& out) {
    out.clear();
    X509* certificate = SSL_get_peer_certificate(ssl);
    if (certificate == nullptr) {
        return;
    }
    char buffer[256];
    X509_NAME* subject = X509_get_subject_name(certificate);
    if (subject != nullptr && X509_NAME_oneline(subject, buffer, sizeof(buffer)) != nullptr) {
        out.assign(buffer);
    }
    X509_free(certificate);
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
