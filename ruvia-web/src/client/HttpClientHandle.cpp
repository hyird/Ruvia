#include "ruvia/web/HttpClientHandle.h"
#include "ruvia/web/detail/client/HttpClientRequestStorage.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <stdexcept>
#include <utility>

#include "ruvia/core/memory/PmrResource.h"
#include "ruvia/http/HttpHeader.h"
#include "ruvia/http/detail/client/HttpClientContentEncoding.h"
#include "ruvia/web/Context.h"
#include "ruvia/web/Streaming.h"
#include "ruvia/web/detail/client/HttpClientRegistry.h"
#include "ruvia/web/detail/client/HttpClientConfigValidation.h"
#include "client/HttpClientResponseState.h"
#include "ruvia/core/memory/PmrObject.h"

namespace ruvia {
namespace {

constexpr std::size_t kResponseBodyReadChunkBytes = std::size_t{16} * 1024;

bool headerNameEquals(std::string_view left, std::string_view right) noexcept {
    return left.size() == right.size() && std::equal(left.begin(), left.end(), right.begin(), [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); });
}

}  // namespace

HttpClientResponse::HttpClientResponse(std::pmr::memory_resource* resource, const WorkerHandle& worker, detail::HttpClientPool& pool)
    : state_(detail::constructPmrObject<detail::HttpClientResponseState>(detail::pmrResourceOrDefault(resource), worker, detail::pmrResourceOrDefault(resource))),
      body_(state_) {
    state_->pool = &pool;
}

