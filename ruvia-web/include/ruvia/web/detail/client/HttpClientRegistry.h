#pragma once

#include <memory>
#include <memory_resource>
#include <span>
#include <string_view>
#include <vector>

#include <asio/io_context.hpp>

#include "ruvia/core/OperationOptions.h"
#include "ruvia/core/Task.h"
#include "ruvia/core/WorkerHandle.h"
#include "ruvia/core/memory/PmrObject.h"
#include "ruvia/web/HttpClientHandle.h"
#include "ruvia/web/detail/client/HttpClientConfigStorage.h"
#include "ruvia/web/detail/client/HttpClientResultBudget.h"
#include "ruvia/web/detail/integration/NamedCapability.h"

namespace ruvia::detail {

class HttpClientPool;

class HttpClientRegistry final {
public:
    HttpClientRegistry(asio::io_context& ioContext, const WorkerHandle& worker,
        std::pmr::memory_resource* resource, const HttpClientConfig& defaultConfig,
        HttpClientResultBudgetConfig resultBudget = {});
    HttpClientRegistry(asio::io_context& ioContext, const WorkerHandle& worker,
        std::pmr::memory_resource* resource, std::span<const HttpClientDefinition> definitions,
        HttpClientResultBudgetConfig resultBudget = {});
    HttpClientRegistry(asio::io_context& ioContext, const WorkerHandle& worker,
        std::pmr::memory_resource* resource, std::span<const HttpClientDefinition> definitions,
        const std::shared_ptr<HttpClientResultBudgetDomain>& resultBudgetDomain);
    HttpClientRegistry(asio::io_context&, WorkerHandle&&, std::pmr::memory_resource*,
        const HttpClientConfig&, HttpClientResultBudgetConfig = {}) = delete;
    HttpClientRegistry(asio::io_context&, WorkerHandle&&, std::pmr::memory_resource*,
        std::span<const HttpClientDefinition>, HttpClientResultBudgetConfig = {}) = delete;
    HttpClientRegistry(asio::io_context&, WorkerHandle&&, std::pmr::memory_resource*,
        std::span<const HttpClientDefinition>,
        const std::shared_ptr<HttpClientResultBudgetDomain>&) = delete;
    ~HttpClientRegistry();
    HttpClientRegistry(const HttpClientRegistry&) = delete;
    HttpClientRegistry& operator=(const HttpClientRegistry&) = delete;

    void closeNow() noexcept;
    [[nodiscard]] Task<void> join();
    [[nodiscard]] HttpClientHandle get(ScopedOperationScope& scope) const;
    [[nodiscard]] HttpClientHandle get(
        ScopedOperationScope& scope, OperationOptions options) const;
    [[nodiscard]] HttpClientHandle get(std::string_view alias, ScopedOperationScope& scope) const;

private:
    using PoolOwner = std::unique_ptr<HttpClientPool, PmrObjectDeleter<HttpClientPool>>;

    void add(asio::io_context& ioContext, const WorkerHandle& worker,
        HttpClientConfigStorage config, HttpClientResultBudgetConfig resultBudget);
    void add(asio::io_context& ioContext, const WorkerHandle& worker,
        HttpClientConfigStorage config,
        const std::shared_ptr<HttpClientResultBudgetDomain>& resultBudgetDomain);
    std::pmr::memory_resource* resource_;
    std::pmr::vector<PoolOwner> pools_;
    NamedCapabilityIndex aliasIndex_;
    bool closing_{false};
};

}  // namespace ruvia::detail
