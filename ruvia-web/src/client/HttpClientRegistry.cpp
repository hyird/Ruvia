#include "ruvia/web/detail/client/HttpClientRegistry.h"

#include <algorithm>
#include <ranges>
#include <stdexcept>

#include "ruvia/core/memory/PmrResource.h"

namespace ruvia::detail {

HttpClientRegistry::HttpClientRegistry(asio::io_context& ioContext, const WorkerHandle& worker,
    std::pmr::memory_resource* resource, const HttpClientConfig& defaultConfig)
    : resource_(pmrResourceOrDefault(resource)),
      pools_(resource_),
      aliasIndex_(resource_) {
    pools_.reserve(1);
    add(ioContext, worker, "default", HttpClientConfigStorage(defaultConfig, resource_));
    buildAliasIndex();
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
    pools_.reserve(definitions.size());
    for (const auto& definition : definitions) {
        add(ioContext, worker, definition.alias,
            HttpClientConfigStorage(definition.config, resource_));
    }
    buildAliasIndex();
}

HttpClientRegistry::~HttpClientRegistry() = default;

void HttpClientRegistry::add(asio::io_context& ioContext, const WorkerHandle& worker,
    std::string_view alias, HttpClientConfigStorage config) {
    if (alias.empty()) {
        throw std::invalid_argument("http client alias must not be empty");
    }
    if (std::ranges::any_of(pools_, [alias](const Entry& entry) { return entry.alias == alias; })) {
        throw std::invalid_argument("duplicate http client alias");
    }
    auto storedAlias = std::pmr::string(alias, resource_);
    auto pool =
        makePmrObject<HttpClientPool>(resource_, ioContext, worker, std::move(config), resource_);
    pools_.push_back(Entry{std::move(storedAlias), std::move(pool)});
    if (pools_.back().alias == "default") {
        defaultPoolIndex_ = pools_.size() - 1;
    }
}

void HttpClientRegistry::buildAliasIndex() {
    aliasIndex_.resize(pools_.size());
    for (std::size_t i = 0; i < aliasIndex_.size(); ++i) {
        aliasIndex_[i] = i;
    }
    std::ranges::sort(
        aliasIndex_, {}, [this](std::size_t i) -> std::string_view { return pools_[i].alias; });
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
    if (!defaultPoolIndex_) {
        throw HttpClientError(
            HttpClientError::Code::kNotConfigured, "fixed HTTP client is not configured");
    }
    return HttpClientHandle(*pools_[*defaultPoolIndex_].pool, resource, scope);
}

HttpClientHandle HttpClientRegistry::get(std::string_view alias,
    std::pmr::memory_resource* resource, ScopedOperationScope& scope) const {
    if (closing_) {
        throw HttpClientError(HttpClientError::Code::kClosing, "http client registry is closing");
    }
    const auto found = std::ranges::lower_bound(aliasIndex_, alias, {},
        [this](std::size_t index) -> std::string_view { return pools_[index].alias; });
    if (found == aliasIndex_.end() || std::string_view(pools_[*found].alias) != alias) {
        throw HttpClientError(
            HttpClientError::Code::kNotConfigured, "named HTTP client is not configured");
    }
    return HttpClientHandle(*pools_[*found].pool, resource, scope);
}

}  // namespace ruvia::detail
