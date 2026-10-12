#pragma once

#include <memory>
#include <memory_resource>
#include <span>
#include <string_view>
#include <vector>

#include <asio/io_context.hpp>

#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/core/operation_options.h"
#include "ruvia/core/task.h"
#include "ruvia/core/worker_handle.h"
#include "ruvia/web/http_client_handle.h"

#include "client/http_client_config_storage.h"
#include "client/http_client_response_memory.h"
#include "client/http_client_result_budget.h"
#include "integration/named_capability.h"

namespace ruvia::detail {

class http_client_pool;

// Every pool of one owner shares the owner's result budget domain: one per
// standalone client, one per application worker. `response_storage` states
// whether `resource` outlives every response the pools return.
class http_client_registry final {
public:
    http_client_registry(asio::io_context& io_context, const worker_handle& worker_value,
        std::pmr::memory_resource* resource, http_client_response_storage response_storage,
        const http_client_config& default_config,
        const std::shared_ptr<http_client_result_budget_domain>& result_budget_domain);
    http_client_registry(asio::io_context& io_context, const worker_handle& worker_value,
        std::pmr::memory_resource* resource, http_client_response_storage response_storage,
        std::span<const http_client_definition_type> definitions,
        const std::shared_ptr<http_client_result_budget_domain>& result_budget_domain);
    http_client_registry(asio::io_context&, worker_handle&&, std::pmr::memory_resource*, http_client_response_storage,
        const http_client_config&, const std::shared_ptr<http_client_result_budget_domain>&) = delete;
    http_client_registry(asio::io_context&, worker_handle&&, std::pmr::memory_resource*, http_client_response_storage,
        std::span<const http_client_definition_type>,
        const std::shared_ptr<http_client_result_budget_domain>&) = delete;
    ~http_client_registry();
    http_client_registry(const http_client_registry&) = delete;
    http_client_registry& operator=(const http_client_registry&) = delete;

    void close_now() noexcept;
    [[nodiscard]] task<void> join();
    [[nodiscard]] http_client_handle get(::ruvia::operation_scope& scope) const;
    [[nodiscard]] http_client_handle get(
        ::ruvia::operation_scope& scope, operation_options options) const;
    [[nodiscard]] http_client_handle get(std::string_view alias, ::ruvia::operation_scope& scope) const;
    // The default alias's pool, for configuration and statistics reads that
    // need no operation capability.
    [[nodiscard]] http_client_pool& default_pool() const;

private:
    using pool_owner_type = std::unique_ptr<http_client_pool, pmr_object_deleter<http_client_pool>>;

    void add(asio::io_context& io_context, const worker_handle& worker, http_client_config_storage config,
        const std::shared_ptr<http_client_result_budget_domain>& result_budget_domain);
    std::pmr::memory_resource* resource_;
    http_client_response_storage response_storage_;
    std::pmr::vector<pool_owner_type> pools_;
    named_capability_index alias_index_;
    bool closing_{false};
};

}  // namespace ruvia::detail
