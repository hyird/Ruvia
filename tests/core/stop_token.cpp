#include "test_harness.h"

#include <atomic>
#include <thread>

#include "ruvia/core/StopToken.h"

RUVIA_TEST(stop_token_registration_runs_once) {
    ruvia::detail::StopSource source;
    std::atomic_int calls{0};
    auto registration = source.token().registerCallback([&calls] { calls.fetch_add(1, std::memory_order_relaxed); });
    RUVIA_CHECK(registration.registered());
    source.requestStop();
    source.requestStop();
    RUVIA_CHECK_EQ(calls.load(std::memory_order_relaxed), 1);
}

RUVIA_TEST(stop_token_registration_can_be_reset) {
    ruvia::detail::StopSource source;
    int calls = 0;
    auto registration = source.token().registerCallback([&calls] { ++calls; });
    registration.reset();
    source.requestStop();
    RUVIA_CHECK_EQ(calls, 0);
}

RUVIA_TEST(stop_token_registration_after_stop_runs_immediately) {
    ruvia::detail::StopSource source;
    source.requestStop();
    int calls = 0;
    auto registration = source.token().registerCallback([&calls] { ++calls; });
    RUVIA_CHECK(!registration.registered());
    RUVIA_CHECK_EQ(calls, 1);
}
