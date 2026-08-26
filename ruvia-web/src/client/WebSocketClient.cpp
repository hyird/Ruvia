#include "ruvia/web/WebSocketClient.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <exception>
#include <limits>
#include <ranges>
#include <span>
#include <system_error>
#include <utility>

#include <asio/connect.hpp>
#include <asio/ssl/error.hpp>
#include <asio/ssl/host_name_verification.hpp>
#include <asio/write.hpp>
#include <openssl/rand.h>
#include <openssl/ssl.h>

#include "ruvia/core/detail/io/AsioAwait.h"
#include "ruvia/core/detail/util/Base64.h"
#include "ruvia/core/detail/worker/WorkerDispatcher.h"
#include "ruvia/http/Http1ClientRequestWriter.h"
#include "ruvia/http/Http1ClientResponseParser.h"
#include "ruvia/http/HttpLimits.h"
#include "ruvia/http/detail/field/HeaderTokenUtils.h"
#include "ruvia/http/detail/util/AsciiCase.h"
#include "ruvia/http/detail/parser/HttpParserSyntax.h"
#include "ruvia/http/detail/websocket/handshake/HttpWebSocketAcceptKey.h"
#include "ruvia/http/detail/websocket/message/HttpWebSocketMessageAccess.h"
#include "ruvia/web/detail/client/WebSocketClientState.h"
#include "ruvia/web/detail/client/HttpClientConfigValidation.h"

