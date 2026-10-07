#pragma once

#include <span>
#include <string_view>

#include "ruvia/core/OperationOptions.h"
#include "ruvia/core/ScopedOperation.h"

namespace ruvia {
namespace detail {
class DbQueryCacheState;
}
class DbHandle;

class DbQueryResultCache final {
public:
    DbQueryResultCache(const DbQueryResultCache& other) noexcept;
    DbQueryResultCache& operator=(const DbQueryResultCache&) = delete;
    [[nodiscard]] ScopedOperation<void> remove(std::span<const std::string_view> ids) const;
    [[nodiscard]] ScopedOperation<void> clear() const;

private:
    friend class DbHandle;
    DbQueryResultCache(detail::DbQueryCacheState& state, ::ruvia::operation_scope& scope, OperationOptions options) noexcept;
    static void expire_capability(void* target) noexcept;
    detail::DbQueryCacheState* state_;
    OperationOptions options_;
    scoped_capability_registration registration_;
};
}  // namespace ruvia
