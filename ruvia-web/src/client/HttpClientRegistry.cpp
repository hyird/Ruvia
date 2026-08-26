#include "ruvia/web/detail/client/HttpClientRegistry.h"

#include <algorithm>
#include <ranges>
#include <stdexcept>

#include "ruvia/http/detail/util/PmrResource.h"
#include "ruvia/web/detail/client/HttpClientConfigValidation.h"

namespace ruvia::detail {

HttpClientRequestView HttpClientRequestStorageAccess::view(
    const HttpClientRequestStorage& request, std::pmr::vector<HttpHeaderView>& headers) {
    headers.clear();
    headers.reserve(request.headers_.size());
    for (const auto& header : request.headers_) headers.emplace_back(header.name, header.value);
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
    : resource_(httpPmrResourceOrDefault(resource)),
      pools_(resource_),
      aliasIndex_(resource_) {
    pools_.reserve(definitions.size());
    for (const auto& definition : definitions) {
        if (definition.alias.empty())
            throw std::invalid_argument("http client alias must not be empty");
        if (std::ranges::any_of(pools_,
                [&definition](const Entry& entry) { return entry.alias == definition.alias; })) {
            throw std::invalid_argument("duplicate http client alias");
        }
        HttpClientConfigStorage config(definition.config, resource_);
        validateHttpClientConfig(config);
        auto pool = makePmrObject<HttpClientPool>(
            resource_, ioContext, worker, std::move(config), resource_);
        pools_.push_back(Entry{std::pmr::string(definition.alias, resource_), std::move(pool)});
        if (pools_.back().alias == "default") defaultPoolIndex_ = pools_.size() - 1;
    }
    aliasIndex_.resize(pools_.size());
    for (std::size_t i = 0; i < aliasIndex_.size(); ++i) aliasIndex_[i] = i;
    std::ranges::sort(
        aliasIndex_, {}, [this](std::size_t i) -> std::string_view { return pools_[i].alias; });
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

HttpClientHandle HttpClientRegistry::get(
    std::pmr::memory_resource* resource, ScopedOperationScope& scope) const {
    if (closing_)
        throw HttpClientError(HttpClientError::Code::kClosing, "http client registry is closing");
    if (!defaultPoolIndex_)
        throw HttpClientError(
            HttpClientError::Code::kNotConfigured, "fixed HTTP client is not configured");
    return HttpClientHandle(*pools_[*defaultPoolIndex_].pool, resource, scope);
}

HttpClientHandle HttpClientRegistry::get(std::string_view alias,
    std::pmr::memory_resource* resource, ScopedOperationScope& scope) const {
    if (closing_)
        throw HttpClientError(HttpClientError::Code::kClosing, "http client registry is closing");
    const auto found = std::ranges::lower_bound(aliasIndex_, alias, {},
        [this](std::size_t index) -> std::string_view { return pools_[index].alias; });
    if (found == aliasIndex_.end() || std::string_view(pools_[*found].alias) != alias) {
        throw HttpClientError(
            HttpClientError::Code::kNotConfigured, "named HTTP client is not configured");
    }
    return HttpClientHandle(*pools_[*found].pool, resource, scope);
}

}  // namespace ruvia::detail