namespace ruvia::detail {
namespace {

[[nodiscard]] bool isIpAddress(std::string_view host) noexcept {
    std::error_code error;
    (void)asio::ip::make_address(host, error);
    return !error;
}

[[nodiscard]] bool isReservedHandshakeHeader(std::string_view name) noexcept {
    constexpr std::array<std::string_view, 8> reserved{
        "host", "connection", "upgrade", "sec-websocket-key",
        "sec-websocket-version", "sec-websocket-protocol",
        "sec-websocket-extensions", "content-length",
    };
    return std::ranges::any_of(reserved, [name](std::string_view candidate) {
        return httpAsciiEqualsIgnoreCase(name, candidate);
    });
}

[[nodiscard]] bool offeredSubprotocol(std::string_view offers, std::string_view selected) noexcept {
    return !httpFindHeaderToken(offers, [selected](std::string_view token) { return token == selected; }).empty();
}

[[nodiscard]] std::string_view selectedAlpn(SSL* ssl) noexcept {
    const unsigned char* selected = nullptr;
    unsigned int size = 0;
    SSL_get0_alpn_selected(ssl, &selected, &size);
    return {reinterpret_cast<const char*>(selected), size};
}

class ActivityGuard final {
public:
    explicit ActivityGuard(bool& active, const char* message)
        : active_(active) {
        if (active_) throw WebSocketClientError(WebSocketClientError::Code::kInvalidState, message);
        active_ = true;
    }
    ~ActivityGuard() { active_ = false; }
private:
    bool& active_;
};

struct StopAbort final {
    std::weak_ptr<WebSocketClientState> state;
    void operator()() noexcept {
        if (const auto owner = state.lock()) owner->requestCancel();
    }
};

}  // namespace

WebSocketClientState::ConfigStorage::ConfigStorage(WebSocketClientConfig config, std::pmr::memory_resource* resource)
    : scheme(config.scheme),
      host(std::move(config.host), resource),
      port(config.port),
      target(std::move(config.target), resource),
      headers(resource),
      subprotocols(std::move(config.subprotocols), resource),
      maxMessageBytes(config.maxMessageBytes),
      connectTimeout(config.connectTimeout),
      readTimeout(config.readTimeout),
      writeTimeout(config.writeTimeout),
      closeHandshakeTimeout(config.closeHandshakeTimeout),
      tlsPeerVerification(config.tlsPeerVerification),
      tcpNoDelay(config.tcpNoDelay),
      tcpKeepAlive(config.tcpKeepAlive),
      caFile(std::move(config.caFile), resource),
      certificateChainFile(std::move(config.certificateChainFile), resource),
      privateKeyFile(std::move(config.privateKeyFile), resource),
      privateKeyPassword(std::move(config.privateKeyPassword), resource),
      userAgent(std::move(config.userAgent), resource) {
    headers.reserve(config.headers.size());
    for (auto& [name, value] : config.headers) headers.emplace_back(name, value, resource);
}

WebSocketClientState::WebSocketClientState(EventLoop loop, WebSocketClientConfig config)
    : loop_(std::move(loop)),
      worker_(loop_.valid() ? loop_.handle() : WorkerHandle{}),
      memory_(),
      config_(std::move(config), memory_.resource()),
      tlsContext_(asio::ssl::context::tls_client),
      resolver_(loop_.valid() ? loop_.ioContext() : throw std::invalid_argument("WebSocket client requires a valid event loop")),
      stream_(loop_.ioContext(), tlsContext_),
      connectTimer_(loop_.ioContext()),
      readTimer_(loop_.ioContext()),
      writeTimer_(loop_.ioContext()),
      input_(memory_.allocator<char>()),
      selectedSubprotocol_(memory_.allocator<char>()) {
    validateConfig();
    configureTls();
    input_.reserve(16 * 1024);
}

WebSocketClientState::~WebSocketClientState() {
    if (phase_.load(std::memory_order_acquire) != Phase::kClosed &&
        phase_.load(std::memory_order_acquire) != Phase::kFresh) {
        std::terminate();
    }
}

void WebSocketClientState::validateConfig() {
    if (config_.host.empty()) throw std::invalid_argument("WebSocket client host must not be empty");
    if (config_.target.empty() || config_.target.front() != '/') throw std::invalid_argument("WebSocket client target must use origin-form");
    if (config_.maxMessageBytes == 0) throw std::invalid_argument("WebSocket client max message bytes must be greater than zero");
    if (config_.connectTimeout.count() <= 0) throw std::invalid_argument("WebSocket client connect timeout must be greater than zero");
    for (const auto timeout : {config_.readTimeout, config_.writeTimeout, config_.closeHandshakeTimeout}) {
        if (timeout.has_value() && timeout->count() <= 0) throw std::invalid_argument("WebSocket client timeout must be greater than zero");
    }
    if (!config_.certificateChainFile.empty() && config_.privateKeyFile.empty()) {
        throw std::invalid_argument("WebSocket client certificate requires a private key");
    }
    for (const auto& header : config_.headers) {
        if (!isValidHttpHeaderName(header.name) || isReservedHandshakeHeader(header.name)) {
            throw std::invalid_argument("invalid or reserved WebSocket client handshake header");
        }
    }
    if (!config_.subprotocols.empty()) {
        bool valid = true;
        std::size_t count = 0;
        httpVisitCommaSeparatedQuotedItems(config_.subprotocols, [&](std::string_view token) {
            valid = valid && !token.empty() && std::ranges::all_of(token, [](char ch) { return isHttpTokenChar(static_cast<unsigned char>(ch)); });
            ++count;
            return valid;
        });
        if (!valid || count == 0) throw std::invalid_argument("invalid WebSocket client subprotocol list");
    }
}

void WebSocketClientState::configureTls() {
    if (config_.tlsPeerVerification == HttpClientTlsPeerVerificationPolicy::kVerify) {
        tlsContext_.set_verify_mode(asio::ssl::verify_peer);
        if (config_.caFile.empty()) tlsContext_.set_default_verify_paths();
        else tlsContext_.load_verify_file(std::string(config_.caFile));
    } else {
        tlsContext_.set_verify_mode(asio::ssl::verify_none);
    }
    if (!config_.privateKeyPassword.empty()) {
        auto password = std::string(config_.privateKeyPassword);
        tlsContext_.set_password_callback([password = std::move(password)](std::size_t, asio::ssl::context_base::password_purpose) { return password; });
    }
    if (!config_.certificateChainFile.empty()) {
        tlsContext_.use_certificate_chain_file(std::string(config_.certificateChainFile));
        tlsContext_.use_private_key_file(std::string(config_.privateKeyFile), asio::ssl::context::pem);
    }
}

void WebSocketClientState::bindStop() {
    std::weak_ptr<WebSocketClientState> weak = shared_from_this();
    stopRegistration_ = loop_.onStop([weak = std::move(weak)] {
        if (const auto state = weak.lock()) state->closeOnWorker(AbortReason::kClosing);
    });
}

void WebSocketClientState::requireCurrent() const {
    if (!worker_.isCurrent()) throw std::logic_error("WebSocket client must be used on its bound event loop");
}

void WebSocketClientState::requireOpen() const {
    requireCurrent();
    if (phase_.load(std::memory_order_acquire) != Phase::kOpen) {
        throw WebSocketClientError(WebSocketClientError::Code::kInvalidState, "WebSocket client is not connected");
    }
}

std::uint16_t WebSocketClientState::port() const noexcept {
    return config_.port.value_or(config_.scheme == WebSocketScheme::kWss ? 443 : 80);
}

bool WebSocketClientState::generateMask(void*, WsMaskKey& key) noexcept {
    return RAND_bytes(reinterpret_cast<unsigned char*>(key.data()), static_cast<int>(key.size())) == 1;
}

void WebSocketClientState::arm(asio::steady_timer& timer, std::optional<std::chrono::milliseconds> timeout, AbortReason reason) {
    if (!timeout.has_value()) return;
    timer.expires_after(*timeout);
    std::weak_ptr<WebSocketClientState> weak = shared_from_this();
    timer.async_wait([weak = std::move(weak), reason](const std::error_code& error) {
        if (!error) {
            if (const auto state = weak.lock()) state->closeOnWorker(reason);
        }
    });
}

void WebSocketClientState::disarm(asio::steady_timer& timer) noexcept {
    std::error_code ignored;
    timer.cancel(ignored);
}

std::optional<std::chrono::milliseconds> WebSocketClientState::effectiveTimeout(const OperationOptions& options, std::optional<std::chrono::milliseconds> configured) const {
    if (!options.timeout.has_value()) return configured;
    if (!configured.has_value()) return options.timeout;
    return std::min(*options.timeout, *configured);
}

void WebSocketClientState::throwAbort() const {
    switch (abortReason_) {
        case AbortReason::kNone: return;
        case AbortReason::kTimeout: throw WebSocketClientError(WebSocketClientError::Code::kTimeout, "WebSocket client operation timed out");
        case AbortReason::kCancelled: throw WebSocketClientError(WebSocketClientError::Code::kCancelled, "WebSocket client operation was cancelled");
        case AbortReason::kClosing: throw WebSocketClientError(WebSocketClientError::Code::kClosing, "WebSocket client is closing");
    }
}

void WebSocketClientState::closeOnWorker(AbortReason reason) noexcept {
    if (!worker_.isCurrent()) std::terminate();
    const auto previous = phase_.exchange(Phase::kClosed, std::memory_order_acq_rel);
    if (previous == Phase::kClosed) return;
    if (abortReason_ == AbortReason::kNone) abortReason_ = reason;
    stopSource_.requestStop();
    resolver_.cancel();
    disarm(connectTimer_);
    disarm(readTimer_);
    disarm(writeTimer_);
    if (protocol_) (void)protocol_->abort();
    std::error_code ignored;
    stream_.lowest_layer().cancel(ignored);
    stream_.lowest_layer().close(ignored);
}

void WebSocketClientState::requestAbort(AbortReason reason) noexcept {
    auto phase = phase_.load(std::memory_order_acquire);
    if (phase == Phase::kClosed) return;
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
        if (!WorkerHandleAccess::deferIfAttached(worker_, [state = std::move(state), reason] { state->closeOnWorker(reason); })) {
            phase_.store(Phase::kClosed, std::memory_order_release);
        }
    } catch (...) {
        phase_.store(Phase::kClosed, std::memory_order_release);
    }
}

