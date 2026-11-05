#include "test_harness.h"

// The per-worker rate-limit table: each key's sliding-window estimate, the
// refusal headers derived from it, slot reclamation, and the overflow policy
// for keys a saturated table cannot place.
//
// Every request pins its arrival at an exact offset from one origin, so the
// window arithmetic is checked to the millisecond without reading a clock.

#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <system_error>

#include "ruvia/web/context.h"
#include "ruvia/web/controller.h"
#include "ruvia/web/rate_limit.h"
#include "ruvia/web/testing.h"

namespace {

class rate_limit_table_controller final {
public:
    RUVIA_CONTROLLER_GROUP("/rate-limit-table")
    RUVIA_ROUTES_BEGIN(rate_limit_table_controller)
    RUVIA_GET("/ok", ok);
    RUVIA_GET("/route-limited", ok, ruvia::rate_limit<2, 1000>);
    RUVIA_ROUTES_END

    ruvia::task<ruvia::http_response> ok(ruvia::context& context_value) {
        co_return context_value.text("ok");
    }
};

constexpr std::array<std::string_view, 4> held_addresses{"192.0.2.1", "192.0.2.2", "192.0.2.3", "192.0.2.4"};

constexpr std::chrono::steady_clock::time_point window_origin{std::chrono::hours(1)};

[[nodiscard]] ruvia::dispatch_response request_at(ruvia::test_app& app, std::string_view address,
    std::chrono::milliseconds offset, std::string_view target = "/rate-limit-table/ok") {
    return app.request(ruvia::test_request::get(target).remote(address).arrival(window_origin + offset));
}

[[nodiscard]] ruvia::http_status_code request_from(
    ruvia::test_app& app, std::string_view address, std::chrono::milliseconds offset = {}) {
    return request_at(app, address, offset).status();
}

[[nodiscard]] std::optional<std::uint64_t> header_number(
    const ruvia::dispatch_response& response, std::string_view name) {
    const auto text = response.header(name);
    std::uint64_t value = 0;
    if (!text.has_value() || text->empty()) {
        return std::nullopt;
    }
    const auto [end, error] = std::from_chars(text->data(), text->data() + text->size(), value);
    if (error != std::errc{} || end != text->data() + text->size()) {
        return std::nullopt;
    }
    return value;
}

void configure_saturating_limit(ruvia::test_app& app, ruvia::rate_limit_overflow_policy overflow_policy) {
    app.rate_limit(ruvia::rate_limit_config{
        .rule_ = {.max_requests_ = 2, .window_ = std::chrono::minutes(10), .overflow_policy_ = overflow_policy},
        .capacity_per_worker_ = held_addresses.size()});
}

void configure_window_limit(ruvia::test_app& app, std::size_t max_requests, std::chrono::milliseconds window,
    std::size_t capacity = ruvia::default_rate_limit_capacity_per_worker) {
    app.rate_limit(ruvia::rate_limit_config{
        .rule_ = {.max_requests_ = max_requests, .window_ = window}, .capacity_per_worker_ = capacity});
}

}  // namespace

RUVIA_TEST(rate_limit_saturated_table_denies_new_keys_and_keeps_counting_held_keys) {
    ruvia::test_app app;
    configure_saturating_limit(app, ruvia::rate_limit_overflow_policy::deny);
    for (const auto address : held_addresses) {
        RUVIA_CHECK(request_from(app, address) == ruvia::http_status::ok);
    }
    // Every slot holds a live entry, so a fifth address cannot be placed.
    RUVIA_CHECK(request_from(app, "192.0.2.5") == ruvia::http_status::too_many_requests);
    RUVIA_CHECK(request_from(app, "192.0.2.6") == ruvia::http_status::too_many_requests);
    // Held keys are still found and counted against their own budget.
    for (const auto address : held_addresses) {
        RUVIA_CHECK(request_from(app, address) == ruvia::http_status::ok);
        RUVIA_CHECK(request_from(app, address) == ruvia::http_status::too_many_requests);
    }
}

