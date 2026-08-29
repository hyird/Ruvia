#pragma once

#include <atomic>
#include <chrono>
#include <exception>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <utility>

#include <asio/ip/tcp.hpp>
#include <asio/ssl/context.hpp>
#include <asio/ssl/stream.hpp>

#include "ruvia/core/EventLoop.h"
#include "ruvia/core/ScopedOperation.h"
#include "ruvia/core/StopToken.h"
#include "ruvia/core/detail/io/OperationDeadline.h"
#include "ruvia/core/detail/worker/WorkerSignal.h"
#include "ruvia/core/detail/worker/WorkerTimer.h"
#include "ruvia/core/memory/MemoryPool.h"
#include "ruvia/http/detail/websocket/WsConnection.h"
#include "ruvia/web/WebSocketClient.h"
#include "ruvia/web/detail/client/WebSocketClientConfigStorage.h"
#include "ruvia/web/detail/websocket/HttpWebSocketLiveness.h"

namespace ruvia {
class Http1ParsedClientResponseHead;
}

namespace ruvia::detail {

class WebSocketClientState final : public std::enable_shared_from_this<WebSocketClientState> {
public:
    WebSocketClientState(EventLoop loop, const WebSocketClientConfig& config);
    ~WebSocketClientState();

    void bindStop();
    [[nodiscard]] Task<void> connect();
    [[nodiscard]] Task<void> shutdown();
    [[nodiscard]] WebSocketClientHandle handle(OperationOptions options);
    void abort() noexcept;
    void requestCancel() noexcept;
    [[nodiscard]] bool connected();
    [[nodiscard]] std::string_view subprotocol();
    [[nodiscard]] const WorkerHandle& worker() const noexcept {
        return worker_;
    }

    [[nodiscard]] ScopedOperation<std::optional<WebSocketMessage>> read(OperationOptions options);
    [[nodiscard]] ScopedOperation<void> write(
        WebSocketOpcode opcode, std::string_view payload, OperationOptions options);
    [[nodiscard]] ScopedOperation<void> close(
        WebSocketCloseOptions options, OperationOptions operationOptions);

private:
    enum class Phase : std::uint8_t { kFresh, kConnecting, kOpen, kClosing, kClosed };
    enum class AbortReason : std::uint8_t { kNone, kTimeout, kCancelled, kClosing };
    enum class WritePhase : std::uint8_t { kIdle, kApplication, kHeartbeat };
    enum class WriteClaim : std::uint8_t { kAcquire, kAdopt };

    class WriteGuard final {
    public:
        WriteGuard(
            WebSocketClientState& state, WritePhase phase, WriteClaim claim = WriteClaim::kAcquire)
            : state_(state),
              phase_(phase) {
            if (phase_ == WritePhase::kIdle) {
                std::terminate();
            }
            if (claim == WriteClaim::kAcquire) {
                if (state_.writePhase_ != WritePhase::kIdle) {
                    throw std::logic_error("concurrent WebSocket client writes are not supported");
                }
                state_.writePhase_ = phase_;
            } else if (state_.writePhase_ != phase_) {
                std::terminate();
            }
        }

        ~WriteGuard() {
            state_.finishWrite(phase_);
        }

        WriteGuard(const WriteGuard&) = delete;
        WriteGuard& operator=(const WriteGuard&) = delete;

    private:
        WebSocketClientState& state_;
        WritePhase phase_;
    };

    [[nodiscard]] static Task<void> connectOwned(std::shared_ptr<WebSocketClientState> state);
    [[nodiscard]] static Task<void> shutdownOwned(std::shared_ptr<WebSocketClientState> state);
    class ActivityLease;
    [[nodiscard]] static Task<std::optional<WebSocketMessage>> readOwned(
        std::shared_ptr<WebSocketClientState> state, OperationOptions options,
        ActivityLease activity);
    [[nodiscard]] static Task<void> writeOwned(std::shared_ptr<WebSocketClientState> state,
        WebSocketOpcode opcode, std::pmr::string payload, OperationOptions options,
        ActivityLease activity);
    [[nodiscard]] static Task<void> closeOwned(std::shared_ptr<WebSocketClientState> state,
        WebSocketCloseOptions options, std::pmr::string reason, OperationOptions operationOptions,
        ActivityLease readActivity, ActivityLease writeActivity, ActivityLease closeActivity);