void WebSocketClientState::requestClose() noexcept { requestAbort(AbortReason::kClosing); }
void WebSocketClientState::requestCancel() noexcept { requestAbort(AbortReason::kCancelled); }

Task<void> WebSocketClientState::connect() {
    return connectOwned(shared_from_this());
}

Task<void> WebSocketClientState::connectOwned(std::shared_ptr<WebSocketClientState> state) {
    state->requireCurrent();
    auto expected = Phase::kFresh;
    if (!state->phase_.compare_exchange_strong(expected, Phase::kConnecting, std::memory_order_acq_rel)) {
        throw WebSocketClientError(WebSocketClientError::Code::kInvalidState, "WebSocket client connect may only be started once");
    }
    state->abortReason_ = AbortReason::kNone;
    try {
        state->arm(state->connectTimer_, state->config_.connectTimeout, AbortReason::kTimeout);
        std::array<char, 8> portBuffer{};
        const auto [end, error] = std::to_chars(portBuffer.data(), portBuffer.data() + portBuffer.size(), state->port());
        if (error != std::errc{}) throw WebSocketClientError(WebSocketClientError::Code::kInvalidConfig, "invalid WebSocket client port");
        const std::string_view portText(portBuffer.data(), static_cast<std::size_t>(end - portBuffer.data()));
        auto resolved = co_await asyncAsio<asio::ip::tcp::resolver::results_type>([&](auto handler) mutable {
            state->resolver_.async_resolve(state->config_.host, portText, std::move(handler));
        });
        state->throwAbort();
        if (resolved.errorCode()) throw WebSocketClientError(WebSocketClientError::Code::kResolveFailed, resolved.errorCode().message());
        auto endpoints = std::move(resolved).takeResult();
        auto connected = co_await asyncAsio([&](auto handler) mutable {
            asio::async_connect(state->stream_.lowest_layer(), endpoints, std::move(handler));
        });
        state->throwAbort();
        if (connected.errorCode()) throw WebSocketClientError(WebSocketClientError::Code::kConnectFailed, connected.errorCode().message());
        std::error_code ignored;
        if (tcpNoDelayEnabled(state->config_.tcpNoDelay)) state->stream_.lowest_layer().set_option(asio::ip::tcp::no_delay(true), ignored);
        if (tcpKeepAliveEnabled(state->config_.tcpKeepAlive)) state->stream_.lowest_layer().set_option(asio::socket_base::keep_alive(true), ignored);

        if (state->config_.scheme == WebSocketScheme::kWss) {
            SSL_clear(state->stream_.native_handle());
            if (!isIpAddress(state->config_.host) && SSL_set_tlsext_host_name(state->stream_.native_handle(), state->config_.host.c_str()) != 1) {
                throw WebSocketClientError(WebSocketClientError::Code::kTlsFailed, "failed to set WebSocket TLS SNI host");
            }
            if (state->config_.tlsPeerVerification == HttpClientTlsPeerVerificationPolicy::kVerify) {
                state->stream_.set_verify_callback(asio::ssl::host_name_verification(std::string(state->config_.host)));
            }
            static constexpr unsigned char http1[] = {8, 'h', 't', 't', 'p', '/', '1', '.', '1'};
            if (SSL_set_alpn_protos(state->stream_.native_handle(), http1, sizeof(http1)) != 0) {
                throw WebSocketClientError(WebSocketClientError::Code::kTlsFailed, "failed to configure WebSocket TLS ALPN");
            }
            auto handshake = co_await asyncAsio([&](auto handler) mutable {
                state->stream_.async_handshake(asio::ssl::stream_base::client, std::move(handler));
            });
            state->throwAbort();
            if (handshake.errorCode()) throw WebSocketClientError(WebSocketClientError::Code::kTlsFailed, handshake.errorCode().message());
            const auto alpn = selectedAlpn(state->stream_.native_handle());
            if (!alpn.empty() && alpn != "http/1.1") {
                throw WebSocketClientError(WebSocketClientError::Code::kHandshakeRejected, "upstream did not negotiate HTTP/1.1 for WebSocket");
            }
        }
        co_await state->performHandshake({.timeout = state->config_.connectTimeout, .stopToken = state->stopSource_.token()});
        state->protocol_.emplace(state->input_, ProtocolByteLimit::limited(state->config_.maxMessageBytes), WebSocketCompression::kDisabled, WsConnectionRole::kClient, &WebSocketClientState::generateMask, nullptr);
        state->disarm(state->connectTimer_);
        state->phase_.store(Phase::kOpen, std::memory_order_release);
    } catch (...) {
        state->disarm(state->connectTimer_);
        state->closeOnWorker(state->abortReason_ == AbortReason::kNone ? AbortReason::kClosing : state->abortReason_);
        throw;
    }
}

