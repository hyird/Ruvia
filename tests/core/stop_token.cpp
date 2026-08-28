#include "test_harness.h"

#include <atomic>
#include <type_traits>
#include <thread>

#include "ruvia/core/StopToken.h"

static_assert(!std::is_move_constructible_v<ruvia::StopRegistration>);
static_assert(!std::is_copy_constructible_v<ruvia::StopRegistration>);
static_assert(std::is_default_constructible_v<ruvia::StopSource>);
static_assert(!std::is_copy_constructible_v<ruvia::StopSource>);
static_assert(!std::is_move_constructible_v<ruvia::StopSource>);

namespace {

struct ResetRegistrationMoveState final {
    ruvia::StopSource* source;
    ruvia::StopRegistration* registration;
    std::atomic_int* moves;
    std::atomic_int* calls;
    int stopOnMove;
    std::atomic_bool* registrationPaused;
    std::atomic_bool* stopCompleted;
};

class ResetRegistrationOnMove final {
public:
    explicit ResetRegistrationOnMove(ResetRegistrationMoveState& state) noexcept
        : state_(&state) {}

    ResetRegistrationOnMove(const ResetRegistrationOnMove&) = delete;
    ResetRegistrationOnMove& operator=(const ResetRegistrationOnMove&) = delete;

