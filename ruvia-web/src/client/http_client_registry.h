#pragma once

#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>

#include <asio/io_context.hpp>

#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/core/operation_timeout.h"
#include "ruvia/core/scoped_operation.h"
#include "ruvia/core/task.h"
#include "ruvia/core/task_scope.h"
#include "ruvia/core/worker_handle.h"
#include "ruvia/web/http_client.h"

#include "client/http_client_config_storage.h"
#include "client/http_client_response_memory.h"
#include "client/http_client_result_budget.h"
#include "integration/named_capability.h"

namespace ruvia::detail {

class http_client_pool;

// Every pool of one owner shares the owner's result budget domain: one per
// standalone client, one per application worker. Process-lifetime storage
// backs the pools and their retained responses.
class http_client_registry final {
public:
    http_client_registry(asio::io_context& io_context, const worker_handle& worker_value,
        std::span<const http_client_definition_type> definitions,
        const std::shared_ptr<http_client_result_budget_domain>& result_budget_domain);
    http_client_registry(asio::io_context& io_context, const worker_handle& worker_value,
        std::span<const http_client_config> configs,
        const std::shared_ptr<http_client_result_budget_domain>& result_budget_domain);
    http_client_registry(asio::io_context&, worker_handle&&,
        std::span<const http_client_definition_type>,
        const std::shared_ptr<http_client_result_budget_domain>&) = delete;
    http_client_registry(asio::io_context&, worker_handle&&,
        std::span<const http_client_config>,
        const std::shared_ptr<http_client_result_budget_domain>&) = delete;
    ~http_client_registry();
    http_client_registry(const http_client_registry&) = delete;
    http_client_registry& operator=(const http_client_registry&) = delete;

    // Owns inputs immediately; TLS files are prepared off-worker before the
    // atomic publication. Shutdown joins started preparation, and discarded
    // cold operations cannot publish. Removed pools retain only live borrows.
    [[nodiscard]] task<void> upsert(const http_client_config& config, operation_options options = {});
    [[nodiscard]] bool remove(std::string_view alias);

    void close_now() noexcept;
    [[nodiscard]] task<void> join();
    [[nodiscard]] http_client_handle get(::ruvia::operation_scope& scope, stop_token host_stop = {}) const;
    [[nodiscard]] http_client_handle get(std::string_view alias, ::ruvia::operation_scope& scope, stop_token host_stop = {}) const;
    [[nodiscard]] http_client_handle get_single_or_default(::ruvia::operation_scope& scope, stop_token host_stop = {}) const;
    [[nodiscard]] std::shared_ptr<http_client_pool> find_origin(http_scheme scheme, std::string_view authority) const;

private:
    using pool_owner_type = std::shared_ptr<http_client_pool>;
    struct alias_binding final {
        pool_owner_type pool_;
        std::size_t order_;
    };
    using alias_map_type = std::pmr::map<std::pmr::string, alias_binding, std::less<>>;
    struct origin_preparation;
    using preparation_owner_type = std::unique_ptr<origin_preparation, pmr_object_deleter<origin_preparation>>;

    [[nodiscard]] task<void> prepare_origin(preparation_owner_type input, operation_options options);
    void require_prepared(stop_token stop, const operation_timeout& timeout) const;
    void publish(std::pmr::string alias, pool_owner_type replacement);

    void require_mutable() const;
    [[nodiscard]] task<void> retire_origin(alias_map_type::iterator position,
        pool_owner_type replacement, task<void> joining);

    template <typename config_type>
    void initialize(std::span<const config_type> configs);
    asio::io_context& io_context_;
    const worker_handle& worker_;
    std::shared_ptr<http_client_result_budget_domain> result_budget_domain_;
    alias_map_type aliases_;
    task_scope retirements_;
    stop_source preparation_stop_;
    std::size_t next_origin_order_{};
    bool closing_{false};
    operation_scope preparations_;
};

}  // namespace ruvia::detail
