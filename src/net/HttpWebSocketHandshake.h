#pragma once

#include <asio/write.hpp>
#include <memory_resource>
#include <string>
#include <string_view>

#include "HttpWebSocketUtils.h"
#include "../AsioAwait.h"
#include "ruvia/app/Task.h"
#include "ruvia/http/HttpParser.h"
#include "ruvia/http/HttpTypes.h"

namespace ruvia::detail {

template <typename Stream>
Task<bool> writeWebSocketHandshake(
    Stream& stream,
    const HttpRequest& request,
    const HttpRequestFlags& flags,
    std::string_view supportedSubprotocols,
    std::pmr::memory_resource* resource) {
    auto accept = webSocketAccept(request.header(HttpRequest::KnownHeader::kSecWebSocketKey), resource);
    const auto subprotocol = chooseWebSocketSubprotocol(request, flags, supportedSubprotocols);
    std::pmr::string response(resource);
    response.append("HTTP/1.1 101 Switching Protocols\r\n");
    response.append("Upgrade: websocket\r\n");
    response.append("Connection: Upgrade\r\n");
    response.append("Sec-WebSocket-Accept: ");
    response.append(accept);
    response.append("\r\n");
    if (!subprotocol.empty()) {
        response.append("Sec-WebSocket-Protocol: ");
        response.append(subprotocol);
        response.append("\r\n");
    }
    response.append("\r\n");

    const auto ec = co_await asyncError([&stream, view = std::string_view(response)](auto handler) mutable {
        asio::async_write(stream, asio::buffer(view), std::move(handler));
    });
    co_return !ec;
}

}  // namespace ruvia::detail
