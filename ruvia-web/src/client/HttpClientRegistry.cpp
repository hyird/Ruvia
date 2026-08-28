#include "ruvia/web/detail/client/HttpClientRegistry.h"

#include "ruvia/core/memory/PmrResource.h"

namespace ruvia::detail {

HttpClientRegistry::HttpClientRegistry(asio::io_context& ioContext, const WorkerHandle& worker,
    std::pmr::memory_resource* resource, const HttpClientConfig& defaultConfig)
    : resource_(pmrResourceOrDefault(resource)),
      pools_(resource_),
      aliasIndex_(resource_) {
    pools_.reserve(1);
    add(ioContext, worker, kDefaultCapabilityAlias,
        HttpClientConfigStorage(defaultConfig, resource_));
    aliasIndex_.build(pools_);
}

HttpClientRequestView HttpClientRequestStorageAccess::view(
    const HttpClientRequestStorage& request, std::pmr::vector<HttpHeaderView>& headers) {
    headers.clear();
    headers.reserve(request.headers_.size());
    for (const auto& header : request.headers_) {
        headers.emplace_back(header.name, header.value);
    }
    HttpClientRequestView result;
    result.method = request.method_;
    result.target = request.target_;
    result.headers = std::span<const HttpHeaderView>(headers);
    result.content = request.hasBody_ ? HttpClientRequestContentView::bytes(request.body_)
                                      : HttpClientRequestContentView::none();
    return result;
}

HttpClientRegistry::HttpClientRegistry(asio::io_context& ioContext, const WorkerHandle& worker,
    std::pmr::memory_resource* resource, std::span<const HttpClientDefinition> definitions)
    : resource_(pmrResourceOrDefault(resource)),
      pools_(resource_),
      aliasIndex_(resource_) {
    validateCapabilityAliases(
        definitions, "HTTP client alias must not be empty", "duplicate HTTP client alias");
    pools_.reserve(definitions.size());
    for (const auto& definition : definitions) {
        add(ioContext, worker, definition.alias,
            HttpClientConfigStorage(definition.config, resource_));
    }
    aliasIndex_.build(pools_);
}

HttpClientRegistry::~HttpClientRegistry() = default;

void HttpClientRegistry::add(asio::io_context& ioContext, const WorkerHandle& worker,
    std::string_view alias, HttpClientConfigStorage config) {
    auto storedAlias = std::pmr::string(alias, resource_);
    auto pool =
        makePmrObject<HttpClientPool>(resource_, ioContext, worker, std::move(config), resource_);
    pools_.push_back(Entry{std::move(storedAlias), std::move(pool)});
}

void HttpClientRegistry::closeNow() noexcept {
    if (closing_) {
        return;
    }
    closing_ = true;
    for (auto& entry : pools_) {
        entry.pool->closeNow();
    }
}

Task<void> HttpClientRegistry::join() {
    closeNow();
    for (std::size_t i = 0; i < pools_.size(); ++i) {
        co_await pools_[i].pool->join();
    }
}

HttpClientHandle HttpClientRegistry::get(
    std::pmr::memory_resource* resource, ScopedOperationScope& scope) const {
    if (closing_) {
        throw HttpClientError(HttpClientError::Code::kClosing, "http client registry is closing");
    }
    const auto defaultPoolIndex = aliasIndex_.defaultIndex();
    if (!defaultPoolIndex.has_value()) {
        throw HttpClientError(
            HttpClientError::Code::kNotConfigured, "fixed HTTP client is not configured");
    }
    return HttpClientHandle(*pools_[*defaultPoolIndex].pool, resource, scope);
}

HttpClientHandle HttpClientRegistry::get(std::string_view alias,
    std::pmr::memory_resource* resource, ScopedOperationScope& scope) const {
    if (closing_) {
        throw HttpClientError(HttpClientError::Code::kClosing, "http client registry is closing");
    }
    const auto found = aliasIndex_.find(alias);
    if (!found.has_value()) {
        throw HttpClientError(
            HttpClientError::Code::kNotConfigured, "named HTTP client is not configured");
    }
    return HttpClientHandle(*pools_[*found].pool, resource, scope);
}

}  // namespace ruvia::detail
