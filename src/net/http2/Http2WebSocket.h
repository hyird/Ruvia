#pragma once

#include <cstddef>
#include <string_view>
#include <system_error>

#include "../ws/HttpWebSocketConnection.h"
#include "Http2StreamState.h"
#include "ruvia/app/Task.h"

namespace ruvia::detail {

// HTTP/2 WebSocket transport (RFC 8441 Extended CONNECT): inbound frames arrive
// as DATA chunks and outbound frames are written as DATA frames through the
// owning Http2ServerSession, so flow control and framing are handled there.
template <typename Session>
class Http2WebSocketTransport final {
public:
    Http2WebSocketTransport(Session& session, Http2StreamState& stream) noexcept
        : session_(session), stream_(stream) {}

    [[nodiscard]] auto executor() const noexcept {
        return session_.socket_.get_executor();
    }

    [[nodiscard]] Task<bool> readMore(std::pmr::string& buffer) {
        auto chunk = co_await session_.readBodyChunk(stream_.id());
        if (!chunk) {
            co_return false;
        }
        buffer.append(chunk->data(), chunk->size());
        co_return true;
    }

    [[nodiscard]] Task<std::error_code> writeFrame(
        std::string_view header,
        std::string_view payload,
        bool endStream) {
        co_await session_.writeData(stream_, header, payload, endStream);
        co_return std::error_code{};
    }

private:
    Session& session_;
    Http2StreamState& stream_;
};

template <typename Session>
using Http2WebSocketConnection = WebSocketConnection<Http2WebSocketTransport<Session>>;

}  // namespace ruvia::detail
