#include "ruvia/web/WebSocketClient.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <exception>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

#include <asio/bind_allocator.hpp>
#include <asio/connect.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/recycling_allocator.hpp>
#include <asio/ssl/error.hpp>
#include <asio/write.hpp>
#include <openssl/rand.h>

#include "ruvia/core/detail/io/AsioAwait.h"
#include "ruvia/core/detail/io/OperationDeadline.h"
#include "ruvia/core/detail/io/TcpSocketOptions.h"
#include "ruvia/core/detail/util/Base64.h"
#include "ruvia/core/detail/worker/WorkerDispatcher.h"
#include "ruvia/http/Http1ClientRequestWriter.h"
#include "ruvia/http/Http1ClientResponseParser.h"
#include "ruvia/http/HttpLimits.h"
#include "ruvia/http/detail/field/HeaderTokenUtils.h"
#include "ruvia/http/detail/util/AsciiCase.h"
#include "ruvia/http/detail/websocket/handshake/HttpWebSocketAcceptKey.h"
#include "ruvia/http/detail/websocket/message/HttpWebSocketMessageAccess.h"
#include "ruvia/web/detail/client/WebSocketClientState.h"

namespace ruvia::detail {

class WebSocketClientState::ActivityLease final {
public:
    explicit ActivityLease(bool& active, const char* message)
        : active_(&active) {
        if (*active_) {
            active_ = nullptr;
            throw WebSocketClientError(WebSocketClientError::Code::kInvalidState, message);
        }
        *active_ = true;
    }

    ActivityLease(const ActivityLease&) = delete;
    ActivityLease& operator=(const ActivityLease&) = delete;
    ActivityLease(ActivityLease&& other) noexcept
        : active_(std::exchange(other.active_, nullptr)) {}
    ActivityLease& operator=(ActivityLease&&) = delete;

    ~ActivityLease() {
        if (active_ != nullptr) {
            *active_ = false;
        }
    }

private:
    bool* active_;
};

namespace {

constexpr std::size_t kHandshakeNonceBytes = 16;
constexpr std::size_t kHandshakeHeaderReserve = 6;
constexpr std::size_t kHandshakeRequestBufferExtraBytes = 1024;
constexpr std::size_t kTransportBufferBytes = std::size_t{16} * 1024;
constexpr std::size_t kCloseHandshakeBufferBytes = std::size_t{4} * 1024;

[[nodiscard]] EventLoop requireEventLoop(EventLoop loop) {
    if (!loop.valid()) {
        throw std::invalid_argument("WebSocket client requires a valid event loop");
    }
    return loop;
}

[[nodiscard]] WebSocketClientError::Code transportErrorCode(bool secure) noexcept {
    return secure ? WebSocketClientError::Code::kTlsFailed : WebSocketClientError::Code::kIoError;
}

struct StopAbort final {
    std::weak_ptr<WebSocketClientState> state_;

