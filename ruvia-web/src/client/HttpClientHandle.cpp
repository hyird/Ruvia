#include "ruvia/web/HttpClientHandle.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <limits>
#include <stdexcept>
#include <utility>

#include "ruvia/core/memory/PmrObject.h"
#include "ruvia/core/memory/PmrResource.h"
#include "ruvia/http/HttpAscii.h"
#include "ruvia/http/HttpConnectUdp.h"
#include "ruvia/http/HttpHeader.h"
#include "ruvia/http/HttpKnownMethod.h"
#include "ruvia/http/HttpRequestTarget.h"
#include "ruvia/web/Context.h"
#include "ruvia/web/Streaming.h"

#include "client/HttpClientConfigValidation.h"
#include "client/HttpClientPool.h"
#include "client/HttpClientRequestStorage.h"
#include "client/HttpClientResponseDecoding.h"
#include "client/HttpClientResponseState.h"
#include "client/HttpClientResultBudget.h"
#include "context/ContextServices.h"
#include "http3/Http3ClientConnection.h"

namespace ruvia {
namespace {

constexpr std::size_t kResponseBodyReadChunkBytes = std::size_t{16} * 1024;

void checkResponseOperationAffinity(void* target) noexcept {
    const auto& state = *static_cast<const detail::HttpClientResponseState*>(target);
    if (auto* domain = state.memoryDomain(); domain != nullptr && !domain->worker().isCurrent()) {
        std::terminate();
    }
}

}  // namespace

HttpClientResponse::HttpClientResponse(detail::HttpClientPool& pool)
    : state_(pool.responseMemory_->createState(pool)),
      body_(state_) {}

HttpClientResponse::HttpClientResponse(detail::HttpClientResponseState* state, bool retain) noexcept
    : state_(state),
      body_(state),
      consumer_(false) {
    if (retain && state_ != nullptr) {
        state_->retainReference();
    }
}

HttpClientResponse::HttpClientResponse(HttpClientResponse&& other) noexcept
    : state_(std::exchange(other.state_, nullptr)),
      body_(state_),
      consumer_(other.consumer_) {
    other.body_.state_ = nullptr;
}

HttpClientResponse& HttpClientResponse::operator=(HttpClientResponse&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    release();
    state_ = std::exchange(other.state_, nullptr);
    consumer_ = other.consumer_;
    body_.state_ = state_;
    other.body_.state_ = nullptr;
    other.consumer_ = false;
    return *this;
}

HttpClientResponse::~HttpClientResponse() {
    release();
}

void HttpClientResponse::release() noexcept {
    if (state_ == nullptr) {
        return;
    }
    checkResponseOperationAffinity(state_);
    auto* state = std::exchange(state_, nullptr);
    body_.state_ = nullptr;
    if (consumer_) {
        state->bodyOperationScope.close();
    }
    if (consumer_ && state->http3Connection != nullptr) {
        auto* connection = state->http3Connection;
        const auto requestId = state->http3RequestId;
        if (!state->complete && !state->abandoned) {
            connection->abandonResponse(requestId);
        }
        connection->consumerReleased(requestId);
    } else if (consumer_ && !state->complete && !state->abandoned && state->pool != nullptr) {
        state->pool->abandonResponse(*state);
    }
    state->releaseReference();
}

std::span<const HttpClientInformationalResponse> HttpClientResponse::informationalResponses() const& noexcept {
    return state_->informational;
}

void detail::HttpClientResponseState::retainInformational(HttpStatusCode status_code, std::span<const HttpHeaderView> fields) {
    // Retained progress metadata has a separate fixed aggregate bound. It must
    // never grow with the duration of an upstream response stream.
    if (informational.size() >= 8) {
        throw HttpClientError(HttpClientError::Code::kProtocolError, "too many informational response heads");
    }
    HttpClientInformationalResponse head(status_code, resource);
    head.headers_.reserve(fields.size());
    std::size_t bytes = informationalFieldBytes;
    for (const auto& field : fields) {
        if (bytes > kMaxHttpHeaderBytes - 32 || field.name().size() > kMaxHttpHeaderBytes - bytes - 32 || field.value().size() > kMaxHttpHeaderBytes - bytes - 32 - field.name().size()) {
            throw HttpClientError(HttpClientError::Code::kProtocolError, "informational response metadata exceeds byte limit");
        }
        bytes += 32 + field.name().size() + field.value().size();
        head.headers_.push_back(HttpHeader::copyOf(field.name(), field.value(), resource));
    }
    informational.push_back(std::move(head));
    informationalFieldBytes = bytes;
}

HttpStatusCode HttpClientResponse::status() const noexcept {
    return state_->status;
}
HttpProtocolVersion HttpClientResponse::protocolVersion() const noexcept {
    return state_->protocolVersion;
}
void HttpClientResponse::reprioritize(HttpPriority priority) & {
    if (state_ == nullptr) {
        throw std::logic_error("response owner has moved");
    }
    checkResponseOperationAffinity(state_);
    if (priority.urgency > 7) {
        throw std::invalid_argument("HTTP priority urgency must be between 0 and 7");
    }
    if (state_->complete || state_->abandoned || state_->pool == nullptr) {
        throw HttpClientError(HttpClientError::Code::kClosing, "response transport is retired");
    }
    state_->pool->reprioritize(*state_, priority);
}
std::span<const HttpHeader> HttpClientResponse::headers() const& noexcept {
    return state_->headers;
}
std::span<const HttpHeader> HttpClientResponse::trailers() const& noexcept {
    return state_->trailers;
}

void detail::HttpClientResponseState::retainReference() noexcept {
    checkResponseOperationAffinity(this);
    if (references == std::numeric_limits<std::size_t>::max()) {
        std::terminate();
    }
    ++references;
}

void detail::HttpClientResponseState::releaseReference() noexcept {
    checkResponseOperationAffinity(this);
    if (references == 0) {
        std::terminate();
    }
    if (--references == 0) {
        if (auto* domain = memoryDomain()) {
            domain->destroyState(this);
        } else {
            detail::destroyPmrObject(this, resource);
        }
    }
}

void detail::HttpClientResponseState::notifyProducerSpace() noexcept {
    // A changed read policy (for example collectAll) can unblock the QUIC
    // driver even when no body storage has been released yet.
    http3BodyBudget.notifyProducer();
    spaceSignal.notify();
}

bool detail::HttpClientResponseState::bindHttp3BodyBudget(
    detail::Http3ClientBodyBudget& budget) noexcept {
    return http3BodyBudget.attach(budget, producerBodyBytes());
}

void detail::HttpClientResponseState::releaseHttp3BodyBudget() noexcept {
    if (http3BodyBudget.retainedBytes() != 0) {
        std::terminate();
    }
    http3BodyBudget.reset();
}

bool detail::HttpClientResponseState::hasHttp3BodyBudget() const noexcept {
    return http3BodyBudget.attached();
}

std::size_t detail::HttpClientResponseState::producerBodyBytes() const noexcept {
    if (pending.size() > std::numeric_limits<std::size_t>::max() - buffered.size()) {
        std::terminate();
    }
    return buffered.size() + pending.size();
}

std::size_t detail::HttpClientResponseState::producerBodyBudgetAvailable() const noexcept {
    return http3BodyBudget.available();
}

bool detail::HttpClientResponseState::retainProducerBodyBytes(std::size_t bytes) noexcept {
    return !http3BodyBudget.attached() || http3BodyBudget.tryRetain(bytes);
}

void detail::HttpClientResponseState::releaseProducerBodyBytes(std::size_t bytes) noexcept {
    if (http3BodyBudget.attached()) {
        http3BodyBudget.release(bytes);
    }
}

void detail::HttpClientResponseState::reconcileProducerBodyBytes() noexcept {
    if (!http3BodyBudget.attached()) {
        return;
    }
    if (!http3BodyBudget.tryReplace(producerBodyBytes())) {
        std::terminate();
    }
}

bool detail::HttpClientResponseState::replaceProducerBodyBytes(std::size_t bytes) noexcept {
    return !http3BodyBudget.attached() || http3BodyBudget.tryReplace(bytes);
}

void detail::HttpClientResponseState::discardPendingBody() noexcept {
    {
        std::pmr::string empty(resource);
        pending.swap(empty);
    }
    reconcileProducerBodyBytes();
    notifyProducerSpace();
}

void detail::HttpClientResponseState::discardResponseBody() noexcept {
    {
        std::pmr::string emptyBuffered(resource);
        std::pmr::string emptyPending(resource);
        buffered.swap(emptyBuffered);
        pending.swap(emptyPending);
    }
    offset = 0;
    reconcileProducerBodyBytes();
    notifyProducerSpace();
}

void detail::HttpClientResponseState::releaseConsumedBodyPrefix() {
    if (offset == 0) {
        return;
    }
    if (offset > buffered.size()) {
        std::terminate();
    }
    if (offset == buffered.size()) {
        {
            std::pmr::string empty(resource);
            buffered.swap(empty);
        }
    } else {
        buffered.erase(0, offset);
    }
    offset = 0;
    reconcileProducerBodyBytes();
    notifyProducerSpace();
}

void detail::HttpClientResponseState::promotePendingData() {
    auto& state = *this;
    if (state.offset != state.buffered.size() || state.pending.empty()) {
        return;
    }
    if (state.http2DataCredit && state.pool != nullptr) {
        state.pool->releaseResponseData(state);
    }
    std::pmr::string empty(state.resource);
    state.buffered.swap(empty);
    state.offset = 0;
    state.buffered.swap(state.pending);
    state.notifyProducerSpace();
}

bool HttpClientResponseBody::complete() const noexcept {
    return state_ == nullptr || (state_->receiveComplete() && state_->offset == state_->buffered.size() &&
                                    state_->pending.empty());
}

ScopedOperation<std::optional<std::span<const std::byte>>> HttpClientResponseBody::read() & {
    if (state_->bodyOperationScope.has_pending_operations()) {
        throw std::logic_error("HTTP client response body operation is already active");
    }
    checkResponseOperationAffinity(state_);
    return ::ruvia::make_scoped_operation(state_->bodyOperationScope,
        state_->consume_body<std::span<const std::byte>>(), checkResponseOperationAffinity, state_);
}

ScopedOperation<std::optional<std::string_view>> HttpClientResponseBody::text() & {
    if (state_->bodyOperationScope.has_pending_operations()) {
        throw std::logic_error("HTTP client response body operation is already active");
    }
    checkResponseOperationAffinity(state_);
    return ::ruvia::make_scoped_operation(state_->bodyOperationScope,
        state_->consume_body<std::string_view>(), checkResponseOperationAffinity, state_);
}

template <typename view>
Task<std::conditional_t<std::is_void_v<view>, void, std::optional<view>>> detail::HttpClientResponseState::consume_body(ResponseStreamWriter* output) {
    auto& state = *this;
    for (;;) {
        // Reclaim a returned borrow only at the next consumption step. A pipe
        // commits its cursor only after the downstream write succeeds.
        state.releaseConsumedBodyPrefix();
        state.incrementalRead = true;
        while ((state.bodyDecodeRequired && !state.receiveComplete()) ||
               (state.buffered.empty() && state.pending.empty() && !state.receiveComplete())) {
            co_await state.dataSignal.wait();
        }
        promotePendingData();
        if (state.offset == state.buffered.size()) {
            if (state.failure) {
                std::rethrow_exception(state.failure);
            }
            if (state.errorCode) {
                constexpr auto message = std::is_void_v<view> ? "HTTP response body forwarding failed" : "HTTP response body read failed";
                throw HttpClientError(static_cast<HttpClientError::Code>(*state.errorCode), message);
            }
            state.discardResponseBody();
            if constexpr (std::is_void_v<view>) {
                co_return;
            } else {
                co_return std::nullopt;
            }
        }
        const auto count = std::min(kResponseBodyReadChunkBytes, state.buffered.size() - state.offset);
        const auto chunk = std::string_view(state.buffered).substr(state.offset, count);
        if constexpr (std::is_void_v<view>) {
            co_await output->write(std::as_bytes(std::span(chunk.data(), chunk.size())));
            state.offset += count;
        } else {
            state.offset += count;
            if constexpr (std::same_as<view, std::string_view>) {
                co_return chunk;
            } else {
                co_return std::as_bytes(std::span(chunk.data(), chunk.size()));
            }
        }
    }
}

template Task<std::optional<std::span<const std::byte>>> detail::HttpClientResponseState::consume_body<std::span<const std::byte>>(ResponseStreamWriter*);
template Task<std::optional<std::string_view>> detail::HttpClientResponseState::consume_body<std::string_view>(ResponseStreamWriter*);
template Task<void> detail::HttpClientResponseState::consume_body<void>(ResponseStreamWriter*);

ScopedOperation<HttpClientResponseBytes> HttpClientResponseBody::readAll(std::size_t maxBytes) & {
    if (state_->bodyOperationScope.has_pending_operations()) {
        throw std::logic_error("HTTP client response body operation is already active");
    }
    checkResponseOperationAffinity(state_);
    return ::ruvia::make_scoped_operation(state_->bodyOperationScope,
        state_->readAll(maxBytes), checkResponseOperationAffinity, state_);
}

Task<HttpClientResponseBytes> detail::HttpClientResponseState::readAll(std::size_t maxBytes) {
    auto& state = *this;
    state.releaseConsumedBodyPrefix();
    state.collectAll = true;
    if (state.http2DataCredit && state.pool != nullptr) {
        state.pool->releaseResponseData(state);
    }
    state.notifyProducerSpace();
    while (!state.receiveComplete()) {
        co_await state.dataSignal.wait();
    }
    if (state.failure) {
        std::rethrow_exception(state.failure);
    }
    if (state.errorCode) {
        throw HttpClientError(
            static_cast<HttpClientError::Code>(*state.errorCode), "HTTP response body read failed");
    }
    const auto remaining = state.buffered.size() - state.offset;
    const auto effectiveLimit = std::min(maxBytes, state.bufferedLimit);
    if (state.pending.size() > effectiveLimit ||
        remaining > effectiveLimit - state.pending.size()) {
        throw HttpClientError(HttpClientError::Code::kResponseTooLarge,
            "HTTP response body exceeds readAll byte limit");
    }
    const auto totalRemaining = remaining + state.pending.size();
    if (state.resultBudgetDomain == nullptr) {
        throw std::logic_error("HTTP client response has no result byte budget");
    }
    auto reservation = detail::HttpClientResultBudgetLease::tryAcquire(
        *state.resultBudgetDomain, totalRemaining);
    if (!reservation) {
        throw HttpClientError(HttpClientError::Code::kResultBudgetExceeded,
            "HTTP client retained result byte budget is exhausted");
    }
    HttpClientResponseBytes result(totalRemaining, std::move(*reservation));
    if (remaining != 0) {
        result.append(std::as_bytes(std::span(state.buffered).subspan(state.offset)));
    }
    if (!state.pending.empty()) {
        result.append(std::as_bytes(std::span(state.pending)));
    }
    // The result uses independent thread-safe storage. Only release the
    // worker-owned response buffers after the complete copy succeeds.
    state.discardResponseBody();
    co_return std::move(result);
}

ScopedOperation<void> HttpClientResponseBody::pipeTo(ResponseStreamWriter& output) & {
    if (state_->bodyOperationScope.has_pending_operations()) {
        throw std::logic_error("HTTP client response body operation is already active");
    }
    checkResponseOperationAffinity(state_);
    return ::ruvia::make_scoped_operation(state_->bodyOperationScope,
        state_->consume_body<void>(&output), checkResponseOperationAffinity, state_);
}

std::optional<std::string_view> HttpClientResponse::header(std::string_view name) const& noexcept {
    const auto match = std::ranges::find_if(state_->headers,
        [name](const auto& header) { return httpAsciiEqualsIgnoreCase(header.name(), name); });
    return match == state_->headers.end() ? std::nullopt
                                          : std::optional<std::string_view>(match->value());
}

std::optional<std::string_view> HttpClientResponse::trailer(std::string_view name) const& noexcept {
    const auto match = std::ranges::find_if(state_->trailers,
        [name](const auto& header) { return httpAsciiEqualsIgnoreCase(header.name(), name); });
    return match == state_->trailers.end() ? std::nullopt
                                           : std::optional<std::string_view>(match->value());
}

HttpClientHandle::HttpClientHandle(detail::HttpClientPool& pool,
    std::pmr::memory_resource* resource, ::ruvia::operation_scope& scope) noexcept
    : pool_(&pool),
      resource_(resource),
      registration_(scope, this, &HttpClientHandle::expire_capability) {}

HttpClientHandle::HttpClientHandle(detail::HttpClientPool& pool,
    std::pmr::memory_resource* resource, ::ruvia::operation_scope& scope,
    OperationOptions options) noexcept
    : pool_(&pool),
      resource_(resource),
      options_(std::move(options)),
      registration_(scope, this, &HttpClientHandle::expire_capability) {}

HttpClientHandle::HttpClientHandle(const HttpClientHandle& other)
    : pool_(other.pool_),
      resource_(other.resource_),
      options_(other.options_),
      registration_(other.registration_, this) {}

void HttpClientHandle::expire_capability(void* target) noexcept {
    static_cast<HttpClientHandle*>(target)->pool_ = nullptr;
}

HttpClientHandle HttpClientHandle::withOptions(OperationOptions options) const {
    detail::validateOperationOptions(options);
    registration_.require_active();
    HttpClientHandle copy(*this);
    copy.options_ = detail::mergeOperationOptions(options_, std::move(options));
    return copy;
}

ScopedOperation<HttpClientResponse> HttpClientHandle::send(
    const HttpClientRequestView& view) const {
    registration_.require_active();
    detail::HttpClientRequestStorage request(
        view.method.view(), view.target.view(), detail::pmrResourceOrDefault(resource_));
    for (const auto& header : view.headers) {
        request.appendHeader(header.name(), header.value());
    }
    if (const auto* bytes = view.content.borrowedBytes()) {
        request.setBody(bytes->value());
    }
    detail::validateOperationOptions(options_);
    return ::ruvia::make_scoped_operation(
        registration_.scope(), pool_->execute(std::move(request), options_));
}

ScopedOperation<HttpClientExchange> HttpClientHandle::openRequest(const HttpClientRequestView& head, HttpClientUploadConfig upload) const {
    registration_.require_active();
    if (head.content.borrowedBytes() != nullptr || upload.maxChunkBytes == 0 || upload.continueTimeout <= std::chrono::milliseconds::zero() ||
        upload.continueTimeout > std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::duration::max()) ||
        (upload.expectation != HttpClientRequestExpectation::kNone && upload.expectation != HttpClientRequestExpectation::kContinue)) {
        throw std::invalid_argument("streaming upload requires a bodyless head and valid upload policy");
    }
    detail::HttpClientRequestStorage request(head.method.view(), head.target.view(), detail::pmrResourceOrDefault(resource_));
    for (const auto& field : head.headers) {
        request.appendHeader(field.name(), field.value());
    }
    return ::ruvia::make_scoped_operation(registration_.scope(), pool_->openRequest(std::move(request), upload, options_));
}

