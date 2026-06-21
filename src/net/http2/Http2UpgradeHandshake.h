#pragma once

#include <array>
#include <string_view>

#include <asio/write.hpp>

#include "../server/HttpDateCache.h"
#include "../../runtime/AsioAwait.h"
#include "ruvia/app/Task.h"

namespace ruvia::detail {

template <typename Stream>
Task<bool> writeHttp2UpgradeHandshake(Stream& stream) {
    static constexpr std::string_view kPrefix =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Connection: Upgrade\r\n"
        "Upgrade: h2c\r\n"
        "Server: ruvia\r\n";

    const auto dateHeader = cachedDateHeader();
    const std::array<asio::const_buffer, 3> buffers{
        asio::buffer(kPrefix),
        asio::buffer(dateHeader),
        asio::buffer("\r\n", 2)};
    const auto ec = co_await asyncError([&stream, &buffers](auto handler) mutable {
        asio::async_write(stream, buffers, std::move(handler));
    });
    co_return !ec;
}

}  // namespace ruvia::detail
