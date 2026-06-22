#pragma once

#include <array>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>

#include <asio.hpp>

#include "../server/ConnectionScanner.h"
#include "HttpWebSocketUtils.h"
#include "../../runtime/AsioAwait.h"
#include "ruvia/app/Task.h"
#include "ruvia/http/WebSocket.h"
#include "ruvia/http/detail/PmrString.h"
#include "ruvia/memory/MemoryPool.h"

namespace ruvia::detail {

template <typename Stream>
class WebSocketConnection final {
public:
    WebSocketConnection(
        Stream& stream,
        std::pmr::memory_resource* resource,
        ConnectionScanner::Entry& scannerEntry,
        WebSocketHeartbeatOptions heartbeatOptions,
        std::size_t maxMessageBytes,
        std::string_view initialBytes)
        : stream_(stream),
          scannerEntry_(scannerEntry),
          heartbeatOptions_(heartbeatOptions),
          maxMessageBytes_(maxMessageBytes),
          buffer_(resource == nullptr ? ProcessMemory::instance().upstreamResource() : resource),
          inbound_(buffer_.get_allocator().resource()),
          backgroundWriteTimer_(stream.get_executor()) {
        backgroundWriteTimer_.expires_at((asio::steady_timer::time_point::max)());
        buffer_.append(initialBytes.data(), initialBytes.size());
        scannerEntry_.webSocketTarget = this;
        scannerEntry_.webSocketTick = &WebSocketConnection::heartbeatTickThunk;
    }

    ~WebSocketConnection() {
        if (scannerEntry_.webSocketTarget == this) {
            scannerEntry_.webSocketTarget = nullptr;
            scannerEntry_.webSocketTick = nullptr;
        }
    }

    [[nodiscard]] static Task<std::optional<WebSocketMessage>> readThunk(void* target) {
        return static_cast<WebSocketConnection*>(target)->read();
    }

    static Task<void> writeThunk(void* target, WebSocketOpcode opcode, std::string_view payload) {
        return static_cast<WebSocketConnection*>(target)->write(opcode, payload);
    }

    static Task<void> closeThunk(void* target, std::uint16_t code, std::string_view reason) {
        return static_cast<WebSocketConnection*>(target)->close(code, reason);
    }

    static bool heartbeatTickThunk(void* target, std::int64_t now) noexcept {
        return static_cast<WebSocketConnection*>(target)->heartbeatTick(now);
    }

    [[nodiscard]] Task<std::optional<WebSocketMessage>> read();
    Task<void> write(WebSocketOpcode opcode, std::string_view payload);
    Task<void> close(std::uint16_t code, std::string_view reason);
    Task<void> detachAndDrainBackgroundWrites();

private:
    void completeBackgroundWrite() noexcept;
    bool heartbeatTick(std::int64_t now) noexcept;
    Task<void> writeFrameNow(WebSocketOpcode opcode, std::string_view payload);
    [[nodiscard]] Task<bool> ensure(std::size_t bytes);
    [[nodiscard]] Task<std::optional<WebSocketFrameView>> readFrame();

    Stream& stream_;
    ConnectionScanner::Entry& scannerEntry_;
    WebSocketHeartbeatOptions heartbeatOptions_{};
    std::size_t maxMessageBytes_{kDefaultMaxWebSocketMessageBytes};
    std::pmr::string buffer_;
    WebSocketInboundAssembler inbound_;
    asio::steady_timer backgroundWriteTimer_;
    std::size_t offset_{0};
    std::size_t pendingCompactUntil_{0};
    std::size_t backgroundWriteCount_{0};
    bool writeActive_{false};
    bool heartbeatWriteActive_{false};
    bool closeSent_{false};
    bool awaitingPong_{false};
    std::int64_t heartbeatPingSentMs_{0};
};

}  // namespace ruvia::detail

#include "HttpWebSocketConnectionHeartbeat.inl"
#include "HttpWebSocketConnectionRead.inl"
#include "HttpWebSocketConnectionWrite.inl"
