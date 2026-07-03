#include "HttpServer.h"

#include <asio/ssl.hpp>
#include <type_traits>

#include "ConnectionScanner.h"
#include "../../http/HttpRequestInternal.h"
#include "HttpBufferedResponse.h"
#include "HttpConnectionState.h"
#include "HttpResponseWriter.h"
#include "HttpServerAccessLog.h"
#include "HttpServerBufferedRoute.h"
#include "HttpServerDynamicRoute.h"
#include "HttpServerSessionUtils.h"
#include "../../http/HttpParserInternal.h"
#include "ruvia/http/Error.h"
#include "ruvia/http/detail/PmrString.h"
#include "../../router/RouteTable.h"
#include "../../runtime/AsioAwait.h"

namespace ruvia::detail {

using TcpSocket = asio::ip::tcp::socket;

// Extracts the verified peer (client) certificate subject DN into `out`, or
// leaves it empty when no client certificate was presented. Used to surface
// mutual-TLS identity to handlers via HttpRequest::clientCertificate().
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

#include "HttpServerSessionEntry.inl"
#include "HttpServerStreamSession.inl"

}  // namespace ruvia::detail
