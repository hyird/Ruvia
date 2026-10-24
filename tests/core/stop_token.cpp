#include "ruvia/core/stop_token.h"

#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>

#include "test_harness.h"

namespace {

// Counts global-heap allocations made by this thread while armed.
thread_local bool global_allocations_armed = false;
thread_local std::size_t global_allocations = 0;

}  // namespace

void* operator new(std::size_t bytes) {
    if (global_allocations_armed) {
        ++global_allocations;
    }
    if (void* block = std::malloc(bytes == 0 ? 1 : bytes)) {
        return block;
    }
    throw std::bad_alloc();
}

void operator delete(void* block) noexcept {
    std::free(block);
}

void operator delete(void* block, std::size_t) noexcept {
    std::free(block);
}

namespace stop_token_test {

struct callback_retirement_state final {
    ruvia::stop_registration* registration_;
    const void* marked_source_{nullptr};
    bool mark_next_move_{false};
    bool reset_requested_{false};
    bool payload_valid_{false};
    int calls_{0};
    int source_destructions_{0};
    int payload_destructions_{0};
};

struct callback_payload final {
    explicit callback_payload(callback_retirement_state& state) noexcept
        : state_(state) {}
    ~callback_payload() {
        ++state_.payload_destructions_;
    }

    callback_retirement_state& state_;
    int value_{42};
};

class reset_registration_when_source_retires final {
public:
    explicit reset_registration_when_source_retires(callback_retirement_state& state)
        : state_(&state),
          payload_(std::make_unique<callback_payload>(state)) {}
    reset_registration_when_source_retires(const reset_registration_when_source_retires&) = delete;
    reset_registration_when_source_retires& operator=(const reset_registration_when_source_retires&) = delete;
    reset_registration_when_source_retires(reset_registration_when_source_retires&& other) noexcept
        : state_(other.state_),
          payload_(std::move(other.payload_)) {
        if (std::exchange(state_->mark_next_move_, false)) {
            state_->marked_source_ = std::addressof(other);
        }
    }
    ~reset_registration_when_source_retires() {
        auto* const state = state_;
        if (state->marked_source_ == this) {
            ++state->source_destructions_;
            request_reset(*state);
        }
    }

    void operator()() const noexcept {
        ++state_->calls_;
        state_->payload_valid_ = payload_ != nullptr && payload_->value_ == 42;
        // A wrapper may transfer an allocated callable without moving its
        // target; self-reset remains exercised for that ownership strategy.
        request_reset(*state_);
    }

private:
    static void request_reset(callback_retirement_state& state) noexcept {
        if (!std::exchange(state.reset_requested_, true)) {
            state.registration_->reset();
        }
    }

    callback_retirement_state* state_;
    std::unique_ptr<callback_payload> payload_;
};

}  // namespace stop_token_test

RUVIA_TEST(stop_registration_self_reset_retires_each_callback_target_once) {
    ruvia::stop_source source;
    ruvia::stop_registration registration;
    stop_token_test::callback_retirement_state state{&registration};
    source.token().register_callback(registration,
        stop_token_test::reset_registration_when_source_retires(state));
    state.mark_next_move_ = true;
    source.request_stop();
    const bool unregistered = !registration.registered();
    registration.reset();

    RUVIA_CHECK(unregistered);
    RUVIA_CHECK(state.reset_requested_);
    RUVIA_CHECK_EQ(state.calls_, 1);
    RUVIA_CHECK(state.payload_valid_);
    RUVIA_CHECK_EQ(state.source_destructions_, state.marked_source_ == nullptr ? 0 : 1);
    RUVIA_CHECK_EQ(state.payload_destructions_, 1);
}

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

RUVIA_TEST(combined_stop_token_flattens_nested_sources) {
    ruvia::stop_source first;
    ruvia::stop_source second;
    ruvia::stop_source third;
    auto nested = ruvia::combine_stop_tokens(
        ruvia::combine_stop_tokens(first.token(), second.token()), third.token());
    int calls = 0;
    auto registration = nested.register_callback([&calls] { ++calls; });
    third.request_stop();
    first.request_stop();
    RUVIA_CHECK(nested.stop_requested());
    RUVIA_CHECK_EQ(calls, 1);
}