Task<void> WebSocketClientState::performHandshake(OperationOptions options) {
    std::array<std::uint8_t, 16> nonce{};
    if (RAND_bytes(nonce.data(), static_cast<int>(nonce.size())) != 1) {
        throw WebSocketClientError(WebSocketClientError::Code::kHandshakeRejected, "failed to generate WebSocket handshake key");
    }
    std::array<char, base64EncodedSize(nonce.size())> key{};
    encodeBase64(key.data(), nonce);
    const std::string_view keyView(key.data(), key.size());

    std::pmr::vector<HttpHeaderView> headers(memory_.resource());
    headers.reserve(config_.headers.size() + 6);
    for (const auto& header : config_.headers) headers.emplace_back(header.name, header.value);
    headers.emplace_back("Upgrade", "websocket");
    headers.emplace_back("Connection", "Upgrade");
    headers.emplace_back("Sec-WebSocket-Key", keyView);
    headers.emplace_back("Sec-WebSocket-Version", "13");
    if (!config_.subprotocols.empty()) headers.emplace_back("Sec-WebSocket-Protocol", config_.subprotocols);
    if (!config_.userAgent.empty()) headers.emplace_back("User-Agent", config_.userAgent);

    std::array<char, kMaxHttpHeaderBytes + 1024> requestBuffer{};
    const auto origin = config_.scheme == WebSocketScheme::kWss
        ? HttpOriginView::https({.host = config_.host, .port = port()})
        : HttpOriginView::http({.host = config_.host, .port = port()});
    auto preparedResult = Http1ClientRequestWriter({.resource = memory_.resource()}).prepare(
        origin, {.method = "GET", .target = config_.target, .headers = headers}, requestBuffer);
    const auto* prepared = preparedResult.prepared();
    if (prepared == nullptr) {
        throw WebSocketClientError(WebSocketClientError::Code::kInvalidConfig,
            preparedResult.failure() ? std::string(http1ClientRequestPrepareErrorMessage(preparedResult.failure()->error())) : "WebSocket handshake request head is too large");
    }
    Http1ClientResponseParser parser(prepared->exchangeState(), {.resource = memory_.resource()});
    co_await writeTransport(prepared->head(), options, config_.writeTimeout);

    std::array<char, 16384> bytes{};
    for (;;) {
        auto result = parser.parse(input_);
        if (const auto* failure = result.failure()) {
            throw WebSocketClientError(WebSocketClientError::Code::kProtocolError, std::string(http1ClientResponseParseErrorMessage(failure->error())));
        }
        if (result.needMore()) {
            if (input_.size() >= kMaxHttpHeaderBytes) throw WebSocketClientError(WebSocketClientError::Code::kProtocolError, "WebSocket handshake response head is too large");
            const auto read = co_await readTransport(bytes, options, config_.connectTimeout);
            if (read == 0) throw WebSocketClientError(WebSocketClientError::Code::kIoError, "upstream closed during WebSocket handshake");
            input_.append(bytes.data(), read);
            continue;
        }
        auto* parsed = result.parsed();
        if (parsed == nullptr || parsed->plan().protocolUpgrade() == nullptr || parsed->head().status() != http_status::kSwitchingProtocols) {
            throw WebSocketClientError(WebSocketClientError::Code::kHandshakeRejected, "upstream rejected the WebSocket upgrade");
        }

        WebSocketAcceptKey expectedAccept{};
        encodeWebSocketAccept(expectedAccept, keyView);
        std::size_t acceptCount = 0;
        std::size_t protocolCount = 0;
        std::size_t extensionCount = 0;
        bool acceptMatches = false;
        bool upgrade = false;
        bool connectionUpgrade = false;
        std::string_view selected;
        for (const auto& header : parsed->head().headers()) {
            if (httpAsciiEqualsIgnoreCase(header.name(), "Sec-WebSocket-Accept")) {
                ++acceptCount;
                acceptMatches = httpTrimOws(header.value()) == std::string_view(expectedAccept.data(), expectedAccept.size());
            } else if (httpAsciiEqualsIgnoreCase(header.name(), "Upgrade")) {
                upgrade = upgrade || httpHasToken(header.value(), "websocket");
            } else if (httpAsciiEqualsIgnoreCase(header.name(), "Connection")) {
                connectionUpgrade = connectionUpgrade || httpHasToken(header.value(), "upgrade");
            } else if (httpAsciiEqualsIgnoreCase(header.name(), "Sec-WebSocket-Protocol")) {
                ++protocolCount;
                selected = httpTrimOws(header.value());
            } else if (httpAsciiEqualsIgnoreCase(header.name(), "Sec-WebSocket-Extensions")) {
                ++extensionCount;
            }
        }
        if (acceptCount != 1 || !acceptMatches || !upgrade || !connectionUpgrade || extensionCount != 0 || protocolCount > 1 ||
            (protocolCount == 1 && !offeredSubprotocol(config_.subprotocols, selected))) {
            throw WebSocketClientError(WebSocketClientError::Code::kHandshakeRejected, "invalid WebSocket handshake response");
        }
        selectedSubprotocol_.assign(selected);
        input_.erase(0, parsed->consumedBytes());
        co_return;
    }
}

