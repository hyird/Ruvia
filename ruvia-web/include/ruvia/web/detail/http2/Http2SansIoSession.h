#pragma once

#include <string_view>

#include <asio/ip/tcp.hpp>
#include <asio/ssl/stream.hpp>

#include "ruvia/core/Task.h"
#include "ruvia/web/detail/http2/Http2SansIoSessionContext.h"

namespace ruvia {
class WorkerMemory;
}

namespace ruvia::detail {

class RouteTable;

// HTTP/2 supports exactly the two transports owned by the Web server. Keeping
// these concrete overloads out of headers prevents every server/test consumer
// from instantiating the complete session coroutine again.
[[nodiscard]] Task<void> runHttp2SansIoSession(asio::ip::tcp::socket& stream, const RouteTable& routes, WorkerMemory& worker, Http2SansIoSessionContext session, std::string_view initialBytes = {});

[[nodiscard]] Task<void> runHttp2SansIoSession(asio::ssl::stream<asio::ip::tcp::socket&>& stream, const RouteTable& routes, WorkerMemory& worker, Http2SansIoSessionContext session, std::string_view initialBytes = {});

}  // namespace ruvia::detail
