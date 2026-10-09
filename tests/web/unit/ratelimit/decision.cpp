#include <chrono>
#include <concepts>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/core/memory/memory_pool.h"
#include "ruvia/http/http_request.h"
#include "ruvia/web/context.h"
#include "ruvia/web/rate_limit.h"
#include "ruvia/web/rate_limit_rule.h"

#include "context/context_access.h"
#include "context/context_services.h"
#include "context_services_fixture.h"
#include "ratelimit/rate_limit_decision.h"
#include "ratelimit/rate_limit_key.h"
#include "server/http1_closing_rejection.h"
#include "test_harness.h"

namespace {

using ruvia::rate_limit_overflow_policy;
using ruvia::rate_limit_rule;
using ruvia::request_memory;
using ruvia::worker_memory;
using ruvia::detail::apply_route_rate_limit;
using ruvia::detail::context_access;
using ruvia::detail::context_services;
using ruvia::detail::decide_request_rate_limit;
using ruvia::detail::rate_limit_decision;
using ruvia::detail::rate_limiter_now_ms;
using ruvia::detail::rate_limiter_type;
using ruvia::detail::route_rate_limit_options;
using ruvia::detail::route_rate_limit_presence;

bool rate_limit_allowed(rate_limit_decision decision) {
    return decision.allowed() != nullptr;
}

struct route_limit_result final {
    bool allowed_{false};
    bool has_response_{false};
    std::uint16_t status_{0};
    std::string retry_after_;
    std::string limit_;
    std::string remaining_;
    std::string reset_;
};

// Runs the per-route limiter over one fresh request that shares the given limiter
// and scope (and the same empty remote address, so they collide on one counter).
route_limit_result run_route_limit(
    rate_limiter_type& limiter, std::uintptr_t scope, const route_rate_limit_options& options) {
    worker_memory worker;
    request_memory memory(worker);
    auto [request, parse_error] =
        ruvia::make_parsed_http_request("GET", "/", {}, {}, memory.resource());
    if (parse_error) {
        throw std::logic_error("invalid rate-limit test request");
    }
    context_services services = ruvia::test::test_context_services().with_rate_limiter(limiter);
    auto context_value = context_access::make(memory, request, scope, services);

    route_limit_result r;
    r.allowed_ = apply_route_rate_limit(context_value, options);
    r.has_response_ = context_access::has_response(context_value);
    if (r.has_response_) {
        auto response = context_access::take_response(context_value);
        r.status_ = response.status().value();
        r.retry_after_ = std::string(response.header("Retry-After").value_or(std::string_view{}));
        r.limit_ = std::string(response.header("X-RateLimit-Limit").value_or(std::string_view{}));
        r.remaining_ =
            std::string(response.header("X-RateLimit-Remaining").value_or(std::string_view{}));
        r.reset_ = std::string(response.header("X-RateLimit-Reset").value_or(std::string_view{}));
    }
    return r;
}

}  // namespace

RUVIA_TEST(rate_limit_allowed_when_no_limiter) {
    // A null limiter means rate limiting is off: always allowed, no dereference.
    const auto decision = decide_request_rate_limit(nullptr, "1.2.3.4");
    RUVIA_CHECK(decision.allowed() != nullptr);
    RUVIA_CHECK(decision.rejection() == nullptr);
}

RUVIA_TEST(rate_limit_allowed_when_limiter_disabled) {
    rate_limiter_type limiter(std::nullopt, route_rate_limit_presence::absent, 1);
    RUVIA_CHECK(!limiter.has_default_rule());
    const auto decision = decide_request_rate_limit(&limiter, "1.2.3.4");
    RUVIA_CHECK(decision.allowed() != nullptr);
}