ScopedOperation<HttpClientTunnelResult> HttpClientHandle::openUdpTunnel(const HttpClientUdpTunnelRequestView& head, HttpClientTunnelConfig config) const {
    registration_.require_active();
    auto* resource = detail::pmrResourceOrDefault(resource_);
    std::pmr::vector<HttpHeaderView> fields(resource);
    fields.reserve(head.headers.size() + 1);
    for (const auto& field : head.headers) {
        if (httpAsciiEqualsIgnoreCase(field.name(), "capsule-protocol")) {
            throw std::invalid_argument("CONNECT-UDP Capsule-Protocol is driver-owned");
        }
        fields.push_back(field);
    }
    fields.emplace_back("Capsule-Protocol", "?1");
    auto authority = detail::clientUriHost(host(), resource);
    if (port() != (scheme() == HttpScheme::kHttps ? 443 : 80)) {
        std::array<char, 5> digits;
        const auto end = std::to_chars(digits.data(), digits.data() + digits.size(), port()).ptr;
        authority.push_back(':');
        authority.append(digits.data(), end);
    }
    // openTunnel owns all fields before this temporary preparation storage dies.
    return openTunnel({.authority = authority, .protocol = "connect-udp", .target = head.target, .headers = fields}, config);
}

ScopedOperation<HttpClientTunnelResult> HttpClientHandle::openTunnel(const HttpClientTunnelRequestView& head, HttpClientTunnelConfig config) const {
    registration_.require_active();
    if ((config.datagrams && head.protocol.empty()) || config.maxChunkBytes == 0 || config.maxChunkBytes > kDefaultMaxBufferedBodyBytes ||
        (head.protocol.empty() ? (!isValidHttpConnectAuthority(head.authority) || !head.target.empty()) : (!isValidHttpMethodToken(head.protocol) || !isValidHttpOriginFormTarget(head.target) || !parseHttpAuthorityHost(BorrowedText(head.authority)) || head.authority.empty()))) {
        throw std::invalid_argument("invalid CONNECT request or tunnel policy");
    }
    detail::HttpClientRequestStorage request("CONNECT", head.target, detail::pmrResourceOrDefault(resource_));
    if (head.protocol == "connect-udp" && (validateHttpConnectUdpRequest({.version = HttpProtocolVersion::kHttp2,
                                                                             .scheme = scheme() == HttpScheme::kHttps ? "https" : "http",
                                                                             .authority = head.authority,
                                                                             .path = head.target,
                                                                             .headers = head.headers})
                                                  .index() != 0)) {
        throw std::invalid_argument("invalid CONNECT-UDP request head");
    }
    request.setTunnel(head.authority, head.protocol);
    for (const auto& field : head.headers) {
        request.appendHeader(field.name(), field.value());
    }
    return ::ruvia::make_scoped_operation(registration_.scope(), pool_->openTunnel(std::move(request), config, options_));
}

