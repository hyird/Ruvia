#include "ruvia/web/WebSocketClient.h"

#include <algorithm>
#include <chrono>
#include <exception>
#include <optional>
#include <stdexcept>
#include <utility>

#include <openssl/rand.h>

#include "ruvia/core/detail/io/TcpSocketOptions.h"
#include "ruvia/core/detail/worker/WorkerDispatcher.h"
#include "ruvia/web/detail/client/WebSocketClientState.h"

#include "client/WebSocketClientInternal.h"

namespace ruvia::detail {
namespace {

[[nodiscard]] EventLoop requireEventLoop(EventLoop loop) {
    if (!loop.valid()) {
        throw std::invalid_argument("WebSocket client requires a valid event loop");
    }
    return loop;
}

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
    input_.reserve(kWebSocketClientTransportBufferBytes);
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
