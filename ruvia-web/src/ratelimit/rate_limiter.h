#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <string_view>
#include <type_traits>
#include <vector>

#include "ruvia/web/rate_limit.h"

namespace ruvia::detail {

// Delays are unsigned milliseconds: Reset can reach two windows, beyond any
// signed millisecond count once a window nears milliseconds::max().
class rate_limit_rejection final {
public:
    constexpr rate_limit_rejection(
        std::uint64_t retry_after_ms, std::uint64_t reset_after_ms, std::size_t max_requests) noexcept
        : retry_after_ms_(retry_after_ms),
          reset_after_ms_(reset_after_ms),
          max_requests_(max_requests) {}

    // For a key the table holds, the earliest delay after which the refusing
    // rule's estimate admits it again. Refusals leave the estimate unchanged,
    // so retrying earlier only meets another refusal.
    [[nodiscard]] constexpr std::uint64_t retry_after_ms() const noexcept {
        return retry_after_ms_;
    }

    // The delay after which the key's sliding-window estimate reaches zero and
    // its whole budget is available again; never shorter than retry_after_ms().
    [[nodiscard]] constexpr std::uint64_t reset_after_ms() const noexcept {
        return reset_after_ms_;
    }

    // The limit of the rule that refused the request.
    [[nodiscard]] constexpr std::size_t max_requests() const noexcept {
        return max_requests_;
    }

private:
    std::uint64_t retry_after_ms_;
    std::uint64_t reset_after_ms_;
    std::size_t max_requests_;
};

enum class route_rate_limit_presence : std::uint8_t {
    absent,
    present,
};

// The counter family a decision consumes: the route a rule guards and the rule
// declaration itself, so stacked rules on one route each keep their own count
// and window.
struct rate_limit_scope final {
    std::uintptr_t route_{0};
    std::uintptr_t rule_{0};

    friend constexpr bool operator==(const rate_limit_scope&, const rate_limit_scope&) noexcept = default;
};

// Normalized IPv6 /64 keys use only 19 bytes. The larger bound also
// accommodates canonical peer strings carrying an IPv6 scope identifier.
inline constexpr std::size_t rate_limit_max_key_bytes = 64;

// Slots one lookup or insertion probes from a key's home slot. A key whose
// probe window holds only live entries of other keys takes the overflow decision.
inline constexpr std::size_t rate_limit_probe_window = 32;

// The 128-bit key of the table's slot-placement PRF.
using rate_limit_hash_key = std::array<std::uint64_t, 2>;

// Each key estimates its request count over the trailing window the way
// pingora-limits' Rate does: the previous window's count weighted by the part
// of it still inside the trailing window, plus the current window's count.
// Windows start at the key's first request and roll on that key's own grid.
struct rate_limit_slot final {
    rate_limit_scope scope_{};
    // 0 marks an empty slot; occupied slots never hash to 0.
    std::uint64_t key_hash_{0};
    std::uint64_t window_start_ms_{0};
    // Two windows after window_start_ms_ (saturating): from then on neither
    // count reaches the estimate, so the entry is no longer live and another
    // key may take the slot.
    std::uint64_t stale_at_ms_{0};
    // Admissions only; never exceeds the rule's max_requests_.
    std::size_t current_count_{0};
    std::size_t previous_count_{0};
    std::uint8_t key_size_{0};
    std::array<char, rate_limit_max_key_bytes> key_{};
};
static_assert(sizeof(rate_limit_slot) <= 128, "worker rate-limit slots must stay compact");

// One sliding-window table owned and accessed by exactly one
// web_worker_runtime worker. Startup allocates every slot from worker_memory;
// request-path lookup mutates ordinary worker-local state and performs no
// allocation, locking, atomics, or cross-thread coordination. Keys are (route
// scope, rule declaration, normalized remote address). Home slots come from a
// keyed hash whose key each table draws at startup, so which keys share a
// probe window cannot be computed offline to lock a victim out or to escape
// counting.
class rate_limiter final {
public:
    rate_limiter(std::optional<rate_limit_rule> default_rule_per_worker,
        route_rate_limit_presence route_rules, std::size_t capacity_value, std::pmr::memory_resource* resource);

    rate_limiter(const rate_limiter&) = delete;
    rate_limiter& operator=(const rate_limiter&) = delete;

    [[nodiscard]] bool has_default_rule() const noexcept {
        return default_rule_per_worker_.has_value();
    }

    // Every decision takes the instant it is made for as a value, so the table
    // itself never reads a clock. Instants must not run backwards per key; an
    // earlier one counts as the start of the key's current window.
    [[nodiscard]] std::optional<rate_limit_rejection> allow_default(
        std::string_view remote_address, std::chrono::steady_clock::time_point now) noexcept;

    // `rule_declaration` identifies the rule's declaration (its worker-local
    // middleware instance), distinct for every declaration on the route.
    [[nodiscard]] std::optional<rate_limit_rejection> allow_route(std::uintptr_t route_scope,
        const void* rule_declaration, std::string_view remote_address, const rate_limit_rule& rule,
        std::chrono::steady_clock::time_point now) noexcept;

private:
    [[nodiscard]] std::optional<rate_limit_rejection> allow(rate_limit_scope scope, std::string_view key,
        const rate_limit_rule& rule, std::chrono::steady_clock::time_point now) noexcept;
    std::optional<rate_limit_rule> default_rule_per_worker_;
    std::pmr::vector<rate_limit_slot> slots_;
    rate_limit_hash_key hash_key_{};
};

}  // namespace ruvia::detail
