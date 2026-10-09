#pragma once

#include <cstdint>
#include <optional>

namespace ruvia {

namespace detail {
struct db_result_access;
}  // namespace detail

// Result of a statement whose contract is side effects rather than a row set.
// PostgreSQL does not expose a portable connection-level insert id, so that
// value is present only when the selected backend supplied one.
class db_exec_result final {
public:
    [[nodiscard]] constexpr std::uint64_t affected_rows() const noexcept {
        return affected_rows_;
    }

    [[nodiscard]] constexpr std::optional<std::uint64_t> last_insert_id() const noexcept {
        return last_insert_id_;
    }

private:
    friend struct detail::db_result_access;

    constexpr db_exec_result(
        std::uint64_t affected_rows, std::optional<std::uint64_t> last_insert_id) noexcept
        : affected_rows_(affected_rows),
          last_insert_id_(last_insert_id) {}

    std::uint64_t affected_rows_{0};
    std::optional<std::uint64_t> last_insert_id_;
};

}  // namespace ruvia