Task<void> WebSocketClientState::writeTransport(std::string_view bytes, OperationOptions options, std::optional<std::chrono::milliseconds> configuredTimeout) {
    if (bytes.empty()) co_return;
    StopRegistration cancellation;
    if (options.stopToken.stoppable()) options.stopToken.registerCallback(cancellation, StopAbort{weak_from_this()});
    if (options.stopToken.stopRequested()) closeOnWorker(AbortReason::kCancelled);
    arm(writeTimer_, effectiveTimeout(options, configuredTimeout), AbortReason::kTimeout);
    const auto completion = config_.scheme == WebSocketScheme::kWss
        ? co_await asyncAsio<std::size_t>([&](auto handler) mutable { asio::async_write(stream_, asio::buffer(bytes), std::move(handler)); })
        : co_await asyncAsio<std::size_t>([&](auto handler) mutable { asio::async_write(stream_.next_layer(), asio::buffer(bytes), std::move(handler)); });
    disarm(writeTimer_);
    cancellation.reset();
    throwAbort();
    if (completion.errorCode()) {
        const auto code = config_.scheme == WebSocketScheme::kWss ? WebSocketClientError::Code::kTlsFailed : WebSocketClientError::Code::kIoError;
        throw WebSocketClientError(code, completion.errorCode().message());
    }
}