quic_path_migration HttpClientHandle::start_quic_path_migration(
    const asio::ip::udp::endpoint& local_endpoint) const {
    registration_.require_active();
    return pool_->start_quic_path_migration(local_endpoint);
}

std::optional<quic_path_migration> HttpClientHandle::path_migration(
    std::uint64_t id) const {
    registration_.require_active();
    return pool_->path_migration(id);
}

quic_operation_status HttpClientHandle::cancel_quic_path_migration(std::uint64_t id) const {
    registration_.require_active();
    return pool_->cancel_quic_path_migration(id);
}

HttpClientStats HttpClientHandle::stats() const {
    registration_.require_active();
    return pool_->stats();
}
std::optional<HttpClientPush> HttpClientHandle::nextPush() const {
    registration_.require_active();
    return pool_->nextPush();
}

std::optional<HttpClientAdvertisement> HttpClientHandle::nextAdvertisement() const {
    registration_.require_active();
    return pool_->nextAdvertisement();
}

std::string_view HttpClientHandle::host() const& {
    registration_.require_active();
    return pool_->host();
}

std::uint16_t HttpClientHandle::port() const {
    registration_.require_active();
    return pool_->port();
}

HttpScheme HttpClientHandle::scheme() const {
    registration_.require_active();
    return pool_->scheme();
}

HttpClientHandle Context::httpClient() const {
    return services().clientRegistries().httpClient(operationScope_, capabilities_.stop_token());
}

HttpClientHandle Context::httpClient(std::string_view alias) const {
    return services().clientRegistries().httpClient(alias, operationScope_, capabilities_.stop_token());
}

}  // namespace ruvia
