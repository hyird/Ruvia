#include "ruvia/web/HttpClient.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cctype>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>

#include "ruvia/http/detail/util/PmrResource.h"
#include "ruvia/http/HttpHeader.h"
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

std::size_t cookieStorageBytes(std::string_view name, std::string_view value) noexcept {
    std::size_t total = 0;
    for (const auto size : {name.size(), value.size(), std::size_t{1}}) {
        if (size > std::numeric_limits<std::size_t>::max() - total) {
            return std::numeric_limits<std::size_t>::max();
        }
        total += size;
    }
    return total;
}

std::size_t configuredCookieBytes(const HttpClientConfig& config) noexcept {
    std::size_t total = 0;
    for (const auto& [name, value] : config.cookies) {
        const auto bytes = cookieStorageBytes(name, value);
        if (bytes > std::numeric_limits<std::size_t>::max() - total) {
            return std::numeric_limits<std::size_t>::max();
        }
        total += bytes;
    }
    return total;
}

HttpClientConfig parseHttpClientOrigin(std::string_view origin, HttpClientConfig config) {
    if (origin.starts_with("https://")) {
        config.scheme = HttpScheme::kHttps;
        origin.remove_prefix(8);
    } else if (origin.starts_with("http://")) {
        config.scheme = HttpScheme::kHttp;
        origin.remove_prefix(7);
    } else {
        throw std::invalid_argument("HTTP client origin must begin with http:// or https://");
    }
    const auto suffix = origin.find_first_of("/?#");
    const auto authority = origin.substr(0, suffix);
    if (suffix != std::string_view::npos && origin.substr(suffix) != "/") {
        throw std::invalid_argument("HTTP client origin must not contain a path, query, or fragment");
    }
    if (authority.empty() || authority.find('@') != std::string_view::npos) {
        throw std::invalid_argument("HTTP client origin authority is invalid");
    }

    std::string_view host;
    std::string_view port;
    const bool bracketedLiteral = authority.front() == '[';
    if (bracketedLiteral) {
        const auto closing = authority.find(']');
        if (closing == std::string_view::npos) throw std::invalid_argument("HTTP client IPv6 origin is invalid");
        host = authority.substr(1, closing - 1);
        const auto remainder = authority.substr(closing + 1);
        if (!remainder.empty()) {
            if (remainder.front() != ':') throw std::invalid_argument("HTTP client origin authority is invalid");
            port = remainder.substr(1);
        }
    } else {
        const auto colon = authority.rfind(':');
        if (colon != std::string_view::npos) {
            if (authority.find(':') != colon) throw std::invalid_argument("HTTP client IPv6 origin must use brackets");
            host = authority.substr(0, colon);
            port = authority.substr(colon + 1);
        } else {
            host = authority;
        }
    }
    config.host.assign(host);
    config.port = 0;
    if (!port.empty()) {
        unsigned int parsedPort = 0;
        const auto [end, error] = std::from_chars(port.data(), port.data() + port.size(), parsedPort);
        if (error != std::errc{} || end != port.data() + port.size() || parsedPort == 0 || parsedPort > 65535) {
            throw std::invalid_argument("HTTP client origin port is invalid");
        }
        config.port = static_cast<std::uint16_t>(parsedPort);
    }
    std::string wireHost;
    if (bracketedLiteral) {
        wireHost.reserve(host.size() + 2);
        wireHost.push_back('[');
        wireHost.append(host);
        wireHost.push_back(']');
    } else {
        wireHost.assign(host);
    }
    if (config.scheme == HttpScheme::kHttps) {
        (void)HttpOriginView::https(wireHost, config.port == 0 ? 443 : config.port);
    } else {
        (void)HttpOriginView::http(wireHost, config.port == 0 ? 80 : config.port);
    }
    detail::HttpClientConfigStorage stored(config, std::pmr::get_default_resource());
    detail::validateHttpClientConfig(stored);
    return config;
}

}  // namespace

HttpClientResponse::HttpClientResponse(std::pmr::memory_resource* resource)
    : headers_(detail::httpPmrResourceOrDefault(resource)), trailers_(detail::httpPmrResourceOrDefault(resource)),
      body_(detail::httpPmrResourceOrDefault(resource)) {}

std::optional<std::string_view> HttpClientResponse::getHeader(std::string_view name) const& noexcept {
    const auto match = std::ranges::find_if(headers_, [name](const auto& header) { return headerNameEquals(header.name(), name); });
    return match == headers_.end() ? std::nullopt : std::optional<std::string_view>(match->value());
}

