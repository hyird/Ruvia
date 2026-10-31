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

class rate_limit_rejection final {
public:
    constexpr rate_limit_rejection(std::chrono::milliseconds retry_after, std::size_t max_requests) noexcept
        : retry_after_(retry_after),
          max_requests_(max_requests) {}

    [[nodiscard]] constexpr std::chrono::milliseconds retry_after() const noexcept {
        return retry_after_;
    }

    // The limit of the rule that refused the request.
    [[nodiscard]] constexpr std::size_t max_requests() const noexcept {
        return max_requests_;
    }

private:
    std::chrono::milliseconds retry_after_;
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

struct rate_limit_slot final {
    rate_limit_scope scope_{};
    // 0 marks an empty slot; occupied slots never hash to 0.
    std::uint64_t key_hash_{0};
    std::uint64_t reset_at_ms_{0};
    std::size_t count_{0};
    std::uint8_t key_size_{0};
    std::array<char, rate_limit_max_key_bytes> key_{};
};
static_assert(sizeof(rate_limit_slot) <= 112, "worker rate-limit slots must stay compact");

// One fixed-window table owned and accessed by exactly one web_worker_runtime
// worker. Startup allocates every slot from worker_memory; request-path lookup
// mutates ordinary worker-local state and performs no allocation, locking,
// atomics, or cross-thread coordination. Keys are (route scope, rule
// declaration, normalized remote address).
class rate_limiter final {
public:
    rate_limiter(std::optional<rate_limit_rule> default_rule_per_worker,
        route_rate_limit_presence route_rules, std::size_t capacity_value, std::pmr::memory_resource* resource);

    rate_limiter(const rate_limiter&) = delete;
    rate_limiter& operator=(const rate_limiter&) = delete;

    [[nodiscard]] bool has_default_rule() const noexcept {
        return default_rule_per_worker_.has_value();
    }

    [[nodiscard]] std::optional<rate_limit_rejection> allow_default(std::string_view remote_address) noexcept;

    // `rule_declaration` identifies the rule's declaration (its worker-local
    // middleware instance), distinct for every declaration on the route.
    [[nodiscard]] std::optional<rate_limit_rejection> allow_route(std::uintptr_t route_scope,
        const void* rule_declaration, std::string_view remote_address, const rate_limit_rule& rule) noexcept;

private:
    [[nodiscard]] std::optional<rate_limit_rejection> allow(
        rate_limit_scope scope, std::string_view key, const rate_limit_rule& rule) noexcept;
    std::optional<rate_limit_rule> default_rule_per_worker_;
    std::pmr::vector<rate_limit_slot> slots_;
};

}  // namespace ruvia::detail
