#include "ruvia/core/stop_token.h"

#include <atomic>
#include <thread>
#include <type_traits>

#include "test_harness.h"

namespace {

struct reset_registration_move_state final {
    ruvia::stop_source* source_;
    ruvia::stop_registration* registration_;
    std::atomic_int* moves_;
    std::atomic_int* calls_;
    int stop_on_move_;
    std::atomic_bool* registration_paused_;
    std::atomic_bool* stop_completed_;
};

class reset_registration_on_move final {
public:
    explicit reset_registration_on_move(reset_registration_move_state& state_value) noexcept
        : state_(&state_value) {}

    reset_registration_on_move(const reset_registration_on_move&) = delete;
    reset_registration_on_move& operator=(const reset_registration_on_move&) = delete;

    reset_registration_on_move(reset_registration_on_move&& other) noexcept
        : state_(other.state_) {
        const int move = state_->moves_->fetch_add(1, std::memory_order_relaxed) + 1;
        if (move != state_->stop_on_move_) {
            return;
        }
        if (state_->registration_paused_ == nullptr) {
            state_->source_->request_stop();
            return;
        }
        state_->registration_paused_->store(true, std::memory_order_release);
        while (!state_->stop_completed_->load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    }

    void operator()() const noexcept {
        state_->calls_->fetch_add(1, std::memory_order_relaxed);
        state_->registration_->reset();
    }

private:
    reset_registration_move_state* state_;
};

// Moving an inline callable into stop_callback_state is the third move. Firing
// there puts stop_requested() after register_callbacks()' preflight check but
// before the first std::stop_callback finishes construction.
constexpr int move_into_callback_state = 3;

}  // namespace

RUVIA_TEST(stop_token_registration_runs_once) {
    ruvia::stop_source source;
    std::atomic_int calls{0};
    auto registration = source.token().register_callback(
        [&calls] { calls.fetch_add(1, std::memory_order_relaxed); });
    RUVIA_CHECK(registration.registered());
    source.request_stop();
    source.request_stop();
    RUVIA_CHECK_EQ(calls.load(std::memory_order_relaxed), 1);
}

RUVIA_TEST(stop_token_registration_can_be_reset) {
    ruvia::stop_source source;
    int calls = 0;
    auto registration = source.token().register_callback([&calls] { ++calls; });
    registration.reset();
    source.request_stop();
    RUVIA_CHECK_EQ(calls, 0);
}

RUVIA_TEST(stop_token_registration_after_stop_runs_immediately) {
    ruvia::stop_source source;
    source.request_stop();
    int calls = 0;
    auto registration = source.token().register_callback([&calls] { ++calls; });
    RUVIA_CHECK(!registration.registered());
    RUVIA_CHECK_EQ(calls, 1);
}

RUVIA_TEST(stop_token_registration_can_reuse_storage) {
    ruvia::stop_source source;
    int calls = 0;
    ruvia::stop_registration registration;
    source.token().register_callback(registration, [&calls] { ++calls; });
    RUVIA_CHECK(registration.registered());
    source.request_stop();
    RUVIA_CHECK_EQ(calls, 1);
}

RUVIA_TEST(stop_token_registration_can_reset_during_synchronous_construction_callback) {
    ruvia::stop_source first;
    ruvia::stop_source second;
    ruvia::stop_registration registration;
    std::atomic_int moves{0};
    std::atomic_int calls{0};

    auto token = ruvia::combine_stop_tokens(first.token(), second.token());
    reset_registration_move_state state_value{
        &first, &registration, &moves, &calls, move_into_callback_state, nullptr, nullptr};
    token.register_callback(registration, reset_registration_on_move(state_value));

    RUVIA_CHECK(first.stop_requested());
    RUVIA_CHECK(!registration.registered());
    RUVIA_CHECK_EQ(calls.load(std::memory_order_relaxed), 1);
    second.request_stop();
    RUVIA_CHECK_EQ(calls.load(std::memory_order_relaxed), 1);
}

RUVIA_TEST(stop_token_registration_can_reset_when_stop_races_with_callback_construction) {
    ruvia::stop_source first;
    ruvia::stop_source second;
    ruvia::stop_registration registration;
    std::atomic_int moves{0};
    std::atomic_int calls{0};
    std::atomic_bool registration_paused{false};
    std::atomic_bool stop_completed{false};

    std::thread stopper([&] {
        while (!registration_paused.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        first.request_stop();
        stop_completed.store(true, std::memory_order_release);
    });

    auto token = ruvia::combine_stop_tokens(first.token(), second.token());
    reset_registration_move_state state_value{&first, &registration, &moves, &calls, move_into_callback_state,
        &registration_paused, &stop_completed};
    token.register_callback(registration, reset_registration_on_move(state_value));
    stopper.join();

    RUVIA_CHECK(first.stop_requested());
    RUVIA_CHECK(!registration.registered());
    RUVIA_CHECK_EQ(calls.load(std::memory_order_relaxed), 1);
    second.request_stop();
    RUVIA_CHECK_EQ(calls.load(std::memory_order_relaxed), 1);
}

RUVIA_TEST(combined_stop_token_observes_either_source) {
    ruvia::stop_source first;
    ruvia::stop_source second;
    auto combined = ruvia::combine_stop_tokens(first.token(), second.token());
    RUVIA_CHECK(combined.stoppable());
    RUVIA_CHECK(!combined.stop_requested());
    second.request_stop();
    RUVIA_CHECK(combined.stop_requested());
}

RUVIA_TEST(combined_stop_registration_outlives_temporary_token) {
    ruvia::stop_source first;
    ruvia::stop_source second;
    int calls = 0;
    auto registration =
        ruvia::combine_stop_tokens(first.token(), second.token()).register_callback([&calls] {
            ++calls;
        });
    RUVIA_CHECK(registration.registered());
    second.request_stop();
    first.request_stop();
    RUVIA_CHECK_EQ(calls, 1);
}

RUVIA_TEST(combined_stop_registration_reuses_storage_after_token_dies) {
    ruvia::stop_source first;
    ruvia::stop_source second;
    int calls = 0;
    ruvia::stop_registration registration;
    ruvia::combine_stop_tokens(first.token(), second.token())
        .register_callback(registration, [&calls] { ++calls; });
    RUVIA_CHECK(registration.registered());
    first.request_stop();
    second.request_stop();
    RUVIA_CHECK_EQ(calls, 1);
}

RUVIA_TEST(combined_stop_token_retains_nested_bridge) {
    ruvia::stop_source first;
    ruvia::stop_source second;
    ruvia::stop_source third;
    auto nested = ruvia::combine_stop_tokens(
        ruvia::combine_stop_tokens(first.token(), second.token()), third.token());
    first.request_stop();
    RUVIA_CHECK(nested.stop_requested());
}

RUVIA_TEST(combined_stop_token_handles_single_and_pre_stopped_inputs) {
    ruvia::stop_source source;
    auto single = ruvia::combine_stop_tokens({}, source.token());
    source.request_stop();
    RUVIA_CHECK(single.stop_requested());

    ruvia::stop_source stopped;
    ruvia::stop_source idle;
    stopped.request_stop();
    auto combined = ruvia::combine_stop_tokens(stopped.token(), idle.token());
    RUVIA_CHECK(combined.stop_requested());
}
