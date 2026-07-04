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
