#include <chrono>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>

#include "ruvia/web/rate_limit_rule.h"

#include "ratelimit/rate_limiter.h"
#include "test_harness.h"

namespace {

using ruvia::rate_limit_overflow_policy;
using ruvia::rate_limit_rule;
using ruvia::detail::rate_limit_decision;
using ruvia::detail::rate_limiter;
using ruvia::detail::rate_limiter_type;
using ruvia::detail::route_rate_limit_presence;

constexpr auto no_route_rules = route_rate_limit_presence::absent;
constexpr auto has_route_rules = route_rate_limit_presence::present;
constexpr std::size_t capacity = ruvia::default_rate_limit_capacity_per_worker;

bool rate_limit_allowed(rate_limit_decision decision) {
    return decision.allowed() != nullptr;
}

struct manual_rate_limiter_clock final {
    [[nodiscard]] static std::int64_t now_ms() noexcept {
        return value;
    }

    static void set(std::int64_t now_ms) noexcept {
        value = now_ms;
    }

    inline static std::int64_t value{0};
};

using manual_rate_limiter_type = rate_limiter<manual_rate_limiter_clock>;

// A window long enough that no reset happens during a test.
rate_limit_rule rule_with(std::size_t max_requests, bool fail_closed = true) {
    return rate_limit_rule{
        .max_requests_ = max_requests,
        .window_ = std::chrono::seconds(60),
        .overflow_policy_ =
            fail_closed ? rate_limit_overflow_policy::deny : rate_limit_overflow_policy::allow,
    };
}

}  // namespace

RUVIA_TEST(rate_limiter_allows_up_to_max_then_denies) {
    rate_limiter_type limiter(rule_with(3), no_route_rules, capacity);
    RUVIA_CHECK(limiter.has_default_rule());
    RUVIA_CHECK(rate_limit_allowed(limiter.allow_default("10.0.0.1")));
    RUVIA_CHECK(rate_limit_allowed(limiter.allow_default("10.0.0.1")));
    RUVIA_CHECK(rate_limit_allowed(limiter.allow_default("10.0.0.1")));
    const auto denied = limiter.allow_default("10.0.0.1");
    RUVIA_CHECK(denied.rejection() != nullptr);  // 4th over the limit
    RUVIA_CHECK(denied.rejection()->retry_after().count() > 0);
}

RUVIA_TEST(rate_limiter_keys_are_independent) {
    rate_limiter_type limiter(rule_with(1), no_route_rules, capacity);
    RUVIA_CHECK(rate_limit_allowed(limiter.allow_default("1.1.1.1")));
    RUVIA_CHECK(rate_limit_allowed(limiter.allow_default("2.2.2.2")));   // distinct key, own budget
    RUVIA_CHECK(!rate_limit_allowed(limiter.allow_default("1.1.1.1")));  // first key now exhausted
    RUVIA_CHECK(!rate_limit_allowed(limiter.allow_default("2.2.2.2")));
}

RUVIA_TEST(rate_limiter_disabled_allows_everything) {
    rate_limiter_type limiter(std::nullopt, no_route_rules, capacity);
    RUVIA_CHECK(!limiter.has_default_rule());
    for (int i = 0; i < 100; ++i) {
        RUVIA_CHECK(rate_limit_allowed(limiter.allow_default("10.0.0.1")));
    }
}

RUVIA_TEST(rate_limiter_skips_table_when_startup_metadata_has_no_rules) {
    rate_limiter_type limiter(std::nullopt, no_route_rules, ruvia::default_rate_limit_capacity_per_worker);
    RUVIA_CHECK(!limiter.has_default_rule());
    RUVIA_CHECK_EQ(limiter.key_capacity(), std::size_t{0});
}

RUVIA_TEST(rate_limiter_allocates_table_for_route_metadata_without_default_rule) {
    rate_limiter_type limiter(std::nullopt, has_route_rules, 8);
    RUVIA_CHECK(!limiter.has_default_rule());
    RUVIA_CHECK_EQ(limiter.key_capacity(), std::size_t{8});
    RUVIA_CHECK(rate_limit_allowed(limiter.allow_route(0x1234, "ip", rule_with(1))));
    RUVIA_CHECK(!rate_limit_allowed(limiter.allow_route(0x1234, "ip", rule_with(1))));
}