RUVIA_TEST(combined_stop_token_merges_duplicates_and_rejects_overflow) {
    std::array<ruvia::stop_source, ruvia::max_stop_token_sources + 1> sources;
    auto combined = sources[0].token();
    for (std::size_t index = 1; index != ruvia::max_stop_token_sources; ++index) {
        combined = ruvia::combine_stop_tokens(combined, sources[index].token());
    }
    combined = ruvia::combine_stop_tokens(combined, sources[0].token());
    bool rejected = false;
    try {
        static_cast<void>(ruvia::combine_stop_tokens(combined, sources.back().token()));
    } catch (const std::length_error&) {
        rejected = true;
    }
    RUVIA_CHECK(rejected);
    sources[ruvia::max_stop_token_sources - 1].request_stop();
    RUVIA_CHECK(combined.stop_requested());
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

RUVIA_TEST(combined_stop_token_observes_later_sources_after_earlier_source_dies) {
    std::optional<ruvia::stop_source> first(std::in_place);
    ruvia::stop_source second;
    const auto combined = ruvia::combine_stop_tokens(first->token(), second.token());
    first.reset();
    RUVIA_CHECK(combined.stoppable());
    int calls = 0;
    auto registration = combined.register_callback([&calls] { ++calls; });
    RUVIA_CHECK(registration.registered());
    second.request_stop();
    RUVIA_CHECK(combined.stop_requested());
    RUVIA_CHECK_EQ(calls, 1);
}

RUVIA_TEST(stop_token_combination_and_registration_never_allocate) {
    std::array<ruvia::stop_source, 16> sources;
    global_allocations = 0;
    global_allocations_armed = true;
    for (auto& source : sources) {
        auto token = source.token();
        int calls = 0;
        auto registration = token.register_callback([&calls] { ++calls; });
        source.request_stop();
        if (calls != 1 || !token.stop_requested()) {
            global_allocations_armed = false;
            RUVIA_CHECK(false);
        }
    }
    global_allocations_armed = false;
    RUVIA_CHECK_EQ(global_allocations, std::size_t{0});
}

RUVIA_TEST(stop_token_outlives_its_source_across_threads) {
    for (const bool request_stop : {false, true}) {
        std::optional<ruvia::stop_source> source;
        std::thread([&] { source.emplace(); }).join();
        auto token = source->token();
        int calls = 0;
        auto registration = token.register_callback([&calls] { ++calls; });
        if (request_stop) {
            source->request_stop();
        }
        source.reset();
        RUVIA_CHECK_EQ(token.stop_requested(), request_stop);
        RUVIA_CHECK_EQ(token.stoppable(), request_stop);
        RUVIA_CHECK_EQ(calls, request_stop ? 1 : 0);

        bool retained_state_valid = false;
        std::thread([retained = std::move(token), &registration, &retained_state_valid, request_stop]() mutable {
            retained_state_valid = retained.stop_requested() == request_stop &&
                                   retained.stoppable() == request_stop;
            retained = {};
            // The registration now owns the last reference, on a thread other
            // than the already-exited thread that created the source.
            registration.reset();
        }).join();
        RUVIA_CHECK(retained_state_valid);
        RUVIA_CHECK(!registration.registered());
        RUVIA_CHECK_EQ(calls, request_stop ? 1 : 0);
    }
}

RUVIA_TEST(stop_registration_reset_waits_for_callback_running_on_another_thread) {
    ruvia::stop_source source;
    std::atomic_bool entered{false};
    std::atomic_bool released{false};
    std::atomic_bool returned{false};
    auto registration = source.token().register_callback([&] {
        entered.store(true, std::memory_order_release);
        while (!released.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        returned.store(true, std::memory_order_release);
    });
    std::thread stopper([&source] { source.request_stop(); });
    while (!entered.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    std::thread releaser([&released] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        released.store(true, std::memory_order_release);
    });
    registration.reset();
    const bool waited = returned.load(std::memory_order_acquire);
    releaser.join();
    stopper.join();
    RUVIA_CHECK(waited);
}

RUVIA_TEST(stop_registration_racing_with_stop_invokes_each_callback_once) {
    constexpr std::size_t rounds = 256;
    std::array<ruvia::stop_source, rounds> sources;
    std::array<std::atomic_int, rounds> calls{};
    std::barrier gate(2);
    std::thread stopper([&] {
        for (std::size_t index = 0; index < rounds; ++index) {
            gate.arrive_and_wait();
            sources[index].request_stop();
            gate.arrive_and_wait();
        }
    });

    for (std::size_t index = 0; index < rounds; ++index) {
        gate.arrive_and_wait();
        auto registration = sources[index].token().register_callback(
            [&, index] { calls[index].fetch_add(1, std::memory_order_relaxed); });
        gate.arrive_and_wait();
        registration.reset();
        sources[index].request_stop();
        RUVIA_CHECK_EQ(calls[index].load(std::memory_order_relaxed), 1);
    }
    stopper.join();
}

RUVIA_TEST(stop_token_registration_synchronous_race_leaves_registration_idle_and_reusable) {
    ruvia::stop_source first;
    ruvia::stop_source second;
    ruvia::stop_source third;
    ruvia::stop_registration registration;
    std::atomic_int moves{0};
    std::atomic_int calls{0};

    struct state_type final {
        ruvia::stop_source* source_{};
        ruvia::stop_source* reuse_source_{};
        ruvia::stop_registration* registration_{};
        std::atomic_int* moves_{};
        std::atomic_int* calls_{};
    };
    struct stop_on_move_without_reset final {
        state_type* state_{};

        explicit stop_on_move_without_reset(state_type& state_value) noexcept
            : state_(&state_value) {}
        stop_on_move_without_reset(const stop_on_move_without_reset&) = delete;
        stop_on_move_without_reset& operator=(const stop_on_move_without_reset&) = delete;
        stop_on_move_without_reset(stop_on_move_without_reset&& other) noexcept
            : state_(other.state_) {
            if (state_->moves_->fetch_add(1, std::memory_order_relaxed) + 1 == move_into_callback_state) {
                state_->source_->request_stop();
            }
        }
        stop_on_move_without_reset& operator=(stop_on_move_without_reset&&) = delete;

        void operator()() const noexcept {
            state_->calls_->fetch_add(1, std::memory_order_relaxed);
            if (state_->reuse_source_ != nullptr) {
                auto* calls_ptr = state_->calls_;
                state_->reuse_source_->token().register_callback(
                    *state_->registration_, [calls_ptr] { calls_ptr->fetch_add(10, std::memory_order_relaxed); });
            }
        }
    };

    auto token = ruvia::combine_stop_tokens(first.token(), second.token());
    state_type first_state{&first, nullptr, &registration, &moves, &calls};
    token.register_callback(registration, stop_on_move_without_reset(first_state));
    RUVIA_CHECK(!registration.registered());
    RUVIA_CHECK_EQ(calls.load(std::memory_order_relaxed), 1);

    ruvia::stop_source fourth;
    moves.store(0, std::memory_order_relaxed);
    auto second_token = ruvia::combine_stop_tokens(fourth.token(), second.token());
    state_type second_state{&fourth, &third, &registration, &moves, &calls};
    second_token.register_callback(registration, stop_on_move_without_reset(second_state));
    RUVIA_CHECK(registration.registered());
    RUVIA_CHECK_EQ(calls.load(std::memory_order_relaxed), 2);
    third.request_stop();
    RUVIA_CHECK_EQ(calls.load(std::memory_order_relaxed), 12);
}

RUVIA_TEST(stop_token_registration_reset_pending_before_stop_already_requested_suppresses_callback) {
    ruvia::stop_source first;
    ruvia::stop_source second;
    ruvia::stop_registration registration;
    std::atomic_int moves{0};
    std::atomic_int calls{0};

    struct state_type final {
        ruvia::stop_source* source_{};
        ruvia::stop_registration* registration_{};
        std::atomic_int* moves_{};
        std::atomic_int* calls_{};
    } state{
        .source_ = &first,
        .registration_ = &registration,
        .moves_ = &moves,
        .calls_ = &calls,
    };

    struct reset_then_stop_on_move final {
        state_type* state_{};

        explicit reset_then_stop_on_move(state_type& state_value) noexcept
            : state_(&state_value) {}
        reset_then_stop_on_move(const reset_then_stop_on_move&) = delete;
        reset_then_stop_on_move& operator=(const reset_then_stop_on_move&) = delete;
        reset_then_stop_on_move(reset_then_stop_on_move&& other) noexcept
            : state_(other.state_) {
            if (state_->moves_->fetch_add(1, std::memory_order_relaxed) + 1 == move_into_callback_state) {
                state_->registration_->reset();
                state_->source_->request_stop();
            }
        }
        reset_then_stop_on_move& operator=(reset_then_stop_on_move&&) = delete;

        void operator()() const noexcept {
            state_->calls_->fetch_add(1, std::memory_order_relaxed);
        }
    };

    auto token = ruvia::combine_stop_tokens(first.token(), second.token());
    token.register_callback(registration, reset_then_stop_on_move(state));
    RUVIA_CHECK(first.stop_requested());
    RUVIA_CHECK(!registration.registered());
    RUVIA_CHECK_EQ(calls.load(std::memory_order_relaxed), 0);
    second.request_stop();
    RUVIA_CHECK_EQ(calls.load(std::memory_order_relaxed), 0);
}