    void requireCurrent() const;
    void requireOpen() const;
    [[nodiscard]] WsConnection& requireProtocol() noexcept;
    [[nodiscard]] std::uint16_t port() const noexcept;
    void closeOnWorker(AbortReason reason) noexcept;
    void startCloseOnWorker() noexcept;
    [[nodiscard]] Task<void> closeOnWorker();
    void finishClose(const TaskCompletionResult<void>& result);
    void requestAbort(AbortReason reason) noexcept;
    [[nodiscard]] Task<void> establishTransport();
    [[nodiscard]] Task<void> performTlsHandshake();
    void validateHandshakeResponse(
        const Http1ParsedClientResponseHead& response, std::string_view key);
    void finishWrite(WritePhase phase) noexcept;
    [[nodiscard]] Task<void> waitForWriteIdle();
    [[nodiscard]] static Task<void> heartbeatOwned(std::shared_ptr<WebSocketClientState> state);
    void finishHeartbeat() noexcept;
    void heartbeatTimerFired() noexcept;
    void armHeartbeatTimer(std::chrono::milliseconds delay);
    void touchActivity() noexcept;
    [[nodiscard]] std::chrono::milliseconds heartbeatDelay(std::int64_t now) const noexcept;
    [[nodiscard]] Task<void> flushOutput(
        OperationOptions options, OperationTimeout operationTimeout);
    [[nodiscard]] Task<std::size_t> readTransport(std::span<char> output, OperationOptions options,
        OperationTimeout operationTimeout,
        std::optional<std::chrono::milliseconds> configuredTimeout);
    [[nodiscard]] Task<void> writeTransport(std::string_view bytes, OperationOptions options,
        OperationTimeout operationTimeout,
        std::optional<std::chrono::milliseconds> configuredTimeout);
    [[nodiscard]] Task<void> performHandshake(OperationOptions options);
    [[nodiscard]] std::optional<std::chrono::milliseconds> effectiveTimeout(
        const OperationTimeout& operationTimeout,
        std::optional<std::chrono::milliseconds> configured) const;
    void arm(WorkerTimerRegistration& timer, std::optional<std::chrono::milliseconds> timeout,
        AbortReason reason);
    void disarm(WorkerTimerRegistration& timer) noexcept;
    void throwAbort() const;
    [[nodiscard]] static bool generateMask(void*, WsMaskKey& key) noexcept;

    EventLoop loop_;
    WorkerHandle worker_;
    WorkerMemory memory_;
    WebSocketClientConfigStorage config_;
    asio::ssl::context tlsContext_;
    asio::ip::tcp::resolver resolver_;
    asio::ssl::stream<asio::ip::tcp::socket> stream_;
    WorkerTimerRegistration connectTimer_;
    WorkerTimerRegistration readTimer_;
    WorkerTimerRegistration writeTimer_;
    WorkerTimerRegistration heartbeatTimer_;
    WorkerTimerRegistration closeHandshakeTimer_;
    WorkerSignal writeSignal_;
    WorkerSignal closeSignal_;
    std::pmr::string input_;
    std::optional<WsConnection> protocol_;
    std::pmr::string selectedSubprotocol_;
    StopSource stopSource_;
    EventLoopStopRegistration stopRegistration_;
    std::atomic<Phase> phase_{Phase::kFresh};
    AbortReason abortReason_{AbortReason::kNone};
    bool readActive_{false};
    bool writeActive_{false};
    bool closeActive_{false};
    WritePhase writePhase_{WritePhase::kIdle};
    WebSocketLivenessState livenessState_{WebSocketLivenessIdle{}};
    std::int64_t lastActiveMs_{0};
    bool connectInFlight_{false};
    bool heartbeatInFlight_{false};
    bool closeTaskStarted_{false};
    bool closeComplete_{false};
    std::exception_ptr closeFailure_;
    ScopedOperationScope operationScope_;
};

}  // namespace ruvia::detail