RUVIA_TEST(rate_limit_saturated_table_admits_new_keys_under_allow_overflow) {
    ruvia::test_app app;
    configure_saturating_limit(app, ruvia::rate_limit_overflow_policy::allow);
    for (const auto address : held_addresses) {
        RUVIA_CHECK(request_from(app, address) == ruvia::http_status::ok);
    }
    // An unplaced key is admitted without a counter, so it is never limited.
    for (int i = 0; i < 3; ++i) {
        RUVIA_CHECK(request_from(app, "192.0.2.5") == ruvia::http_status::ok);
    }
    RUVIA_CHECK(request_from(app, held_addresses.front()) == ruvia::http_status::ok);
    RUVIA_CHECK(request_from(app, held_addresses.front()) == ruvia::http_status::too_many_requests);
}

RUVIA_TEST(rate_limit_budget_spent_before_a_window_boundary_does_not_refill_after_it) {
    using std::chrono::milliseconds;
    ruvia::test_app app;
    configure_window_limit(app, 10, milliseconds(1000));
    constexpr std::string_view client = "198.51.100.1";
    // The first request opens the key's window at 0; the other nine go in its
    // last millisecond.
    RUVIA_CHECK(request_from(app, client) == ruvia::http_status::ok);
    for (int i = 1; i < 10; ++i) {
        RUVIA_CHECK(request_from(app, client, milliseconds(999)) == ruvia::http_status::ok);
    }
    RUVIA_CHECK(request_from(app, client, milliseconds(999)) == ruvia::http_status::too_many_requests);

    // At the boundary a fixed window would grant ten fresh requests; here the
    // previous ten still count in full, so the estimate sits at the limit.
    const auto at_boundary = request_at(app, client, milliseconds(1000));
    RUVIA_CHECK(at_boundary.status() == ruvia::http_status::too_many_requests);
    // 10 * 999 / 1000 < 10 one millisecond later; draining 10 takes the rest
    // of this window. Both round up to one second.
    RUVIA_CHECK(header_number(at_boundary, "Retry-After") == std::optional<std::uint64_t>(1));
    RUVIA_CHECK(header_number(at_boundary, "X-RateLimit-Reset") == std::optional<std::uint64_t>(1));

    int admitted = 0;
    for (int i = 0; i < 10; ++i) {
        admitted += request_from(app, client, milliseconds(1001)) == ruvia::http_status::ok ? 1 : 0;
    }
    RUVIA_CHECK_EQ(admitted, 1);

    // With one admitted, 10 * remaining / 1000 + 1 < 10 needs remaining
    // below 900 ms: exactly 100 ms after 1001, which Retry-After rounds up.
    const auto refused = request_at(app, client, milliseconds(1001));
    RUVIA_CHECK(refused.status() == ruvia::http_status::too_many_requests);
    RUVIA_CHECK(header_number(refused, "Retry-After") == std::optional<std::uint64_t>(1));
    RUVIA_CHECK(header_number(refused, "X-RateLimit-Remaining") == std::optional<std::uint64_t>(0));
    // The current request fades out across the next window: 999 + 1000 ms.
    RUVIA_CHECK(header_number(refused, "X-RateLimit-Reset") == std::optional<std::uint64_t>(2));
    RUVIA_CHECK(request_from(app, client, milliseconds(1100)) == ruvia::http_status::too_many_requests);
    RUVIA_CHECK(request_from(app, client, milliseconds(1101)) == ruvia::http_status::ok);
}

RUVIA_TEST(rate_limit_admits_steady_traffic_paced_at_the_limit) {
    using std::chrono::milliseconds;
    ruvia::test_app app;
    configure_window_limit(app, 4, milliseconds(400));
    // Gaps longer than window / limit keep the estimate below the limit at
    // every arrival, across three window boundaries.
    for (int i = 0; i < 13; ++i) {
        RUVIA_CHECK(request_from(app, "198.51.100.2", milliseconds(101 * i)) == ruvia::http_status::ok);
    }

    // Gaps of exactly window / limit put the estimate exactly at the limit
    // whenever an arrival coincides with a window start; one millisecond
    // later the previous window has faded enough.
    constexpr std::string_view aligned = "198.51.100.3";
    for (int i = 0; i < 4; ++i) {
        RUVIA_CHECK(request_from(app, aligned, milliseconds(100 * i)) == ruvia::http_status::ok);
    }
    RUVIA_CHECK(request_from(app, aligned, milliseconds(400)) == ruvia::http_status::too_many_requests);
    RUVIA_CHECK(request_from(app, aligned, milliseconds(401)) == ruvia::http_status::ok);
}