RUVIA_TEST(rate_limit_rejection_owns_web_error_and_retry_headers) {
    const auto decision = rate_limit_decision::reject(std::chrono::milliseconds(1'001));
    const auto* rejection = decision.rejection();
    RUVIA_CHECK(rejection != nullptr);

    const auto error = ruvia::detail::rate_limit_rejection_error();
    RUVIA_CHECK_EQ(error.status(), ruvia::http_status::too_many_requests);
    RUVIA_CHECK_EQ(error.code(), std::string_view("too_many_requests"));
    RUVIA_CHECK_EQ(error.message(), std::string_view("rate limit exceeded"));

    ruvia::http_response response;
    ruvia::detail::apply_rate_limit_rejection_headers(response, *rejection);
    RUVIA_CHECK_EQ(response.header("Retry-After"), std::string_view("2"));
    RUVIA_CHECK(!response.header("X-RateLimit-Limit").has_value());

    ruvia::detail::apply_route_rate_limit_rejection_headers(response, *rejection, 7);
    RUVIA_CHECK_EQ(response.header("Retry-After"), std::string_view("2"));
    RUVIA_CHECK_EQ(response.header("X-RateLimit-Limit"), std::string_view("7"));
    RUVIA_CHECK_EQ(response.header("X-RateLimit-Remaining"), std::string_view("0"));
    RUVIA_CHECK_EQ(response.header("X-RateLimit-Reset"), std::string_view("2"));
}

RUVIA_TEST(http1_closing_rejection_has_exclusive_error_alternatives) {
    using ruvia::detail::http1_closing_rejection;

    const http1_closing_rejection none;
    RUVIA_CHECK(none.error() == nullptr);
    RUVIA_CHECK(none.get_rate_limit() == nullptr);

    const auto ordinary = http1_closing_rejection::error(ruvia::http_error_info(
        {.status_ = ruvia::http_status::bad_request, .message_ = "bad request"}));
    RUVIA_CHECK(ordinary.error() != nullptr);
    RUVIA_CHECK_EQ(ordinary.error()->status(), ruvia::http_status::bad_request);
    RUVIA_CHECK(ordinary.get_rate_limit() == nullptr);

    const auto decision = rate_limit_decision::reject(std::chrono::milliseconds(125));
    const auto limited = http1_closing_rejection::get_rate_limit(
        ruvia::detail::rate_limit_rejection_error(), *decision.rejection());
    RUVIA_CHECK(limited.error() != nullptr);
    RUVIA_CHECK_EQ(limited.error()->status(), ruvia::http_status::too_many_requests);
    RUVIA_CHECK(limited.get_rate_limit() != nullptr);
    RUVIA_CHECK_EQ(limited.get_rate_limit()->retry_after(), std::chrono::milliseconds(125));
}

RUVIA_TEST(rate_limit_enforces_per_key_request_budget) {
    // The core allow/deny behavior: within a single window a key gets exactly
    // max_requests admissions and is then denied, while a different key is counted
    // independently and is unaffected. A 60s window keeps every call in the test
    // inside one window, so the outcome is deterministic without clock control.
    const auto rule = rate_limit_rule{
        .max_requests_ = 3,
        .window_ = std::chrono::seconds(60),
    };
    rate_limiter_type limiter(rule, route_rate_limit_presence::absent, 16);
    RUVIA_CHECK(limiter.has_default_rule());

    RUVIA_CHECK(rate_limit_allowed(decide_request_rate_limit(&limiter, "10.0.0.1")));
    RUVIA_CHECK(rate_limit_allowed(decide_request_rate_limit(&limiter, "10.0.0.1")));
    RUVIA_CHECK(rate_limit_allowed(decide_request_rate_limit(&limiter, "10.0.0.1")));
    const auto denied = decide_request_rate_limit(&limiter, "10.0.0.1");
    RUVIA_CHECK(denied.allowed() == nullptr);
    // A denied request reports a positive time until the window resets.
    RUVIA_CHECK(denied.rejection() != nullptr);
    RUVIA_CHECK(denied.rejection()->retry_after().count() > 0);

    // A different address has its own budget and is still admitted.
    RUVIA_CHECK(rate_limit_allowed(decide_request_rate_limit(&limiter, "10.0.0.2")));
}

RUVIA_TEST(rate_limit_oversized_key_honors_fail_mode) {
    // A remote address longer than the fixed inline key buffer cannot be tracked.
    // Under fail_closed (the default) such a request is DENIED rather than silently
    // admitted, so an attacker cannot bypass the limiter with an overlong key.
    const auto closed = rate_limit_rule{
        .max_requests_ = 5,
        .window_ = std::chrono::seconds(1),
        .overflow_policy_ = rate_limit_overflow_policy::deny,
    };
    rate_limiter_type closed_limiter(closed, route_rate_limit_presence::absent, 8);
    const std::string long_key(100, 'a');
    RUVIA_CHECK(!rate_limit_allowed(decide_request_rate_limit(&closed_limiter, long_key)));

    // Under failOpen the same request is admitted (availability over strictness).
    const auto open = rate_limit_rule{
        .max_requests_ = 5,
        .window_ = std::chrono::seconds(1),
        .overflow_policy_ = rate_limit_overflow_policy::allow,
    };
    rate_limiter_type open_limiter(open, route_rate_limit_presence::absent, 8);
    RUVIA_CHECK(rate_limit_allowed(decide_request_rate_limit(&open_limiter, long_key)));
}

RUVIA_TEST(rate_limiter_now_ms_is_positive_and_monotonic) {
    const auto first = rate_limiter_now_ms();
    const auto second = rate_limiter_now_ms();
    RUVIA_CHECK(first > 0);
    RUVIA_CHECK(second >= first);
}

RUVIA_TEST(rate_limit_rule_rejects_invalid_fixed_windows) {
    bool zero_requests_rejected = false;
    try {
        ruvia::detail::validate_rate_limit_rule({
            .max_requests_ = 0,
            .window_ = std::chrono::milliseconds(1),
        });
    } catch (const std::invalid_argument&) {
        zero_requests_rejected = true;
    }
    RUVIA_CHECK(zero_requests_rejected);

    bool zero_window_rejected = false;
    try {
        ruvia::detail::validate_rate_limit_rule({
            .max_requests_ = 1,
            .window_ = std::chrono::milliseconds(0),
        });
    } catch (const std::invalid_argument&) {
        zero_window_rejected = true;
    }
    RUVIA_CHECK(zero_window_rejected);

    bool invalid_overflow_policy_rejected = false;
    try {
        ruvia::detail::validate_rate_limit_rule({
            .max_requests_ = 1,
            .window_ = std::chrono::milliseconds(1),
            .overflow_policy_ = static_cast<rate_limit_overflow_policy>(0xFF),
        });
    } catch (const std::invalid_argument&) {
        invalid_overflow_policy_rejected = true;
    }
    RUVIA_CHECK(invalid_overflow_policy_rejected);

    const auto valid = rate_limit_rule{
        .max_requests_ = 100,
        .window_ = std::chrono::seconds(60),
        .overflow_policy_ = rate_limit_overflow_policy::allow,
    };
    RUVIA_CHECK_EQ(valid.max_requests_, std::size_t{100});
    RUVIA_CHECK(valid.window_ == std::chrono::milliseconds(60000));
    RUVIA_CHECK(valid.overflow_policy_ == rate_limit_overflow_policy::allow);
}

RUVIA_TEST(route_rate_limit_429_carries_retry_after_and_ratelimit_headers) {
    // The per-route limiter's rejection path (apply_route_rate_limit) had no coverage:
    // the rate_limiter_type core is tested, but not the 429 response it produces with the
    // Retry-After and X-rate_limit-* advisory headers a client relies on.
    rate_limiter_type limiter(std::nullopt, route_rate_limit_presence::present,
        ruvia::default_rate_limit_capacity_per_worker, std::pmr::get_default_resource());
    const route_rate_limit_options options{.rule_ = {
                                               .max_requests_ = 1,
                                               .window_ = std::chrono::seconds(60),
                                           }};
    const std::uintptr_t scope = 0xABCD;

    // The first request under this (scope, empty-IP) key is admitted with no response.
    const auto first = run_route_limit(limiter, scope, options);
    RUVIA_CHECK(first.allowed_);
    RUVIA_CHECK(!first.has_response_);

    // The second exceeds max_requests=1 -> short-circuited with a 429 and the full
    // advisory header set.
    const auto second = run_route_limit(limiter, scope, options);
    RUVIA_CHECK(!second.allowed_);
    RUVIA_CHECK(second.has_response_);
    RUVIA_CHECK_EQ(second.status_, std::uint16_t{429});
    RUVIA_CHECK_EQ(second.limit_, std::string("1"));      // X-rate_limit-Limit = max_requests
    RUVIA_CHECK_EQ(second.remaining_, std::string("0"));  // X-rate_limit-Remaining = 0 when blocked
    // Retry-After is a positive whole number of seconds (ceil of the ms remaining in
    // the 60s window), and X-rate_limit-Reset mirrors it.
    RUVIA_CHECK(!second.retry_after_.empty());
    const int retry = std::stoi(second.retry_after_);
    RUVIA_CHECK(retry >= 1 && retry <= 60);
    RUVIA_CHECK_EQ(second.reset_, second.retry_after_);
}

namespace {
std::string rate_limit_key(std::string_view remote_address) {
    char buffer[ruvia::detail::rate_limit_key_buffer_bytes];
    const auto key = ruvia::detail::rate_limit_key_for(remote_address, buffer);
    return std::string(key);
}
}  // namespace

RUVIA_TEST(rate_limit_key_groups_ipv6_by_64_prefix) {
    // A client typically controls an entire IPv6 /64 (or larger). Keying on the full
    // address would let it rotate addresses to bypass the per-IP limit and exhaust
    // the shared slot table, so genuine IPv6 is grouped by its /64 network prefix.
    // Two addresses sharing a /64 must yield the same key...
    RUVIA_CHECK_EQ(rate_limit_key("2001:db8:1:2::1"), rate_limit_key("2001:db8:1:2::dead:beef"));
    RUVIA_CHECK_EQ(
        rate_limit_key("2001:db8:1:2:ffff:ffff:ffff:ffff"), rate_limit_key("2001:db8:1:2::1"));
    // ...and different /64s must yield different keys (no over-grouping).
    RUVIA_CHECK(rate_limit_key("2001:db8:1:2::1") != rate_limit_key("2001:db8:1:3::1"));
    RUVIA_CHECK(rate_limit_key("2001:db8:1:2::1") != rate_limit_key("2001:db8:2:2::1"));

    // IPv4 passes through unchanged (each host is already its own key).
    RUVIA_CHECK_EQ(rate_limit_key("203.0.113.7"), std::string("203.0.113.7"));
    RUVIA_CHECK(rate_limit_key("203.0.113.7") != rate_limit_key("203.0.113.8"));

    // IPv4-mapped IPv6 must NOT collapse to one /64 -- each mapped host stays distinct.
    RUVIA_CHECK(rate_limit_key("::ffff:203.0.113.7") != rate_limit_key("::ffff:203.0.113.8"));
    // A dual-stack listener presents the same IPv4 client as dotted form or as
    // ::ffff:a.b.c.d; those spellings must share one slot or the client bypasses
    // the limiter by connecting both ways.
    RUVIA_CHECK_EQ(rate_limit_key("::ffff:203.0.113.7"), rate_limit_key("203.0.113.7"));
    RUVIA_CHECK_EQ(rate_limit_key("::ffff:203.0.113.7"), std::string("203.0.113.7"));

    // Scoped IPv6 addresses carry an interface/zone identifier. That scope is
    // part of the peer identity for link-local addresses, so it must not be
    // stripped by the /64 grouping path.
    RUVIA_CHECK_EQ(rate_limit_key("fe80::1%1"), std::string("fe80::1%1"));
    RUVIA_CHECK(rate_limit_key("fe80::1%1") != rate_limit_key("fe80::1%2"));
}

RUVIA_TEST(rate_limit_key_preserves_malformed_ipv6_instead_of_parsing_a_prefix) {
    std::string address = "2001:db8::1";
    address.push_back('\0');
    address.append("suffix");
    RUVIA_CHECK_EQ(rate_limit_key(address), address);
}
