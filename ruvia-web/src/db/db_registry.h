#pragma once

#include "ruvia/core/worker_handle.h"
#include "ruvia/web/db/db.h"
#include "ruvia/web/detail/db/db_backend.h"

#include "db/db_config_storage.h"

namespace ruvia {
class redis_handle;
}

#if !defined(RUVIA_ENABLE_MARIADB) && !defined(RUVIA_ENABLE_POSTGRESQL)

#include <memory_resource>
#include <span>

#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>

namespace ruvia::detail {
class redis_registry;

class db_registry final {
public:
    db_registry(asio::io_context&, const worker_handle&, std::pmr::memory_resource*,
        std::span<const db_definition>, redis_registry* = nullptr) {}

    db_registry(const db_registry&) = delete;
    db_registry& operator=(const db_registry&) = delete;

    [[nodiscard]] task<void> connect() {
        co_return;
    }
    void close_now() noexcept {}
    [[nodiscard]] bool empty() const noexcept {
        return true;
    }
};

}  // namespace ruvia::detail

#else

#include <memory>
#include <memory_resource>
#include <span>
#include <string_view>
#include <variant>
#include <vector>

#include <asio/io_context.hpp>

#include "ruvia/core/memory/pmr_object.h"

#include "db/db_pool.h"
#include "integration/named_capability.h"

namespace ruvia::detail {

class redis_registry;
#ifdef RUVIA_ENABLE_REDIS
class db_query_cache_state;
#endif

class db_registry final {
public:
    db_registry(asio::io_context& io_context, const worker_handle& worker_value,
        std::pmr::memory_resource* resource, const db_config& default_config);
    db_registry(asio::io_context& io_context, const worker_handle& worker_value,
        std::pmr::memory_resource* resource, std::span<const db_definition> databases, redis_registry* redis = nullptr);
#ifdef RUVIA_ENABLE_REDIS
    db_registry(asio::io_context& io_context, const worker_handle& worker_value,
        std::pmr::memory_resource* resource, const db_config& config,
        const redis_handle& redis, const db_cache_config& policy);
#endif
    ~db_registry();

    db_registry(const db_registry&) = delete;
    db_registry& operator=(const db_registry&) = delete;

    task<void> connect();
    void close_now() noexcept;
    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] db_handle get(::ruvia::operation_scope& operation_scope) const;
    [[nodiscard]] db_handle get(std::string_view alias, ::ruvia::operation_scope& operation_scope) const;

    template <typename driver_type>
    using pool_owner_ptr_type = std::unique_ptr<db_pool<driver_type>, pmr_object_deleter<db_pool<driver_type>>>;
#if defined(RUVIA_ENABLE_MARIADB) && defined(RUVIA_ENABLE_POSTGRESQL)
    using pool_owner_type = std::variant<pool_owner_ptr_type<mariadb_driver>, pool_owner_ptr_type<postgresql_driver>>;
#elif defined(RUVIA_ENABLE_MARIADB)
    using pool_owner_type = std::variant<pool_owner_ptr_type<mariadb_driver>>;
#else
    using pool_owner_type = std::variant<pool_owner_ptr_type<postgresql_driver>>;
#endif

private:
    void add(asio::io_context& io_context, const worker_handle& worker, db_config_storage config);

    std::pmr::memory_resource* resource_;
    struct entry_type final {
        pool_owner_type pool_;
#ifdef RUVIA_ENABLE_REDIS
        std::unique_ptr<db_query_cache_state, pmr_object_deleter<db_query_cache_state>> cache_;
#endif
    };
#ifdef RUVIA_ENABLE_REDIS
    void attach_cache(std::size_t index, const worker_handle& worker, const redis_handle& redis,
        const db_cache_config_storage& policy, const db_config_storage& config, std::string_view alias);
    ::ruvia::operation_scope cache_scope_;
#endif
    [[nodiscard]] db_handle handle(const entry_type& entry, ::ruvia::operation_scope& operation_scope) const;
    std::pmr::vector<entry_type> entries_;
    named_capability_index alias_index_;
};

}  // namespace ruvia::detail

#endif