Task<std::size_t> WebSocketClientState::readTransport(std::span<char> output, OperationOptions options, std::optional<std::chrono::milliseconds> configuredTimeout) {
    StopRegistration cancellation;
    if (options.stopToken.stoppable()) options.stopToken.registerCallback(cancellation, StopAbort{weak_from_this()});
    if (options.stopToken.stopRequested()) closeOnWorker(AbortReason::kCancelled);
    arm(readTimer_, effectiveTimeout(options, configuredTimeout), AbortReason::kTimeout);
    const auto completion = config_.scheme == WebSocketScheme::kWss
        ? co_await asyncAsio<std::size_t>([&](auto handler) mutable { stream_.async_read_some(asio::buffer(output.data(), output.size()), std::move(handler)); })
        : co_await asyncAsio<std::size_t>([&](auto handler) mutable { stream_.next_layer().async_read_some(asio::buffer(output.data(), output.size()), std::move(handler)); });
    disarm(readTimer_);
    cancellation.reset();
    throwAbort();
    if (completion.errorCode() == asio::error::eof || completion.errorCode() == asio::ssl::error::stream_truncated) co_return 0;
    if (completion.errorCode()) {
        const auto code = config_.scheme == WebSocketScheme::kWss ? WebSocketClientError::Code::kTlsFailed : WebSocketClientError::Code::kIoError;
        throw WebSocketClientError(code, completion.errorCode().message());
    }
    co_return completion.result();
}

Task<void> WebSocketClientState::flushOutput(OperationOptions options) {
    for (;;) {
        const auto plan = protocol_->outputPlan();
        if (!plan.bytes().empty()) {
            co_await writeTransport(plan.bytes(), options, config_.writeTimeout);
            if (protocol_->consumeOutput(plan.bytes().size()) == WsOutputConsumeStatus::kOutOfRange) std::terminate();
            continue;
        }
        if (plan.disposition() == WsTransportDisposition::kEndTransport) {
            protocol_->commitTransportEnd();
            closeOnWorker(AbortReason::kNone);
        }
        co_return;
    }
}

