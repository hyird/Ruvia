#include "test_harness.h"

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

#include "http/ContextInternal.h"
#include "http/ContextServices.h"
#include "HttpRequestInternal.h"
#include "net/server/RateLimitDecision.h"
#include "net/server/RateLimitKey.h"
#include "ruvia/app/RateLimitRule.h"
#include "ruvia/http/Context.h"
#include "ruvia/http/RateLimit.h"
#include "ruvia/memory/MemoryPool.h"

namespace {

using ruvia::HttpRequest;
using ruvia::RateLimitRule;
using ruvia::RequestMemory;
using ruvia::WorkerMemory;
using ruvia::detail::applyRouteRateLimit;
using ruvia::detail::RouteRateLimitOptions;
using ruvia::detail::ContextAccess;
using ruvia::detail::ContextServices;
using ruvia::detail::HttpRequestAccess;
using ruvia::detail::RateLimiter;
using ruvia::detail::rateLimiterNowMs;
using ruvia::detail::rateLimitRequestAllowed;

struct RouteLimitResult final {
    bool allowed{false};
    bool hasResponse{false};
    std::uint16_t status{0};
    std::string retryAfter;
    std::string limit;
    std::string remaining;
    std::string reset;
};

// Runs the per-route limiter over one fresh request that shares the given limiter
// and scope (and the same empty remote address, so they collide on one counter).
RouteLimitResult runRouteLimit(RateLimiter& limiter, std::uintptr_t scope,
                               const RouteRateLimitOptions& options) {
    WorkerMemory worker;
    RequestMemory memory(worker);
    HttpRequest request = HttpRequestAccess::make();
    HttpRequestAccess::reset(request);
    HttpRequestAccess::setResource(request, memory.resource());
    ContextServices services(nullptr, nullptr, nullptr, &limiter);
    auto context = ContextAccess::make(memory, request, scope, services);

    RouteLimitResult r;
    r.allowed = applyRouteRateLimit(context, options, options.rule.maxRequests);
    r.hasResponse = ContextAccess::hasResponse(context);
    if (r.hasResponse) {
        auto response = ContextAccess::takeResponse(context);
        r.status = response.status();
        r.retryAfter = std::string(response.header("Retry-After"));
        r.limit = std::string(response.header("X-RateLimit-Limit"));
        r.remaining = std::string(response.header("X-RateLimit-Remaining"));
        r.reset = std::string(response.header("X-RateLimit-Reset"));
    }
    return r;
}

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

RUVIA_TEST(rate_limit_enforces_per_key_request_budget) {
    // The core allow/deny behavior: within a single window a key gets exactly
    // maxRequests admissions and is then denied, while a different key is counted
    // independently and is unaffected. A 60s window keeps every call in the test
    // inside one window, so the outcome is deterministic without clock control.
    RateLimitRule rule;
    rule.maxRequests = 3;
    rule.window = std::chrono::seconds(60);
    rule.slotCount = 16;
    RateLimiter limiter(rule);
    RUVIA_CHECK(limiter.enabled());

    RUVIA_CHECK(rateLimitRequestAllowed(&limiter, "10.0.0.1").allowed);
    RUVIA_CHECK(rateLimitRequestAllowed(&limiter, "10.0.0.1").allowed);
    RUVIA_CHECK(rateLimitRequestAllowed(&limiter, "10.0.0.1").allowed);
    const auto denied = rateLimitRequestAllowed(&limiter, "10.0.0.1");
    RUVIA_CHECK(!denied.allowed);
    // A denied request reports a positive time until the window resets.
    RUVIA_CHECK(denied.resetAfterMs > 0);

    // A different address has its own budget and is still admitted.
    RUVIA_CHECK(rateLimitRequestAllowed(&limiter, "10.0.0.2").allowed);
}

RUVIA_TEST(rate_limit_oversized_key_honors_fail_mode) {
    // A remote address longer than the fixed 64-byte key buffer cannot be tracked.
    // Under failClosed (the default) such a request is DENIED rather than silently
    // admitted, so an attacker cannot bypass the limiter with an overlong key.
    RateLimitRule closed;
    closed.maxRequests = 5;
    closed.slotCount = 8;
    closed.failClosed = true;
    RateLimiter closedLimiter(closed);
    const std::string longKey(65, 'a');  // > kMaxKeyBytes (64)
    RUVIA_CHECK(!rateLimitRequestAllowed(&closedLimiter, longKey).allowed);

    // Under failOpen the same request is admitted (availability over strictness).
    RateLimitRule open = closed;
    open.failClosed = false;
    RateLimiter openLimiter(open);
    RUVIA_CHECK(rateLimitRequestAllowed(&openLimiter, longKey).allowed);
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

RUVIA_TEST(route_rate_limit_429_carries_retry_after_and_ratelimit_headers) {
    // The per-route limiter's rejection path (applyRouteRateLimit) had no coverage:
    // the RateLimiter core is tested, but not the 429 response it produces with the
    // Retry-After and X-RateLimit-* advisory headers a client relies on.
    RateLimiter limiter(RateLimitRule{}, std::pmr::get_default_resource());
    RouteRateLimitOptions options;
    options.rule.maxRequests = 1;
    options.rule.window = std::chrono::seconds(60);
    const std::uintptr_t scope = 0xABCD;

    // The first request under this (scope, empty-IP) key is admitted with no response.
    const auto first = runRouteLimit(limiter, scope, options);
    RUVIA_CHECK(first.allowed);
    RUVIA_CHECK(!first.hasResponse);

    // The second exceeds maxRequests=1 -> short-circuited with a 429 and the full
    // advisory header set.
    const auto second = runRouteLimit(limiter, scope, options);
    RUVIA_CHECK(!second.allowed);
    RUVIA_CHECK(second.hasResponse);
    RUVIA_CHECK_EQ(second.status, std::uint16_t{429});
    RUVIA_CHECK_EQ(second.limit, std::string("1"));       // X-RateLimit-Limit = maxRequests
    RUVIA_CHECK_EQ(second.remaining, std::string("0"));   // X-RateLimit-Remaining = 0 when blocked
    // Retry-After is a positive whole number of seconds (ceil of the ms remaining in
    // the 60s window), and X-RateLimit-Reset mirrors it.
    RUVIA_CHECK(!second.retryAfter.empty());
    const int retry = std::stoi(second.retryAfter);
    RUVIA_CHECK(retry >= 1 && retry <= 60);
    RUVIA_CHECK_EQ(second.reset, second.retryAfter);
}

namespace {
std::string rateLimitKey(std::string_view remoteAddress) {
    char buffer[ruvia::detail::kRateLimitKeyBufferBytes];
    const auto key = ruvia::detail::rateLimitKeyFor(remoteAddress, buffer);
    return std::string(key);
}
}  // namespace

RUVIA_TEST(rate_limit_key_groups_ipv6_by_64_prefix) {
    // A client typically controls an entire IPv6 /64 (or larger). Keying on the full
    // address would let it rotate addresses to bypass the per-IP limit and exhaust
    // the shared slot table, so genuine IPv6 is grouped by its /64 network prefix.
    // Two addresses sharing a /64 must yield the same key...
    RUVIA_CHECK_EQ(rateLimitKey("2001:db8:1:2::1"), rateLimitKey("2001:db8:1:2::dead:beef"));
    RUVIA_CHECK_EQ(rateLimitKey("2001:db8:1:2:ffff:ffff:ffff:ffff"), rateLimitKey("2001:db8:1:2::1"));
    // ...and different /64s must yield different keys (no over-grouping).
    RUVIA_CHECK(rateLimitKey("2001:db8:1:2::1") != rateLimitKey("2001:db8:1:3::1"));
    RUVIA_CHECK(rateLimitKey("2001:db8:1:2::1") != rateLimitKey("2001:db8:2:2::1"));

    // IPv4 passes through unchanged (each host is already its own key).
    RUVIA_CHECK_EQ(rateLimitKey("203.0.113.7"), std::string("203.0.113.7"));
    RUVIA_CHECK(rateLimitKey("203.0.113.7") != rateLimitKey("203.0.113.8"));

    // IPv4-mapped IPv6 must NOT collapse to one /64 -- each mapped host stays distinct.
    RUVIA_CHECK(rateLimitKey("::ffff:203.0.113.7") != rateLimitKey("::ffff:203.0.113.8"));
}
