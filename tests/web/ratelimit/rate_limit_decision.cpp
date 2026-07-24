#include "test_harness.h"

#include <chrono>
#include <concepts>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/web/detail/http/context/ContextAccess.h"
#include "ruvia/web/detail/http/context/ContextServices.h"
#include "ruvia/http/detail/request/HttpRequestAccess.h"
#include "ruvia/web/detail/ratelimit/RateLimitDecision.h"
#include "ruvia/web/detail/server/http1/Http1ClosingRejection.h"
#include "ruvia/web/detail/ratelimit/RateLimitKey.h"
#include "ruvia/web/RateLimitRule.h"
#include "ruvia/web/Context.h"
#include "ruvia/web/RateLimit.h"
#include "ruvia/core/memory/MemoryPool.h"

namespace {

using ruvia::HttpRequest;
using ruvia::RateLimitOverflowPolicy;
using ruvia::RateLimitRule;
using ruvia::RequestMemory;
using ruvia::WorkerMemory;
using ruvia::detail::applyRouteRateLimit;
using ruvia::detail::ContextAccess;
using ruvia::detail::ContextServices;
using ruvia::detail::decideRequestRateLimit;
using ruvia::detail::HttpRequestAccess;
using ruvia::detail::RateLimitDecision;
using ruvia::detail::RateLimiter;
using ruvia::detail::rateLimiterNowMs;
using ruvia::detail::RouteRateLimitOptions;
using ruvia::detail::RouteRateLimitPresence;

bool rateLimitAllowed(RateLimitDecision decision) {
    return decision.allowed() != nullptr;
}

template <typename Decision>
concept HasTemporaryRateLimitAlternative = requires(Decision decision) {
    std::move(decision).allowed();
    std::move(decision).rejection();
};

static_assert(!std::default_initializable<RateLimitDecision>);
static_assert(!std::default_initializable<ruvia::detail::RateLimitAllowed>);
static_assert(!std::default_initializable<ruvia::detail::RateLimitRejection>);
static_assert(!HasTemporaryRateLimitAlternative<RateLimitDecision>);

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
RouteLimitResult runRouteLimit(RateLimiter& limiter, std::uintptr_t scope, const RouteRateLimitOptions& options) {
    WorkerMemory worker;
    RequestMemory memory(worker);
    HttpRequest request = HttpRequestAccess::make();
    HttpRequestAccess::reset(request);
    HttpRequestAccess::setResource(request, memory.resource());
    ContextServices services(nullptr, nullptr, &limiter);
    auto context = ContextAccess::make(memory, request, scope, services);

    RouteLimitResult r;
    r.allowed = applyRouteRateLimit(context, options);
    r.hasResponse = ContextAccess::hasResponse(context);
    if (r.hasResponse) {
        auto response = ContextAccess::takeResponse(context);
        r.status = response.status().value();
        r.retryAfter = std::string(response.header("Retry-After").value_or(std::string_view{}));
        r.limit = std::string(response.header("X-RateLimit-Limit").value_or(std::string_view{}));
        r.remaining = std::string(response.header("X-RateLimit-Remaining").value_or(std::string_view{}));
        r.reset = std::string(response.header("X-RateLimit-Reset").value_or(std::string_view{}));
    }
    return r;
}

}  // namespace

RUVIA_TEST(rate_limit_allowed_when_no_limiter) {
    // A null limiter means rate limiting is off: always allowed, no dereference.
    const auto decision = decideRequestRateLimit(nullptr, "1.2.3.4");
    RUVIA_CHECK(decision.allowed() != nullptr);
    RUVIA_CHECK(decision.rejection() == nullptr);
}

RUVIA_TEST(rate_limit_allowed_when_limiter_disabled) {
    RateLimiter limiter(std::nullopt, RouteRateLimitPresence::kAbsent, 1);
    RUVIA_CHECK(!limiter.hasDefaultRule());
    const auto decision = decideRequestRateLimit(&limiter, "1.2.3.4");
    RUVIA_CHECK(decision.allowed() != nullptr);
}