ScopedOperation<std::optional<WebSocketMessage>> WebSocketClientState::read(OperationOptions options) {
    validateOperationOptions(options);
    return makeScopedOperation(operationScope_, readOwned(shared_from_this(), std::move(options)));
}

Task<std::optional<WebSocketMessage>> WebSocketClientState::readOwned(std::shared_ptr<WebSocketClientState> state, OperationOptions options) {
    state->requireOpen();
    ActivityGuard guard(state->readActive_, "concurrent WebSocket client reads are not supported");
    std::array<char, 16384> bytes{};
    for (;;) {
        const auto event = state->protocol_->poll();
        if (!event.has_value()) {
            const auto count = co_await state->readTransport(bytes, options, state->config_.readTimeout);
            if (count == 0) {
                state->protocol_->notifyTransportEof();
                state->closeOnWorker(AbortReason::kNone);
                co_return std::nullopt;
            }
            state->input_.append(bytes.data(), count);
            continue;
        }
        if (const auto* message = event->message()) co_return WebSocketMessageAccess::make(message->opcode(), message->payload());
        if (event->ping() != nullptr) {
            co_await state->flushOutput(options);
            continue;
        }
        if (event->pong() != nullptr) continue;
        if (event->close() != nullptr || event->protocolError() != nullptr || event->transportEnd() != nullptr) {
            co_await state->flushOutput(options);
            co_return std::nullopt;
        }
    }
}

ScopedOperation<void> WebSocketClientState::write(WebSocketOpcode opcode, std::string_view payload, OperationOptions options) {
    validateOperationOptions(options);
    std::pmr::string owned(payload, memory_.resource());
    return makeScopedOperation(operationScope_, writeOwned(shared_from_this(), opcode, std::move(owned), std::move(options)));
}

Task<void> WebSocketClientState::writeOwned(std::shared_ptr<WebSocketClientState> state, WebSocketOpcode opcode, std::pmr::string payload, OperationOptions options) {
    state->requireOpen();
    ActivityGuard guard(state->writeActive_, "concurrent WebSocket client writes are not supported");
    const auto submitted = state->protocol_->submitFrame(opcode, payload);
    switch (submitted) {
        case WsFrameSubmitStatus::kAccepted: break;
        case WsFrameSubmitStatus::kMessageTooLarge: throw WebSocketClientError(WebSocketClientError::Code::kMessageTooLarge, "WebSocket client message exceeds configured limit");
        case WsFrameSubmitStatus::kNotOpen: throw WebSocketClientError(WebSocketClientError::Code::kClosing, "WebSocket client is closing");
        default: throw WebSocketClientError(WebSocketClientError::Code::kProtocolError, "invalid WebSocket client frame payload");
    }
    co_await state->flushOutput(options);
}

ScopedOperation<void> WebSocketClientState::close(WebSocketCloseOptions options, OperationOptions operationOptions) {
    validateOperationOptions(operationOptions);
    std::pmr::string reason(options.reason.view(), memory_.resource());
    return makeScopedOperation(operationScope_, closeOwned(shared_from_this(), options, std::move(reason), std::move(operationOptions)));
}

Task<void> WebSocketClientState::closeOwned(std::shared_ptr<WebSocketClientState> state, WebSocketCloseOptions options, std::pmr::string reason, OperationOptions operationOptions) {
    state->requireOpen();
    ActivityGuard readGuard(state->readActive_, "WebSocket client close cannot overlap read");
    ActivityGuard writeGuard(state->writeActive_, "WebSocket client close cannot overlap write");
    const auto submitted = state->protocol_->submitClose(options.code, reason);
    if (submitted != WsCloseSubmitStatus::kAccepted) {
        throw WebSocketClientError(WebSocketClientError::Code::kProtocolError, "invalid WebSocket client close payload");
    }
    state->phase_.store(Phase::kClosing, std::memory_order_release);
    co_await state->flushOutput(operationOptions);
    std::array<char, 4096> bytes{};
    for (;;) {
        const auto event = state->protocol_->poll();
        if (!event.has_value()) {
            const auto count = co_await state->readTransport(bytes, operationOptions, state->config_.closeHandshakeTimeout);
            if (count == 0) {
                state->protocol_->notifyTransportEof();
                state->closeOnWorker(AbortReason::kNone);
                co_return;
            }
            state->input_.append(bytes.data(), count);
            continue;
        }
        if (event->ping() != nullptr) {
            co_await state->flushOutput(operationOptions);
            continue;
        }
        if (event->close() != nullptr || event->protocolError() != nullptr || event->transportEnd() != nullptr) {
            co_await state->flushOutput(operationOptions);
            if (state->phase_.load(std::memory_order_acquire) != Phase::kClosed) state->closeOnWorker(AbortReason::kNone);
            co_return;
        }
    }
}

