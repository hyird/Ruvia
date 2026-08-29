#pragma once

#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>

#include <asio.hpp>

#include "ruvia/core/detail/io/ConnectionScanner.h"
#include "ruvia/http/detail/websocket/message/HttpWebSocketPermessageDeflate.h"
#include "ruvia/http/detail/websocket/WsConnection.h"
#include "ruvia/web/detail/websocket/HttpWebSocketLiveness.h"
#include "ruvia/web/detail/websocket/WsTransportReadResult.h"
#include "ruvia/core/detail/io/AsioAwait.h"
#include "ruvia/core/detail/worker/WorkerSignal.h"
#include "ruvia/core/Task.h"
#include "ruvia/web/WebSocket.h"
#include "ruvia/http/detail/util/PmrString.h"
#include "ruvia/core/memory/PmrResource.h"

namespace ruvia::detail {

// Transport-agnostic WebSocket connection (RFC 6455). All protocol behavior,
// including frame reassembly, write serialization, heartbeats, and close,
// lives here; the HTTP/1.1 and HTTP/2 transports differ only in the Transport
// policy, which supplies four transport-specific operations:
//   asio-executor executor() const;
//   Task<WsTransportReadResult> readMore(std::pmr::string& buffer);
//   Task<std::error_code> writeBytes(std::string_view, WsTransportDisposition);
//   void abort() noexcept;  // abort this WebSocket transport, not an unrelated h2 stream
template <typename Transport>
class WebSocketConnection final {
public:
    WebSocketConnection(Transport transport, const WorkerHandle& worker, ConnectionScanner::Entry& scannerEntry, WebSocketLifecycleOptions lifecycleOptions, ProtocolByteLimit messageLimit, std::pmr::memory_resource* resource, std::string_view initialBytes = {}, WebSocketCompression compression = WebSocketCompression::kDisabled)
        : transport_(std::move(transport)),
          scannerEntry_(scannerEntry),
          lifecycleOptions_(lifecycleOptions),
          buffer_(pmrResourceOrDefault(resource)),
          protocol_(buffer_, messageLimit, compression),
          backgroundWriteSignal_(worker),
          readerDoneSignal_(worker) {
        buffer_.append(initialBytes.data(), initialBytes.size());
        scannerEntry_.registerPeriodicCheck(periodicCheck_, this, &WebSocketConnection::heartbeatTickThunk);
    }

    WebSocketConnection(Transport, WorkerHandle&&, ConnectionScanner::Entry&, WebSocketLifecycleOptions, ProtocolByteLimit, std::pmr::memory_resource*, std::string_view = {}, WebSocketCompression = WebSocketCompression::kDisabled) = delete;

    ~WebSocketConnection() = default;

    WebSocketConnection(const WebSocketConnection&) = delete;
    WebSocketConnection& operator=(const WebSocketConnection&) = delete;

    static void heartbeatTickThunk(void* target, std::int64_t now) noexcept {
        static_cast<WebSocketConnection*>(target)->heartbeatTick(now);
    }

    [[nodiscard]] Task<std::optional<WebSocketMessage>> read();
    Task<void> write(WebSocketOpcode opcode, std::string_view payload);
    Task<void> close(::ruvia::WebSocketCloseOptions options = {});
    void abort() noexcept {
        abortTransport();
    }
    Task<void> detachAndDrainWrites();

private:
    enum class WritePhase : std::uint8_t {
        kIdle,
        kApplication,
        kHeartbeat,
    };

    enum class WriteClaim : std::uint8_t {
        kAcquire,
        kAdopt,
    };

    enum class ReadPhase : std::uint8_t {
        kIdle,
        kReserved,
        kActive,
    };

