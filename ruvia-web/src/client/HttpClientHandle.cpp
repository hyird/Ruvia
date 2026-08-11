#include "ruvia/web/HttpClientHandle.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <stdexcept>
#include <utility>

#include "ruvia/http/detail/util/PmrResource.h"
#include "ruvia/http/HttpHeader.h"
#include "ruvia/http/detail/client/HttpClientContentEncoding.h"
#include "ruvia/http/detail/cookie/CookieValidation.h"
#include "ruvia/web/Context.h"
#include "ruvia/web/detail/client/HttpClientRegistry.h"
#include "ruvia/web/detail/client/HttpClientConfigValidation.h"

namespace ruvia {
namespace {

bool headerNameEquals(std::string_view left, std::string_view right) noexcept {
    return left.size() == right.size() && std::equal(left.begin(), left.end(), right.begin(), [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
    });
}

}  // namespace

HttpClientResponse::HttpClientResponse(std::pmr::memory_resource* resource)
    : headers_(detail::httpPmrResourceOrDefault(resource)), trailers_(detail::httpPmrResourceOrDefault(resource)),
      body_(detail::httpPmrResourceOrDefault(resource)) {}

std::optional<std::string_view> HttpClientResponse::header(std::string_view name) const& noexcept {
    const auto match = std::ranges::find_if(headers_, [name](const auto& header) { return headerNameEquals(header.name(), name); });
    return match == headers_.end() ? std::nullopt : std::optional<std::string_view>(match->value());
}

std::optional<std::string_view> HttpClientResponse::trailer(std::string_view name) const& noexcept {
    const auto match = std::ranges::find_if(trailers_, [name](const auto& header) { return headerNameEquals(header.name(), name); });
    return match == trailers_.end() ? std::nullopt : std::optional<std::string_view>(match->value());
}

namespace detail {

void HttpClientPool::decodeResponseContentEncoding(HttpClientResponse& response, bool contentSemanticsPresent, std::size_t maxDecodedBytes, std::pmr::memory_resource* resource) {
    if (!contentSemanticsPresent) {
        return;
    }
    const auto parsedCoding = httpClientContentCodingOf(response.headers_);
    const auto* coding = parsedCoding.coding();
    if (coding == nullptr) {
        throw HttpClientError(HttpClientError::Code::kProtocolError, "unsupported HTTP response Content-Encoding");
    }
    if (*coding == HttpContentCoding::kIdentity) {
        return;
    }
    auto decoded = decodeHttpContent(*coding, response.body_, maxDecodedBytes, resource);
    if (auto* content = decoded.decoded()) {
        auto bytes = std::move(*content).takeBytes();
        response.body_.swap(bytes);
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

HttpClientRequest::HttpClientRequest(std::pmr::memory_resource* resource)
    : method_("GET", detail::httpPmrResourceOrDefault(resource)),
      target_("/", detail::httpPmrResourceOrDefault(resource)),
      headers_(detail::httpPmrResourceOrDefault(resource)),
      body_(detail::httpPmrResourceOrDefault(resource)) {}

HttpClientRequest& HttpClientRequest::setMethod(HttpKnownMethod method) {
    const auto token = knownHttpMethodToken(method);
    if (token.empty()) throw std::invalid_argument("unknown HTTP method requires an explicit token");
    return setMethod(token);
}

HttpClientRequest& HttpClientRequest::setMethod(std::string_view method) {
    method_.assign(method);
    return *this;
}

HttpClientRequest& HttpClientRequest::setTarget(std::string_view target) {
    target_.assign(target);
    return *this;
}

HttpClientRequest& HttpClientRequest::addHeader(std::string_view name, std::string_view value) {
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

HttpClientRequest& HttpClientRequest::removeHeader(std::string_view name) {
    std::erase_if(headers_, [name](const Header& header) { return headerNameEquals(header.name, name); });
    return *this;
}

HttpClientRequest& HttpClientRequest::setContentType(std::string_view contentType) {
    auto* const resource = headers_.get_allocator().resource();
    Header replacement("content-type", contentType, resource);
    headers_.reserve(headers_.size() + 1);
    removeHeader("content-type");
    headers_.push_back(std::move(replacement));
    return *this;
}

HttpClientRequest& HttpClientRequest::addCookie(std::string_view name, std::string_view value) {
    if (!isValidHttpHeaderName(name) || !detail::isValidCookieValue(value)) {
        throw std::invalid_argument("invalid HTTP client request cookie");
    }
    const auto match = std::ranges::find_if(headers_, [](const Header& header) { return headerNameEquals(header.name, "cookie"); });
    auto* const resource = headers_.get_allocator().resource();
    auto appendCookiePair = [name, value](std::pmr::string& target) {
        if (!target.empty()) target.append("; ");
        target.append(name);
        target.push_back('=');
        target.append(value);
    };
    if (match == headers_.end()) {
        std::pmr::string cookieValue(resource);
        appendCookiePair(cookieValue);
        headers_.emplace_back("cookie", "", resource);
        headers_.back().value.swap(cookieValue);
    } else {
        std::pmr::string cookieValue(match->value, resource);
        appendCookiePair(cookieValue);
        match->value.swap(cookieValue);
    }
    return *this;
}

HttpClientRequest& HttpClientRequest::setBody(std::string_view body) {
    body_.assign(body);
    hasBody_ = true;
    return *this;
}

HttpClientRequest& HttpClientRequest::setBody(std::span<const std::byte> body) {
    body_.assign(reinterpret_cast<const char*>(body.data()), body.size());
    hasBody_ = true;
    return *this;
}

HttpClientRequest& HttpClientRequest::clearBody() noexcept {
    body_.clear();
    hasBody_ = false;
    return *this;
}

HttpClientHandle::HttpClientHandle(detail::HttpClientPool& pool, std::pmr::memory_resource* resource, detail::ScopedOperationScope& scope) noexcept
    : detail::ScopedCapabilityNode(scope, &HttpClientHandle::expireCapability), pool_(&pool), resource_(resource) {}

HttpClientHandle::HttpClientHandle(const HttpClientHandle& other)
    : detail::ScopedCapabilityNode(other), pool_(other.pool_), resource_(other.resource_), options_(other.options_) {}

void HttpClientHandle::expireCapability(detail::ScopedCapabilityNode& capability) noexcept {
    static_cast<HttpClientHandle&>(capability).pool_ = nullptr;
}

HttpClientHandle HttpClientHandle::withOptions(HttpClientOperationOptions options) const {
    detail::validateHttpClientOperationOptions(options);
    requireActive();
    HttpClientHandle copy(*this);
    copy.options_ = detail::mergeHttpClientOperationOptions(options_, std::move(options));
    return copy;
}

HttpClientRequest HttpClientHandle::newRequest() const {
    requireActive();
    return HttpClientRequest(detail::httpPmrResourceOrDefault(resource_));
}

ScopedOperation<HttpClientResponse> HttpClientHandle::sendRequest(HttpClientRequest request, HttpClientOperationOptions options) const {
    requireActive();
    options = detail::mergeHttpClientOperationOptions(options_, std::move(options));
    detail::validateHttpClientOperationOptions(options);
    return detail::makeScopedOperation(
        operationScope(),
        pool_->execute(std::move(request), std::move(options), detail::httpPmrResourceOrDefault(resource_)));
}

ScopedOperation<HttpClientResponse> HttpClientHandle::sendRequest(HttpClientRequest request, std::chrono::milliseconds timeout) const {
    return sendRequest(std::move(request), HttpClientOperationOptions{.timeout = timeout, .stopToken = {}});
}

HttpClientStats HttpClientHandle::stats() const {
    requireActive();
    return pool_->stats();
}

std::size_t HttpClientHandle::bufferedRequests() const { return stats().bufferedRequests; }
std::size_t HttpClientHandle::outstandingRequests() const {
    const auto value = stats();
    return value.bufferedRequests + value.inFlightRequests;
}
std::size_t HttpClientHandle::bytesSent() const { return stats().bytesSent; }
std::size_t HttpClientHandle::bytesReceived() const { return stats().bytesReceived; }

std::string_view HttpClientHandle::host() const& {
    requireActive();
    return pool_->host();
}

std::uint16_t HttpClientHandle::port() const {
    requireActive();
    return pool_->port();
}

bool HttpClientHandle::secure() const {
    requireActive();
    return pool_->secure();
}

bool HttpClientHandle::onDefaultPort() const {
    return port() == (secure() ? 443 : 80);
}

HttpClientHandle Context::httpClient() const {
    if (httpClients_ == nullptr) throw HttpClientError(HttpClientError::Code::kNotConfigured, "http client is not configured");
    return httpClients_->get(resource(), operationScope_).withOptions(
        HttpClientOperationOptions{.timeout = std::nullopt, .stopToken = stopToken_});
}

HttpClientHandle Context::httpClient(std::string_view alias) const {
    if (httpClients_ == nullptr) throw HttpClientError(HttpClientError::Code::kNotConfigured, "http client is not configured");
    return httpClients_->get(alias, resource(), operationScope_).withOptions(
        HttpClientOperationOptions{.timeout = std::nullopt, .stopToken = stopToken_});
}

}  // namespace ruvia
