#pragma once

#include <array>
#include <span>
#include <string_view>

#include <asio/write.hpp>

#include "ruvia/http/detail/http2/Http2Upgrade.h"
#include "ruvia/core/detail/AsioAwait.h"
#include "ruvia/core/Task.h"

namespace ruvia::detail {

template <typename Stream>
Task<bool> writeHttp2UpgradeHandshake(Stream& stream) {
    std::array<asio::const_buffer, 3> buffers;
    std::size_t count = 0;
    forEachHttp2UpgradeResponsePart([&buffers, &count](std::string_view part) {
        buffers[count++] = asio::buffer(part);
    });
    const auto activeBuffers = std::span<const asio::const_buffer>(buffers.data(), count);
    const auto ec = co_await asyncError([&stream, activeBuffers](auto handler) mutable {
        asio::async_write(stream, activeBuffers, std::move(handler));
    });
    co_return !ec;
}

}  // namespace ruvia::detail
