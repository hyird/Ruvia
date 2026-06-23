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

#include "HttpServerSessionEntry.inl"
#include "HttpServerStreamSession.inl"

}  // namespace ruvia::detail
