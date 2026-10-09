#include "client/http_client_registry.h"

#include <exception>

#include "ruvia/core/memory/pmr_resource.h"

#include "client/http_client_config_storage.h"
#include "client/http_client_pool.h"

namespace ruvia::detail {
namespace {

// This runs in the first member initializer so invalid input is rejected before
// even implementation-specific PMR container bookkeeping can touch owner memory.
[[nodiscard]] std::pmr::memory_resource* validated_http_client_resource(
    std::pmr::memory_resource* resource, std::span<const http_client_definition_type> definitions) {
    validate_capability_aliases(
        definitions, "HTTP client alias must not be empty", "duplicate HTTP client alias");
    return pmr_resource_or_default(resource);
}

[[nodiscard]] std::pmr::memory_resource* validated_http_client_resource(
    std::pmr::memory_resource* resource, std::span<const http_client_definition_type> definitions,
    const std::shared_ptr<http_client_result_budget_domain>& result_budget_domain) {
    if (!result_budget_domain) {
        throw std::invalid_argument("HTTP client result budget domain must not be null");
    }
    return validated_http_client_resource(resource, definitions);
}

}  // namespace

http_client_registry::http_client_registry(asio::io_context& io_context, const worker_handle& worker_value,
    std::pmr::memory_resource* resource, const http_client_config& default_config,
    http_client_result_budget_config result_budget)
    : resource_(pmr_resource_or_default(resource)),
      pools_(resource_),
      alias_index_(resource_) {
    alias_index_.build({default_capability_alias});
    pools_.reserve(1);
    add(io_context, worker_value, http_client_config_storage(default_config, resource_), result_budget);
}

http_client_registry::http_client_registry(asio::io_context& io_context, const worker_handle& worker_value,
    std::pmr::memory_resource* resource, std::span<const http_client_definition_type> definitions,
    http_client_result_budget_config result_budget)
    : resource_(validated_http_client_resource(resource, definitions)),
      pools_(resource_),
      alias_index_(resource_) {
    alias_index_.build(definitions);
    pools_.reserve(definitions.size());
    for (const auto& definition : definitions) {
        add(io_context, worker_value, http_client_config_storage(definition.config_, resource_), result_budget);
    }
}

http_client_registry::http_client_registry(asio::io_context& io_context, const worker_handle& worker_value,
    std::pmr::memory_resource* resource, std::span<const http_client_definition_type> definitions,
    const std::shared_ptr<http_client_result_budget_domain>& result_budget_domain)
    : resource_(validated_http_client_resource(resource, definitions, result_budget_domain)),
      pools_(resource_),
      alias_index_(resource_) {
    alias_index_.build(definitions);
    pools_.reserve(definitions.size());
    for (const auto& definition : definitions) {
        add(io_context, worker_value, http_client_config_storage(definition.config_, resource_),
            result_budget_domain);
    }
}

http_client_registry::~http_client_registry() = default;

void http_client_registry::add(asio::io_context& io_context, const worker_handle& worker_value,
    http_client_config_storage config, http_client_result_budget_config result_budget) {
    auto pool = make_pmr_object<http_client_pool>(
        resource_, io_context, worker_value, std::move(config), result_budget, resource_);
    pools_.push_back(std::move(pool));
}

void http_client_registry::add(asio::io_context& io_context, const worker_handle& worker_value,
    http_client_config_storage config,
    const std::shared_ptr<http_client_result_budget_domain>& result_budget_domain) {
    auto pool = make_pmr_object<http_client_pool>(
        resource_, io_context, worker_value, std::move(config), result_budget_domain, resource_);
    pools_.push_back(std::move(pool));
}

void http_client_registry::close_now() noexcept {
    if (closing_) {
        return;
    }
    closing_ = true;
    for (auto& pool : pools_) {
        pool->close_now();
    }
}

task<void> http_client_registry::join() {
    close_now();
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

http_client_handle http_client_registry::get(::ruvia::operation_scope& scope) const {
    if (closing_) {
        throw http_client_error(http_client_error::code_type::closing, "http client registry is closing");
    }
    const auto default_pool_index = alias_index_.default_index();
    if (!default_pool_index.has_value()) {
        throw http_client_error(
            http_client_error::code_type::not_configured, "fixed HTTP client is not configured");
    }
    return http_client_handle(*pools_[*default_pool_index], resource_, scope);
}

http_client_handle http_client_registry::get(
    ::ruvia::operation_scope& scope, operation_options options) const {
    if (closing_) {
        throw http_client_error(http_client_error::code_type::closing, "http client registry is closing");
    }
    const auto default_pool_index = alias_index_.default_index();
    if (!default_pool_index.has_value()) {
        throw http_client_error(
            http_client_error::code_type::not_configured, "fixed HTTP client is not configured");
    }
    validate_operation_options(options);
    return http_client_handle(
        *pools_[*default_pool_index], resource_, scope, std::move(options));
}

http_client_handle http_client_registry::get(
    std::string_view alias, ::ruvia::operation_scope& scope) const {
    if (closing_) {
        throw http_client_error(http_client_error::code_type::closing, "http client registry is closing");
    }
    const auto found = alias_index_.find(alias);
    if (!found.has_value()) {
        throw http_client_error(
            http_client_error::code_type::not_configured, "named HTTP client is not configured");
    }
    return http_client_handle(*pools_[*found], resource_, scope);
}

}  // namespace ruvia::detail
