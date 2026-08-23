#include "test_harness.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>

#include "ruvia/web/detail/ratelimit/RateLimiter.h"
#include "ruvia/web/RateLimitRule.h"

namespace {

using ruvia::RateLimitOverflowPolicy;
using ruvia::RateLimitRule;
using ruvia::detail::BasicRateLimiter;
using ruvia::detail::RateLimitDecision;
using ruvia::detail::RateLimiter;
using ruvia::detail::RouteRateLimitPresence;

constexpr auto kNoRouteRules = RouteRateLimitPresence::kAbsent;
constexpr auto kHasRouteRules = RouteRateLimitPresence::kPresent;
constexpr std::size_t kCapacity = ruvia::kDefaultRateLimitCapacityPerWorker;

bool rateLimitAllowed(RateLimitDecision decision) {
    return decision.allowed() != nullptr;
}

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
    return RateLimitRule{
        .maxRequests = maxRequests,
        .window = std::chrono::seconds(60),
        .overflowPolicy = failClosed ? RateLimitOverflowPolicy::kDeny : RateLimitOverflowPolicy::kAllow,
    };
}

}  // namespace

RUVIA_TEST(rate_limiter_allows_up_to_max_then_denies) {
    RateLimiter limiter(ruleWith(3), kNoRouteRules, kCapacity);
    RUVIA_CHECK(limiter.hasDefaultRule());
    RUVIA_CHECK(rateLimitAllowed(limiter.allowDefault("10.0.0.1")));
    RUVIA_CHECK(rateLimitAllowed(limiter.allowDefault("10.0.0.1")));
    RUVIA_CHECK(rateLimitAllowed(limiter.allowDefault("10.0.0.1")));
    const auto denied = limiter.allowDefault("10.0.0.1");
    RUVIA_CHECK(denied.rejection() != nullptr);  // 4th over the limit
    RUVIA_CHECK(denied.rejection()->retryAfter().count() > 0);
}

RUVIA_TEST(rate_limiter_keys_are_independent) {
    RateLimiter limiter(ruleWith(1), kNoRouteRules, kCapacity);
    RUVIA_CHECK(rateLimitAllowed(limiter.allowDefault("1.1.1.1")));
    RUVIA_CHECK(rateLimitAllowed(limiter.allowDefault("2.2.2.2")));   // distinct key, own budget
    RUVIA_CHECK(!rateLimitAllowed(limiter.allowDefault("1.1.1.1")));  // first key now exhausted
    RUVIA_CHECK(!rateLimitAllowed(limiter.allowDefault("2.2.2.2")));
}

RUVIA_TEST(rate_limiter_disabled_allows_everything) {
    RateLimiter limiter(std::nullopt, kNoRouteRules, kCapacity);
    RUVIA_CHECK(!limiter.hasDefaultRule());
    for (int i = 0; i < 100; ++i) {
        RUVIA_CHECK(rateLimitAllowed(limiter.allowDefault("10.0.0.1")));
    }
}

RUVIA_TEST(rate_limiter_skips_table_when_startup_metadata_has_no_rules) {
    RateLimiter limiter(std::nullopt, kNoRouteRules, ruvia::kDefaultRateLimitCapacityPerWorker);
    RUVIA_CHECK(!limiter.hasDefaultRule());
    RUVIA_CHECK_EQ(limiter.keyCapacity(), std::size_t{0});
}

RUVIA_TEST(rate_limiter_allocates_table_for_route_metadata_without_default_rule) {
    RateLimiter limiter(std::nullopt, kHasRouteRules, 8);
    RUVIA_CHECK(!limiter.hasDefaultRule());
    RUVIA_CHECK_EQ(limiter.keyCapacity(), std::size_t{8});
    RUVIA_CHECK(rateLimitAllowed(limiter.allowRoute(0x1234, "ip", ruleWith(1))));
    RUVIA_CHECK(!rateLimitAllowed(limiter.allowRoute(0x1234, "ip", ruleWith(1))));
}

