#include "test_harness.h"

// A saturated per-worker rate-limit table keeps counting the keys it holds and
// applies the overflow policy to every key it cannot place.

#include <array>
#include <chrono>
#include <string_view>

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
    RUVIA_ROUTES_END

    ruvia::task<ruvia::http_response> ok(ruvia::context& context_value) {
        co_return context_value.text("ok");
    }
};

constexpr std::array<std::string_view, 4> held_addresses{"192.0.2.1", "192.0.2.2", "192.0.2.3", "192.0.2.4"};

[[nodiscard]] ruvia::http_status_code request_from(ruvia::test_app& app, std::string_view address) {
    return app.request(ruvia::test_request::get("/rate-limit-table/ok").remote(address)).status();
}

void configure_saturating_limit(ruvia::test_app& app, ruvia::rate_limit_overflow_policy overflow_policy) {
    app.rate_limit(ruvia::rate_limit_config{
        .rule_ = {.max_requests_ = 2, .window_ = std::chrono::minutes(10), .overflow_policy_ = overflow_policy},
        .capacity_per_worker_ = held_addresses.size()});
}

}  // namespace

RUVIA_TEST(rate_limit_saturated_table_denies_new_keys_and_keeps_counting_held_keys) {
    ruvia::test_app app;
    configure_saturating_limit(app, ruvia::rate_limit_overflow_policy::deny);
    for (const auto address : held_addresses) {
        RUVIA_CHECK(request_from(app, address) == ruvia::http_status::ok);
    }
    // Every slot holds a live window, so a fifth address cannot be placed.
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
