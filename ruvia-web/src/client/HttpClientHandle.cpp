#include "ruvia/web/HttpClientHandle.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <stdexcept>
#include <utility>

#include "ruvia/http/detail/util/PmrResource.h"
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

bool headerNameEquals(std::string_view left, std::string_view right) noexcept {
    return left.size() == right.size() && std::equal(left.begin(), left.end(), right.begin(), [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
    });
}

}  // namespace

HttpClientResponse::HttpClientResponse(
    std::pmr::memory_resource* resource,
    const WorkerHandle& worker,
    detail::HttpClientPool& pool)
    : state_(detail::constructPmrObject<detail::HttpClientResponseState>(
          detail::httpPmrResourceOrDefault(resource), worker, detail::httpPmrResourceOrDefault(resource))),
      body_(state_) {
    state_->pool = &pool;
}

HttpClientResponse::HttpClientResponse(detail::HttpClientResponseState* state, bool retain) noexcept
    : state_(state), body_(state), consumer_(false) {
    if (retain && state_ != nullptr) ++state_->references;
}

HttpClientResponse::HttpClientResponse(HttpClientResponse&& other) noexcept
    : state_(std::exchange(other.state_, nullptr)), body_(state_), consumer_(other.consumer_) {
    other.body_.state_ = nullptr;
}

HttpClientResponse& HttpClientResponse::operator=(HttpClientResponse&& other) noexcept {
    if (this == &other) return *this;
    if (body_.operationScope_.hasPendingOperations() || other.body_.operationScope_.hasPendingOperations()) std::terminate();
    std::swap(state_, other.state_);
    std::swap(consumer_, other.consumer_);
    body_.state_ = state_;
    other.body_.state_ = other.state_;
    return *this;
}

HttpClientResponse::~HttpClientResponse() {
    body_.operationScope_.close();
    release();
}

void HttpClientResponse::release() noexcept {
    if (state_ == nullptr) return;
    auto* state = std::exchange(state_, nullptr);
    body_.state_ = nullptr;
    if (consumer_ && !state->complete && !state->abandoned && state->pool != nullptr) state->pool->abandonResponse(*state);
    if (state->references == 0) std::terminate();
    if (--state->references == 0) detail::destroyPmrObject(state, state->resource);
}

HttpStatusCode HttpClientResponse::status() const noexcept { return state_->status; }
HttpProtocolVersion HttpClientResponse::protocolVersion() const noexcept { return state_->protocolVersion; }
std::span<const HttpClientResponseHeader> HttpClientResponse::headers() const& noexcept { return state_->headers; }
std::span<const HttpClientResponseHeader> HttpClientResponse::trailers() const& noexcept { return state_->trailers; }

HttpClientResponseBody::HttpClientResponseBody(HttpClientResponseBody&& other) noexcept
    : state_(std::exchange(other.state_, nullptr)) {
    if (other.operationScope_.hasPendingOperations()) std::terminate();
}

HttpClientResponseBody& HttpClientResponseBody::operator=(HttpClientResponseBody&& other) noexcept {
    if (this == &other) return *this;
    if (operationScope_.hasPendingOperations() || other.operationScope_.hasPendingOperations()) std::terminate();
    state_ = std::exchange(other.state_, nullptr);
    readActive_ = false;
    return *this;
}

HttpClientResponseBody::~HttpClientResponseBody() {
    operationScope_.close();
}

bool HttpClientResponseBody::complete() const noexcept {
    return state_ == nullptr ||
        (state_->complete && state_->offset == state_->buffered.size() && state_->pending.empty());
}

ScopedOperation<std::optional<std::string_view>> HttpClientResponseBody::read() {
    if (readActive_) throw std::logic_error("HTTP client response body operation is already active");
    return detail::makeScopedOperation(operationScope_, readTask());
}

Task<std::optional<std::string_view>> HttpClientResponseBody::readTask() {
    struct Guard final {
        bool& active;
        explicit Guard(bool& value) : active(value) { active = true; }
        ~Guard() { active = false; }
    } guard(readActive_);
    state_->incrementalRead = true;
    while (state_->offset == state_->buffered.size() && state_->pending.empty() && !state_->complete) {
        state_->buffered.clear();
        state_->offset = 0;
        co_await state_->dataSignal.wait();
    }
    if (state_->offset == state_->buffered.size() && !state_->pending.empty()) {
        if (state_->http2DataPending && state_->pool != nullptr) state_->pool->releaseResponseData(*state_);
        state_->buffered.clear();
        state_->offset = 0;
        state_->buffered.swap(state_->pending);
        state_->spaceSignal.notify();
    }
    if (state_->offset == state_->buffered.size()) {
        if (state_->failure) std::rethrow_exception(state_->failure);
        if (state_->errorCode) throw HttpClientError(static_cast<HttpClientError::Code>(*state_->errorCode), "HTTP response body read failed");
        co_return std::nullopt;
    }
    constexpr std::size_t kChunkBytes = 16 * 1024;
    const auto count = std::min(kChunkBytes, state_->buffered.size() - state_->offset);
    const auto chunk = std::string_view(state_->buffered).substr(state_->offset, count);
    state_->offset += count;
    co_return chunk;
}

