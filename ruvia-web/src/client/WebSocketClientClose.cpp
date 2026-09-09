#include <array>
#include <exception>
#include <optional>
#include <stdexcept>
#include <system_error>
#include <utility>

#include <asio/bind_executor.hpp>

#include "ruvia/core/AsioTask.h"
#include "ruvia/core/detail/worker/WorkerDispatcher.h"
#include "ruvia/web/detail/client/WebSocketClientInternal.h"
#include "ruvia/web/detail/client/WebSocketClientState.h"

namespace ruvia::detail {

void WebSocketClientState::closeOnWorker(AbortReason reason) noexcept {
    if (!worker_.isCurrent()) {
        std::terminate();
    }
    const auto previous = phase_.exchange(Phase::kClosed, std::memory_order_acq_rel);
    if (previous == Phase::kClosed) {
        return;
    }
    if (abortReason_ == AbortReason::kNone) {
        abortReason_ = reason;
    }
    stopSource_.requestStop();
    resolver_.cancel();
    disarm(connectTimer_);
    disarm(readTimer_);
    disarm(writeTimer_);
    disarm(heartbeatTimer_);
    disarm(closeHandshakeTimer_);
    writeSignal_.notify();
    livenessState_ = WebSocketLivenessIdle{};
    if (protocol_) {
        (void)protocol_->abort();
    }
    std::error_code ignored;
    (void)stream_.lowest_layer().cancel(ignored);
    (void)stream_.lowest_layer().close(ignored);
}

void WebSocketClientState::requestAbort(AbortReason reason) noexcept {
    const auto phase = phase_.load(std::memory_order_acquire);
    if (phase == Phase::kClosed) {
        return;
    }
    if (phase == Phase::kFresh) {
        phase_.store(Phase::kClosed, std::memory_order_release);
        return;
    }
    if (worker_.isCurrent()) {
        closeOnWorker(reason);
        return;
    }
    try {
        auto state = shared_from_this();
        if (!WorkerHandleAccess::deferIfAttached(
                worker_, [state = std::move(state), reason] { state->closeOnWorker(reason); })) {
            phase_.store(Phase::kClosed, std::memory_order_release);
        }
    } catch (...) {
        phase_.store(Phase::kClosed, std::memory_order_release);
    }
}

void WebSocketClientState::abort() noexcept {
    requestAbort(AbortReason::kClosing);
}

void WebSocketClientState::requestCancel() noexcept {
    requestAbort(AbortReason::kCancelled);
}

Task<void> WebSocketClientState::shutdownOwned(std::shared_ptr<WebSocketClientState> state) {
    if (!state->worker_.isCurrent()) {
        throw std::logic_error("WebSocket client shutdown must run on its bound event loop");
    }
    state->startCloseOnWorker();
    while (!state->closeState_.complete()) {
        co_await state->closeState_.wait();
    }
    state->closeState_.rethrowFailure();
}

void WebSocketClientState::startCloseOnWorker() noexcept {
    if (!worker_.isCurrent()) {
        std::terminate();
    }
    closeOnWorker(AbortReason::kClosing);
    stopSource_.requestStop();
    if (!closeState_.startTask()) {
        return;
    }
    try {
        auto state = shared_from_this();
        asyncStartTask(closeOnWorker(),
            asio::bind_executor(loop_.executor(),
                [state](const TaskCompletionResult<void>& result) { state->finishClose(result); }));
    } catch (...) {
        phase_.store(Phase::kClosed, std::memory_order_release);
        std::terminate();
    }
}

Task<void> WebSocketClientState::closeOnWorker() {
    while (connectInFlight_ || heartbeatInFlight_) {
        co_await closeState_.wait();
    }
    co_await operationScope_.closeAndJoin();
}

void WebSocketClientState::finishClose(const TaskCompletionResult<void>& result) {
    phase_.store(Phase::kClosed, std::memory_order_release);
    closeState_.finish(result);
}

Task<void> WebSocketClientState::shutdown() {
    return shutdownOwned(shared_from_this());
}

ScopedOperation<void> WebSocketClientState::close(
    WebSocketCloseOptions options, OperationOptions operationOptions) {
    validateOperationOptions(operationOptions);
    requireCurrent();
    std::pmr::string reason(options.reason.view(), memory_.resource());
    return makeScopedOperation(operationScope_,
        closeOwned(shared_from_this(), options, std::move(reason), std::move(operationOptions),
            ActivityLease(readActive_, "WebSocket client close cannot overlap read"),
            ActivityLease(writeActive_, "WebSocket client close cannot overlap write"),
            ActivityLease(closeActive_, "WebSocket client close is already in progress")));
}

Task<void> WebSocketClientState::closeOwned(std::shared_ptr<WebSocketClientState> state,
    WebSocketCloseOptions options, std::pmr::string reason, OperationOptions operationOptions,
    ActivityLease readActivity, ActivityLease writeActivity, ActivityLease closeActivity) {
    static_cast<void>(readActivity);
    static_cast<void>(writeActivity);
    static_cast<void>(closeActivity);
    state->requireOpen();
    OperationGuard operation(*state, operationOptions);
    {
        co_await state->waitForWriteIdle();
        state->requireOpen();
        WriteGuard writeGuard(*state, WritePhase::kApplication);
        const auto submitted = state->requireProtocol().submitClose(options.code, reason);
        if (submitted != WsCloseSubmitStatus::kAccepted) {
            throw WebSocketClientError(WebSocketClientError::Code::kProtocolError,
                "invalid WebSocket client close payload");
        }
        state->phase_.store(Phase::kClosing, std::memory_order_release);
        co_await state->flushOutput();
    }
    // The close-handshake limit starts after the local Close frame is committed.
    // Keep it on its own timer so peer traffic and control-frame responses cannot
    // restart the deadline for the next transport read.
    state->arm(state->closeHandshakeTimer_, state->config_.closeHandshakeTimeout,
        AbortReason::kTimeout);
    std::array<char, kWebSocketClientCloseHandshakeBufferBytes> bytes{};
    for (;;) {
        std::optional<WsEvent> event;
        {
            co_await state->waitForWriteIdle();
            WriteGuard writeGuard(*state, WritePhase::kApplication);
            event = state->requireProtocol().poll();
            if (event.has_value() && event->ping() != nullptr) {
                co_await state->flushOutput();
            }
        }
        if (!event.has_value()) {
            const auto count = co_await state->readTransport(bytes, std::nullopt);
            if (count == 0) {
                state->requireProtocol().notifyTransportEof();
                state->closeOnWorker(AbortReason::kNone);
                co_return;
            }
            state->input_.append(bytes.data(), count);
            continue;
        }
        if (event->protocolError() != nullptr) {
            co_await WebSocketClientState::throwProtocolErrorAfterFlush(state,
                "WebSocket peer violated the protocol during close handshake");
            std::terminate();
        }
        if (event->close() != nullptr || event->transportEnd() != nullptr) {
            co_await state->waitForWriteIdle();
            WriteGuard writeGuard(*state, WritePhase::kApplication);
            co_await state->flushOutput();
            if (state->phase_.load(std::memory_order_acquire) != Phase::kClosed) {
                state->closeOnWorker(AbortReason::kNone);
            }
            co_return;
        }
    }
}

}  // namespace ruvia::detail