RUVIA_TEST(rate_limiter_resets_after_window) {
    const auto rule = rate_limit_rule{
        .max_requests_ = 1,
        .window_ = std::chrono::milliseconds(20),
    };
    // The window starts with this key's first request instead of aligning to a
    // process-wide clock boundary.
    manual_rate_limiter_clock::set(1'007);
    manual_rate_limiter_type limiter(rule, no_route_rules, capacity);
    RUVIA_CHECK(rate_limit_allowed(limiter.allow_default("k")));
    RUVIA_CHECK(!rate_limit_allowed(limiter.allow_default("k")));
    manual_rate_limiter_clock::set(1'026);
    RUVIA_CHECK(!rate_limit_allowed(limiter.allow_default("k")));
    manual_rate_limiter_clock::set(1'027);
    RUVIA_CHECK(rate_limit_allowed(limiter.allow_default("k")));  // new fixed window admits again
}

RUVIA_TEST(rate_limiter_route_scope_independent_of_default_rule) {
    rate_limiter_type limiter(rule_with(1), has_route_rules, capacity);
    const rate_limit_rule route_rule = rule_with(1);
    const std::uintptr_t route_scope = 0xABCD;  // distinct from the default-rule scope
    RUVIA_CHECK(rate_limit_allowed(limiter.allow_default("ip")));
    RUVIA_CHECK(rate_limit_allowed(
        limiter.allow_route(route_scope, "ip", route_rule)));  // separate scope/budget
    RUVIA_CHECK(!rate_limit_allowed(limiter.allow_default("ip")));
    RUVIA_CHECK(!rate_limit_allowed(limiter.allow_route(route_scope, "ip", route_rule)));
}

RUVIA_TEST(rate_limiter_rejects_non_power_of_two_capacity) {
    bool rejected = false;
    try {
        rate_limiter_type limiter(rule_with(1), no_route_rules, 3);
        (void)limiter;
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    RUVIA_CHECK(rejected);
}

RUVIA_TEST(rate_limiter_route_enforced_when_default_rule_disabled) {
    // An absent default rule does NOT disable route rate limiting:
    // route rules share this worker's limiter table, so the slots must exist and be
    // enforced even though allow_default always allows. Startup metadata therefore
    // explicitly records route-rule presence instead of inferring it from the default.
    rate_limiter_type limiter(std::nullopt, has_route_rules, capacity);
    RUVIA_CHECK(!limiter.has_default_rule());
    RUVIA_CHECK(rate_limit_allowed(limiter.allow_default("ip")));  // default off -> always allowed
    RUVIA_CHECK(rate_limit_allowed(limiter.allow_default("ip")));
    const rate_limit_rule route_rule = rule_with(1);
    RUVIA_CHECK(rate_limit_allowed(limiter.allow_route(0x1234, "ip", route_rule)));  // route budget: 1
    RUVIA_CHECK(
        !rate_limit_allowed(limiter.allow_route(0x1234, "ip", route_rule)));  // route still enforced
}

RUVIA_TEST(rate_limiter_oversized_key_follows_fail_closed) {
    const std::string oversized(100, 'a');  // exceeds the 64-byte key cap
    rate_limiter_type closed(rule_with(1, /*fail_closed=*/true), no_route_rules, capacity);
    RUVIA_CHECK(!rate_limit_allowed(closed.allow_default(oversized)));  // fail closed -> deny
    rate_limiter_type open(rule_with(1, /*fail_closed=*/false), no_route_rules, capacity);
    RUVIA_CHECK(rate_limit_allowed(open.allow_default(oversized)));  // fail open -> allow
}

RUVIA_TEST(rate_limiter_route_rule_owns_fail_policy) {
    const std::string oversized(100, 'a');  // exceeds the 64-byte key cap
    rate_limiter_type limiter(rule_with(1, /*fail_closed=*/true), has_route_rules, capacity);
    const rate_limit_rule route_open = rule_with(1, /*fail_closed=*/false);
    RUVIA_CHECK(rate_limit_allowed(limiter.allow_route(0xCAFE, oversized, route_open)));
}

RUVIA_TEST(rate_limiter_workers_own_independent_budgets) {
    rate_limiter_type first_worker(rule_with(1), no_route_rules, 8);
    rate_limiter_type second_worker(rule_with(1), no_route_rules, 8);

    RUVIA_CHECK(rate_limit_allowed(first_worker.allow_default("shared.key")));
    RUVIA_CHECK(!rate_limit_allowed(first_worker.allow_default("shared.key")));
    RUVIA_CHECK(rate_limit_allowed(second_worker.allow_default("shared.key")));
    RUVIA_CHECK(!rate_limit_allowed(second_worker.allow_default("shared.key")));
}

RUVIA_TEST(rate_limiter_full_worker_table_honors_fail_policy) {
    rate_limiter_type closed(rule_with(1, /*fail_closed=*/true), no_route_rules, 1);
    RUVIA_CHECK(rate_limit_allowed(closed.allow_default("first")));
    RUVIA_CHECK(!rate_limit_allowed(closed.allow_default("second")));

    rate_limiter_type open(rule_with(1, /*fail_closed=*/false), no_route_rules, 1);
    RUVIA_CHECK(rate_limit_allowed(open.allow_default("first")));
    RUVIA_CHECK(rate_limit_allowed(open.allow_default("second")));
}

RUVIA_TEST(rate_limiter_reclaims_expired_worker_slot) {
    const auto rule = rate_limit_rule{
        .max_requests_ = 1,
        .window_ = std::chrono::milliseconds(10),
    };
    manual_rate_limiter_clock::set(1'000);
    manual_rate_limiter_type limiter(rule, no_route_rules, 1);

    RUVIA_CHECK(rate_limit_allowed(limiter.allow_default("first")));
    RUVIA_CHECK(!rate_limit_allowed(limiter.allow_default("second")));
    manual_rate_limiter_clock::set(1'010);
    RUVIA_CHECK(rate_limit_allowed(limiter.allow_default("second")));
    RUVIA_CHECK(!rate_limit_allowed(limiter.allow_default("second")));
}
