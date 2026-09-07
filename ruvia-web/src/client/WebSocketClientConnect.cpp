#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <asio/connect.hpp>
#include <openssl/rand.h>

#include "ruvia/core/detail/io/AsioAwait.h"
#include "ruvia/core/detail/io/OperationDeadline.h"
#include "ruvia/core/detail/io/TcpSocketOptions.h"
#include "ruvia/core/detail/util/Base64.h"
#include "ruvia/http/Http1ClientRequestWriter.h"
#include "ruvia/http/Http1ClientResponseParser.h"
#include "ruvia/http/HttpClient.h"
#include "ruvia/http/HttpHeader.h"
#include "ruvia/http/HttpLimits.h"
#include "ruvia/http/detail/field/HeaderTokenUtils.h"
#include "ruvia/http/detail/util/AsciiCase.h"
#include "ruvia/http/detail/websocket/handshake/HttpWebSocketAcceptKey.h"
#include "ruvia/web/detail/client/ClientTransport.h"
#include "ruvia/web/detail/client/WebSocketClientState.h"

#include "client/WebSocketClientInternal.h"

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
        co_await state->performHandshake(
            {.timeout = state->config_.connectTimeout, .stopToken = state->stopSource_.token()});
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

void WebSocketClientState::validateHandshakeResponse(
    const Http1ParsedClientResponseHead& response, std::string_view key) {
    if (response.plan().protocolUpgrade() == nullptr ||
        response.head().status() != http_status::kSwitchingProtocols) {
        throw WebSocketClientError(WebSocketClientError::Code::kHandshakeRejected,
            "upstream rejected the WebSocket upgrade");
    }

    WebSocketAcceptKey expectedAccept{};
    encodeWebSocketAccept(expectedAccept, key);

    std::size_t acceptHeaderCount = 0;
    std::size_t protocolHeaderCount = 0;
    std::size_t extensionHeaderCount = 0;
    bool acceptMatches = false;
    bool hasUpgrade = false;
    bool hasConnectionUpgrade = false;
    std::string_view selectedSubprotocol;
    for (const auto& header : response.head().headers()) {
        if (httpAsciiEqualsIgnoreCase(header.name(), "Sec-WebSocket-Accept")) {
            ++acceptHeaderCount;
            acceptMatches = httpTrimOws(header.value()) ==
                            std::string_view(expectedAccept.data(), expectedAccept.size());
        } else if (httpAsciiEqualsIgnoreCase(header.name(), "Upgrade")) {
            hasUpgrade = hasUpgrade || httpHasToken(header.value(), "websocket");
        } else if (httpAsciiEqualsIgnoreCase(header.name(), "Connection")) {
            hasConnectionUpgrade = hasConnectionUpgrade || httpHasToken(header.value(), "upgrade");
        } else if (httpAsciiEqualsIgnoreCase(header.name(), "Sec-WebSocket-Protocol")) {
            ++protocolHeaderCount;
            selectedSubprotocol = httpTrimOws(header.value());
        } else if (httpAsciiEqualsIgnoreCase(header.name(), "Sec-WebSocket-Extensions")) {
            ++extensionHeaderCount;
        }
    }

    const bool valid = acceptHeaderCount == 1 && acceptMatches && hasUpgrade &&
                       hasConnectionUpgrade && extensionHeaderCount == 0 &&
                       protocolHeaderCount <= 1 &&
                       (protocolHeaderCount == 0 || config_.offersSubprotocol(selectedSubprotocol));
    if (!valid) {
        throw WebSocketClientError(
            WebSocketClientError::Code::kHandshakeRejected, "invalid WebSocket handshake response");
    }
    selectedSubprotocol_.assign(selectedSubprotocol);
}

Task<void> WebSocketClientState::performHandshake(OperationOptions options) {
    std::array<std::uint8_t, kWebSocketClientHandshakeNonceBytes> nonce{};
    if (RAND_bytes(nonce.data(), static_cast<int>(nonce.size())) != 1) {
        throw WebSocketClientError(WebSocketClientError::Code::kHandshakeRejected,
            "failed to generate WebSocket handshake key");
    }
    std::array<char, base64EncodedSize(nonce.size())> key{};
    encodeBase64(key.data(), nonce);
    const std::string_view keyView(key.data(), key.size());

    std::pmr::vector<HttpHeaderView> headers(memory_.resource());
    headers.reserve(config_.headers.size() + kWebSocketClientHandshakeHeaderReserve);
    for (const auto& header : config_.headers) {
        headers.emplace_back(header.name, header.value);
    }
    headers.emplace_back("Upgrade", "websocket");
    headers.emplace_back("Connection", "Upgrade");
    headers.emplace_back("Sec-WebSocket-Key", keyView);
    headers.emplace_back("Sec-WebSocket-Version", "13");
    if (!config_.subprotocolHeader.empty()) {
        headers.emplace_back("Sec-WebSocket-Protocol", config_.subprotocolHeader);
    }
    if (!config_.userAgent.empty()) {
        headers.emplace_back("User-Agent", config_.userAgent);
    }

    std::array<char, kMaxHttpHeaderBytes + kWebSocketClientHandshakeRequestBufferExtraBytes>
        requestBuffer{};
    const auto origin = [&] {
        const HttpOriginOptions originOptions{.host = config_.host, .port = port()};
        if (config_.scheme == WebSocketScheme::kWss) {
            return HttpOriginView::https(originOptions);
        }
        return HttpOriginView::http(originOptions);
    }();
    auto preparedResult =
        Http1ClientRequestWriter({.resource = memory_.resource()})
            .prepare(origin, {.method = "GET", .target = config_.target, .headers = headers},
                requestBuffer);
    const auto* prepared = preparedResult.prepared();
    if (prepared == nullptr) {
        const auto message = preparedResult.failure()
                                 ? std::string(http1ClientRequestPrepareErrorMessage(
                                       preparedResult.failure()->error()))
                                 : "WebSocket handshake request head is too large";
        throw WebSocketClientError(WebSocketClientError::Code::kInvalidConfig, message);
    }
    Http1ClientResponseParser parser(prepared->exchangeState(), {.resource = memory_.resource()});
    const OperationTimeout operationTimeout(options.timeout);
    co_await writeTransport(prepared->head(), options, operationTimeout, config_.writeTimeout);

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
            const auto read =
                co_await readTransport(bytes, options, operationTimeout, config_.connectTimeout);
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
        validateHandshakeResponse(*parsed, keyView);
        input_.erase(0, parsed->consumedBytes());
        co_return;
    }
}

}  // namespace ruvia::detail
