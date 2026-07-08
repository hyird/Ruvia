#pragma once

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>

#include <asio.hpp>

#include "net/server/ConnectionScanner.h"
#include "HttpWebSocketPermessageDeflate.h"
#include "HttpWebSocketUtils.h"
#include "runtime/AsioAwait.h"
#include "ruvia/http/HttpLimits.h"
#include "ruvia/app/Task.h"
#include "ruvia/http/WebSocket.h"
#include "ruvia/http/detail/PmrString.h"
#include "ruvia/memory/PmrResource.h"

namespace ruvia::detail {

// Transport-agnostic WebSocket connection (RFC 6455). All protocol behavior,
// including frame reassembly, write serialization, heartbeats, and close,
// lives here; the HTTP/1.1 and HTTP/2 transports differ only in the Transport
// policy, which supplies the three transport-specific operations:
//   asio-executor executor() const;
//   Task<bool> readMore(std::pmr::string& buffer);  // append >=1 byte, false on EOF
//   Task<std::error_code> writeFrame(std::string_view header,
//                                    std::string_view payload, bool endStream);
template <typename Transport>
class WebSocketConnection final {
public:
    WebSocketConnection(
        Transport transport,
        ConnectionScanner::Entry& scannerEntry,
        WebSocketHeartbeatOptions heartbeatOptions,
        std::size_t maxMessageBytes,
        std::pmr::memory_resource* resource,
        std::string_view initialBytes = {},
        bool permessageDeflate = false)
        : transport_(std::move(transport)),
          scannerEntry_(scannerEntry),
          heartbeatOptions_(heartbeatOptions),
          maxMessageBytes_(maxMessageBytes),
          buffer_(pmrResourceOrDefault(resource)),
          inbound_(buffer_.get_allocator().resource()),
          outboundDeflated_(buffer_.get_allocator().resource()),
          inboundInflated_(buffer_.get_allocator().resource()),
          permessageDeflate_(permessageDeflate),
          backgroundWriteTimer_(transport_.executor()) {
        backgroundWriteTimer_.expires_at((asio::steady_timer::time_point::max)());
        buffer_.append(initialBytes.data(), initialBytes.size());
        if (permessageDeflate_) {
            deflate_.emplace();
        }
        scannerEntry_.setWebSocketHeartbeat(this, &WebSocketConnection::heartbeatTickThunk);
    }

    ~WebSocketConnection() {
        scannerEntry_.clearWebSocketHeartbeat(this);
    }

    WebSocketConnection(const WebSocketConnection&) = delete;
    WebSocketConnection& operator=(const WebSocketConnection&) = delete;

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
    Task<void> writeHeartbeatPing();
    Task<void> waitForHeartbeatWrite();
    Task<void> writeExclusive(WebSocketOpcode opcode, std::string_view payload, bool endStream);
    Task<void> writeFrameNow(WebSocketOpcode opcode, std::string_view payload, bool endStream);
    [[nodiscard]] Task<bool> ensure(std::size_t bytes);
    [[nodiscard]] Task<std::optional<WebSocketFrameView>> readFrame();

    Transport transport_;
    ConnectionScanner::Entry& scannerEntry_;
    WebSocketHeartbeatOptions heartbeatOptions_{};
    std::size_t maxMessageBytes_{kDefaultMaxWebSocketMessageBytes};
    std::pmr::string buffer_;
    WebSocketInboundAssembler inbound_;
    std::pmr::string outboundDeflated_;
    std::pmr::string inboundInflated_;
    std::optional<WebSocketDeflate> deflate_;
    bool permessageDeflate_{false};
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
