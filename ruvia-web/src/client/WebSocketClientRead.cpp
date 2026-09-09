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

#include "ruvia/core/StopToken.h"
#include "ruvia/core/detail/io/AsioAwait.h"
#include "ruvia/http/detail/websocket/message/HttpWebSocketMessageAccess.h"
#include "ruvia/web/detail/client/WebSocketClientState.h"
#include "ruvia/web/detail/websocket/HttpWebSocketLiveness.h"

#include "client/WebSocketClientInternal.h"

namespace ruvia::detail {

ScopedOperation<std::optional<WebSocketMessage>> WebSocketClientState::read(
    OperationOptions options) {
    validateOperationOptions(options);
    requireCurrent();
    return makeScopedOperation(operationScope_,
        readOwned(shared_from_this(), std::move(options),
            ActivityLease(readActive_, "concurrent WebSocket client reads are not supported")));
}

Task<std::optional<WebSocketMessage>> WebSocketClientState::readOwned(
    std::shared_ptr<WebSocketClientState> state, OperationOptions options, ActivityLease activity) {
    static_cast<void>(activity);
    state->requireOpen();
    OperationGuard operation(*state, options);
    std::array<char, kWebSocketClientTransportBufferBytes> bytes{};
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
            const auto count = co_await state->readTransport(
                bytes, state->config_.readTimeout);
            if (count == 0) {
                state->requireProtocol().notifyTransportEof();
                state->closeOnWorker(AbortReason::kNone);
                co_return std::nullopt;
            }
            state->input_.append(bytes.data(), count);
            continue;
        }
        if (const auto* message = event->message()) {
            co_return WebSocketMessageAccess::make(message->opcode(), message->payload());
        }
        if (event->pong() != nullptr) {
            const bool awaitingPong =
                std::holds_alternative<WebSocketSendingPing>(state->livenessState_) ||
                std::holds_alternative<WebSocketAwaitingPong>(state->livenessState_);
            state->livenessState_ = WebSocketLivenessIdle{};
            if (awaitingPong) {
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
    auto initiateRead = [this, output](auto handler) {
        if (config_.scheme == WebSocketScheme::kWss) {
            stream_.async_read_some(asio::buffer(output.data(), output.size()), std::move(handler));
        } else {
            stream_.next_layer().async_read_some(
                asio::buffer(output.data(), output.size()), std::move(handler));
        }
    };
    const auto completion = co_await asyncAsio<std::size_t>(std::move(initiateRead));
    disarm(readTimer_);
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