    ResetRegistrationOnMove(ResetRegistrationOnMove&& other) noexcept
        : state_(other.state_) {
        const int move = state_->moves->fetch_add(1, std::memory_order_relaxed) + 1;
        if (move != state_->stopOnMove) {
            return;
        }
        if (state_->registrationPaused == nullptr) {
            state_->source->requestStop();
            return;
        }
        state_->registrationPaused->store(true, std::memory_order_release);
        while (!state_->stopCompleted->load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    }

    void operator()() const noexcept {
        state_->calls->fetch_add(1, std::memory_order_relaxed);
        state_->registration->reset();
    }

private:
    ResetRegistrationMoveState* state_;
};

// Moving an inline callable into StopCallbackState is the third move. Firing
// there puts stop_requested() after registerCallbacks()' preflight check but
// before the first std::stop_callback finishes construction.
constexpr int kMoveIntoCallbackState = 3;
static_assert(sizeof(ResetRegistrationOnMove) <= 3 * sizeof(void*));
static_assert(std::is_nothrow_move_constructible_v<ResetRegistrationOnMove>);

}  // namespace

RUVIA_TEST(stop_token_registration_runs_once) {
    ruvia::StopSource source;
    std::atomic_int calls{0};
    auto registration = source.token().registerCallback([&calls] { calls.fetch_add(1, std::memory_order_relaxed); });
    RUVIA_CHECK(registration.registered());
    source.requestStop();
    source.requestStop();
    RUVIA_CHECK_EQ(calls.load(std::memory_order_relaxed), 1);
}

RUVIA_TEST(stop_token_registration_can_be_reset) {
    ruvia::StopSource source;
    int calls = 0;
    auto registration = source.token().registerCallback([&calls] { ++calls; });
    registration.reset();
    source.requestStop();
    RUVIA_CHECK_EQ(calls, 0);
}

RUVIA_TEST(stop_token_registration_after_stop_runs_immediately) {
    ruvia::StopSource source;
    source.requestStop();
    int calls = 0;
    auto registration = source.token().registerCallback([&calls] { ++calls; });
    RUVIA_CHECK(!registration.registered());
    RUVIA_CHECK_EQ(calls, 1);
}

RUVIA_TEST(stop_token_registration_can_reuse_storage) {
    ruvia::StopSource source;
    int calls = 0;
    ruvia::StopRegistration registration;
    source.token().registerCallback(registration, [&calls] { ++calls; });
    RUVIA_CHECK(registration.registered());
    source.requestStop();
    RUVIA_CHECK_EQ(calls, 1);
}

RUVIA_TEST(stop_token_registration_can_reset_during_synchronous_construction_callback) {
    ruvia::StopSource first;
    ruvia::StopSource second;
    ruvia::StopRegistration registration;
    std::atomic_int moves{0};
    std::atomic_int calls{0};

    auto token = ruvia::combineStopTokens(first.token(), second.token());
    ResetRegistrationMoveState state{&first, &registration, &moves, &calls, kMoveIntoCallbackState, nullptr, nullptr};
    token.registerCallback(registration, ResetRegistrationOnMove(state));

    RUVIA_CHECK(first.stopRequested());
    RUVIA_CHECK(!registration.registered());
    RUVIA_CHECK_EQ(calls.load(std::memory_order_relaxed), 1);
    second.requestStop();
    RUVIA_CHECK_EQ(calls.load(std::memory_order_relaxed), 1);
}

RUVIA_TEST(stop_token_registration_can_reset_when_stop_races_with_callback_construction) {
    ruvia::StopSource first;
    ruvia::StopSource second;
    ruvia::StopRegistration registration;
    std::atomic_int moves{0};
    std::atomic_int calls{0};
    std::atomic_bool registrationPaused{false};
    std::atomic_bool stopCompleted{false};

    std::thread stopper([&] {
        while (!registrationPaused.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        first.requestStop();
        stopCompleted.store(true, std::memory_order_release);
    });

    auto token = ruvia::combineStopTokens(first.token(), second.token());
    ResetRegistrationMoveState state{&first, &registration, &moves, &calls, kMoveIntoCallbackState, &registrationPaused, &stopCompleted};
    token.registerCallback(registration, ResetRegistrationOnMove(state));
    stopper.join();

    RUVIA_CHECK(first.stopRequested());
    RUVIA_CHECK(!registration.registered());
    RUVIA_CHECK_EQ(calls.load(std::memory_order_relaxed), 1);
    second.requestStop();
    RUVIA_CHECK_EQ(calls.load(std::memory_order_relaxed), 1);
}

RUVIA_TEST(combined_stop_token_observes_either_source) {
    ruvia::StopSource first;
    ruvia::StopSource second;
    auto combined = ruvia::combineStopTokens(first.token(), second.token());
    RUVIA_CHECK(combined.stoppable());
    RUVIA_CHECK(!combined.stopRequested());
    second.requestStop();
    RUVIA_CHECK(combined.stopRequested());
}

RUVIA_TEST(combined_stop_registration_outlives_temporary_token) {
    ruvia::StopSource first;
    ruvia::StopSource second;
    int calls = 0;
    auto registration = ruvia::combineStopTokens(first.token(), second.token()).registerCallback([&calls] { ++calls; });
    RUVIA_CHECK(registration.registered());
    second.requestStop();
    first.requestStop();
    RUVIA_CHECK_EQ(calls, 1);
}

RUVIA_TEST(combined_stop_registration_reuses_storage_after_token_dies) {
    ruvia::StopSource first;
    ruvia::StopSource second;
    int calls = 0;
    ruvia::StopRegistration registration;
    ruvia::combineStopTokens(first.token(), second.token()).registerCallback(registration, [&calls] { ++calls; });
    RUVIA_CHECK(registration.registered());
    first.requestStop();
    second.requestStop();
    RUVIA_CHECK_EQ(calls, 1);
}

RUVIA_TEST(combined_stop_token_retains_nested_bridge) {
    ruvia::StopSource first;
    ruvia::StopSource second;
    ruvia::StopSource third;
    auto nested = ruvia::combineStopTokens(ruvia::combineStopTokens(first.token(), second.token()), third.token());
    first.requestStop();
    RUVIA_CHECK(nested.stopRequested());
}

RUVIA_TEST(combined_stop_token_handles_single_and_pre_stopped_inputs) {
    ruvia::StopSource source;
    auto single = ruvia::combineStopTokens({}, source.token());
    source.requestStop();
    RUVIA_CHECK(single.stopRequested());

    ruvia::StopSource stopped;
    ruvia::StopSource idle;
    stopped.requestStop();
    auto combined = ruvia::combineStopTokens(stopped.token(), idle.token());
    RUVIA_CHECK(combined.stopRequested());
}
