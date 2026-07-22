#pragma once

#include <cstddef>
#include <string_view>
#include <system_error>

#include <asio.hpp>

#include "ruvia/web/detail/websocket/HttpWebSocketConnection.h"
#include "ruvia/core/detail/AsioAwait.h"
#include "ruvia/core/detail/SocketUtils.h"
#include "ruvia/core/Task.h"
#include "ruvia/http/detail/util/PmrString.h"

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

    [[nodiscard]] Task<WsTransportReadResult> readMore(std::pmr::string& buffer) {
        const auto oldSize = buffer.size();
        resizePmrStringForOverwrite(buffer, oldSize + 4096);
        auto readCompletion = co_await asyncAsio<std::size_t>(
            [this, oldSize, &buffer](auto handler) mutable {
                stream_.async_read_some(
                    asio::buffer(buffer.data() + oldSize, buffer.size() - oldSize),
                    std::move(handler));
            });
        const auto ec = readCompletion.errorCode();
        const auto bytesRead = readCompletion.result();
        if (ec) {
            buffer.resize(oldSize);
            co_return WsTransportReadResult::makeFailure(ec);
        }
        if (bytesRead == 0) {
            buffer.resize(oldSize);
            co_return WsTransportReadResult::makeEnd();
        }
        buffer.resize(oldSize + bytesRead);
        co_return WsTransportReadResult::makeData();
    }

    [[nodiscard]] Task<std::error_code> writeBytes(
        std::string_view bytes,
        WsTransportDisposition /*disposition*/) {
        if (bytes.empty()) {
            co_return std::error_code{};
        }
        const auto buffer = asio::buffer(bytes.data(), bytes.size());
        const auto writeCompletion = co_await asyncAsio(
            [this, buffer](auto handler) mutable {
                asio::async_write(stream_, buffer, std::move(handler));
            });
        co_return writeCompletion.errorCode();
    }

    void abort() noexcept {
        if constexpr (requires(Stream& value) { value.next_layer(); }) {
            closeSocket(stream_.next_layer());
        } else {
            closeSocket(stream_);
        }
    }

private:
    Stream& stream_;
};

template <typename Stream>
using SocketWebSocketConnection = WebSocketConnection<WebSocketSocketTransport<Stream>>;

}  // namespace ruvia::detail
