#include <chrono>
#include <exception>
#include <optional>
#include <string_view>
#include <utility>

#include <asio/buffer.hpp>
#include <asio/write.hpp>

#include "ruvia/core/StopToken.h"
#include "ruvia/core/detail/io/AsioAwait.h"
#include "ruvia/web/detail/client/WebSocketClientInternal.h"
#include "ruvia/web/detail/client/WebSocketClientState.h"

namespace ruvia::detail {

void WebSocketClientState::finishWrite(WritePhase phase) noexcept {
    if (writePhase_ != phase) {
        std::terminate();
    }
    writePhase_ = WritePhase::kIdle;
    writeSignal_.notify();
}

Task<void> WebSocketClientState::waitForWriteIdle() {
    for (;;) {
        throwAbort();
        if (writePhase_ == WritePhase::kIdle) {
            co_return;
        }
        co_await writeSignal_.wait();
    }
}

Task<void> WebSocketClientState::writeTransport(
    std::string_view bytes, std::optional<std::chrono::milliseconds> configuredTimeout) {
    if (bytes.empty()) {
        co_return;
    }
    arm(writeTimer_, configuredTimeout, AbortReason::kTimeout);
    auto initiateWrite = [this, bytes](auto handler) {
        if (config_.scheme == WebSocketScheme::kWss) {
            asio::async_write(stream_, asio::buffer(bytes), std::move(handler));
        } else {
            asio::async_write(stream_.next_layer(), asio::buffer(bytes), std::move(handler));
        }
    };
    const auto completion = co_await asyncAsio<std::size_t>(std::move(initiateWrite));
    disarm(writeTimer_);
    throwAbort();
    if (completion.errorCode()) {
        throw WebSocketClientError(
            webSocketClientTransportErrorCode(config_.scheme == WebSocketScheme::kWss),
            completion.errorCode().message());
    }
    touchActivity();
}

Task<void> WebSocketClientState::flushOutput() {
    for (;;) {
        auto& protocol = requireProtocol();
        const auto plan = protocol.outputPlan();
        if (!plan.bytes().empty()) {
            co_await writeTransport(plan.bytes(), config_.writeTimeout);
            if (protocol.consumeOutput(plan.bytes().size()) == WsOutputConsumeStatus::kOutOfRange) {
                std::terminate();
            }
            continue;
        }
        if (plan.disposition() == WsTransportDisposition::kEndTransport) {
            protocol.commitTransportEnd();
            closeOnWorker(AbortReason::kNone);
        }
        co_return;
    }
}

Task<void> WebSocketClientState::throwProtocolErrorAfterFlush(
    std::shared_ptr<WebSocketClientState> state, std::string_view message) {
    co_await state->waitForWriteIdle();
    try {
        WriteGuard writeGuard(*state, WritePhase::kApplication);
        co_await state->flushOutput();
    } catch (const WebSocketClientError& error) {
        if (error.code() != WebSocketClientError::Code::kIoError &&
            error.code() != WebSocketClientError::Code::kTlsFailed) {
            throw;
        }
    }
    if (state->phase_.load(std::memory_order_acquire) != Phase::kClosed) {
        state->closeOnWorker(AbortReason::kNone);
    }
    throw WebSocketClientError(WebSocketClientError::Code::kProtocolError, message);
}

ScopedOperation<void> WebSocketClientState::write(
    WebSocketOpcode opcode, std::string_view payload, OperationOptions options) {
    validateOperationOptions(options);
    requireCurrent();
    std::pmr::string owned(payload, memory_.resource());
    return makeScopedOperation(operationScope_,
        writeOwned(shared_from_this(), opcode, std::move(owned), std::move(options),
            ActivityLease(writeActive_, "concurrent WebSocket client writes are not supported")));
}

Task<void> WebSocketClientState::writeOwned(std::shared_ptr<WebSocketClientState> state,
    WebSocketOpcode opcode, std::pmr::string payload, OperationOptions options,
    ActivityLease activity) {
    static_cast<void>(activity);
    state->requireOpen();
    OperationGuard operation(*state, options);
    co_await state->waitForWriteIdle();
    state->requireOpen();
    WriteGuard writeGuard(*state, WritePhase::kApplication);
    const auto submitted = state->requireProtocol().submitFrame(opcode, payload);
    switch (submitted) {
        case WsFrameSubmitStatus::kAccepted:
            break;
        case WsFrameSubmitStatus::kMessageTooLarge:
            throw WebSocketClientError(WebSocketClientError::Code::kMessageTooLarge,
                "WebSocket client message exceeds configured limit");
        case WsFrameSubmitStatus::kNotOpen:
            throw WebSocketClientError(
                WebSocketClientError::Code::kClosing, "WebSocket client is closing");
        default:
            throw WebSocketClientError(WebSocketClientError::Code::kProtocolError,
                "invalid WebSocket client frame payload");
    }
    co_await state->flushOutput();
}

}  // namespace ruvia::detail
