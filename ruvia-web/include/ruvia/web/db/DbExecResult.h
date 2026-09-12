#pragma once

#include <cstdint>
#include <optional>

namespace ruvia {

namespace detail {
struct DbResultAccess;
struct RedisOrmResultAccess;
}  // namespace detail

// Result of a statement whose contract is side effects rather than a row set.
// PostgreSQL does not expose a portable connection-level insert id, so that
// value is present only when the selected backend supplied one.
class DbExecResult final {
public:
    [[nodiscard]] constexpr std::uint64_t affectedRows() const noexcept {
        return affectedRows_;
    }

    [[nodiscard]] constexpr std::optional<std::uint64_t> lastInsertId() const noexcept {
        return lastInsertId_;
    }

private:
    friend struct detail::DbResultAccess;
    friend struct detail::RedisOrmResultAccess;

    constexpr DbExecResult(
        std::uint64_t affectedRows, std::optional<std::uint64_t> lastInsertId) noexcept
        : affectedRows_(affectedRows),
          lastInsertId_(lastInsertId) {}

    std::uint64_t affectedRows_{0};
    std::optional<std::uint64_t> lastInsertId_;
};

}  // namespace ruvia
