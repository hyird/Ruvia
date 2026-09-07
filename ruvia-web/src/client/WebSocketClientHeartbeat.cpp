#include <algorithm>
#include <chrono>
#include <exception>
#include <memory>
#include <utility>
#include <variant>

#include <asio/bind_allocator.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/recycling_allocator.hpp>

#include "ruvia/core/AsioTask.h"
#include "ruvia/core/WorkerHandle.h"
#include "ruvia/core/detail/worker/WorkerDispatcher.h"
#include "ruvia/core/detail/worker/WorkerTimer.h"
#include "ruvia/web/detail/client/WebSocketClientState.h"
#include "ruvia/web/detail/websocket/HttpWebSocketLiveness.h"

namespace ruvia::detail {

std::chrono::milliseconds WebSocketClientState::heartbeatDelay(std::int64_t now) const noexcept {
    std::int64_t deadline = now + 1;
    if (const auto* pong = std::get_if<WebSocketAwaitingPong>(&livenessState_)) {
        deadline = pong->sentAtMs() + config_.heartbeat.pongTimeout->count();
    } else if (std::holds_alternative<WebSocketLivenessIdle>(livenessState_)) {
        deadline = lastActiveMs_ + config_.heartbeat.pingInterval->count();
    }
    return std::chrono::milliseconds(std::max<std::int64_t>(1, deadline - now));
}

void WebSocketClientState::armHeartbeatTimer(std::chrono::milliseconds delay) {
    if (!config_.heartbeat.pingInterval.has_value()) {
        return;
    }
    heartbeatTimer_.cancel();
    std::weak_ptr<WebSocketClientState> weak = shared_from_this();
    WorkerHandleAccess::scheduleTimer(worker_, heartbeatTimer_,
        workerTimerDeadlineAfter(std::max(delay, std::chrono::milliseconds{1})),
        [weak = std::move(weak)](WorkerTimerOutcome outcome) noexcept {
            if (outcome != WorkerTimerOutcome::kExpired) {
                return;
            }
            if (const auto state = weak.lock()) {
                state->heartbeatTimerFired();
            }
        });
}

void WebSocketClientState::touchActivity() noexcept {
    lastActiveMs_ = webSocketSteadyNowMs();
    if (phase_.load(std::memory_order_acquire) != Phase::kOpen ||
        !config_.heartbeat.pingInterval.has_value() ||
        !std::holds_alternative<WebSocketLivenessIdle>(livenessState_)) {
        return;
    }
    try {
        armHeartbeatTimer(*config_.heartbeat.pingInterval);
    } catch (...) {
        closeOnWorker(AbortReason::kClosing);
    }
}

void WebSocketClientState::heartbeatTimerFired() noexcept {
    if (phase_.load(std::memory_order_acquire) != Phase::kOpen ||
        !config_.heartbeat.pingInterval.has_value()) {
        return;
    }

    const auto now = webSocketSteadyNowMs();
    const WebSocketLifecycleOptions options{
        .heartbeat = config_.heartbeat, .closeHandshakeTimeout = config_.closeHandshakeTimeout};
    switch (webSocketLivenessDecision(options, requireProtocol().livenessMode(), livenessState_,
        writePhase_ != WritePhase::kIdle, lastActiveMs_, now)) {
        case WebSocketLivenessDecision::kIdle:
            try {
                armHeartbeatTimer(heartbeatDelay(now));
            } catch (...) {
                closeOnWorker(AbortReason::kClosing);
            }
            return;
        case WebSocketLivenessDecision::kAbortTransport:
            closeOnWorker(AbortReason::kTimeout);
            return;
        case WebSocketLivenessDecision::kSendPing:
            break;
    }

    livenessState_ = WebSocketSendingPing{};
    writePhase_ = WritePhase::kHeartbeat;
    heartbeatInFlight_ = true;
    try {
        auto state = shared_from_this();
        asio::co_spawn(loop_.executor(), taskAsAwaitable(heartbeatOwned(std::move(state))),
            asio::bind_allocator(asio::recycling_allocator<void>(), asio::detached));
    } catch (...) {
        finishHeartbeat();
        finishWrite(WritePhase::kHeartbeat);
        closeOnWorker(AbortReason::kClosing);
    }
}

Task<void> WebSocketClientState::heartbeatOwned(std::shared_ptr<WebSocketClientState> state) {
    {
        WriteGuard writeGuard(*state, WritePhase::kHeartbeat, WriteClaim::kAdopt);
        try {
            state->requireOpen();
            const auto submitted = state->requireProtocol().submitFrame(WebSocketOpcode::kPing, {});
            if (submitted != WsFrameSubmitStatus::kAccepted) {
                throw WebSocketClientError(WebSocketClientError::Code::kProtocolError,
                    "failed to submit WebSocket client heartbeat");
            }
            co_await state->flushOutput(OperationOptions{.stopToken = state->stopSource_.token()},
                OperationTimeout(std::nullopt));
            const auto pingSentAtMs = webSocketSteadyNowMs();
            if (state->phase_.load(std::memory_order_acquire) == Phase::kOpen &&
                std::holds_alternative<WebSocketSendingPing>(state->livenessState_)) {
                state->livenessState_ = WebSocketAwaitingPong(pingSentAtMs);
                state->armHeartbeatTimer(*state->config_.heartbeat.pongTimeout);
            }
        } catch (...) {
            if (state->phase_.load(std::memory_order_acquire) != Phase::kClosed) {
                state->closeOnWorker(AbortReason::kClosing);
            }
        }
    }
    state->finishHeartbeat();
}

void WebSocketClientState::finishHeartbeat() noexcept {
    if (!heartbeatInFlight_) {
        std::terminate();
    }
    heartbeatInFlight_ = false;
    closeState_.notifyProgress();
}

}  // namespace ruvia::detail