    class WriteGuard final {
    public:
        WriteGuard(WebSocketConnection& connection, WritePhase phase, WriteClaim claim = WriteClaim::kAcquire)
            : connection_(connection),
              phase_(phase) {
            if (phase_ == WritePhase::kIdle) {
                std::terminate();
            }
            if (claim == WriteClaim::kAcquire) {
                if (connection_.writePhase_ != WritePhase::kIdle) {
                    throw std::logic_error("concurrent websocket writes are not supported");
                }
                connection_.writePhase_ = phase_;
            } else if (connection_.writePhase_ != phase_) {
                std::terminate();
            }
        }

        ~WriteGuard() {
            connection_.finishWrite(phase_);
        }

        WriteGuard(const WriteGuard&) = delete;
        WriteGuard& operator=(const WriteGuard&) = delete;

    private:
        WebSocketConnection& connection_;
        WritePhase phase_;
    };

    class WriteOperationLease final {
    public:
        explicit WriteOperationLease(bool& active)
            : active_(&active) {
            if (*active_) {
                active_ = nullptr;
                throw std::logic_error("concurrent websocket writes are not supported");
            }
            *active_ = true;
        }

        WriteOperationLease(const WriteOperationLease&) = delete;
        WriteOperationLease& operator=(const WriteOperationLease&) = delete;
        WriteOperationLease(WriteOperationLease&& other) noexcept
            : active_(std::exchange(other.active_, nullptr)) {}
        WriteOperationLease& operator=(WriteOperationLease&&) = delete;

        ~WriteOperationLease() {
            if (active_ != nullptr) {
                *active_ = false;
            }
        }

    private:
        bool* active_;
    };

    class ReadGuard final {
    public:
        explicit ReadGuard(WebSocketConnection& connection)
            : connection_(&connection) {
            if (connection_->readPhase_ != ReadPhase::kIdle) {
                connection_ = nullptr;
                throw std::logic_error("concurrent websocket reads are not supported");
            }
            connection_->readPhase_ = ReadPhase::kReserved;
        }

        ReadGuard(const ReadGuard&) = delete;
        ReadGuard& operator=(const ReadGuard&) = delete;
        ReadGuard(ReadGuard&& other) noexcept
            : connection_(std::exchange(other.connection_, nullptr)) {}
        ReadGuard& operator=(ReadGuard&&) = delete;

        void start() {
            if (connection_ == nullptr || connection_->readPhase_ != ReadPhase::kReserved) {
                std::terminate();
            }
            connection_->readPhase_ = ReadPhase::kActive;
        }

        ~ReadGuard() {
            if (connection_ != nullptr) {
                const bool started = connection_->readPhase_ == ReadPhase::kActive;
                connection_->readPhase_ = ReadPhase::kIdle;
                if (started) {
                    connection_->readerDoneSignal_.notify();
                }
            }
        }

    private:
        WebSocketConnection* connection_;
    };

    void finishWrite(WritePhase phase) noexcept;
    void heartbeatTick(std::int64_t now) noexcept;
    Task<std::optional<WebSocketMessage>> readOwned(ReadGuard readGuard);
    Task<void> writeOwned(WebSocketOpcode opcode, std::string_view payload, WriteOperationLease writeLease);
    Task<void> closeOwned(::ruvia::WebSocketCloseOptions options, WriteOperationLease writeLease);
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
    WritePhase writePhase_{WritePhase::kIdle};
    bool writeActive_{false};
    ReadPhase readPhase_{ReadPhase::kIdle};
    WebSocketLivenessState livenessState_{WebSocketLivenessIdle{}};
    // Declared last so destruction unregisters before any callback target state
    // starts to disappear.
    ConnectionScanner::PeriodicCheckRegistration periodicCheck_;
};

}  // namespace ruvia::detail

#include "ruvia/web/detail/websocket/HttpWebSocketConnectionHeartbeat.inl"
#include "ruvia/web/detail/websocket/HttpWebSocketConnectionRead.inl"
#include "ruvia/web/detail/websocket/HttpWebSocketConnectionWrite.inl"
