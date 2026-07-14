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

#include "ruvia/core/detail/ConnectionScanner.h"
#include "ruvia/http/detail/websocket/HttpWebSocketPermessageDeflate.h"
#include "ruvia/http/detail/websocket/HttpWebSocketUtils.h"
#include "ruvia/http/detail/websocket/WsConnection.h"
#include "ruvia/web/detail/websocket/HttpWebSocketLiveness.h"
#include "ruvia/core/detail/AsioAwait.h"
#include "ruvia/core/detail/WorkerSignal.h"
#include "ruvia/http/HttpLimits.h"
#include "ruvia/core/Task.h"
#include "ruvia/web/WebSocket.h"
#include "ruvia/http/detail/PmrString.h"
#include "ruvia/core/memory/PmrResource.h"

namespace ruvia::detail {

// Transport-agnostic WebSocket connection (RFC 6455). All protocol behavior,
// including frame reassembly, write serialization, heartbeats, and close,
// lives here; the HTTP/1.1 and HTTP/2 transports differ only in the Transport
// policy, which supplies four transport-specific operations:
//   asio-executor executor() const;
//   Task<bool> readMore(std::pmr::string& buffer);  // append >=1 byte, false on EOF
//   Task<std::error_code> writeBytes(std::string_view, WsTransportDisposition);
//   void abort() noexcept;  // abort this WebSocket transport, not an unrelated h2 stream
template <typename Transport>
class WebSocketConnection final {
public:
    WebSocketConnection(
        Transport transport,
        ConnectionScanner::Entry& scannerEntry,
        WebSocketLifecycleOptions lifecycleOptions,
        ProtocolByteLimit messageLimit,
        std::pmr::memory_resource* resource,
        std::string_view initialBytes = {},
        WebSocketDeflateNegotiation deflate =
            WebSocketDeflateNegotiation::kDisabled)
        : transport_(std::move(transport)),
          scannerEntry_(scannerEntry),
          lifecycleOptions_(lifecycleOptions),
          buffer_(pmrResourceOrDefault(resource)),
          protocol_(buffer_, messageLimit, deflate),
          backgroundWriteSignal_(transport_.executor()),
          readerDoneSignal_(transport_.executor()) {
        buffer_.append(initialBytes.data(), initialBytes.size());
        scannerEntry_.registerPeriodicCheck(
            periodicCheck_,
            this,
            &WebSocketConnection::heartbeatTickThunk);
    }

    ~WebSocketConnection() = default;

    WebSocketConnection(const WebSocketConnection&) = delete;
    WebSocketConnection& operator=(const WebSocketConnection&) = delete;

    static bool heartbeatTickThunk(void* target, std::int64_t now) noexcept {
        return static_cast<WebSocketConnection*>(target)->heartbeatTick(now);
    }

    [[nodiscard]] Task<std::optional<WebSocketMessage>> read();
    Task<void> write(WebSocketOpcode opcode, std::string_view payload);
    Task<void> close(std::uint16_t code, std::string_view reason);
    void abort() noexcept { abortTransport(); }
    Task<void> detachAndDrainBackgroundWrites();

private:
    class ReadGuard final {
    public:
        explicit ReadGuard(WebSocketConnection& connection) : connection_(connection) {
            if (connection_.readActive_) {
                throw std::logic_error("concurrent websocket reads are not supported");
            }
            connection_.readActive_ = true;
        }

        ~ReadGuard() {
            connection_.readActive_ = false;
            connection_.readerDoneSignal_.notify();
        }

        ReadGuard(const ReadGuard&) = delete;
        ReadGuard& operator=(const ReadGuard&) = delete;

    private:
        WebSocketConnection& connection_;
    };

    void completeBackgroundWrite() noexcept;
    bool heartbeatTick(std::int64_t now) noexcept;
    Task<void> writeHeartbeatPing();
    Task<void> waitForHeartbeatWrite();
    Task<void> waitForWriteIdle();
    Task<void> writeExclusive(WebSocketOpcode opcode, std::string_view payload);
    Task<void> writeFrameNow(WebSocketOpcode opcode, std::string_view payload);
    Task<void> flushProtocolOutputExclusive();
    Task<void> flushProtocolOutputNow();
    void abortTransport() noexcept;
    void notifyWriteIdle() noexcept;

    Transport transport_;
    ConnectionScanner::Entry& scannerEntry_;
    WebSocketLifecycleOptions lifecycleOptions_{};
    std::pmr::string buffer_;
    WsConnection protocol_;
    WorkerSignal backgroundWriteSignal_;
    WorkerSignal readerDoneSignal_;
    std::size_t backgroundWriteCount_{0};
    bool writeActive_{false};
    bool readActive_{false};
    bool heartbeatWriteActive_{false};
    bool awaitingPong_{false};
    std::int64_t heartbeatPingSentMs_{0};
    std::int64_t localCloseStartedMs_{-1};
    // Declared last so destruction unregisters before any callback target state
    // starts to disappear.
    ConnectionScanner::PeriodicCheckRegistration periodicCheck_;
};

}  // namespace ruvia::detail

#include "ruvia/web/detail/websocket/HttpWebSocketConnectionHeartbeat.inl"
#include "ruvia/web/detail/websocket/HttpWebSocketConnectionRead.inl"
#include "ruvia/web/detail/websocket/HttpWebSocketConnectionWrite.inl"
