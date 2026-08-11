#include "ruvia/web/detail/client/HttpClientRegistry.h"

#include <algorithm>
#include <ranges>
#include <stdexcept>

#include "ruvia/http/detail/util/PmrResource.h"
#include "ruvia/web/detail/client/HttpClientConfigValidation.h"

namespace ruvia::detail {

HttpClientRequestView HttpClientRequestAccess::view(const HttpClientRequest& request, std::pmr::vector<HttpHeaderView>& headers) {
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

HttpClientRegistry::HttpClientRegistry(asio::io_context& ioContext, const WorkerHandle& worker, std::pmr::memory_resource* resource, std::span<const HttpClientDefinition> definitions)
    : ioContext_(ioContext), worker_(worker), resource_(httpPmrResourceOrDefault(resource)), pools_(resource_), aliasIndex_(resource_) {
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

HttpClientHandle HttpClientRegistry::get(std::pmr::memory_resource* resource, ScopedOperationScope& scope) const {
    if (!defaultPoolIndex_) throw HttpClientError(HttpClientError::Code::kNotConfigured, "default http client is not configured");
    return HttpClientHandle(*pools_[*defaultPoolIndex_].pool, resource, scope);
}

HttpClientHandle HttpClientRegistry::get(std::string_view alias, std::pmr::memory_resource* resource, ScopedOperationScope& scope) const {
    const auto match = std::ranges::lower_bound(aliasIndex_, alias, {}, [this](std::size_t i) -> std::string_view { return pools_[i].alias; });
    if (match == aliasIndex_.end() || pools_[*match].alias != alias) {
        throw HttpClientError(HttpClientError::Code::kNotConfigured, "http client is not configured");
    }
    return HttpClientHandle(*pools_[*match].pool, resource, scope);
}

}  // namespace ruvia::detail
