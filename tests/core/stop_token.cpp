#include "test_harness.h"

#include <atomic>
#include <type_traits>
#include <thread>

#include "ruvia/core/StopToken.h"

static_assert(!std::is_move_constructible_v<ruvia::StopRegistration>);
static_assert(!std::is_copy_constructible_v<ruvia::StopRegistration>);

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

RUVIA_TEST(stop_token_registration_can_reuse_storage) {
    ruvia::detail::StopSource source;
    int calls = 0;
    ruvia::StopRegistration registration;
    source.token().registerCallback(registration, [&calls] { ++calls; });
    RUVIA_CHECK(registration.registered());
    source.requestStop();
    RUVIA_CHECK_EQ(calls, 1);
}

RUVIA_TEST(combined_stop_token_observes_either_source) {
    ruvia::detail::StopSource first;
    ruvia::detail::StopSource second;
    auto combined = ruvia::combineStopTokens(first.token(), second.token());
    RUVIA_CHECK(combined.stoppable());
    RUVIA_CHECK(!combined.stopRequested());
    second.requestStop();
    RUVIA_CHECK(combined.stopRequested());
}

RUVIA_TEST(combined_stop_registration_outlives_temporary_token) {
    ruvia::detail::StopSource first;
    ruvia::detail::StopSource second;
    int calls = 0;
    auto registration = ruvia::combineStopTokens(first.token(), second.token()).registerCallback([&calls] { ++calls; });
    RUVIA_CHECK(registration.registered());
    second.requestStop();
    first.requestStop();
    RUVIA_CHECK_EQ(calls, 1);
}

RUVIA_TEST(combined_stop_registration_reuses_storage_after_token_dies) {
    ruvia::detail::StopSource first;
    ruvia::detail::StopSource second;
    int calls = 0;
    ruvia::StopRegistration registration;
    ruvia::combineStopTokens(first.token(), second.token()).registerCallback(registration, [&calls] { ++calls; });
    RUVIA_CHECK(registration.registered());
    first.requestStop();
    second.requestStop();
    RUVIA_CHECK_EQ(calls, 1);
}

RUVIA_TEST(combined_stop_token_retains_nested_bridge) {
    ruvia::detail::StopSource first;
    ruvia::detail::StopSource second;
    ruvia::detail::StopSource third;
    auto nested = ruvia::combineStopTokens(ruvia::combineStopTokens(first.token(), second.token()), third.token());
    first.requestStop();
    RUVIA_CHECK(nested.stopRequested());
}

RUVIA_TEST(combined_stop_token_handles_single_and_pre_stopped_inputs) {
    ruvia::detail::StopSource source;
    auto single = ruvia::combineStopTokens({}, source.token());
    source.requestStop();
    RUVIA_CHECK(single.stopRequested());

    ruvia::detail::StopSource stopped;
    ruvia::detail::StopSource idle;
    stopped.requestStop();
    auto combined = ruvia::combineStopTokens(stopped.token(), idle.token());
    RUVIA_CHECK(combined.stopRequested());
}
