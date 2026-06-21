#include "HttpServer.h"

#include <asio/ssl.hpp>
#include <type_traits>

#include "ConnectionScanner.h"
#include "HttpBufferedResponse.h"
#include "HttpConnectionState.h"
#include "HttpResponseWriter.h"
#include "HttpServerBufferedRoute.h"
#include "HttpServerSessionUtils.h"
#include "ruvia/http/Error.h"
#include "ruvia/http/HttpParser.h"
#include "../../router/RouteTable.h"
#include "../../runtime/AsioAwait.h"

namespace ruvia::detail {

using TcpSocket = asio::ip::tcp::socket;

#include "HttpServerSessionEntry.inl"
#include "HttpServerStreamSession.inl"

}  // namespace ruvia::detail
