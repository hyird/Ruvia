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

class DbQueryResultCache final : private detail::ScopedCapabilityNode {
public:
    DbQueryResultCache(const DbQueryResultCache&) noexcept = default;
    DbQueryResultCache& operator=(const DbQueryResultCache&) = delete;
    [[nodiscard]] ScopedOperation<void> remove(std::span<const std::string_view> ids) const;
    [[nodiscard]] ScopedOperation<void> clear() const;

private:
    friend class DbHandle;
    DbQueryResultCache(detail::DbQueryCacheState& state, detail::ScopedOperationScope& scope, OperationOptions options) noexcept;
    static void expireCapability(detail::ScopedCapabilityNode& node) noexcept;
    detail::DbQueryCacheState* state_;
    OperationOptions options_;
};
}  // namespace ruvia
