#include "test_harness.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "net/server/RateLimiter.h"
#include "ruvia/app/RateLimitRule.h"

namespace {

using ruvia::RateLimitRule;
using ruvia::detail::RateLimiter;

// A window long enough that no reset happens during a test.
RateLimitRule ruleWith(std::size_t maxRequests, bool failClosed = true) {
    RateLimitRule rule;
    rule.maxRequests = maxRequests;
    rule.window = std::chrono::seconds(60);
    rule.failClosed = failClosed;
    return rule;
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
    RateLimiter limiter(ruleWith(0));  // maxRequests == 0 disables
    RUVIA_CHECK(!limiter.enabled());
    for (int i = 0; i < 100; ++i) {
        RUVIA_CHECK(limiter.allowGlobal("10.0.0.1").allowed);
    }
}

RUVIA_TEST(rate_limiter_resets_after_window) {
    RateLimitRule rule;
    rule.maxRequests = 1;
    rule.window = std::chrono::milliseconds(20);
    RateLimiter limiter(rule);
    RUVIA_CHECK(limiter.allowGlobal("k").allowed);
    RUVIA_CHECK(!limiter.allowGlobal("k").allowed);
    std::this_thread::sleep_for(std::chrono::milliseconds(40));  // cross the window boundary
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

RUVIA_TEST(rate_limiter_oversized_key_follows_fail_closed) {
    const std::string oversized(100, 'a');  // exceeds the 64-byte key cap
    RateLimiter closed(ruleWith(1, /*failClosed=*/true));
    RUVIA_CHECK(!closed.allowGlobal(oversized).allowed);  // fail closed -> deny
    RateLimiter open(ruleWith(1, /*failClosed=*/false));
    RUVIA_CHECK(open.allowGlobal(oversized).allowed);  // fail open -> allow
}

// The core lock-free guarantee: under heavy contention on ONE key within a single
// window, the limiter admits EXACTLY maxRequests — never over-admits (a lost CAS
// race would let clients exceed the limit) and never under-admits.
RUVIA_TEST(rate_limiter_concurrent_admits_exactly_max) {
    constexpr int kMax = 100;
    constexpr int kThreads = 8;
    constexpr int kPerThread = 50;  // 400 attempts >> kMax
    RateLimiter limiter(ruleWith(kMax));

    std::atomic<int> admitted{0};
    std::vector<std::thread> pool;
    pool.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        pool.emplace_back([&limiter, &admitted] {
            for (int i = 0; i < kPerThread; ++i) {
                if (limiter.allowGlobal("shared.key").allowed) {
                    admitted.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    for (auto& thread : pool) {
        thread.join();
    }
    RUVIA_CHECK_EQ(admitted.load(), kMax);
}
