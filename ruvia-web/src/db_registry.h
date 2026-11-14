#pragma once

#include <cstddef>
#include <memory>
#include <span>
#include <string_view>
#include <variant>
#include <vector>

#include <asio/io_context.hpp>

#include "db_config_storage.h"
#include "db_pool.h"
#include "db_query_cache.h"
#include "db_session.h"
#include "startup_config.h"
#include "task.h"
#include "worker_ref.h"

#if defined(RUVIA_ENABLE_MARIADB) || defined(RUVIA_ENABLE_POSTGRESQL)

namespace ruvia {
class redis_session;
}

namespace ruvia::detail {

class redis_registry;

class db_registry final {
public:
    db_registry(asio::io_context& io_context, const worker_ref& worker_value, const db_config& default_config);
    db_registry(asio::io_context& io_context, const worker_ref& worker_value, std::span<const db_definition> databases, redis_registry* redis = nullptr);
#ifdef RUVIA_ENABLE_REDIS
    db_registry(asio::io_context& io_context, const worker_ref& worker_value, const db_config& config, const redis_session& redis, const db_cache_config& policy);
#endif
    ~db_registry();

    db_registry(const db_registry&) = delete;
    db_registry& operator=(const db_registry&) = delete;

    task<void> connect();
    void close_now() noexcept;
    [[nodiscard]] bool empty() const noexcept;
    // `io_metrics` is the request's counter block, or null outside a request.
    [[nodiscard]] db_session get(::ruvia::operation_scope& operation_scope, request_io_metrics* io_metrics) const;
    [[nodiscard]] db_session get(std::string_view alias, ::ruvia::operation_scope& operation_scope, request_io_metrics* io_metrics) const;

    template <typename driver_type>
    using pool_owner_ptr_type = std::unique_ptr<db_pool<driver_type>>;
#if defined(RUVIA_ENABLE_MARIADB) && defined(RUVIA_ENABLE_POSTGRESQL)
    using pool_owner_type = std::variant<pool_owner_ptr_type<mariadb_driver>, pool_owner_ptr_type<postgresql_driver>>;
#elif defined(RUVIA_ENABLE_MARIADB)
    using pool_owner_type = std::variant<pool_owner_ptr_type<mariadb_driver>>;
#else
    using pool_owner_type = std::variant<pool_owner_ptr_type<postgresql_driver>>;
#endif

private:
    void add(asio::io_context& io_context, const worker_ref& worker, db_config_storage config);

    struct entry_type final {
        explicit entry_type(pool_owner_type pool) noexcept;
        pool_owner_type pool_;
#ifdef RUVIA_ENABLE_REDIS
        std::unique_ptr<db_query_cache_state> cache_;
#endif
    };

#ifdef RUVIA_ENABLE_REDIS
    void attach_cache(std::size_t index, const worker_ref& worker_value, const redis_session& redis, const db_cache_config_storage& policy, const db_config_storage& config, std::string_view alias);
#endif
    [[nodiscard]] db_session session(const entry_type& entry, ::ruvia::operation_scope& operation_scope, request_io_metrics* io_metrics) const;

    std::vector<entry_type> entries_;
    named_capability_index alias_index_;
#ifdef RUVIA_ENABLE_REDIS
    mutable ::ruvia::operation_scope cache_scope_;
#endif
};

}  // namespace ruvia::detail

#endif