RUVIA_TEST(rate_limiter_resets_after_window) {
    const auto rule = RateLimitRule{
        .maxRequests = 1,
        .window = std::chrono::milliseconds(20),
    };
    ManualRateLimiterClock::set(1'000);
    ManualRateLimiter limiter(rule, kNoRouteRules, kCapacity);
    RUVIA_CHECK(rateLimitAllowed(limiter.allowDefault("k")));
    RUVIA_CHECK(!rateLimitAllowed(limiter.allowDefault("k")));
    ManualRateLimiterClock::set(1'019);
    RUVIA_CHECK(!rateLimitAllowed(limiter.allowDefault("k")));
    ManualRateLimiterClock::set(1'020);
    RUVIA_CHECK(rateLimitAllowed(limiter.allowDefault("k")));  // new fixed window admits again
}

RUVIA_TEST(rate_limiter_route_scope_independent_of_default_rule) {
    RateLimiter limiter(ruleWith(1), kHasRouteRules, kCapacity);
    const RateLimitRule routeRule = ruleWith(1);
    const std::uintptr_t routeScope = 0xABCD;  // distinct from the default-rule scope
    RUVIA_CHECK(rateLimitAllowed(limiter.allowDefault("ip")));
    RUVIA_CHECK(rateLimitAllowed(limiter.allowRoute(routeScope, "ip", routeRule)));  // separate scope/budget
    RUVIA_CHECK(!rateLimitAllowed(limiter.allowDefault("ip")));
    RUVIA_CHECK(!rateLimitAllowed(limiter.allowRoute(routeScope, "ip", routeRule)));
}

RUVIA_TEST(rate_limiter_rejects_non_power_of_two_capacity) {
    bool rejected = false;
    try {
        RateLimiter limiter(ruleWith(1), kNoRouteRules, 3);
        (void)limiter;
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    RUVIA_CHECK(rejected);
}

RUVIA_TEST(rate_limiter_route_enforced_when_default_rule_disabled) {
    // An absent default rule does NOT disable route rate limiting:
    // route rules share this worker's limiter table, so the slots must exist and be
    // enforced even though allowDefault always allows. Startup metadata therefore
    // explicitly records route-rule presence instead of inferring it from the default.
    RateLimiter limiter(std::nullopt, kHasRouteRules, kCapacity);
    RUVIA_CHECK(!limiter.hasDefaultRule());
    RUVIA_CHECK(rateLimitAllowed(limiter.allowDefault("ip")));  // default off -> always allowed
    RUVIA_CHECK(rateLimitAllowed(limiter.allowDefault("ip")));
    const RateLimitRule routeRule = ruleWith(1);
    RUVIA_CHECK(rateLimitAllowed(limiter.allowRoute(0x1234, "ip", routeRule)));   // route budget: 1
    RUVIA_CHECK(!rateLimitAllowed(limiter.allowRoute(0x1234, "ip", routeRule)));  // route still enforced
}

RUVIA_TEST(rate_limiter_oversized_key_follows_fail_closed) {
    const std::string oversized(100, 'a');  // exceeds the 64-byte key cap
    RateLimiter closed(ruleWith(1, /*failClosed=*/true), kNoRouteRules, kCapacity);
    RUVIA_CHECK(!rateLimitAllowed(closed.allowDefault(oversized)));  // fail closed -> deny
    RateLimiter open(ruleWith(1, /*failClosed=*/false), kNoRouteRules, kCapacity);
    RUVIA_CHECK(rateLimitAllowed(open.allowDefault(oversized)));  // fail open -> allow
}

RUVIA_TEST(rate_limiter_route_rule_owns_fail_policy) {
    const std::string oversized(100, 'a');  // exceeds the 64-byte key cap
    RateLimiter limiter(ruleWith(1, /*failClosed=*/true), kHasRouteRules, kCapacity);
    const RateLimitRule routeOpen = ruleWith(1, /*failClosed=*/false);
    RUVIA_CHECK(rateLimitAllowed(limiter.allowRoute(0xCAFE, oversized, routeOpen)));
}

RUVIA_TEST(rate_limiter_workers_own_independent_budgets) {
    RateLimiter firstWorker(ruleWith(1), kNoRouteRules, 8);
    RateLimiter secondWorker(ruleWith(1), kNoRouteRules, 8);

    RUVIA_CHECK(rateLimitAllowed(firstWorker.allowDefault("shared.key")));
    RUVIA_CHECK(!rateLimitAllowed(firstWorker.allowDefault("shared.key")));
    RUVIA_CHECK(rateLimitAllowed(secondWorker.allowDefault("shared.key")));
    RUVIA_CHECK(!rateLimitAllowed(secondWorker.allowDefault("shared.key")));
}

RUVIA_TEST(rate_limiter_full_worker_table_honors_fail_policy) {
    RateLimiter closed(ruleWith(1, /*failClosed=*/true), kNoRouteRules, 1);
    RUVIA_CHECK(rateLimitAllowed(closed.allowDefault("first")));
    RUVIA_CHECK(!rateLimitAllowed(closed.allowDefault("second")));

    RateLimiter open(ruleWith(1, /*failClosed=*/false), kNoRouteRules, 1);
    RUVIA_CHECK(rateLimitAllowed(open.allowDefault("first")));
    RUVIA_CHECK(rateLimitAllowed(open.allowDefault("second")));
}

RUVIA_TEST(rate_limiter_reclaims_expired_worker_slot) {
    const auto rule = RateLimitRule{
        .maxRequests = 1,
        .window = std::chrono::milliseconds(10),
    };
    ManualRateLimiterClock::set(1'000);
    ManualRateLimiter limiter(rule, kNoRouteRules, 1);

    RUVIA_CHECK(rateLimitAllowed(limiter.allowDefault("first")));
    RUVIA_CHECK(!rateLimitAllowed(limiter.allowDefault("second")));
    ManualRateLimiterClock::set(1'010);
    RUVIA_CHECK(rateLimitAllowed(limiter.allowDefault("second")));
    RUVIA_CHECK(!rateLimitAllowed(limiter.allowDefault("second")));
}
