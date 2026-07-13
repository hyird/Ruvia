#include "test_harness.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

#include "ruvia/web/detail/server/RateLimiter.h"
#include "ruvia/web/RateLimitRule.h"

namespace {

using ruvia::RateLimitRule;
using ruvia::RateLimitOverflowPolicy;
using ruvia::detail::BasicRateLimiter;
using ruvia::detail::RateLimiter;

struct ManualRateLimiterClock final {
    [[nodiscard]] static std::int64_t nowMs() noexcept {
        return value;
    }

    static void set(std::int64_t nowMs) noexcept {
        value = nowMs;
    }

    inline static std::int64_t value{0};
};

using ManualRateLimiter = BasicRateLimiter<ManualRateLimiterClock>;

// A window long enough that no reset happens during a test.
RateLimitRule ruleWith(std::size_t maxRequests, bool failClosed = true) {
    return RateLimitRule::fixedWindow(
        maxRequests,
        std::chrono::seconds(60),
        failClosed
            ? RateLimitOverflowPolicy::kDeny
            : RateLimitOverflowPolicy::kAllow);
}

}  // namespace

RUVIA_TEST(rate_limiter_allows_up_to_max_then_denies) {
    RateLimiter limiter(ruleWith(3));
    RUVIA_CHECK(limiter.enabled());
    RUVIA_CHECK(limiter.allowGlobal("10.0.0.1").allowed);
    RUVIA_CHECK(limiter.allowGlobal("10.0.0.1").allowed);
    RUVIA_CHECK(limiter.allowGlobal("10.0.0.1").allowed);
    RUVIA_CHECK(!limiter.allowGlobal("10.0.0.1").allowed);  // 4th over the limit
    // resetAfterMs is a positive hint toward the window end.
    RUVIA_CHECK(limiter.allowGlobal("10.0.0.1").resetAfterMs > 0);
}

RUVIA_TEST(rate_limiter_keys_are_independent) {
    RateLimiter limiter(ruleWith(1));
    RUVIA_CHECK(limiter.allowGlobal("1.1.1.1").allowed);
    RUVIA_CHECK(limiter.allowGlobal("2.2.2.2").allowed);   // distinct key, own budget
    RUVIA_CHECK(!limiter.allowGlobal("1.1.1.1").allowed);  // first key now exhausted
    RUVIA_CHECK(!limiter.allowGlobal("2.2.2.2").allowed);
}

RUVIA_TEST(rate_limiter_disabled_allows_everything) {
    RateLimiter limiter(std::nullopt);
    RUVIA_CHECK(!limiter.enabled());
    for (int i = 0; i < 100; ++i) {
        RUVIA_CHECK(limiter.allowGlobal("10.0.0.1").allowed);
    }
}

