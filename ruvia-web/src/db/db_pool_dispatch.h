#pragma once

#include <stdexcept>
#include <utility>
#include <variant>

#include "ruvia/web/db/db_types.h"
#include "ruvia/web/detail/db/db_backend.h"

#include "db/db_pool.h"

namespace ruvia::detail {

// Every pool operation dispatches on the configured backend; reaching the end
// means the build has no driver for it.
[[noreturn]] inline void throw_unavailable_db_backend() {
    throw std::logic_error("database backend is not available");
}

// db_pool_ref_type is a closed backend set. Keep its single checked dispatch here so
// handle, stream, transaction, and registry operations cannot drift into
// subtly different null or unavailable-backend behavior.
template <typename visitor_type>
decltype(auto) visit_db_pool([[maybe_unused]] const db_pool_ref_type& pool, [[maybe_unused]] visitor_type&& visitor) {
#ifdef RUVIA_ENABLE_MARIADB
    if (const auto* client = std::get_if<db_pool<mariadb_driver>*>(&pool);
        client != nullptr && *client != nullptr) {
        return std::forward<visitor_type>(visitor)(**client);
    }
#endif
#ifdef RUVIA_ENABLE_POSTGRESQL
    if (const auto* client = std::get_if<db_pool<postgresql_driver>*>(&pool);
        client != nullptr && *client != nullptr) {
        return std::forward<visitor_type>(visitor)(**client);
    }
#endif
    throw_unavailable_db_backend();
}

// Destruction and immediate close paths cannot report an empty backend. They
// deliberately ignore it while preserving the same closed-set dispatch.
template <typename visitor_type>
void visit_db_pool_if_present([[maybe_unused]] const db_pool_ref_type& pool, [[maybe_unused]] visitor_type&& visitor) noexcept {
#ifdef RUVIA_ENABLE_MARIADB
    if (const auto* client = std::get_if<db_pool<mariadb_driver>*>(&pool);
        client != nullptr && *client != nullptr) {
        std::forward<visitor_type>(visitor)(**client);
        return;
    }
#endif
#ifdef RUVIA_ENABLE_POSTGRESQL
    if (const auto* client = std::get_if<db_pool<postgresql_driver>*>(&pool);
        client != nullptr && *client != nullptr) {
        std::forward<visitor_type>(visitor)(**client);
    }
#endif
}

[[nodiscard]] inline db_driver db_pool_driver(const db_pool_ref_type& pool) {
    return visit_db_pool(pool, [](const auto& client) noexcept { return client.config().driver_; });
}

}  // namespace ruvia::detail
