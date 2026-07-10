#pragma once

#include <array>
#include <string_view>

#include <asio/write.hpp>

#include "ruvia/http/detail/server/HttpDateCache.h"
#include "ruvia/http/detail/http2/Http2Upgrade.h"
#include "ruvia/core/detail/AsioAwait.h"
#include "ruvia/core/Task.h"

namespace ruvia::detail {

template <typename Stream>
Task<bool> writeHttp2UpgradeHandshake(Stream& stream) {
    const auto dateHeader = cachedDateHeader();
    const std::array<asio::const_buffer, 3> buffers{
        asio::buffer(kHttp2UpgradeResponsePrefix),
        asio::buffer(dateHeader),
        asio::buffer("\r\n", 2)};
    const auto ec = co_await asyncError([&stream, &buffers](auto handler) mutable {
        asio::async_write(stream, buffers, std::move(handler));
    });
    co_return !ec;
}

}  // namespace ruvia::detail
