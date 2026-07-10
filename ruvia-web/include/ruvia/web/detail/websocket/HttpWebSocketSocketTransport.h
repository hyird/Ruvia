#pragma once

#include <cstddef>
#include <string_view>
#include <system_error>

#include <asio.hpp>

#include "ruvia/web/detail/websocket/HttpWebSocketConnection.h"
#include "ruvia/core/detail/AsioAwait.h"
#include "ruvia/core/Task.h"
#include "ruvia/http/detail/PmrString.h"

namespace ruvia::detail {

// HTTP/1.1 WebSocket transport: reads and writes the framed bytes directly on
// the upgraded byte stream (plain TCP socket or TLS stream).
template <typename Stream>
class WebSocketSocketTransport final {
public:
    explicit WebSocketSocketTransport(Stream& stream) noexcept : stream_(stream) {}

    [[nodiscard]] auto executor() const noexcept {
        return stream_.get_executor();
    }

    [[nodiscard]] Task<bool> readMore(std::pmr::string& buffer) {
        const auto oldSize = buffer.size();
        resizePmrStringForOverwrite(buffer, oldSize + 4096);
        const auto [ec, bytesRead] = co_await asyncResult<std::size_t>(
            [this, oldSize, &buffer](auto handler) mutable {
                stream_.async_read_some(
                    asio::buffer(buffer.data() + oldSize, buffer.size() - oldSize),
                    std::move(handler));
            });
        if (ec || bytesRead == 0) {
            buffer.resize(oldSize);
            co_return false;
        }
        buffer.resize(oldSize + bytesRead);
        co_return true;
    }

    [[nodiscard]] Task<std::error_code> writeBytes(
        std::string_view bytes,
        bool /*endStream*/) {
        const auto buffer = asio::buffer(bytes.data(), bytes.size());
        co_return co_await asyncError([this, buffer](auto handler) mutable {
            asio::async_write(stream_, buffer, std::move(handler));
        });
    }

private:
    Stream& stream_;
};

template <typename Stream>
using SocketWebSocketConnection = WebSocketConnection<WebSocketSocketTransport<Stream>>;

}  // namespace ruvia::detail
