#include <array>
#include <chrono>
#include <exception>
#include <optional>
#include <span>
#include <utility>
#include <variant>

#include <asio/buffer.hpp>
#include <asio/error.hpp>
#include <asio/ssl/error.hpp>

#include "ruvia/core/Async.h"
#include "ruvia/core/StopToken.h"

#include "client/WebSocketClientInternal.h"
#include "client/WebSocketClientState.h"
#include "websocket/HttpWebSocketLiveness.h"

namespace ruvia::detail {

ScopedOperation<std::optional<WebSocketMessage>> WebSocketClientState::read(
    OperationOptions options) {
    validateOperationOptions(options);
    requireCurrent();
    return ::ruvia::make_scoped_operation(operationScope_,
        readOwned(shared_from_this(), std::move(options),
            claim_activity(readActive_, "concurrent WebSocket client reads are not supported")),
        &WebSocketClientState::checkOperationAffinity, &worker_);
}

Task<std::optional<WebSocketMessage>> WebSocketClientState::readOwned(
    std::shared_ptr<WebSocketClientState> state, OperationOptions options, operation_lane_lease activity) {
    static_cast<void>(activity);
    state->requireOpen();
    OperationGuard operation(*state, options);
    std::array<char, kWebSocketClientTransportBufferBytes> bytes{};
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
            const auto count = co_await state->readTransport(
                bytes, state->config_.readTimeout);
            if (count == 0) {
                state->requireProtocol().notifyTransportEof();
                state->closeOnWorker(AbortReason::kNone);
                throw WebSocketClientError(WebSocketClientError::Code::kProtocolError,
                    "WebSocket transport ended before peer Close");
            }
            (void)state->requireProtocol().feed(std::string_view(bytes.data(), count));
            continue;
        }
        if (const auto* message = event->message()) {
            co_return WebSocketMessage::borrow(message->opcode(), message->payload());
        }
        if (const auto* pong = event->pong()) {
            if (webSocketHeartbeatPongMatches(state->livenessState_, pong->payload())) {
                state->livenessState_ = WebSocketLivenessIdle{};
                state->touchActivity();
            }
            continue;
        }
        if (event->protocolError() != nullptr) {
            co_await WebSocketClientState::throwProtocolErrorAfterFlush(
                state, "WebSocket peer violated the protocol");
            std::terminate();
        }
        if (event->close() != nullptr || event->transportEnd() != nullptr) {
            co_await state->waitForWriteIdle();
            WriteGuard writeGuard(*state, WritePhase::kApplication);
            co_await state->flushOutput();
            co_return std::nullopt;
        }
    }
}

Task<std::size_t> WebSocketClientState::readTransport(
    std::span<char> output, std::optional<std::chrono::milliseconds> configuredTimeout) {
    arm(readTimer_, configuredTimeout, AbortReason::kTimeout);
    try {
        const auto count = http3_ ? co_await http3_->read(output) : (http2_ ? co_await http2_->read(output) : co_await readSocket(output));
        disarm(readTimer_);
        throwAbort();
        co_return count;
    } catch (...) {
        disarm(readTimer_);
        throw;
    }
}

Task<std::size_t> WebSocketClientState::readSocket(std::span<char> output) {
    auto initiateRead = [this, output](auto handler) {
        if (config_.scheme == WebSocketScheme::kWss) {
            stream_.async_read_some(asio::buffer(output.data(), output.size()), std::move(handler));
        } else {
            stream_.next_layer().async_read_some(
                asio::buffer(output.data(), output.size()), std::move(handler));
        }
    };
    const auto completion = co_await ruvia::asyncAsio<std::size_t>(std::move(initiateRead));
    throwAbort();
    if (completion.errorCode() == asio::error::eof ||
        completion.errorCode() == asio::ssl::error::stream_truncated) {
        co_return 0;
    }
    if (completion.errorCode()) {
        throw WebSocketClientError(
            webSocketClientTransportErrorCode(config_.scheme == WebSocketScheme::kWss),
            completion.errorCode().message());
    }
    touchActivity();
    co_return completion.result();
}

}  // namespace ruvia::detail