HttpClientResponse::HttpClientResponse(detail::HttpClientResponseState* state, bool retain) noexcept
    : state_(state),
      body_(state),
      consumer_(false) {
    if (retain && state_ != nullptr) {
        ++state_->references;
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
    std::swap(state_, other.state_);
    std::swap(consumer_, other.consumer_);
    body_.state_ = state_;
    other.body_.state_ = other.state_;
    return *this;
}

HttpClientResponse::~HttpClientResponse() {
    release();
}

void HttpClientResponse::release() noexcept {
    if (state_ == nullptr) {
        return;
    }
    auto* state = std::exchange(state_, nullptr);
    body_.state_ = nullptr;
    if (consumer_) {
        state->bodyOperationScope.close();
    }
    if (consumer_ && !state->complete && !state->abandoned && state->pool != nullptr) {
        state->pool->abandonResponse(*state);
    }
    if (state->references == 0) {
        std::terminate();
    }
    if (--state->references == 0) {
        detail::destroyPmrObject(state, state->resource);
    }
}

HttpStatusCode HttpClientResponse::status() const noexcept {
    return state_->status;
}
HttpProtocolVersion HttpClientResponse::protocolVersion() const noexcept {
    return state_->protocolVersion;
}
std::span<const HttpClientResponseHeader> HttpClientResponse::headers() const& noexcept {
    return state_->headers;
}
std::span<const HttpClientResponseHeader> HttpClientResponse::trailers() const& noexcept {
    return state_->trailers;
}

bool HttpClientResponseBody::complete() const noexcept {
    return state_ == nullptr || (state_->complete && state_->offset == state_->buffered.size() && state_->pending.empty());
}

ScopedOperation<std::optional<std::string_view>> HttpClientResponseBody::read() & {
    if (state_->bodyOperationScope.hasPendingOperations()) {
        throw std::logic_error("HTTP client response body operation is already active");
    }
    return detail::makeScopedOperation(state_->bodyOperationScope, readTask(*state_));
}

Task<std::optional<std::string_view>> HttpClientResponseBody::readTask(detail::HttpClientResponseState& state) {
    state.incrementalRead = true;
    while (state.offset == state.buffered.size() && state.pending.empty() && !state.complete) {
        state.buffered.clear();
        state.offset = 0;
        co_await state.dataSignal.wait();
    }
    if (state.offset == state.buffered.size() && !state.pending.empty()) {
        if (state.http2DataPending && state.pool != nullptr) {
            state.pool->releaseResponseData(state);
        }
        state.buffered.clear();
        state.offset = 0;
        state.buffered.swap(state.pending);
        state.spaceSignal.notify();
    }
    if (state.offset == state.buffered.size()) {
        if (state.failure) {
            std::rethrow_exception(state.failure);
        }
        if (state.errorCode) {
            throw HttpClientError(static_cast<HttpClientError::Code>(*state.errorCode), "HTTP response body read failed");
        }
        co_return std::nullopt;
    }
    const auto count = std::min(kResponseBodyReadChunkBytes, state.buffered.size() - state.offset);
    const auto chunk = std::string_view(state.buffered).substr(state.offset, count);
    state.offset += count;
    co_return chunk;
}

ScopedOperation<std::pmr::string> HttpClientResponseBody::readAll(std::size_t maxBytes) & {
    if (state_->bodyOperationScope.hasPendingOperations()) {
        throw std::logic_error("HTTP client response body operation is already active");
    }
    return detail::makeScopedOperation(state_->bodyOperationScope, readAllTask(*state_, maxBytes));
}

Task<std::pmr::string> HttpClientResponseBody::readAllTask(detail::HttpClientResponseState& state, std::size_t maxBytes) {
    state.collectAll = true;
    if (state.http2DataPending && state.pool != nullptr) {
        state.pool->releaseResponseData(state);
    }
    state.spaceSignal.notify();
    while (!state.complete) {
        co_await state.dataSignal.wait();
    }
    if (state.failure) {
        std::rethrow_exception(state.failure);
    }
    if (state.errorCode) {
        throw HttpClientError(static_cast<HttpClientError::Code>(*state.errorCode), "HTTP response body read failed");
    }
    const auto remaining = state.buffered.size() - state.offset;
    const auto totalRemaining = remaining + state.pending.size();
    const auto effectiveLimit = std::min(maxBytes, state.bufferedLimit);
    if (totalRemaining > effectiveLimit) {
        throw HttpClientError(HttpClientError::Code::kResponseTooLarge, "HTTP response body exceeds readAll byte limit");
    }
    std::pmr::string result(state.resource);
    result.assign(state.buffered.data() + state.offset, remaining);
    result.append(state.pending);
    state.offset = state.buffered.size();
    state.pending.clear();
    co_return result;
}

ScopedOperation<void> HttpClientResponseBody::pipeTo(ResponseStreamWriter& output) & {
    if (state_->bodyOperationScope.hasPendingOperations()) {
        throw std::logic_error("HTTP client response body operation is already active");
    }
    return detail::makeScopedOperation(state_->bodyOperationScope, pipeToTask(*state_, output));
}

Task<void> HttpClientResponseBody::pipeToTask(detail::HttpClientResponseState& state, ResponseStreamWriter& output) {
    state.incrementalRead = true;
    for (;;) {
        while (state.offset == state.buffered.size() && state.pending.empty() && !state.complete) {
            state.buffered.clear();
            state.offset = 0;
            co_await state.dataSignal.wait();
        }
        if (state.offset == state.buffered.size() && !state.pending.empty()) {
            if (state.http2DataPending && state.pool != nullptr) {
                state.pool->releaseResponseData(state);
            }
            state.buffered.clear();
            state.offset = 0;
            state.buffered.swap(state.pending);
            state.spaceSignal.notify();
        }
        if (state.offset == state.buffered.size()) {
            if (state.failure) {
                std::rethrow_exception(state.failure);
            }
            if (state.errorCode) {
                throw HttpClientError(static_cast<HttpClientError::Code>(*state.errorCode), "HTTP response body forwarding failed");
            }
            co_return;
        }
        const auto count = std::min(kResponseBodyReadChunkBytes, state.buffered.size() - state.offset);
        const auto chunk = std::string_view(state.buffered).substr(state.offset, count);
        co_await output.write(chunk);
        state.offset += count;
    }
}

std::optional<std::string_view> HttpClientResponse::header(std::string_view name) const& noexcept {
    const auto match = std::ranges::find_if(state_->headers, [name](const auto& header) { return headerNameEquals(header.name(), name); });
    return match == state_->headers.end() ? std::nullopt : std::optional<std::string_view>(match->value());
}

std::optional<std::string_view> HttpClientResponse::trailer(std::string_view name) const& noexcept {
    const auto match = std::ranges::find_if(state_->trailers, [name](const auto& header) { return headerNameEquals(header.name(), name); });
    return match == state_->trailers.end() ? std::nullopt : std::optional<std::string_view>(match->value());
}

namespace detail {

void HttpClientPool::decodeResponseContentEncoding(HttpClientResponse& response, bool contentSemanticsPresent, std::size_t maxDecodedBytes, std::pmr::memory_resource* resource) {
    if (!contentSemanticsPresent) {
        return;
    }
    if (response.state_->incrementalRead) {
        return;
    }
    const auto parsedCoding = httpClientContentCodingOf(response.state_->headers);
    const auto* coding = parsedCoding.coding();
    if (coding == nullptr) {
        throw HttpClientError(HttpClientError::Code::kProtocolError, "unsupported HTTP response Content-Encoding");
    }
    if (*coding == HttpContentCoding::kIdentity) {
        return;
    }
    if (!response.state_->pending.empty()) {
        response.state_->buffered.append(response.state_->pending);
        response.state_->pending.clear();
    }
    auto decoded = decodeHttpContent(*coding, response.state_->buffered, {.maxDecodedBytes = maxDecodedBytes, .resource = resource});
    if (auto* content = decoded.decoded()) {
        auto bytes = std::move(*content).takeBytes();
        response.state_->buffered.swap(bytes);
        return;
    }
    const auto* failure = decoded.failure();
    if (failure != nullptr && failure->error() == HttpContentDecodeError::kDecodedSizeExceeded) {
        throw HttpClientError(HttpClientError::Code::kResponseTooLarge, "HTTP response exceeds configured byte limit");
    }
    if (failure != nullptr && failure->error() == HttpContentDecodeError::kDecoderFailure) {
        throw HttpClientError(HttpClientError::Code::kProtocolError, "HTTP response content-coding decoder failed");
    }
    throw HttpClientError(HttpClientError::Code::kProtocolError, "invalid HTTP response Content-Encoding");
}

}  // namespace detail

detail::HttpClientRequestStorage::HttpClientRequestStorage(std::string_view method, std::string_view target, std::pmr::memory_resource* resource)
    : method_(method, detail::pmrResourceOrDefault(resource)),
      target_(target, detail::pmrResourceOrDefault(resource)),
      headers_(detail::pmrResourceOrDefault(resource)),
      body_(detail::pmrResourceOrDefault(resource)) {}

detail::HttpClientRequestStorage& detail::HttpClientRequestStorage::appendHeader(std::string_view name, std::string_view value) {
    auto& header = headers_.emplace_back(name, value, headers_.get_allocator().resource());
    // HTTP field names are case-insensitive, but HTTP/2 requires their wire form
    // to be lowercase (RFC 9113 Section 8.2). Normalize once at the owning public
    // request boundary so the same request remains valid after ALPN selects either
    // HTTP/1.1 or HTTP/2; invalid non-token bytes are deliberately left for the
    // shared protocol validators to reject at submission time.
    for (auto& ch : header.name) {
        ch = static_cast<char>(detail::httpAsciiToLower(static_cast<unsigned char>(ch)));
    }
    return *this;
}

detail::HttpClientRequestStorage& detail::HttpClientRequestStorage::setBody(std::string_view body) {
    body_.assign(body);
    hasBody_ = true;
    return *this;
}

HttpClientHandle::HttpClientHandle(detail::HttpClientPool& pool, std::pmr::memory_resource* resource, detail::ScopedOperationScope& scope) noexcept
    : detail::ScopedCapabilityNode(scope, &HttpClientHandle::expireCapability),
      pool_(&pool),
      resource_(resource) {}

HttpClientHandle::HttpClientHandle(const HttpClientHandle& other) = default;

void HttpClientHandle::expireCapability(detail::ScopedCapabilityNode& capability) noexcept {
    static_cast<HttpClientHandle&>(capability).pool_ = nullptr;
}

HttpClientHandle HttpClientHandle::withOptions(OperationOptions options) const {
    detail::validateOperationOptions(options);
    requireActive();
    HttpClientHandle copy(*this);
    copy.options_ = detail::mergeOperationOptions(options_, std::move(options));
    return copy;
}

ScopedOperation<HttpClientResponse> HttpClientHandle::send(const HttpClientRequestView& view) const {
    requireActive();
    detail::HttpClientRequestStorage request(view.method.view(), view.target.view(), detail::pmrResourceOrDefault(resource_));
    for (const auto& header : view.headers) {
        request.appendHeader(header.name(), header.value());
    }
    if (const auto* bytes = view.content.borrowedBytes()) {
        request.setBody(bytes->value());
    }
    detail::validateOperationOptions(options_);
    return detail::makeScopedOperation(operationScope(), pool_->execute(std::move(request), options_, detail::pmrResourceOrDefault(resource_)));
}

HttpClientStats HttpClientHandle::stats() const {
    requireActive();
    return pool_->stats();
}

std::string_view HttpClientHandle::host() const& {
    requireActive();
    return pool_->host();
}

std::uint16_t HttpClientHandle::port() const {
    requireActive();
    return pool_->port();
}

HttpScheme HttpClientHandle::scheme() const {
    requireActive();
    return pool_->scheme();
}

HttpClientHandle Context::httpClient() const {
    return clientRegistries_.httpClient(resource(), operationScope_, stopToken_);
}

HttpClientHandle Context::httpClient(std::string_view alias) const {
    return clientRegistries_.httpClient(alias, resource(), operationScope_, stopToken_);
}

}  // namespace ruvia