ScopedOperation<std::pmr::string> HttpClientResponseBody::readAll(std::size_t maxBytes) {
    if (readActive_) throw std::logic_error("HTTP client response body operation is already active");
    return detail::makeScopedOperation(operationScope_, readAllTask(maxBytes));
}

Task<std::pmr::string> HttpClientResponseBody::readAllTask(std::size_t maxBytes) {
    struct Guard final {
        bool& active;
        explicit Guard(bool& value) : active(value) { active = true; }
        ~Guard() { active = false; }
    } guard(readActive_);
    state_->collectAll = true;
    if (state_->http2DataPending && state_->pool != nullptr) state_->pool->releaseResponseData(*state_);
    state_->spaceSignal.notify();
    while (!state_->complete) co_await state_->dataSignal.wait();
    if (state_->failure) std::rethrow_exception(state_->failure);
    if (state_->errorCode) throw HttpClientError(static_cast<HttpClientError::Code>(*state_->errorCode), "HTTP response body read failed");
    const auto remaining = state_->buffered.size() - state_->offset;
    const auto totalRemaining = remaining + state_->pending.size();
    const auto effectiveLimit = std::min(maxBytes, state_->bufferedLimit);
    if (totalRemaining > effectiveLimit) {
        throw HttpClientError(HttpClientError::Code::kResponseTooLarge,
            "HTTP response body exceeds readAll byte limit");
    }
    std::pmr::string result(state_->resource);
    result.assign(state_->buffered.data() + state_->offset, remaining);
    result.append(state_->pending);
    state_->offset = state_->buffered.size();
    state_->pending.clear();
    co_return result;
}

ScopedOperation<void> HttpClientResponseBody::pipeTo(ResponseStreamWriter& output) {
    if (readActive_) throw std::logic_error("HTTP client response body operation is already active");
    return detail::makeScopedOperation(operationScope_, pipeToTask(output));
}

Task<void> HttpClientResponseBody::pipeToTask(ResponseStreamWriter& output) {
    struct Guard final {
        bool& active;
        explicit Guard(bool& value) : active(value) { active = true; }
        ~Guard() { active = false; }
    } guard(readActive_);
    state_->incrementalRead = true;
    constexpr std::size_t kChunkBytes = 16 * 1024;
    for (;;) {
        while (state_->offset == state_->buffered.size() && state_->pending.empty() && !state_->complete) {
            state_->buffered.clear();
            state_->offset = 0;
            co_await state_->dataSignal.wait();
        }
        if (state_->offset == state_->buffered.size() && !state_->pending.empty()) {
            if (state_->http2DataPending && state_->pool != nullptr) state_->pool->releaseResponseData(*state_);
            state_->buffered.clear();
            state_->offset = 0;
            state_->buffered.swap(state_->pending);
            state_->spaceSignal.notify();
        }
        if (state_->offset == state_->buffered.size()) {
            if (state_->failure) std::rethrow_exception(state_->failure);
            if (state_->errorCode) throw HttpClientError(static_cast<HttpClientError::Code>(*state_->errorCode), "HTTP response body forwarding failed");
            co_return;
        }
        const auto count = std::min(kChunkBytes, state_->buffered.size() - state_->offset);
        const auto chunk = std::string_view(state_->buffered).substr(state_->offset, count);
        co_await output.write(chunk);
        state_->offset += count;
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
    if (response.state_->incrementalRead) return;
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
    auto decoded = decodeHttpContent(*coding, response.state_->buffered, maxDecodedBytes, resource);
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

detail::HttpClientRequestStorage::HttpClientRequestStorage(
    std::string_view method,
    std::string_view target,
    std::pmr::memory_resource* resource)
    : method_(method, detail::httpPmrResourceOrDefault(resource)),
      target_(target, detail::httpPmrResourceOrDefault(resource)),
      headers_(detail::httpPmrResourceOrDefault(resource)),
      body_(detail::httpPmrResourceOrDefault(resource)) {}

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
    : detail::ScopedCapabilityNode(scope, &HttpClientHandle::expireCapability), pool_(&pool), resource_(resource) {}

HttpClientHandle::HttpClientHandle(const HttpClientHandle& other)
    : detail::ScopedCapabilityNode(other), pool_(other.pool_), resource_(other.resource_), options_(other.options_) {}

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

ScopedOperation<HttpClientResponse> HttpClientHandle::send(
    const HttpClientRequestView& view,
    OperationOptions options) const {
    requireActive();
    detail::HttpClientRequestStorage request(
        view.method.view(), view.target.view(), detail::httpPmrResourceOrDefault(resource_));
    for (const auto& header : view.headers) request.appendHeader(header.name(), header.value());
    if (const auto* bytes = view.content.borrowedBytes()) request.setBody(bytes->value());
    options = detail::mergeOperationOptions(options_, std::move(options));
    detail::validateOperationOptions(options);
    return detail::makeScopedOperation(
        operationScope(),
        pool_->execute(std::move(request), std::move(options), detail::httpPmrResourceOrDefault(resource_)));
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

HttpClientHandle Context::httpClient(HttpClientConfig config) const {
    if (httpClients_ == nullptr) throw HttpClientError(HttpClientError::Code::kNotConfigured, "http client is not configured");
    return httpClients_->get(std::move(config), resource(), operationScope_).withOptions(
        OperationOptions{.timeout = std::nullopt, .stopToken = stopToken_});
}

}  // namespace ruvia
