#include "ruvia/web/detail/client/HttpClientRegistry.h"

#include <exception>

#include "ruvia/core/memory/PmrResource.h"
#include "ruvia/web/detail/client/HttpClientConfigStorage.h"
#include "ruvia/web/detail/client/HttpClientPool.h"

namespace ruvia::detail {
namespace {

// This runs in the first member initializer so invalid input is rejected before
// even implementation-specific PMR container bookkeeping can touch owner memory.
[[nodiscard]] std::pmr::memory_resource* validatedHttpClientResource(
    std::pmr::memory_resource* resource, std::span<const HttpClientDefinition> definitions) {
    validateCapabilityAliases(
        definitions, "HTTP client alias must not be empty", "duplicate HTTP client alias");
    return pmrResourceOrDefault(resource);
}

[[nodiscard]] std::pmr::memory_resource* validatedHttpClientResource(
    std::pmr::memory_resource* resource, std::span<const HttpClientDefinition> definitions,
    const std::shared_ptr<HttpClientResultBudgetDomain>& resultBudgetDomain) {
    if (!resultBudgetDomain) {
        throw std::invalid_argument("HTTP client result budget domain must not be null");
    }
    return validatedHttpClientResource(resource, definitions);
}

}  // namespace

HttpClientRegistry::HttpClientRegistry(asio::io_context& ioContext, const WorkerHandle& worker,
    std::pmr::memory_resource* resource, const HttpClientConfig& defaultConfig,
    HttpClientResultBudgetConfig resultBudget)
    : resource_(pmrResourceOrDefault(resource)),
      pools_(resource_),
      aliasIndex_(resource_) {
    aliasIndex_.build({kDefaultCapabilityAlias});
    pools_.reserve(1);
    add(ioContext, worker, HttpClientConfigStorage(defaultConfig, resource_), resultBudget);
}

HttpClientRegistry::HttpClientRegistry(asio::io_context& ioContext, const WorkerHandle& worker,
    std::pmr::memory_resource* resource, std::span<const HttpClientDefinition> definitions,
    HttpClientResultBudgetConfig resultBudget)
    : resource_(validatedHttpClientResource(resource, definitions)),
      pools_(resource_),
      aliasIndex_(resource_) {
    aliasIndex_.build(definitions);
    pools_.reserve(definitions.size());
    for (const auto& definition : definitions) {
        add(ioContext, worker, HttpClientConfigStorage(definition.config, resource_), resultBudget);
    }
}

HttpClientRegistry::HttpClientRegistry(asio::io_context& ioContext, const WorkerHandle& worker,
    std::pmr::memory_resource* resource, std::span<const HttpClientDefinition> definitions,
    const std::shared_ptr<HttpClientResultBudgetDomain>& resultBudgetDomain)
    : resource_(validatedHttpClientResource(resource, definitions, resultBudgetDomain)),
      pools_(resource_),
      aliasIndex_(resource_) {
    aliasIndex_.build(definitions);
    pools_.reserve(definitions.size());
    for (const auto& definition : definitions) {
        add(ioContext, worker, HttpClientConfigStorage(definition.config, resource_),
            resultBudgetDomain);
    }
}

HttpClientRegistry::~HttpClientRegistry() = default;

void HttpClientRegistry::add(asio::io_context& ioContext, const WorkerHandle& worker,
    HttpClientConfigStorage config, HttpClientResultBudgetConfig resultBudget) {
    auto pool = makePmrObject<HttpClientPool>(
        resource_, ioContext, worker, std::move(config), resultBudget, resource_);
    pools_.push_back(std::move(pool));
}

void HttpClientRegistry::add(asio::io_context& ioContext, const WorkerHandle& worker,
    HttpClientConfigStorage config,
    const std::shared_ptr<HttpClientResultBudgetDomain>& resultBudgetDomain) {
    auto pool = makePmrObject<HttpClientPool>(
        resource_, ioContext, worker, std::move(config), resultBudgetDomain, resource_);
    pools_.push_back(std::move(pool));
}

void HttpClientRegistry::closeNow() noexcept {
    if (closing_) {
        return;
    }
    closing_ = true;
    for (auto& pool : pools_) {
        pool->closeNow();
    }
}

Task<void> HttpClientRegistry::join() {
    closeNow();
    std::exception_ptr failure;
    for (std::size_t i = 0; i < pools_.size(); ++i) {
        try {
            co_await pools_[i]->join();
        } catch (...) {
            if (failure == nullptr) {
                failure = std::current_exception();
            }
        }
    }
    if (failure != nullptr) {
        std::rethrow_exception(failure);
    }
}

HttpClientHandle HttpClientRegistry::get(ScopedOperationScope& scope) const {
    if (closing_) {
        throw HttpClientError(HttpClientError::Code::kClosing, "http client registry is closing");
    }
    const auto defaultPoolIndex = aliasIndex_.defaultIndex();
    if (!defaultPoolIndex.has_value()) {
        throw HttpClientError(
            HttpClientError::Code::kNotConfigured, "fixed HTTP client is not configured");
    }
    return HttpClientHandle(*pools_[*defaultPoolIndex], resource_, scope);
}

HttpClientHandle HttpClientRegistry::get(
    ScopedOperationScope& scope, OperationOptions options) const {
    if (closing_) {
        throw HttpClientError(HttpClientError::Code::kClosing, "http client registry is closing");
    }
    const auto defaultPoolIndex = aliasIndex_.defaultIndex();
    if (!defaultPoolIndex.has_value()) {
        throw HttpClientError(
            HttpClientError::Code::kNotConfigured, "fixed HTTP client is not configured");
    }
    validateOperationOptions(options);
    return HttpClientHandle(
        *pools_[*defaultPoolIndex], resource_, scope, std::move(options));
}

HttpClientHandle HttpClientRegistry::get(
    std::string_view alias, ScopedOperationScope& scope) const {
    if (closing_) {
        throw HttpClientError(HttpClientError::Code::kClosing, "http client registry is closing");
    }
    const auto found = aliasIndex_.find(alias);
    if (!found.has_value()) {
        throw HttpClientError(
            HttpClientError::Code::kNotConfigured, "named HTTP client is not configured");
    }
    return HttpClientHandle(*pools_[*found], resource_, scope);
}

}  // namespace ruvia::detail