    void operator()() noexcept {
        if (const auto owner = state_.lock()) {
            owner->requestCancel();
        }
    }
};

}  // namespace

WebSocketClientState::WebSocketClientState(EventLoop loop, const WebSocketClientConfig& config)
    : loop_(requireEventLoop(std::move(loop))),
      worker_(loop_.handle()),
      memory_(),
      config_(config, memory_.resource()),
      tlsContext_(asio::ssl::context::tls_client),
      resolver_(loop_.ioContext()),
      stream_(loop_.ioContext(), tlsContext_),
      writeSignal_(worker_),
      closeState_(worker_),
      input_(memory_.allocator<char>()),
      selectedSubprotocol_(memory_.allocator<char>()) {
    if (config_.scheme == WebSocketScheme::kWss) {
        configureClientTlsContext(tlsContext_, config_.transport.view());
    }
    input_.reserve(kTransportBufferBytes);
}

WebSocketClientState::~WebSocketClientState() {
    const auto phase = phase_.load(std::memory_order_acquire);
    if ((phase != Phase::kClosed && phase != Phase::kFresh) ||
        (closeState_.taskStarted() && !closeState_.complete())) {
        std::terminate();
    }
}

void WebSocketClientState::bindStop() {
    std::weak_ptr<WebSocketClientState> weak = shared_from_this();
    stopRegistration_ = loop_.onStop([weak = std::move(weak)] {
        if (const auto state = weak.lock()) {
            state->startCloseOnWorker();
        }
    });
}

void WebSocketClientState::requireCurrent() const {
    if (!worker_.isCurrent()) {
        throw std::logic_error("WebSocket client must be used on its bound event loop");
    }
}

void WebSocketClientState::requireOpen() const {
    requireCurrent();
    if (phase_.load(std::memory_order_acquire) != Phase::kOpen) {
        throw WebSocketClientError(
            WebSocketClientError::Code::kInvalidState, "WebSocket client is not connected");
    }
}

WsConnection& WebSocketClientState::requireProtocol() noexcept {
    if (!protocol_.has_value()) {
        std::terminate();
    }
    return protocol_.value();
}

std::uint16_t WebSocketClientState::port() const noexcept {
    return config_.port.value_or(config_.scheme == WebSocketScheme::kWss ? 443 : 80);
}

bool WebSocketClientState::generateMask(void*, WsMaskKey& key) noexcept {
    return RAND_bytes(reinterpret_cast<unsigned char*>(key.data()), static_cast<int>(key.size())) ==
           1;
}

void WebSocketClientState::arm(WorkerTimerRegistration& timer,
    std::optional<std::chrono::milliseconds> timeout, AbortReason reason) {
    timer.cancel();
    if (!timeout.has_value()) {
        return;
    }
    std::weak_ptr<WebSocketClientState> weak = shared_from_this();
    WorkerHandleAccess::scheduleTimer(worker_, timer, workerTimerDeadlineAfter(*timeout),
        [weak = std::move(weak), reason](WorkerTimerOutcome outcome) noexcept {
            if (outcome != WorkerTimerOutcome::kExpired) {
                return;
            }
            if (const auto state = weak.lock()) {
                state->closeOnWorker(reason);
            }
        });
}

void WebSocketClientState::disarm(WorkerTimerRegistration& timer) noexcept {
    timer.cancel();
}

std::optional<std::chrono::milliseconds> WebSocketClientState::effectiveTimeout(
    const OperationTimeout& operationTimeout,
    std::optional<std::chrono::milliseconds> configured) const {
    const auto remaining = operationTimeout.remaining();
    if (!remaining.has_value()) {
        return configured;
    }
    if (!configured.has_value()) {
        return remaining;
    }
    return std::min(*remaining, *configured);
}

void WebSocketClientState::throwAbort() const {
    switch (abortReason_) {
        case AbortReason::kNone:
            return;
        case AbortReason::kTimeout:
            throw WebSocketClientError(
                WebSocketClientError::Code::kTimeout, "WebSocket client operation timed out");
        case AbortReason::kCancelled:
            throw WebSocketClientError(
                WebSocketClientError::Code::kCancelled, "WebSocket client operation was cancelled");
        case AbortReason::kClosing:
            throw WebSocketClientError(
                WebSocketClientError::Code::kClosing, "WebSocket client is closing");
    }
}

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

Task<void> WebSocketClientState::connect() {
    return connectOwned(shared_from_this());
}

Task<void> WebSocketClientState::shutdown() {
    return shutdownOwned(shared_from_this());
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

void WebSocketClientState::finishWrite(WritePhase phase) noexcept {
    if (writePhase_ != phase) {
        std::terminate();
    }
    writePhase_ = WritePhase::kIdle;
    writeSignal_.notify();
}

Task<void> WebSocketClientState::waitForWriteIdle() {
    while (writePhase_ != WritePhase::kIdle) {
        co_await writeSignal_.wait();
    }
}

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
            // Ordinary I/O shares lastActiveMs_ and may update it while this
            // coroutine is suspended. The Pong deadline belongs to this
            // completed heartbeat write, so use its own timestamp.
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
    std::array<std::uint8_t, kHandshakeNonceBytes> nonce{};
    if (RAND_bytes(nonce.data(), static_cast<int>(nonce.size())) != 1) {
        throw WebSocketClientError(WebSocketClientError::Code::kHandshakeRejected,
            "failed to generate WebSocket handshake key");
    }
    std::array<char, base64EncodedSize(nonce.size())> key{};
    encodeBase64(key.data(), nonce);
    const std::string_view keyView(key.data(), key.size());

    std::pmr::vector<HttpHeaderView> headers(memory_.resource());
    headers.reserve(config_.headers.size() + kHandshakeHeaderReserve);
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

    std::array<char, kMaxHttpHeaderBytes + kHandshakeRequestBufferExtraBytes> requestBuffer{};
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

    std::array<char, kTransportBufferBytes> bytes{};
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

Task<void> WebSocketClientState::writeTransport(std::string_view bytes, OperationOptions options,
    OperationTimeout operationTimeout, std::optional<std::chrono::milliseconds> configuredTimeout) {
    if (bytes.empty()) {
        co_return;
    }
    StopRegistration cancellation;
    if (options.stopToken.stoppable()) {
        options.stopToken.registerCallback(cancellation, StopAbort{weak_from_this()});
    }
    if (options.stopToken.stopRequested()) {
        closeOnWorker(AbortReason::kCancelled);
    }
    arm(writeTimer_, effectiveTimeout(operationTimeout, configuredTimeout), AbortReason::kTimeout);
    auto initiateWrite = [this, bytes](auto handler) {
        if (config_.scheme == WebSocketScheme::kWss) {
            asio::async_write(stream_, asio::buffer(bytes), std::move(handler));
        } else {
            asio::async_write(stream_.next_layer(), asio::buffer(bytes), std::move(handler));
        }
    };
    const auto completion = co_await asyncAsio<std::size_t>(std::move(initiateWrite));
    disarm(writeTimer_);
    cancellation.reset();
    throwAbort();
    if (completion.errorCode()) {
        throw WebSocketClientError(transportErrorCode(config_.scheme == WebSocketScheme::kWss),
            completion.errorCode().message());
    }
    touchActivity();
}

Task<std::size_t> WebSocketClientState::readTransport(std::span<char> output,
    OperationOptions options, OperationTimeout operationTimeout,
    std::optional<std::chrono::milliseconds> configuredTimeout) {
    StopRegistration cancellation;
    if (options.stopToken.stoppable()) {
        options.stopToken.registerCallback(cancellation, StopAbort{weak_from_this()});
    }
    if (options.stopToken.stopRequested()) {
        closeOnWorker(AbortReason::kCancelled);
    }
    arm(readTimer_, effectiveTimeout(operationTimeout, configuredTimeout), AbortReason::kTimeout);
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
    cancellation.reset();
    throwAbort();
    if (completion.errorCode() == asio::error::eof ||
        completion.errorCode() == asio::ssl::error::stream_truncated) {
        co_return 0;
    }
    if (completion.errorCode()) {
        throw WebSocketClientError(transportErrorCode(config_.scheme == WebSocketScheme::kWss),
            completion.errorCode().message());
    }
    touchActivity();
    co_return completion.result();
}

Task<void> WebSocketClientState::flushOutput(
    OperationOptions options, OperationTimeout operationTimeout) {
    for (;;) {
        auto& protocol = requireProtocol();
        const auto plan = protocol.outputPlan();
        if (!plan.bytes().empty()) {
            co_await writeTransport(plan.bytes(), options, operationTimeout, config_.writeTimeout);
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
    std::shared_ptr<WebSocketClientState> state, OperationOptions options,
    OperationTimeout operationTimeout, std::string_view message) {
    co_await state->waitForWriteIdle();
    try {
        WriteGuard writeGuard(*state, WritePhase::kApplication);
        co_await state->flushOutput(std::move(options), operationTimeout);
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
    const OperationTimeout operationTimeout(options.timeout);
    std::array<char, kTransportBufferBytes> bytes{};
    for (;;) {
        std::optional<WsEvent> event;
        {
            co_await state->waitForWriteIdle();
            WriteGuard writeGuard(*state, WritePhase::kApplication);
            event = state->requireProtocol().poll();
            if (event.has_value() && event->ping() != nullptr) {
                co_await state->flushOutput(options, operationTimeout);
            }
        }
        if (!event.has_value()) {
            const auto count = co_await state->readTransport(
                bytes, options, operationTimeout, state->config_.readTimeout);
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
            co_await WebSocketClientState::throwProtocolErrorAfterFlush(state, std::move(options),
                operationTimeout, "WebSocket peer violated the protocol");
            std::terminate();
        }
        if (event->close() != nullptr || event->transportEnd() != nullptr) {
            co_await state->waitForWriteIdle();
            WriteGuard writeGuard(*state, WritePhase::kApplication);
            co_await state->flushOutput(options, operationTimeout);
            co_return std::nullopt;
        }
    }
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
    const OperationTimeout operationTimeout(options.timeout);
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
    co_await state->flushOutput(options, operationTimeout);
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
    const OperationTimeout operationTimeout(operationOptions.timeout);
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
        co_await state->flushOutput(operationOptions, operationTimeout);
    }
    // The close-handshake limit starts after the local Close frame is committed.
    // Keep it on its own timer so peer traffic and control-frame responses cannot
    // restart the deadline for the next transport read.
    const auto handshakeTimeout =
        state->effectiveTimeout(operationTimeout, state->config_.closeHandshakeTimeout);
    if (handshakeTimeout.has_value() && handshakeTimeout->count() == 0) {
        state->closeOnWorker(AbortReason::kTimeout);
        state->throwAbort();
    }
    state->arm(state->closeHandshakeTimer_, handshakeTimeout, AbortReason::kTimeout);
    std::array<char, kCloseHandshakeBufferBytes> bytes{};
    for (;;) {
        std::optional<WsEvent> event;
        {
            co_await state->waitForWriteIdle();
            WriteGuard writeGuard(*state, WritePhase::kApplication);
            event = state->requireProtocol().poll();
            if (event.has_value() && event->ping() != nullptr) {
                co_await state->flushOutput(operationOptions, operationTimeout);
            }
        }
        if (!event.has_value()) {
            const auto count = co_await state->readTransport(
                bytes, operationOptions, operationTimeout, std::nullopt);
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
                std::move(operationOptions), operationTimeout,
                "WebSocket peer violated the protocol during close handshake");
            std::terminate();
        }
        if (event->close() != nullptr || event->transportEnd() != nullptr) {
            co_await state->waitForWriteIdle();
            WriteGuard writeGuard(*state, WritePhase::kApplication);
            co_await state->flushOutput(operationOptions, operationTimeout);
            if (state->phase_.load(std::memory_order_acquire) != Phase::kClosed) {
                state->closeOnWorker(AbortReason::kNone);
            }
            co_return;
        }
    }
}

WebSocketClientHandle WebSocketClientState::handle(OperationOptions options) {
    requireOpen();
    validateOperationOptions(options);
    options = mergeOperationOptions(
        OperationOptions{.stopToken = stopSource_.token()}, std::move(options));
    return WebSocketClientHandle(shared_from_this(), operationScope_, std::move(options));
}

bool WebSocketClientState::connected() {
    requireCurrent();
    return phase_.load(std::memory_order_acquire) == Phase::kOpen;
}

std::string_view WebSocketClientState::subprotocol() {
    requireCurrent();
    return selectedSubprotocol_;
}

}  // namespace ruvia::detail

namespace ruvia {

WebSocketClientHandle::WebSocketClientHandle(std::shared_ptr<detail::WebSocketClientState> state,
    detail::ScopedOperationScope& scope, OperationOptions options) noexcept
    : detail::ScopedCapabilityNode(scope, &WebSocketClientHandle::expireCapability),
      state_(std::move(state)),
      options_(std::move(options)) {}

WebSocketClientHandle::WebSocketClientHandle(const WebSocketClientHandle& other) noexcept = default;

void WebSocketClientHandle::expireCapability(detail::ScopedCapabilityNode& capability) noexcept {
    static_cast<WebSocketClientHandle&>(capability).state_.reset();
}

WebSocketClientHandle WebSocketClientHandle::withOptions(OperationOptions options) const {
    detail::validateOperationOptions(options);
    requireActive();
    return WebSocketClientHandle(
        state_, operationScope(), detail::mergeOperationOptions(options_, std::move(options)));
}

ScopedOperation<std::optional<WebSocketMessage>> WebSocketClientHandle::read() const {
    requireActive();
    return state_->read(options_);
}

ScopedOperation<void> WebSocketClientHandle::text(std::string_view payload) const {
    requireActive();
    return state_->write(WebSocketOpcode::kText, payload, options_);
}

ScopedOperation<void> WebSocketClientHandle::binary(std::string_view payload) const {
    requireActive();
    return state_->write(WebSocketOpcode::kBinary, payload, options_);
}

ScopedOperation<void> WebSocketClientHandle::ping(std::string_view payload) const {
    requireActive();
    return state_->write(WebSocketOpcode::kPing, payload, options_);
}

ScopedOperation<void> WebSocketClientHandle::pong(std::string_view payload) const {
    requireActive();
    return state_->write(WebSocketOpcode::kPong, payload, options_);
}

ScopedOperation<void> WebSocketClientHandle::close(WebSocketCloseOptions options) const {
    requireActive();
    return state_->close(options, options_);
}

void WebSocketClientHandle::abort() noexcept {
    if (state_) {
        state_->abort();
    }
}

WebSocketClient::WebSocketClient(EventLoop loop, const WebSocketClientConfig& config)
    : state_(std::make_shared<detail::WebSocketClientState>(std::move(loop), config)) {
    state_->bindStop();
}

WebSocketClient::~WebSocketClient() {
    state_->abort();
}

Task<void> WebSocketClient::connect() & {
    return state_->connect();
}

WebSocketClientHandle WebSocketClient::withOptions(OperationOptions options) const& {
    return state_->handle(std::move(options));
}

ScopedOperation<std::optional<WebSocketMessage>> WebSocketClient::read() const& {
    return withOptions({}).read();
}

ScopedOperation<void> WebSocketClient::text(std::string_view payload) const& {
    return withOptions({}).text(payload);
}

ScopedOperation<void> WebSocketClient::binary(std::string_view payload) const& {
    return withOptions({}).binary(payload);
}

ScopedOperation<void> WebSocketClient::ping(std::string_view payload) const& {
    return withOptions({}).ping(payload);
}

ScopedOperation<void> WebSocketClient::pong(std::string_view payload) const& {
    return withOptions({}).pong(payload);
}

ScopedOperation<void> WebSocketClient::close(WebSocketCloseOptions options) const& {
    return withOptions({}).close(options);
}

void WebSocketClient::abort() noexcept {
    state_->abort();
}

Task<void> WebSocketClient::shutdown() & {
    return state_->shutdown();
}

bool WebSocketClient::connected() const {
    return state_->connected();
}

std::string_view WebSocketClient::subprotocol() const& {
    return state_->subprotocol();
}

const WorkerHandle& WebSocketClient::worker() const& noexcept {
    return state_->worker();
}

}  // namespace ruvia
