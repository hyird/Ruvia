#include <array>
#include <exception>
#include <optional>
#include <stdexcept>
#include <system_error>
#include <utility>

#include "ruvia/core/AsioTask.h"
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
    if (http2_) {
        http2_->stop();
    }
    if (http3_) {
        http3_->stop();
    }
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
    if (http2_ || http3_) {
        startCloseOnWorker();
    }
}

void WebSocketClientState::requestAbort(AbortReason reason) noexcept {
    auto phase = phase_.load(std::memory_order_acquire);
    if (phase == Phase::kClosed) {
        return;
    }
    if (phase == Phase::kFresh && phase_.compare_exchange_strong(phase, Phase::kClosed,
                                      std::memory_order_acq_rel, std::memory_order_acquire)) {
        return;
    }
    if (worker_.isCurrent()) {
        closeOnWorker(reason);
        return;
    }
    try {
        auto state = shared_from_this();
        if (!loop_.defer_cleanup(
                [state = std::move(state), reason] { state->closeOnWorker(reason); }) &&
            phase_.load(std::memory_order_acquire) != Phase::kClosed) {
            std::terminate();
        }
    } catch (...) {
        if (phase_.load(std::memory_order_acquire) != Phase::kClosed) {
            std::terminate();
        }
    }
}

void WebSocketClientState::abort() noexcept {
    requestAbort(AbortReason::kClosing);
}

void WebSocketClientState::requestCancel() noexcept {
    requestAbort(AbortReason::kCancelled);
}

Task<void> WebSocketClientState::shutdownOwned(
    std::shared_ptr<WebSocketClientState> state, ClientCloseState::ObservationMode mode) {
    auto* owner = state.get();
    return owner->closeState_.shutdown_owned(std::move(state), [owner] { owner->startCloseOnWorker(); }, mode);
}

void WebSocketClientState::startCloseOnWorker() noexcept {
    if (!worker_.isCurrent()) {
        std::terminate();
    }
    closeOnWorker(AbortReason::kClosing);
    stopSource_.requestStop();
    closeState_.start_cleanup(shared_from_this(), [this] { return closeOnWorker(); }, [this](std::exception_ptr failure) { finishClose(std::move(failure)); });
}

Task<void> WebSocketClientState::closeOnWorker() {
    while (connectInFlight_ || heartbeatInFlight_) {
        co_await closeState_.wait();
    }
    co_await operationScope_.close_and_join();
    if (http2_) {
        co_await http2_->join();
    }
    if (http3_) {
        co_await http3_->join();
    }
}

void WebSocketClientState::finishClose(std::exception_ptr failure) {
    if (connectInFlight_ || heartbeatInFlight_ || operationScope_.has_pending_operations()) {
        std::terminate();
    }
    phase_.store(Phase::kClosed, std::memory_order_release);
    closeState_.finish(std::move(failure));
}

Task<void> WebSocketClientState::shutdown() {
    return shutdownOwned(shared_from_this(), ClientCloseState::ObservationMode::kCaller);
}

ScopedOperation<void> WebSocketClientState::close(
    WebSocketCloseOptions options, OperationOptions operationOptions) {
    validateOperationOptions(operationOptions);
    requireCurrent();
    std::pmr::string reason(options.reason.view(), memory_.resource());
    auto readActivity = claim_activity(readActive_, "WebSocket client close cannot overlap read");
    auto writeActivity = claim_activity(writeActive_, "WebSocket client close cannot overlap write");
    auto closeActivity = claim_activity(closeActive_, "WebSocket client close is already in progress");
    return ::ruvia::make_scoped_operation(operationScope_,
        closeOwned(shared_from_this(), options, std::move(reason), std::move(operationOptions),
            std::move(readActivity), std::move(writeActivity), std::move(closeActivity)),
        &WebSocketClientState::checkOperationAffinity, &worker_);
}

Task<void> WebSocketClientState::closeOwned(std::shared_ptr<WebSocketClientState> state,
    WebSocketCloseOptions options, std::pmr::string reason, OperationOptions operationOptions,
    operation_lane_lease readActivity, operation_lane_lease writeActivity, operation_lane_lease closeActivity) {
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
        if (submitted != WebSocketCloseSubmitStatus::kAccepted) {
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
        std::optional<WebSocketEvent> event;
        {
            co_await state->waitForWriteIdle();
            WriteGuard writeGuard(*state, WritePhase::kApplication);
            event = state->requireProtocol().nextEvent();
            if (event.has_value() && event->ping() != nullptr) {
                co_await state->flushOutput();
            }
        }
        if (!event.has_value()) {
            const auto count = co_await state->readTransport(bytes, std::nullopt);
            if (count == 0) {
                state->requireProtocol().notifyTransportEof();
                state->closeOnWorker(AbortReason::kNone);
                throw WebSocketClientError(WebSocketClientError::Code::kProtocolError,
                    "WebSocket transport ended before peer Close");
            }
            (void)state->requireProtocol().feed(std::string_view(bytes.data(), count));
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
