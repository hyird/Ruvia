#include "ruvia/web/detail/client/HttpClientRegistry.h"

#include <algorithm>
#include <ranges>
#include <stdexcept>

#include "ruvia/http/detail/util/PmrResource.h"
#include "ruvia/web/detail/client/HttpClientConfigValidation.h"

namespace ruvia::detail {
namespace {
thread_local HttpClientRegistry* currentHttpClientRegistry = nullptr;
}

HttpClientRequestView HttpClientRequestAccess::view(const HttpClientRequest& request, std::pmr::vector<HttpHeaderView>& headers) {
    headers.clear();
    headers.reserve(request.headers_.size());
    for (const auto& header : request.headers_) headers.emplace_back(header.name, header.value);
    HttpClientRequestView result;
    result.method = request.method_;
    result.target = request.path_;
    result.headers = std::span<const HttpHeaderView>(headers);
    result.content = request.hasBody_ ? HttpClientRequestContentView::bytes(request.body_) : HttpClientRequestContentView::none();
    return result;
}

bool HttpClientRequestAccess::usesResource(const HttpClientRequest& request, std::pmr::memory_resource* resource) noexcept {
    auto* const resolved = httpPmrResourceOrDefault(resource);
    if (request.method_.get_allocator().resource() != resolved ||
        request.path_.get_allocator().resource() != resolved ||
        request.headers_.get_allocator().resource() != resolved ||
        request.body_.get_allocator().resource() != resolved) {
        return false;
    }
    return std::ranges::all_of(request.headers_, [resolved](const HttpClientRequest::Header& header) noexcept {
        return header.name.get_allocator().resource() == resolved &&
            header.value.get_allocator().resource() == resolved;
    });
}

HttpClientRequest HttpClientRequestAccess::clone(const HttpClientRequest& request, std::pmr::memory_resource* resource) {
    auto* targetResource = httpPmrResourceOrDefault(resource);
    HttpClientRequest copy(targetResource);
    copy.method_.assign(request.method_);
    copy.path_.assign(request.path_);
    copy.headers_.reserve(request.headers_.size());
    for (const auto& header : request.headers_) {
        copy.headers_.emplace_back(header.name, header.value, targetResource);
    }
    copy.body_.assign(request.body_);
    copy.hasBody_ = request.hasBody_;
    return copy;
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
        pools_.push_back(Entry{std::pmr::string(definition.alias, resource_), std::move(pool), 0});
        if (pools_.back().alias == "default") defaultPoolIndex_ = pools_.size() - 1;
    }
    aliasIndex_.resize(pools_.size());
    for (std::size_t i = 0; i < aliasIndex_.size(); ++i) aliasIndex_[i] = i;
    std::ranges::sort(aliasIndex_, {}, [this](std::size_t i) -> std::string_view { return pools_[i].alias; });
}

HttpClientPool& HttpClientRegistry::getOrCreate(std::uint64_t dynamicId, const HttpClientConfig& publicConfig) {
    if (dynamicId == 0) throw std::invalid_argument("dynamic HTTP client id must not be zero");
    if (closing_) throw HttpClientError(HttpClientError::Code::kClosing, "HTTP client registry is closing");
    const auto existing = std::ranges::find_if(pools_, [dynamicId](const Entry& entry) { return entry.dynamicId == dynamicId; });
    if (existing != pools_.end()) return *existing->pool;
    HttpClientConfigStorage config(publicConfig, resource_);
    validateHttpClientConfig(config);
    auto pool = makePmrObject<HttpClientPool>(resource_, ioContext_, worker_, std::move(config), resource_);
    pools_.push_back(Entry{std::pmr::string(resource_), std::move(pool), dynamicId});
    return *pools_.back().pool;
}

HttpClientPool* HttpClientRegistry::find(std::uint64_t dynamicId) noexcept {
    const auto existing = std::ranges::find_if(pools_, [dynamicId](const Entry& entry) { return entry.dynamicId == dynamicId; });
    return existing == pools_.end() ? nullptr : existing->pool.get();
}

void HttpClientRegistry::bindCurrent() noexcept {
    if (currentHttpClientRegistry != nullptr && currentHttpClientRegistry != this) std::terminate();
    currentHttpClientRegistry = this;
}

void HttpClientRegistry::unbindCurrent() noexcept {
    if (currentHttpClientRegistry != this) std::terminate();
    currentHttpClientRegistry = nullptr;
}

HttpClientRegistry* HttpClientRegistry::current() noexcept { return currentHttpClientRegistry; }

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

HttpClient HttpClientRegistry::get(std::pmr::memory_resource* resource, ScopedOperationScope& scope) const {
    if (!defaultPoolIndex_) throw HttpClientError(HttpClientError::Code::kNotConfigured, "default http client is not configured");
    return HttpClient(*pools_[*defaultPoolIndex_].pool, resource, scope);
}

HttpClient HttpClientRegistry::get(std::string_view alias, std::pmr::memory_resource* resource, ScopedOperationScope& scope) const {
    const auto match = std::ranges::lower_bound(aliasIndex_, alias, {}, [this](std::size_t i) -> std::string_view { return pools_[i].alias; });
    if (match == aliasIndex_.end() || pools_[*match].alias != alias) {
        throw HttpClientError(HttpClientError::Code::kNotConfigured, "http client is not configured");
    }
    return HttpClient(*pools_[*match].pool, resource, scope);
}

}  // namespace ruvia::detail