std::optional<std::string_view> HttpClientResponse::getTrailer(std::string_view name) const& noexcept {
    const auto match = std::ranges::find_if(trailers_, [name](const auto& header) { return headerNameEquals(header.name(), name); });
    return match == trailers_.end() ? std::nullopt : std::optional<std::string_view>(match->value());
}

HttpClientRequest::HttpClientRequest(std::pmr::memory_resource* resource)
    : method_("GET", detail::httpPmrResourceOrDefault(resource)),
      path_("/", detail::httpPmrResourceOrDefault(resource)),
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

HttpClientRequest& HttpClientRequest::setPath(std::string_view path) {
    path_.assign(path);
    return *this;
}

HttpClientRequest& HttpClientRequest::addHeader(std::string_view name, std::string_view value) {
    headers_.emplace_back(name, value, headers_.get_allocator().resource());
    return *this;
}

HttpClientRequest& HttpClientRequest::removeHeader(std::string_view name) {
    std::erase_if(headers_, [name](const Header& header) { return headerNameEquals(header.name, name); });
    return *this;
}

HttpClientRequest& HttpClientRequest::setContentTypeString(std::string_view contentType) {
    removeHeader("content-type");
    return addHeader("content-type", contentType);
}

HttpClientRequest& HttpClientRequest::addCookie(std::string_view name, std::string_view value) {
    if (!isValidHttpHeaderName(name) || !detail::isValidCookieValue(value)) {
        throw std::invalid_argument("invalid HTTP client request cookie");
    }
    const auto match = std::ranges::find_if(headers_, [](const Header& header) { return headerNameEquals(header.name, "cookie"); });
    if (match == headers_.end()) {
        headers_.emplace_back("cookie", "", headers_.get_allocator().resource());
        headers_.back().value.reserve(name.size() + value.size() + 1);
        headers_.back().value.append(name);
        headers_.back().value.push_back('=');
        headers_.back().value.append(value);
    } else {
        if (!match->value.empty()) match->value.append("; ");
        match->value.append(name);
        match->value.push_back('=');
        match->value.append(value);
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

HttpClient::HttpClient(detail::HttpClientPool& pool, std::pmr::memory_resource* resource, detail::ScopedOperationScope& scope) noexcept
    : detail::ScopedCapabilityNode(scope, &HttpClient::expireCapability), pool_(&pool), resource_(resource) {}

HttpClient::HttpClient(std::uint64_t dynamicId, HttpClientConfig config)
    : dynamicId_(dynamicId), dynamicConfig_(std::move(config)) {}

HttpClient::HttpClient(const HttpClient& other)
    : detail::ScopedCapabilityNode(other), pool_(other.pool_), resource_(other.resource_), options_(other.options_),
      dynamicId_(other.dynamicId_), dynamicConfig_(other.dynamicConfig_) {}

void HttpClient::expireCapability(detail::ScopedCapabilityNode& capability) noexcept {
    static_cast<HttpClient&>(capability).pool_ = nullptr;
}

HttpClient HttpClient::withOptions(HttpClientOperationOptions options) const {
    if (dynamicId_ == 0) requireActive();
    HttpClient copy(*this);
    copy.options_ = std::move(options);
    return copy;
}

HttpClientRequest HttpClient::newRequest() const {
    if (dynamicId_ == 0) requireActive();
    return HttpClientRequest(detail::httpPmrResourceOrDefault(resource_));
}

HttpClientPtr HttpClient::newHttpClient(std::string_view origin, HttpClientConfig config) {
    static std::atomic<std::uint64_t> nextId{0};
    auto id = nextId.fetch_add(1, std::memory_order_relaxed) + 1;
    if (id == 0) std::terminate();
    return HttpClientPtr(new HttpClient(id, parseHttpClientOrigin(origin, std::move(config))));
}

detail::HttpClientPool& HttpClient::resolvePool() const {
    if (dynamicId_ == 0) {
        requireActive();
        return *pool_;
    }
    auto* registry = detail::HttpClientRegistry::current();
    if (registry == nullptr) {
        throw HttpClientError(HttpClientError::Code::kNotConfigured, "dynamic HTTP client must be used on a Ruvia Web worker");
    }
    return registry->getOrCreate(dynamicId_, dynamicConfig_);
}

Task<HttpClientResponse> HttpClient::sendRequest(HttpClientRequest request, HttpClientOperationOptions options) const {
    if (!options.timeout.has_value()) options.timeout = options_.timeout;
    if (!options.stopToken.stoppable()) options.stopToken = options_.stopToken;
    return resolvePool().execute(
        std::move(request), std::move(options), detail::httpPmrResourceOrDefault(resource_));
}

Task<HttpClientResponse> HttpClient::sendRequest(HttpClientRequest request, std::chrono::milliseconds timeout) const {
    return sendRequest(std::move(request), HttpClientOperationOptions{.timeout = timeout, .stopToken = {}});
}

HttpClientStats HttpClient::stats() const noexcept {
    if (dynamicId_ != 0) {
        auto* registry = detail::HttpClientRegistry::current();
        auto* pool = registry == nullptr ? nullptr : registry->find(dynamicId_);
        return pool == nullptr ? HttpClientStats{} : pool->stats();
    }
    return pool_ == nullptr ? HttpClientStats{} : pool_->stats();
}

std::size_t HttpClient::requestsBufferSize() const noexcept { return stats().requestsBuffered; }
std::size_t HttpClient::outstandingRequests() const noexcept {
    const auto value = stats();
    return value.requestsBuffered + value.requestsInFlight;
}
std::size_t HttpClient::bytesSent() const noexcept { return stats().bytesSent; }
std::size_t HttpClient::bytesReceived() const noexcept { return stats().bytesReceived; }

std::string_view HttpClient::host() const& {
    if (dynamicId_ != 0) return dynamicConfig_.host;
    requireActive();
    return pool_->host();
}

std::uint16_t HttpClient::port() const {
    if (dynamicId_ != 0) return dynamicConfig_.port != 0 ? dynamicConfig_.port : (dynamicConfig_.scheme == HttpScheme::kHttps ? 443 : 80);
    requireActive();
    return pool_->port();
}

bool HttpClient::secure() const {
    if (dynamicId_ != 0) return dynamicConfig_.scheme == HttpScheme::kHttps;
    requireActive();
    return pool_->secure();
}

bool HttpClient::onDefaultPort() const {
    return port() == (secure() ? 443 : 80);
}

void HttpClient::setUserAgent(std::string_view userAgent) const {
    if (dynamicId_ != 0) {
        if (auto* registry = detail::HttpClientRegistry::current()) {
            registry->getOrCreate(dynamicId_, dynamicConfig_).setUserAgent(userAgent);
        } else {
            dynamicConfig_.userAgent.assign(userAgent);
        }
        return;
    }
    requireActive();
    pool_->setUserAgent(userAgent);
}

void HttpClient::enableCookies(bool enabled) const {
    if (dynamicId_ != 0) {
        if (auto* registry = detail::HttpClientRegistry::current()) {
            registry->getOrCreate(dynamicId_, dynamicConfig_).enableCookies(enabled);
        } else {
            dynamicConfig_.cookiesEnabled = enabled;
        }
        return;
    }
    requireActive();
    pool_->enableCookies(enabled);
}

void HttpClient::addCookie(std::string_view name, std::string_view value) const {
    if (dynamicId_ != 0) {
        if (!isValidHttpHeaderName(name) || !detail::isValidCookieValue(value)) throw std::invalid_argument("invalid HTTP client cookie");
        if (auto* registry = detail::HttpClientRegistry::current()) {
            registry->getOrCreate(dynamicId_, dynamicConfig_).addCookie(name, value);
        } else {
            const auto match = std::ranges::find_if(dynamicConfig_.cookies, [name](const auto& cookie) { return cookie.first == name; });
            const auto replacedBytes = match == dynamicConfig_.cookies.end()
                ? 0
                : cookieStorageBytes(match->first, match->second);
            const auto retainedBytes = configuredCookieBytes(dynamicConfig_) - replacedBytes;
            const auto replacementBytes = cookieStorageBytes(name, value);
            if ((match == dynamicConfig_.cookies.end() &&
                    dynamicConfig_.cookies.size() >= dynamicConfig_.maxCookiesPerWorker) ||
                retainedBytes > dynamicConfig_.maxCookieBytesPerWorker ||
                replacementBytes > dynamicConfig_.maxCookieBytesPerWorker - retainedBytes) {
                throw std::length_error("HTTP client cookie jar capacity exceeded");
            }
            if (match == dynamicConfig_.cookies.end()) dynamicConfig_.cookies.emplace_back(name, value);
            else match->second.assign(value);
        }
        return;
    }
    requireActive();
    pool_->addCookie(name, value);
}

HttpClient Context::httpClient() const {
    if (httpClients_ == nullptr) throw HttpClientError(HttpClientError::Code::kNotConfigured, "http client is not configured");
    return httpClients_->get(resource(), operationScope_);
}

HttpClient Context::httpClient(std::string_view alias) const {
    if (httpClients_ == nullptr) throw HttpClientError(HttpClientError::Code::kNotConfigured, "http client is not configured");
    return httpClients_->get(alias, resource(), operationScope_);
}

}  // namespace ruvia