RUVIA_TEST(rate_limiter_resets_after_window) {
    const auto rule = RateLimitRule::fixedWindow(
        1, std::chrono::milliseconds(20));
    ManualRateLimiterClock::set(1'000);
    ManualRateLimiter limiter(rule);
    RUVIA_CHECK(limiter.allowGlobal("k").allowed);
    RUVIA_CHECK(!limiter.allowGlobal("k").allowed);
    ManualRateLimiterClock::set(1'019);
    RUVIA_CHECK(!limiter.allowGlobal("k").allowed);
    ManualRateLimiterClock::set(1'020);
    RUVIA_CHECK(limiter.allowGlobal("k").allowed);  // new fixed window admits again
}

RUVIA_TEST(rate_limiter_route_scope_independent_of_global) {
    RateLimiter limiter(ruleWith(1));
    const RateLimitRule routeRule = ruleWith(1);
    const std::uintptr_t routeScope = 0xABCD;  // distinct from the internal global scope
    RUVIA_CHECK(limiter.allowGlobal("ip").allowed);
    RUVIA_CHECK(limiter.allowRoute(routeScope, "ip", routeRule).allowed);  // separate scope/budget
    RUVIA_CHECK(!limiter.allowGlobal("ip").allowed);
    RUVIA_CHECK(!limiter.allowRoute(routeScope, "ip", routeRule).allowed);
}

RUVIA_TEST(rate_limiter_route_enforced_when_app_rule_disabled) {
    // An absent app rule does NOT disable route rate limiting:
    // route rules share this worker's limiter table, so the slots must exist and be
    // enforced even though allowGlobal always allows. This is why construction cannot
    // skip slot allocation based on the app rule alone.
    RateLimiter limiter(std::nullopt);
    RUVIA_CHECK(!limiter.enabled());
    RUVIA_CHECK(limiter.allowGlobal("ip").allowed);   // global off -> always allowed
    RUVIA_CHECK(limiter.allowGlobal("ip").allowed);
    const RateLimitRule routeRule = ruleWith(1);
    RUVIA_CHECK(limiter.allowRoute(0x1234, "ip", routeRule).allowed);   // route budget: 1
    RUVIA_CHECK(!limiter.allowRoute(0x1234, "ip", routeRule).allowed);  // route still enforced
}

RUVIA_TEST(rate_limiter_oversized_key_follows_fail_closed) {
    const std::string oversized(100, 'a');  // exceeds the 64-byte key cap
    RateLimiter closed(ruleWith(1, /*failClosed=*/true));
    RUVIA_CHECK(!closed.allowGlobal(oversized).allowed);  // fail closed -> deny
    RateLimiter open(ruleWith(1, /*failClosed=*/false));
    RUVIA_CHECK(open.allowGlobal(oversized).allowed);  // fail open -> allow
}

RUVIA_TEST(rate_limiter_route_rule_owns_fail_policy) {
    const std::string oversized(100, 'a');  // exceeds the 64-byte key cap
    RateLimiter limiter(ruleWith(1, /*failClosed=*/true));
    const RateLimitRule routeOpen = ruleWith(1, /*failClosed=*/false);
    RUVIA_CHECK(limiter.allowRoute(0xCAFE, oversized, routeOpen).allowed);
}

RUVIA_TEST(rate_limiter_workers_own_independent_budgets) {
    RateLimiter firstWorker(ruleWith(1), 8);
    RateLimiter secondWorker(ruleWith(1), 8);

    RUVIA_CHECK(firstWorker.allowGlobal("shared.key").allowed);
    RUVIA_CHECK(!firstWorker.allowGlobal("shared.key").allowed);
    RUVIA_CHECK(secondWorker.allowGlobal("shared.key").allowed);
    RUVIA_CHECK(!secondWorker.allowGlobal("shared.key").allowed);
}

RUVIA_TEST(rate_limiter_full_worker_table_honors_fail_policy) {
    RateLimiter closed(ruleWith(1, /*failClosed=*/true), 1);
    RUVIA_CHECK(closed.allowGlobal("first").allowed);
    RUVIA_CHECK(!closed.allowGlobal("second").allowed);

    RateLimiter open(ruleWith(1, /*failClosed=*/false), 1);
    RUVIA_CHECK(open.allowGlobal("first").allowed);
    RUVIA_CHECK(open.allowGlobal("second").allowed);
}

RUVIA_TEST(rate_limiter_reclaims_expired_worker_slot) {
    const auto rule = RateLimitRule::fixedWindow(
        1, std::chrono::milliseconds(10));
    ManualRateLimiterClock::set(1'000);
    ManualRateLimiter limiter(rule, 1);

    RUVIA_CHECK(limiter.allowGlobal("first").allowed);
    RUVIA_CHECK(!limiter.allowGlobal("second").allowed);
    ManualRateLimiterClock::set(1'010);
    RUVIA_CHECK(limiter.allowGlobal("second").allowed);
    RUVIA_CHECK(!limiter.allowGlobal("second").allowed);
}