RUVIA_TEST(rate_limit_refusal_headers_follow_the_estimate) {
    ruvia::test_app app;
    configure_window_limit(app, 2, std::chrono::minutes(10));
    constexpr std::string_view client = "198.51.100.4";
    RUVIA_CHECK(request_from(app, client) == ruvia::http_status::ok);
    RUVIA_CHECK(request_from(app, client) == ruvia::http_status::ok);
    const auto refused = request_at(app, client, {});
    RUVIA_CHECK(refused.status() == ruvia::http_status::too_many_requests);
    RUVIA_CHECK(header_number(refused, "X-RateLimit-Limit") == std::optional<std::uint64_t>(2));
    RUVIA_CHECK(header_number(refused, "X-RateLimit-Remaining") == std::optional<std::uint64_t>(0));
    // The spent window carries its full count into the next one, which admits
    // once its first millisecond has passed: 600'001 ms, rounded up.
    RUVIA_CHECK(header_number(refused, "Retry-After") == std::optional<std::uint64_t>(601));
    // The two requests fade out across the next window: zero after 1'200'000 ms.
    RUVIA_CHECK(header_number(refused, "X-RateLimit-Reset") == std::optional<std::uint64_t>(1200));
}

RUVIA_TEST(rate_limit_refusal_headers_do_not_saturate_for_the_longest_window) {
    ruvia::test_app app;
    configure_window_limit(app, 1, std::chrono::milliseconds::max());
    constexpr std::string_view client = "198.51.100.5";
    RUVIA_CHECK(request_from(app, client) == ruvia::http_status::ok);
    const auto refused = request_at(app, client, {});
    RUVIA_CHECK(refused.status() == ruvia::http_status::too_many_requests);
    // Retry: the whole window plus one millisecond, 2^63 ms. Reset: two whole
    // windows, 2^64 - 2 ms, beyond any signed millisecond count. Both are
    // rounded up to seconds without clamping.
    RUVIA_CHECK(header_number(refused, "Retry-After") == std::optional<std::uint64_t>(9'223'372'036'854'776ULL));
    RUVIA_CHECK(header_number(refused, "X-RateLimit-Reset") == std::optional<std::uint64_t>(18'446'744'073'709'552ULL));
}

RUVIA_TEST(rate_limit_table_reclaims_a_slot_only_after_two_windows) {
    using std::chrono::milliseconds;
    ruvia::test_app app;
    configure_window_limit(app, 2, milliseconds(1000), held_addresses.size());
    for (const auto address : held_addresses) {
        RUVIA_CHECK(request_from(app, address) == ruvia::http_status::ok);
    }
    constexpr std::string_view newcomer = "192.0.2.5";
    RUVIA_CHECK(request_from(app, newcomer) == ruvia::http_status::too_many_requests);
    // After one window each held count has only become its key's previous
    // count, which the estimate still reads.
    RUVIA_CHECK(request_from(app, newcomer, milliseconds(1999)) == ruvia::http_status::too_many_requests);
    RUVIA_CHECK(request_from(app, newcomer, milliseconds(2000)) == ruvia::http_status::ok);
}

RUVIA_TEST(route_rate_limit_counts_with_the_sliding_window_estimate) {
    using std::chrono::milliseconds;
    ruvia::test_app app;
    constexpr std::string_view client = "198.51.100.6";
    constexpr std::string_view target = "/rate-limit-table/route-limited";
    RUVIA_CHECK(request_at(app, client, {}, target).status() == ruvia::http_status::ok);
    RUVIA_CHECK(request_at(app, client, {}, target).status() == ruvia::http_status::ok);
    RUVIA_CHECK(request_at(app, client, {}, target).status() == ruvia::http_status::too_many_requests);
    // rate_limit<2, 1000>: 2 * 1000 / 1000 is still the limit at the
    // boundary, 2 * 999 / 1000 is below it a millisecond later.
    RUVIA_CHECK(request_at(app, client, milliseconds(1000), target).status() == ruvia::http_status::too_many_requests);
    RUVIA_CHECK(request_at(app, client, milliseconds(1001), target).status() == ruvia::http_status::ok);
}
