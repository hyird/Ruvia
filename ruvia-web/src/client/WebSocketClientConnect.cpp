#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <asio/connect.hpp>
#include <openssl/rand.h>

#include "ruvia/core/detail/io/AsioAwait.h"
#include "ruvia/core/detail/io/TcpSocketOptions.h"
#include "ruvia/http/Http1ClientRequestWriter.h"
#include "ruvia/http/Http1ClientResponseParser.h"
#include "ruvia/http/Http1ClosePolicy.h"
#include "ruvia/http/Http1WebSocketClientHandshake.h"
#include "ruvia/http/HttpClient.h"
#include "ruvia/http/HttpLimits.h"
#include "ruvia/web/detail/client/ClientTransport.h"
#include "ruvia/web/detail/client/WebSocketClientInternal.h"
#include "ruvia/web/detail/client/WebSocketClientState.h"

namespace ruvia::detail {

Task<void> WebSocketClientState::connect() {
    return connectOwned(shared_from_this());
}

Task<void> WebSocketClientState::establishTransport() {
    arm(connectTimer_, config_.connectTimeout, AbortReason::kTimeout);

    ClientPortTextBuffer portBuffer{};
    const auto portText = formatClientPort(port(), portBuffer);
    auto resolved =
        co_await asyncAsio<asio::ip::tcp::resolver::results_type>([&](auto handler) mutable {
            resolver_.async_resolve(config_.host, portText, std::move(handler));
        });
    throwAbort();
    if (resolved.errorCode()) {
        throw WebSocketClientError(
            WebSocketClientError::Code::kResolveFailed, resolved.errorCode().message());
    }

    auto endpoints = std::move(resolved).takeResult();
    auto connected = co_await asyncAsio([&](auto handler) mutable {
        asio::async_connect(stream_.lowest_layer(), endpoints, std::move(handler));
    });
    throwAbort();
    if (connected.errorCode()) {
        throw WebSocketClientError(
            WebSocketClientError::Code::kConnectFailed, connected.errorCode().message());
    }

    const auto transport = config_.transport.view();
    configureTcpSocketOptions(stream_.next_layer(), transport.tcpNoDelay, transport.tcpKeepAlive);

    if (config_.scheme == WebSocketScheme::kWss) {
        co_await performTlsHandshake();
    }
}

Task<void> WebSocketClientState::performTlsHandshake() {
    const auto tlsSetup = prepareClientTlsStream(
        stream_, config_.host, config_.transport.view(), ClientAlpnMode::kHttp11);
    if (tlsSetup != ClientTlsSetupError::kNone) {
        throw WebSocketClientError(
            WebSocketClientError::Code::kTlsFailed, clientTlsSetupErrorMessage(tlsSetup));
    }

    auto handshake = co_await asyncAsio([&](auto handler) mutable {
        stream_.async_handshake(asio::ssl::stream_base::client, std::move(handler));
    });
    throwAbort();
    if (handshake.errorCode()) {
        throw WebSocketClientError(
            WebSocketClientError::Code::kTlsFailed, handshake.errorCode().message());
    }

    const auto alpn = selectedClientAlpn(stream_.native_handle());
    if (!alpn.empty() && alpn != "http/1.1") {
        throw WebSocketClientError(WebSocketClientError::Code::kHandshakeRejected,
            "upstream did not negotiate HTTP/1.1 for WebSocket");
    }
}

Task<void> WebSocketClientState::connectOwned(std::shared_ptr<WebSocketClientState> state) {
    state->requireCurrent();
    auto expected = Phase::kFresh;
    if (!state->phase_.compare_exchange_strong(
            expected, Phase::kConnecting, std::memory_order_acq_rel)) {
        throw WebSocketClientError(WebSocketClientError::Code::kInvalidState,
            "WebSocket client connect may only be started once");
    }
    state->abortReason_ = AbortReason::kNone;
    state->connectInFlight_ = true;
    try {
        co_await state->establishTransport();
        co_await state->performHandshake();
        state->protocol_.emplace(state->input_,
            ProtocolByteLimit::limited(state->config_.maxMessageBytes),
            WebSocketCompression::kDisabled, WsConnectionRole::kClient,
            &WebSocketClientState::generateMask, nullptr);
        state->disarm(state->connectTimer_);
        auto open = Phase::kConnecting;
        if (!state->phase_.compare_exchange_strong(
                open, Phase::kOpen, std::memory_order_acq_rel, std::memory_order_acquire)) {
            throw WebSocketClientError(
                WebSocketClientError::Code::kClosing, "WebSocket client closed while connecting");
        }
        state->lastActiveMs_ = webSocketSteadyNowMs();
        state->livenessState_ = WebSocketLivenessIdle{};
        if (state->config_.heartbeat.pingInterval.has_value()) {
            state->armHeartbeatTimer(*state->config_.heartbeat.pingInterval);
        }
        state->connectInFlight_ = false;
        state->closeState_.notifyProgress();
    } catch (...) {
        state->disarm(state->connectTimer_);
        state->closeOnWorker(state->abortReason_ == AbortReason::kNone ? AbortReason::kClosing
                                                                       : state->abortReason_);
        state->connectInFlight_ = false;
        state->closeState_.notifyProgress();
        throw;
    }
}

Task<void> WebSocketClientState::performHandshake() {
    std::array<std::uint8_t, kWebSocketClientHandshakeNonceBytes> nonce{};
    if (RAND_bytes(nonce.data(), static_cast<int>(nonce.size())) != 1) {
        throw WebSocketClientError(WebSocketClientError::Code::kHandshakeRejected,
            "failed to generate WebSocket handshake key");
    }

    std::pmr::vector<HttpHeaderView> headers(memory_.resource());
    headers.reserve(config_.headers.size());
    for (const auto& header : config_.headers) {
        headers.emplace_back(header.name, header.value);
    }
    std::pmr::vector<std::string_view> subprotocols(memory_.resource());
    subprotocols.reserve(config_.subprotocols.size());
    for (const auto& protocol : config_.subprotocols) {
        subprotocols.push_back(protocol);
    }
    Http1WebSocketClientHandshake handshake(
        {.nonce = nonce, .headers = headers, .subprotocols = subprotocols, .userAgent = config_.userAgent},
        memory_.resource());

    std::array<char, kMaxHttpHeaderBytes + kWebSocketClientHandshakeRequestBufferExtraBytes>
        requestBuffer{};
    const auto wireHost = clientUriHost(config_.host, memory_.resource());
    const auto origin = [&] {
        const HttpOriginOptions originOptions{.host = wireHost, .port = port()};
        if (config_.scheme == WebSocketScheme::kWss) {
            return HttpOriginView::https(originOptions);
        }
        return HttpOriginView::http(originOptions);
    }();
    auto preparedResult = handshake.prepareRequest(origin, config_.target, requestBuffer);
    const auto* prepared = preparedResult.prepared();
    if (prepared == nullptr) {
        const auto message = preparedResult.failure()
                                 ? std::string(http1ClientRequestPrepareErrorMessage(
                                       preparedResult.failure()->error()))
                                 : "WebSocket handshake request head is too large";
        throw WebSocketClientError(WebSocketClientError::Code::kInvalidConfig, message);
    }
    Http1ClientResponseParser parser(prepared->exchangeState(), {.resource = memory_.resource()});
    co_await writeTransport(prepared->head(), config_.writeTimeout);

    std::array<char, kWebSocketClientTransportBufferBytes> bytes{};
    for (;;) {
        auto result = parser.parse(input_);
        if (const auto* failure = result.failure()) {
            throw WebSocketClientError(WebSocketClientError::Code::kProtocolError,
                std::string(http1ClientResponseParseErrorMessage(failure->error())));
        }
        if (result.needMore()) {
            if (input_.size() >= kMaxHttpHeaderBytes) {
                throw WebSocketClientError(WebSocketClientError::Code::kProtocolError,
                    "WebSocket handshake response head is too large");
            }
            const auto read = co_await readTransport(bytes, config_.connectTimeout);
            if (read == 0) {
                throw WebSocketClientError(WebSocketClientError::Code::kIoError,
                    "upstream closed during WebSocket handshake");
            }
            input_.append(bytes.data(), read);
            continue;
        }
        auto* parsed = result.parsed();
        if (parsed == nullptr) {
            throw WebSocketClientError(WebSocketClientError::Code::kHandshakeRejected,
                "upstream rejected the WebSocket upgrade");
        }
        if (const auto* informational = parsed->plan().informational()) {
            if (informational->persistence() == Http1ClosePolicy::kCloseAfterResponse) {
                throw WebSocketClientError(WebSocketClientError::Code::kHandshakeRejected,
                    "upstream closed the HTTP exchange before the WebSocket upgrade");
            }
            input_.erase(0, parsed->consumedBytes());
            continue;
        }
        const auto validated = handshake.validateResponse(*parsed);
        if (!validated) {
            throw WebSocketClientError(WebSocketClientError::Code::kHandshakeRejected,
                "invalid WebSocket handshake response");
        }
        selectedSubprotocol_.assign(validated->selectedSubprotocol);
        input_.erase(0, parsed->consumedBytes());
        co_return;
    }
}

}  // namespace ruvia::detail
