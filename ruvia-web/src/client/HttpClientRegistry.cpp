#include "ruvia/web/detail/client/HttpClientRegistry.h"

#include <algorithm>
#include <ranges>
#include <stdexcept>

#include "ruvia/http/detail/util/PmrResource.h"
#include "ruvia/web/detail/client/HttpClientConfigValidation.h"

namespace ruvia::detail {

bool HttpClientPool::matches(const HttpClientConfig& value) const noexcept {
    const auto textEquals = [](const std::pmr::string& left, const std::string& right) noexcept {
        return std::string_view(left) == std::string_view(right);
    };
    const auto port = value.port.value_or(value.scheme == HttpScheme::kHttps ? 443 : 80);
    if (config_.scheme != value.scheme || !textEquals(config_.host, value.host) || config_.port != port ||
        config_.connectionsPerWorker != value.connectionsPerWorker ||
        config_.maxConcurrentHttp2StreamsPerConnection != value.maxConcurrentHttp2StreamsPerConnection ||
        config_.maxBufferedRequestsPerWorker != value.maxBufferedRequestsPerWorker ||
        config_.maxCookiesPerWorker != value.maxCookiesPerWorker ||
        config_.maxCookieBytesPerWorker != value.maxCookieBytesPerWorker ||
        config_.connectTimeout != value.connectTimeout || config_.writeTimeout != value.writeTimeout ||
        config_.requestTimeout != value.requestTimeout || config_.acquireTimeout != value.acquireTimeout ||
        config_.maxResponseBytes != value.maxResponseBytes || config_.protocol != value.protocol ||
        config_.verifyCertificate != value.verifyCertificate || config_.tcpNoDelay != value.tcpNoDelay ||
        config_.keepAlive != value.keepAlive || config_.cookiesEnabled != value.cookiesEnabled ||
        !textEquals(config_.caFile, value.caFile) || !textEquals(config_.certificateChainFile, value.certificateChainFile) ||
        !textEquals(config_.privateKeyFile, value.privateKeyFile) || !textEquals(config_.privateKeyPassword, value.privateKeyPassword) ||
        !textEquals(config_.userAgent, value.userAgent) || config_.cookies.size() != value.cookies.size()) {
        return false;
    }
    for (std::size_t i = 0; i < config_.cookies.size(); ++i) {
        if (!textEquals(config_.cookies[i].first, value.cookies[i].first) || !textEquals(config_.cookies[i].second, value.cookies[i].second)) return false;
    }
    return true;
}

HttpClientRequestView HttpClientRequestStorageAccess::view(const HttpClientRequestStorage& request, std::pmr::vector<HttpHeaderView>& headers) {
    headers.clear();
    headers.reserve(request.headers_.size());
    for (const auto& header : request.headers_) headers.emplace_back(header.name, header.value);
    HttpClientRequestView result;
    result.method = request.method_;
    result.target = request.target_;
    result.headers = std::span<const HttpHeaderView>(headers);
    result.content = request.hasBody_ ? HttpClientRequestContentView::bytes(request.body_) : HttpClientRequestContentView::none();
    return result;
}

HttpClientRegistry::HttpClientRegistry(asio::io_context& ioContext, const WorkerHandle& worker, std::pmr::memory_resource* resource, std::span<const HttpClientDefinition> definitions, std::size_t maxOriginsPerWorker)
    : resource_(httpPmrResourceOrDefault(resource)), ioContext_(&ioContext), worker_(&worker), maxOriginsPerWorker_(maxOriginsPerWorker), pools_(resource_), aliasIndex_(resource_) {
    pools_.reserve(definitions.size());
    for (const auto& definition : definitions) {
        if (definition.alias.empty()) throw std::invalid_argument("http client alias must not be empty");
        if (std::ranges::any_of(pools_, [&definition](const Entry& entry) { return entry.alias == definition.alias; })) {
            throw std::invalid_argument("duplicate http client alias");
        }
        HttpClientConfigStorage config(definition.config, resource_);
        validateHttpClientConfig(config);
        auto pool = makePmrObject<HttpClientPool>(resource_, ioContext, worker, std::move(config), resource_);
        pools_.push_back(Entry{std::pmr::string(definition.alias, resource_), std::move(pool)});
        if (pools_.back().alias == "default") defaultPoolIndex_ = pools_.size() - 1;
    }
    aliasIndex_.resize(pools_.size());
    for (std::size_t i = 0; i < aliasIndex_.size(); ++i) aliasIndex_[i] = i;
    std::ranges::sort(aliasIndex_, {}, [this](std::size_t i) -> std::string_view { return pools_[i].alias; });
}

HttpClientRegistry::~HttpClientRegistry() = default;

void HttpClientRegistry::closeNow() noexcept {
    if (closing_) return;
    closing_ = true;
    for (auto& entry : pools_) entry.pool->closeNow();
}

Task<void> HttpClientRegistry::join() {
    closeNow();
    for (std::size_t i = 0; i < pools_.size(); ++i) co_await pools_[i].pool->join();
}

HttpClientHandle HttpClientRegistry::get(HttpClientConfig config, std::pmr::memory_resource* resource, ScopedOperationScope& scope) {
    if (closing_) throw HttpClientError(HttpClientError::Code::kClosing, "http client registry is closing");
    validateHttpClientConfig(config);
    const auto port = config.port.value_or(config.scheme == HttpScheme::kHttps ? 443 : 80);
    for (auto& entry : pools_) {
        if (entry.pool->scheme() == config.scheme && entry.pool->host() == config.host && entry.pool->port() == port) {
            if (!entry.pool->matches(config)) {
                throw HttpClientError(HttpClientError::Code::kInvalidRequest, "HTTP client configuration changed for a cached origin");
            }
            return HttpClientHandle(*entry.pool, resource, scope);
        }
    }
    if (pools_.size() >= maxOriginsPerWorker_) {
        throw HttpClientError(HttpClientError::Code::kQueueFull, "HTTP client origin cache is full");
    }
    HttpClientConfigStorage stored(config, resource_);
    auto pool = makePmrObject<HttpClientPool>(resource_, *ioContext_, *worker_, std::move(stored), resource_);
    auto* result = pool.get();
    pools_.push_back(Entry{std::pmr::string(config.host, resource_), std::move(pool)});
    return HttpClientHandle(*result, resource, scope);
}

HttpClientHandle HttpClientRegistry::get(std::pmr::memory_resource* resource, ScopedOperationScope& scope) const {
    if (!defaultPoolIndex_) throw HttpClientError(HttpClientError::Code::kNotConfigured, "fixed HTTP client is not configured");
    return HttpClientHandle(*pools_[*defaultPoolIndex_].pool, resource, scope);
}

}  // namespace ruvia::detail
