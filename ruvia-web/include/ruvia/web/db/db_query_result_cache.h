#pragma once

#include <span>
#include <string_view>

#include "ruvia/core/operation_options.h"
#include "ruvia/core/scoped_operation.h"

namespace ruvia {
namespace detail {
class db_query_cache_state;
}
class db_handle;

class db_query_result_cache final {
public:
    db_query_result_cache(const db_query_result_cache& other) noexcept;
    db_query_result_cache& operator=(const db_query_result_cache&) = delete;
    [[nodiscard]] scoped_operation<void> remove(std::span<const std::string_view> ids) const;
    [[nodiscard]] scoped_operation<void> clear() const;

private:
    friend class db_handle;
    db_query_result_cache(detail::db_query_cache_state& state_value, ::ruvia::operation_scope& scope, operation_options options) noexcept;
    static void expire_capability(void* target) noexcept;
    detail::db_query_cache_state* state_;
    operation_options options_;
    scoped_capability_registration registration_;
};
}  // namespace ruvia