RUVIA_TEST(rate_limit_rejection_owns_web_error_and_retry_headers) {
    const auto decision = RateLimitDecision::reject(std::chrono::milliseconds(1'001));
    const auto* rejection = decision.rejection();
    RUVIA_CHECK(rejection != nullptr);

    const auto error = ruvia::detail::rateLimitRejectionError();
    RUVIA_CHECK_EQ(error.status(), ruvia::http_status::kTooManyRequests);
    RUVIA_CHECK_EQ(error.code(), std::string_view("too_many_requests"));
    RUVIA_CHECK_EQ(error.message(), std::string_view("rate limit exceeded"));

    ruvia::HttpResponse response;
    ruvia::detail::applyRateLimitRejectionHeaders(response, *rejection);
    RUVIA_CHECK_EQ(response.header("Retry-After"), std::string_view("2"));
    RUVIA_CHECK(!response.header("X-RateLimit-Limit").has_value());

    ruvia::detail::applyRouteRateLimitRejectionHeaders(response, *rejection, 7);
    RUVIA_CHECK_EQ(response.header("Retry-After"), std::string_view("2"));
    RUVIA_CHECK_EQ(response.header("X-RateLimit-Limit"), std::string_view("7"));
    RUVIA_CHECK_EQ(response.header("X-RateLimit-Remaining"), std::string_view("0"));
    RUVIA_CHECK_EQ(response.header("X-RateLimit-Reset"), std::string_view("2"));
}

RUVIA_TEST(http1_closing_rejection_has_exclusive_error_alternatives) {
    using ruvia::detail::Http1ClosingRejection;

    const Http1ClosingRejection none;
    RUVIA_CHECK(none.error() == nullptr);
    RUVIA_CHECK(none.rateLimit() == nullptr);

    const auto ordinary = Http1ClosingRejection::error(ruvia::HttpErrorInfo(ruvia::http_status::kBadRequest, {}, "bad request"));
    RUVIA_CHECK(ordinary.error() != nullptr);
    RUVIA_CHECK_EQ(ordinary.error()->status(), ruvia::http_status::kBadRequest);
    RUVIA_CHECK(ordinary.rateLimit() == nullptr);

    const auto decision = RateLimitDecision::reject(std::chrono::milliseconds(125));
    const auto limited = Http1ClosingRejection::rateLimit(ruvia::detail::rateLimitRejectionError(), *decision.rejection());
    RUVIA_CHECK(limited.error() != nullptr);
    RUVIA_CHECK_EQ(limited.error()->status(), ruvia::http_status::kTooManyRequests);
    RUVIA_CHECK(limited.rateLimit() != nullptr);
    RUVIA_CHECK_EQ(limited.rateLimit()->retryAfter(), std::chrono::milliseconds(125));
}

RUVIA_TEST(rate_limit_enforces_per_key_request_budget) {
    // The core allow/deny behavior: within a single window a key gets exactly
    // maxRequests admissions and is then denied, while a different key is counted
    // independently and is unaffected. A 60s window keeps every call in the test
    // inside one window, so the outcome is deterministic without clock control.
    const auto rule = RateLimitRule::fixedWindow(3, std::chrono::seconds(60));
    RateLimiter limiter(rule, RouteRateLimitPresence::kAbsent, 16);
    RUVIA_CHECK(limiter.hasDefaultRule());

    RUVIA_CHECK(rateLimitAllowed(decideRequestRateLimit(&limiter, "10.0.0.1")));
    RUVIA_CHECK(rateLimitAllowed(decideRequestRateLimit(&limiter, "10.0.0.1")));
    RUVIA_CHECK(rateLimitAllowed(decideRequestRateLimit(&limiter, "10.0.0.1")));
    const auto denied = decideRequestRateLimit(&limiter, "10.0.0.1");
    RUVIA_CHECK(denied.allowed() == nullptr);
    // A denied request reports a positive time until the window resets.
    RUVIA_CHECK(denied.rejection() != nullptr);
    RUVIA_CHECK(denied.rejection()->retryAfter().count() > 0);

    // A different address has its own budget and is still admitted.
    RUVIA_CHECK(rateLimitAllowed(decideRequestRateLimit(&limiter, "10.0.0.2")));
}

RUVIA_TEST(rate_limit_oversized_key_honors_fail_mode) {
    // A remote address longer than the fixed inline key buffer cannot be tracked.
    // Under failClosed (the default) such a request is DENIED rather than silently
    // admitted, so an attacker cannot bypass the limiter with an overlong key.
    const auto closed = RateLimitRule::fixedWindow(5, std::chrono::seconds(1), RateLimitOverflowPolicy::kDeny);
    RateLimiter closedLimiter(closed, RouteRateLimitPresence::kAbsent, 8);
    const std::string longKey(100, 'a');
    RUVIA_CHECK(!rateLimitAllowed(decideRequestRateLimit(&closedLimiter, longKey)));

    // Under failOpen the same request is admitted (availability over strictness).
    const auto open = RateLimitRule::fixedWindow(5, std::chrono::seconds(1), RateLimitOverflowPolicy::kAllow);
    RateLimiter openLimiter(open, RouteRateLimitPresence::kAbsent, 8);
    RUVIA_CHECK(rateLimitAllowed(decideRequestRateLimit(&openLimiter, longKey)));
}

RUVIA_TEST(rate_limiter_now_ms_is_positive_and_monotonic) {
    const auto first = rateLimiterNowMs();
    const auto second = rateLimiterNowMs();
    RUVIA_CHECK(first > 0);
    RUVIA_CHECK(second >= first);
}

RUVIA_TEST(rate_limit_rule_rejects_invalid_fixed_windows) {
    bool zeroRequestsRejected = false;
    try {
        (void)RateLimitRule::fixedWindow(0, std::chrono::milliseconds(1));
    } catch (const std::invalid_argument&) {
        zeroRequestsRejected = true;
    }
    RUVIA_CHECK(zeroRequestsRejected);

    bool zeroWindowRejected = false;
    try {
        (void)RateLimitRule::fixedWindow(1, std::chrono::milliseconds(0));
    } catch (const std::invalid_argument&) {
        zeroWindowRejected = true;
    }
    RUVIA_CHECK(zeroWindowRejected);

    const auto valid = RateLimitRule::fixedWindow(100, std::chrono::seconds(60), RateLimitOverflowPolicy::kAllow);
    RUVIA_CHECK_EQ(valid.maxRequests(), std::size_t{100});
    RUVIA_CHECK(valid.window() == std::chrono::milliseconds(60000));
    RUVIA_CHECK(valid.overflowPolicy() == RateLimitOverflowPolicy::kAllow);
}

RUVIA_TEST(route_rate_limit_429_carries_retry_after_and_ratelimit_headers) {
    // The per-route limiter's rejection path (applyRouteRateLimit) had no coverage:
    // the RateLimiter core is tested, but not the 429 response it produces with the
    // Retry-After and X-RateLimit-* advisory headers a client relies on.
    RateLimiter limiter(std::nullopt, RouteRateLimitPresence::kPresent, ruvia::kDefaultRateLimitSlotsPerWorker, std::pmr::get_default_resource());
    const RouteRateLimitOptions options{.rule = RateLimitRule::fixedWindow(1, std::chrono::seconds(60))};
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
    RUVIA_CHECK_EQ(second.limit, std::string("1"));      // X-RateLimit-Limit = maxRequests
    RUVIA_CHECK_EQ(second.remaining, std::string("0"));  // X-RateLimit-Remaining = 0 when blocked
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
