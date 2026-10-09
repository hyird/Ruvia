#include "db/db_registry.h"

#include <algorithm>
#include <chrono>
#include <exception>
#include <memory>
#include <memory_resource>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>

#include "ruvia/web/detail/db/db_utils.h"

#include "db/db_query_cache_state.h"
#include "redis/redis_registry.h"

namespace ruvia {
namespace {

[[nodiscard]] detail::db_pool_ref_type pool_ref(const detail::db_registry::pool_owner_type& owner_value) noexcept {
    return std::visit(
        [](const auto& value) -> detail::db_pool_ref_type {
            using value_type = std::remove_cvref_t<decltype(value)>;
            if constexpr (std::is_same_v<value_type, std::monostate>) {
                return {};
            } else {
                return value == nullptr ? detail::db_pool_ref_type{} : detail::db_pool_ref_type{value.get()};
            }
        },
        owner_value);
}

task<void> connect_pool(detail::db_pool_ref_type pool) {
    return detail::visit_db_pool(pool, [](auto& client) { return client.connect(); });
}

void close_pool(detail::db_pool_ref_type pool) noexcept {
    detail::visit_db_pool_if_present(pool, [](auto& client) noexcept { client.close_now(); });
}

}  // namespace

detail::db_registry::db_registry(asio::io_context& io_context, const worker_handle& worker_value,
    std::pmr::memory_resource* resource, const db_config& default_config)
    : resource_(detail::pmr_resource_or_default(resource)),
      entries_(resource_),
      alias_index_(resource_) {
    alias_index_.build({default_capability_alias});
    entries_.reserve(1);
    add(io_context, worker_value, db_config_storage(default_config, resource_));
}

detail::db_registry::db_registry(asio::io_context& io_context, const worker_handle& worker_value,
    std::pmr::memory_resource* resource, std::span<const detail::db_definition> databases, redis_registry* redis)
    : resource_(detail::pmr_resource_or_default(resource)),
      entries_(resource_),
      alias_index_(resource_) {
    validate_capability_aliases(
        databases, "database alias must not be empty", "duplicate database alias");
    alias_index_.build(databases);
    entries_.reserve(databases.size());
    for (const auto& definition : databases) {
        add(io_context, worker_value, db_config_storage(definition.config_, resource_));
        if (definition.query_cache_) {
#ifdef RUVIA_ENABLE_REDIS
            if (redis == nullptr) {
                throw std::invalid_argument("database query cache requires an existing Redis registry");
            }
            const auto store_value = redis->get(definition.query_cache_->redis_alias_, cache_scope_);
            attach_cache(entries_.size() - 1, worker_value, store_value, definition.query_cache_->policy_,
                definition.config_, definition.alias_);
#else
            (void)redis;
            throw std::invalid_argument("database query caching requires Redis support");
#endif
        }
    }
}

detail::db_registry::db_registry(asio::io_context& io_context, const worker_handle& worker_value,
    std::pmr::memory_resource* resource, const db_config& config,
    const redis_handle& redis, const db_cache_config& policy)
    : db_registry(io_context, worker_value, resource, config) {
    attach_cache(0, worker_value, redis, db_cache_config_storage(policy, resource_),
        db_config_storage(config, resource_), default_capability_alias);
}

void detail::db_registry::attach_cache(std::size_t index, const worker_handle& worker_value,
    const redis_handle& redis, const db_cache_config_storage& policy,
    const db_config_storage& config, std::string_view alias) {
#ifdef RUVIA_ENABLE_REDIS
    if (redis.worker().id() != worker_value.id()) {
        throw std::invalid_argument("database query cache Redis capability must belong to the same worker");
    }
    const auto identity = db_cache_scope(policy.name_space_, alias, config, resource_);
    entries_[index].cache_ = make_pmr_object<db_query_cache_state>(resource_, redis, policy, identity, resource_);
#else
    (void)index;
    (void)worker_value;
    (void)redis;
    (void)policy;
    (void)config;
    (void)alias;
    throw std::invalid_argument("database query caching requires Redis support");
#endif
}

detail::db_registry::~db_registry() = default;

void detail::db_registry::add(
    asio::io_context& io_context, const worker_handle& worker_value, db_config_storage config) {
    pool_owner_type owner;
    switch (config.driver_) {
        case db_driver::unspecified:
            std::terminate();
        case db_driver::mariadb:
#ifdef RUVIA_ENABLE_MARIADB
            owner = detail::make_pmr_object<mariadb_pool>(
                resource_, io_context, worker_value, std::move(config), resource_);
            break;
#else
            std::terminate();
#endif
        case db_driver::postgresql:
#ifdef RUVIA_ENABLE_POSTGRESQL
            owner = detail::make_pmr_object<postgresql_pool>(
                resource_, io_context, worker_value, std::move(config), resource_);
            break;
#else
            std::terminate();
#endif
    }

    entries_.push_back(entry_type{std::move(owner), {}});
}

task<void> detail::db_registry::connect() {
    for (auto& entry : entries_) {
        co_await connect_pool(pool_ref(entry.pool_));
    }
}

void detail::db_registry::close_now() noexcept {
    for (auto& entry : entries_) {
        if (entry.cache_) {
            entry.cache_->close_now();
        }
        close_pool(pool_ref(entry.pool_));
    }
}

bool detail::db_registry::empty() const noexcept {
    return entries_.empty();
}

db_handle detail::db_registry::get(::ruvia::operation_scope& operation_scope) const {
    const auto default_pool_index = alias_index_.default_index();
    if (!default_pool_index.has_value()) {
        throw db_error(
            db_error::code_type::not_configured, std::nullopt, "default database is not configured");
    }
    return db_handle(pool_ref(entries_[*default_pool_index].pool_), resource_, operation_scope, entries_[*default_pool_index].cache_.get());
}

db_handle detail::db_registry::get(
    std::string_view alias, ::ruvia::operation_scope& operation_scope) const {
    const auto match = alias_index_.find(alias);
    if (match.has_value()) {
        return db_handle(pool_ref(entries_[*match].pool_), resource_, operation_scope, entries_[*match].cache_.get());
    }
    throw db_error(db_error::code_type::not_configured, std::nullopt, "database is not configured");
}

}  // namespace ruvia