WebSocketClientHandle WebSocketClientState::handle(OperationOptions options) {
    requireOpen();
    validateOperationOptions(options);
    options = mergeOperationOptions(OperationOptions{.stopToken = stopSource_.token()}, std::move(options));
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

WebSocketClientHandle::WebSocketClientHandle(std::shared_ptr<detail::WebSocketClientState> state, detail::ScopedOperationScope& scope, OperationOptions options) noexcept
    : detail::ScopedCapabilityNode(scope, &WebSocketClientHandle::expireCapability), state_(std::move(state)), options_(std::move(options)) {}

WebSocketClientHandle::WebSocketClientHandle(const WebSocketClientHandle& other) noexcept
    : detail::ScopedCapabilityNode(other), state_(other.state_), options_(other.options_) {}

void WebSocketClientHandle::expireCapability(detail::ScopedCapabilityNode& capability) noexcept {
    static_cast<WebSocketClientHandle&>(capability).state_.reset();
}

WebSocketClientHandle WebSocketClientHandle::withOptions(OperationOptions options) const {
    requireActive();
    return WebSocketClientHandle(state_, operationScope(), detail::mergeOperationOptions(options_, std::move(options)));
}

ScopedOperation<std::optional<WebSocketMessage>> WebSocketClientHandle::read() const {
    requireActive();
    return state_->read(options_);
}

ScopedOperation<void> WebSocketClientHandle::text(std::string_view payload) const { requireActive(); return state_->write(WebSocketOpcode::kText, payload, options_); }
ScopedOperation<void> WebSocketClientHandle::binary(std::string_view payload) const { requireActive(); return state_->write(WebSocketOpcode::kBinary, payload, options_); }
ScopedOperation<void> WebSocketClientHandle::ping(std::string_view payload) const { requireActive(); return state_->write(WebSocketOpcode::kPing, payload, options_); }
ScopedOperation<void> WebSocketClientHandle::pong(std::string_view payload) const { requireActive(); return state_->write(WebSocketOpcode::kPong, payload, options_); }
ScopedOperation<void> WebSocketClientHandle::close(WebSocketCloseOptions options) const { requireActive(); return state_->close(options, options_); }
void WebSocketClientHandle::abort() noexcept { if (state_) state_->requestClose(); }

WebSocketClient::WebSocketClient(EventLoop loop, WebSocketClientConfig config)
    : state_(std::make_shared<detail::WebSocketClientState>(std::move(loop), std::move(config))) { state_->bindStop(); }
WebSocketClient::~WebSocketClient() { state_->requestClose(); }
Task<void> WebSocketClient::connect() { return state_->connect(); }
WebSocketClientHandle WebSocketClient::withOptions(OperationOptions options) const { return state_->handle(std::move(options)); }
ScopedOperation<std::optional<WebSocketMessage>> WebSocketClient::read() const { return withOptions({}).read(); }
ScopedOperation<void> WebSocketClient::text(std::string_view payload) const { return withOptions({}).text(payload); }
ScopedOperation<void> WebSocketClient::binary(std::string_view payload) const { return withOptions({}).binary(payload); }
ScopedOperation<void> WebSocketClient::ping(std::string_view payload) const { return withOptions({}).ping(payload); }
ScopedOperation<void> WebSocketClient::pong(std::string_view payload) const { return withOptions({}).pong(payload); }
ScopedOperation<void> WebSocketClient::close(WebSocketCloseOptions options) const { return withOptions({}).close(options); }
void WebSocketClient::close() noexcept { state_->requestClose(); }
bool WebSocketClient::connected() const { return state_->connected(); }
std::string_view WebSocketClient::subprotocol() const& { return state_->subprotocol(); }
const WorkerHandle& WebSocketClient::worker() const& noexcept { return state_->worker(); }

}  // namespace ruvia
