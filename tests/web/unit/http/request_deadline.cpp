#include "test_harness.h"

// The deadline mechanism itself: a stop source that trips on whichever comes
// first, the worker stopping or the clock elapsing -- and, just as importantly,
// that trips on NEITHER once the request it belongs to is gone.

#include <chrono>
#include <optional>

#include "ruvia/core/stop_token.h"

#include "server/request_deadline.h"

using namespace std::chrono_literals;
using ruvia::stop_source;
using ruvia::detail::effective_handler_deadline;
using ruvia::detail::request_deadline;

RUVIA_TEST(effective_handler_deadline_takes_the_stricter_scope) {
    // Neither declared.
    RUVIA_CHECK(effective_handler_deadline(std::nullopt, 0) == 0ms);
    // Only one side declared.
    RUVIA_CHECK(
        effective_handler_deadline(std::optional<std::chrono::milliseconds>(2000ms), 0) == 2000ms);
    RUVIA_CHECK(effective_handler_deadline(std::nullopt, 500) == 500ms);
    // Both: a route may tighten...
    RUVIA_CHECK(
        effective_handler_deadline(std::optional<std::chrono::milliseconds>(2000ms), 500) == 500ms);
    // ...but never extend, which is the one composition rule.
    RUVIA_CHECK(effective_handler_deadline(std::optional<std::chrono::milliseconds>(2000ms), 30000) ==
                2000ms);
}

RUVIA_TEST(request_deadline_trips_on_worker_stop_without_claiming_it_elapsed) {
    stop_source worker_stop;
    request_deadline deadline_value(worker_stop.token());

    RUVIA_CHECK(!deadline_value.token().stop_requested());
    RUVIA_CHECK(!deadline_value.exceeded());

    worker_stop.request_stop();

    // The one token a handler observes answers both questions...
    RUVIA_CHECK(deadline_value.token().stop_requested());
    // ...but exceeded() must still say the clock did NOT run out, or a handler
    // cannot tell a shutdown from a timeout.
    RUVIA_CHECK(!deadline_value.exceeded());
}

RUVIA_TEST(request_deadline_starts_already_stopped_when_the_worker_is_gone) {
    stop_source worker_stop;
    worker_stop.request_stop();

    // register_callback runs the callback immediately for an already-stopped
    // token, so a request accepted mid-shutdown must not start with a live one.
    request_deadline deadline_value(worker_stop.token());
    RUVIA_CHECK(deadline_value.token().stop_requested());
    RUVIA_CHECK(!deadline_value.exceeded());
}

RUVIA_TEST(request_deadline_is_inert_before_it_is_armed) {
    stop_source worker_stop;
    {
        request_deadline deadline_value(worker_stop.token());
        RUVIA_CHECK(!deadline_value.token().stop_requested());
    }
    // Destroying an unarmed deadline must not disturb the worker source, whose
    // registration list it just left.
    RUVIA_CHECK(!worker_stop.stop_requested());
    worker_stop.request_stop();
    RUVIA_CHECK(worker_stop.stop_requested());
}
