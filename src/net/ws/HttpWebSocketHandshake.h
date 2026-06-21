#pragma once

#include <array>
#include <asio/write.hpp>
#include <string_view>

#include "HttpWebSocketUtils.h"
#include "../../runtime/AsioAwait.h"
#include "ruvia/app/Task.h"
#include "ruvia/http/HttpParser.h"
#include "ruvia/http/HttpTypes.h"

namespace ruvia::detail {

template <typename Stream>
Task<bool> writeWebSocketHandshake(
    Stream& stream,
    const HttpRequest& request,
    const HttpRequestFlags& flags,
    std::string_view supportedSubprotocols) {
    const auto subprotocol = chooseWebSocketSubprotocol(request, flags, supportedSubprotocols);
    static constexpr std::string_view kPrefix =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: ";
    static constexpr std::string_view kSubprotocolPrefix = "Sec-WebSocket-Protocol: ";
    static constexpr std::string_view kCrlf = "\r\n";

    WebSocketAcceptKey accept;
    encodeWebSocketAccept(accept, request.header(HttpRequest::KnownHeader::kSecWebSocketKey));
    if (subprotocol.empty()) {
        const std::array<asio::const_buffer, 3> buffers{
            asio::buffer(kPrefix),
            asio::buffer(accept),
            asio::buffer("\r\n\r\n", 4)};
        const auto ec = co_await asyncError([&stream, &buffers](auto handler) mutable {
            asio::async_write(stream, buffers, std::move(handler));
        });
        co_return !ec;
    }

    const std::array<asio::const_buffer, 6> buffers{
        asio::buffer(kPrefix),
        asio::buffer(accept),
        asio::buffer(kCrlf),
        asio::buffer(kSubprotocolPrefix),
        asio::buffer(subprotocol),
        asio::buffer("\r\n\r\n", 4)};
    const auto ec = co_await asyncError([&stream, &buffers](auto handler) mutable {
        asio::async_write(stream, buffers, std::move(handler));
    });
    co_return !ec;
}

}  // namespace ruvia::detail
