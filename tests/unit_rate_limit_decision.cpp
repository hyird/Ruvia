#include "test_harness.h"

#include <string_view>

#include "net/server/RateLimitDecision.h"
#include "ruvia/app/RateLimitRule.h"

namespace {

using ruvia::RateLimitRule;
using ruvia::detail::RateLimiter;
using ruvia::detail::rateLimiterNowMs;
using ruvia::detail::rateLimitRequestAllowed;

}  // namespace

RUVIA_TEST(rate_limit_allowed_when_no_limiter) {
    // A null limiter means rate limiting is off: always allowed, no dereference.
    const auto check = rateLimitRequestAllowed(nullptr, "1.2.3.4");
    RUVIA_CHECK(check.allowed);
}

RUVIA_TEST(rate_limit_allowed_when_limiter_disabled) {
    RateLimitRule rule;    // maxRequests == 0 -> disabled
    rule.slotCount = 1;    // keep the slot table tiny for the test
    RateLimiter limiter(rule);
    RUVIA_CHECK(!limiter.enabled());
    RUVIA_CHECK(rateLimitRequestAllowed(&limiter, "1.2.3.4").allowed);
}

RUVIA_TEST(rate_limiter_now_ms_is_positive_and_monotonic) {
    const auto first = rateLimiterNowMs();
    const auto second = rateLimiterNowMs();
    RUVIA_CHECK(first > 0);
    RUVIA_CHECK(second >= first);
}

RUVIA_TEST(rate_limit_rule_normalization_clamps_unsafe_fields) {
    using ruvia::detail::kMaxRateLimitRequests;
    using ruvia::detail::normalizeRateLimitRule;

    // A zero or negative window would break the fixed-window math -> clamp to 1ms.
    {
        RateLimitRule rule;
        rule.window = std::chrono::milliseconds::zero();
        RUVIA_CHECK(normalizeRateLimitRule(rule).window == std::chrono::milliseconds(1));
    }
    {
        RateLimitRule rule;
        rule.window = std::chrono::milliseconds(-5);
        RUVIA_CHECK(normalizeRateLimitRule(rule).window == std::chrono::milliseconds(1));
    }
    // An empty slot table is unusable -> clamp to at least one slot.
    {
        RateLimitRule rule;
        rule.slotCount = 0;
        RUVIA_CHECK_EQ(normalizeRateLimitRule(rule).slotCount, std::size_t{1});
    }
    // maxRequests must fit the counter width -> clamp to the maximum.
    {
        RateLimitRule rule;
        rule.maxRequests = kMaxRateLimitRequests + 1000;
        RUVIA_CHECK_EQ(normalizeRateLimitRule(rule).maxRequests, kMaxRateLimitRequests);
    }
    // A valid rule passes through unchanged.
    {
        RateLimitRule rule;
        rule.maxRequests = 100;
        rule.window = std::chrono::seconds(60);
        rule.slotCount = 1024;
        const auto normalized = normalizeRateLimitRule(rule);
        RUVIA_CHECK_EQ(normalized.maxRequests, std::size_t{100});
        RUVIA_CHECK(normalized.window == std::chrono::milliseconds(60000));
        RUVIA_CHECK_EQ(normalized.slotCount, std::size_t{1024});
    }
}
