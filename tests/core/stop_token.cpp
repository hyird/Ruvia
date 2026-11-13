#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <concepts>
#include <condition_variable>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <asio/bind_allocator.hpp>
#include <asio/bind_executor.hpp>
#include <asio/co_spawn.hpp>
#include <asio/error.hpp>
#include <asio/io_context.hpp>
#include <asio/post.hpp>
#include <asio/steady_timer.hpp>

#include "ruvia/core/event_loop.h"
#include "ruvia/core/task.h"
#include "ruvia/core/worker_ref.h"
#include "ruvia/core/worker_wait.h"

#include "worker_task_fixture.h"

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

TEST(Ruvia, stop_registration_self_reset_retires_each_callback_target_once) {
    ruvia::stop_source source;
    ruvia::move_only_function<void()> empty_fn;
    EXPECT_TRUE(empty_fn == nullptr);
    EXPECT_TRUE(nullptr == empty_fn);
    ruvia::move_only_function<void()> valid_fn{[] {}};
    EXPECT_TRUE(valid_fn != nullptr);
    EXPECT_TRUE(nullptr != valid_fn);
    ruvia::stop_registration registration;
    stop_token_test::callback_retirement_state state{&registration};
    source.token().register_callback(registration,
        stop_token_test::reset_registration_when_source_retires(state));
    state.mark_next_move_ = true;
    source.request_stop();
    const bool unregistered = !registration.registered();
    registration.reset();

    EXPECT_TRUE(unregistered);
    EXPECT_TRUE(state.reset_requested_);
    EXPECT_EQ(state.calls_, 1);
    EXPECT_TRUE(state.payload_valid_);
    EXPECT_EQ(state.source_destructions_, state.marked_source_ == nullptr ? 0 : 1);
    EXPECT_EQ(state.payload_destructions_, 1);
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
// there puts stop_requested() after register_callback()'s preflight check but
// before the intrusive node finishes construction.
constexpr int move_into_callback_state = 3;

}  // namespace

TEST(Ruvia, stop_token_registration_runs_once) {
    ruvia::stop_source source;
    std::atomic_int calls{0};
    ruvia::stop_registration registration;
    source.token().register_callback(
        registration, [&calls] { calls.fetch_add(1, std::memory_order_relaxed); });
    EXPECT_TRUE(registration.registered());
    source.request_stop();
    source.request_stop();
    EXPECT_EQ(calls.load(std::memory_order_relaxed), 1);
}

TEST(Ruvia, stop_token_registration_can_be_reset) {
    ruvia::stop_source source;
    int calls = 0;
    ruvia::stop_registration registration;
    source.token().register_callback(registration, [&calls] { ++calls; });
    registration.reset();
    source.request_stop();
    EXPECT_EQ(calls, 0);
}

TEST(Ruvia, stop_token_registration_after_stop_runs_immediately) {
    ruvia::stop_source source;
    source.request_stop();
    int calls = 0;
    ruvia::stop_registration registration;
    source.token().register_callback(registration, [&calls] { ++calls; });
    EXPECT_TRUE(!registration.registered());
    EXPECT_EQ(calls, 1);
}

TEST(Ruvia, stop_token_registration_reuse_replaces_previous_callback) {
    ruvia::stop_source source;
    int first_calls = 0;
    int second_calls = 0;
    ruvia::stop_registration registration;
    source.token().register_callback(registration, [&first_calls] { ++first_calls; });
    source.token().register_callback(registration, [&second_calls] { ++second_calls; });
    EXPECT_TRUE(registration.registered());
    source.request_stop();
    EXPECT_EQ(first_calls, 0);
    EXPECT_EQ(second_calls, 1);
}

TEST(Ruvia, stop_token_replacement_preserves_entry_state_when_retirement_clears_token) {
    struct clear_token final {
        clear_token(ruvia::stop_token& token, ruvia::stop_source* source) noexcept
            : token_(token),
              source_(source) {}
        ~clear_token() {
            token_ = {};
            if (source_ != nullptr) {
                source_->request_stop();
            }
        }
        ruvia::stop_token& token_;
        ruvia::stop_source* source_;
    };
    for (const bool stop_during_retirement : {false, true}) {
        int calls = 0;
        ruvia::stop_source source;
        auto token = source.token();
        ruvia::stop_registration registration;
        auto payload = std::make_unique<clear_token>(token, stop_during_retirement ? &source : nullptr);
        token.register_callback(registration, [owned = std::move(payload)] {});

        token.register_callback(registration, [&calls] { ++calls; });
        EXPECT_TRUE(!token.has_state());
        EXPECT_EQ(registration.registered(), !stop_during_retirement);
        EXPECT_EQ(calls, static_cast<int>(stop_during_retirement));
        source.request_stop();
        source.request_stop();
        EXPECT_EQ(calls, 1);
    }
}

TEST(Ruvia, stop_token_replacement_survives_token_destruction) {
    struct destroy_token final {
        explicit destroy_token(std::unique_ptr<ruvia::stop_token>& owner) noexcept
            : owner_(owner) {}
        ~destroy_token() {
            owner_.reset();
        }
        std::unique_ptr<ruvia::stop_token>& owner_;
    };
    int calls = 0;
    ruvia::stop_source source;
    auto token = std::make_unique<ruvia::stop_token>(source.token());
    ruvia::stop_registration registration;
    auto payload = std::make_unique<destroy_token>(token);
    token->register_callback(registration, [owned = std::move(payload)] {});

    token->register_callback(registration, [&calls] { ++calls; });
    EXPECT_TRUE(!token);
    EXPECT_TRUE(registration.registered());
    source.request_stop();
    source.request_stop();
    EXPECT_EQ(calls, 1);
}

TEST(Ruvia, stop_token_registration_can_reset_during_synchronous_construction_callback) {
    ruvia::stop_source first;
    ruvia::stop_source second;
    ruvia::stop_registration registration;
    std::atomic_int moves{0};
    std::atomic_int calls{0};

    ruvia::stop_group group{first.token(), second.token()};
    auto token = group.token();
    reset_registration_move_state state_value{
        &first, &registration, &moves, &calls, move_into_callback_state, nullptr, nullptr};
    token.register_callback(registration, reset_registration_on_move(state_value));

    EXPECT_TRUE(first.stop_requested());
    EXPECT_TRUE(!registration.registered());
    EXPECT_EQ(calls.load(std::memory_order_relaxed), 1);
    second.request_stop();
    EXPECT_EQ(calls.load(std::memory_order_relaxed), 1);
}

TEST(Ruvia, stop_token_registration_can_reset_when_stop_races_with_callback_construction) {
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

    ruvia::stop_group group{first.token(), second.token()};
    auto token = group.token();
    reset_registration_move_state state_value{&first, &registration, &moves, &calls, move_into_callback_state,
        &registration_paused, &stop_completed};
    token.register_callback(registration, reset_registration_on_move(state_value));
    stopper.join();

    EXPECT_TRUE(first.stop_requested());
    EXPECT_TRUE(!registration.registered());
    EXPECT_EQ(calls.load(std::memory_order_relaxed), 1);
    second.request_stop();
    EXPECT_EQ(calls.load(std::memory_order_relaxed), 1);
}

TEST(Ruvia, combined_stop_token_observes_either_source) {
    ruvia::stop_source first;
    ruvia::stop_source second;
    ruvia::stop_group group{first.token(), second.token()};
    auto combined = group.token();
    EXPECT_TRUE(combined.stoppable());
    EXPECT_TRUE(!combined.stop_requested());
    second.request_stop();
    EXPECT_TRUE(combined.stop_requested());
}

TEST(Ruvia, combined_stop_registration_outlives_temporary_token) {
    ruvia::stop_source first;
    ruvia::stop_source second;
    int calls = 0;
    ruvia::stop_group group{first.token(), second.token()};
    auto registration = group.token().register_callback([&calls] { ++calls; });
    EXPECT_TRUE(registration.registered());
    second.request_stop();
    first.request_stop();
    EXPECT_EQ(calls, 1);
}

TEST(Ruvia, combined_stop_registration_reuses_storage_after_token_dies) {
    ruvia::stop_source first;
    ruvia::stop_source second;
    int calls = 0;
    ruvia::stop_registration registration;
    ruvia::stop_group group{first.token(), second.token()};
    group.token().register_callback(registration, [&calls] { ++calls; });
    EXPECT_TRUE(registration.registered());
    first.request_stop();
    second.request_stop();
    EXPECT_EQ(calls, 1);
}

TEST(Ruvia, stop_group_propagates_nested_sources) {
    ruvia::stop_source first;
    ruvia::stop_source second;
    ruvia::stop_source third;
    ruvia::stop_group inner{first.token(), second.token()};
    ruvia::stop_group outer{inner.token(), third.token()};
    auto nested = outer.token();
    int calls = 0;
    ruvia::stop_registration registration;
    nested.register_callback(registration, [&calls] { ++calls; });
    third.request_stop();
    first.request_stop();
    EXPECT_TRUE(nested.stop_requested());
    EXPECT_EQ(calls, 1);
}

TEST(Ruvia, stop_group_supports_many_parents_and_merges_duplicate_notifications) {
    for (std::size_t selected = 0; selected != 8; ++selected) {
        std::array<ruvia::stop_source, 8> sources;
        std::array<ruvia::stop_token, 9> tokens;
        for (std::size_t index = 0; index != sources.size(); ++index) {
            tokens[index] = sources[index].token();
        }
        tokens.back() = tokens.front();
        ruvia::stop_group group{tokens};
        int calls = 0;
        auto registration = group.token().register_callback([&calls] { ++calls; });
        sources[selected].request_stop();
        EXPECT_EQ(calls, 1);
        for (auto& source : sources) {
            source.request_stop();
        }
        EXPECT_EQ(calls, 1);
    }
}

TEST(Ruvia, combined_stop_token_handles_single_and_pre_stopped_inputs) {
    ruvia::stop_source source;
    ruvia::stop_group single_group{{}, source.token(), source.token()};
    auto single = single_group.token();
    EXPECT_TRUE(single == source.token());
    source.request_stop();
    EXPECT_TRUE(single.stop_requested());

    ruvia::stop_source stopped;
    ruvia::stop_source idle;
    stopped.request_stop();
    ruvia::stop_group group{stopped.token(), idle.token()};
    auto combined = group.token();
    EXPECT_TRUE(combined == stopped.token());
    EXPECT_TRUE(combined.stop_requested());
}

TEST(Ruvia, combined_stop_token_observes_later_sources_after_earlier_source_dies) {
    std::optional<ruvia::stop_source> first(std::in_place);
    ruvia::stop_source second;
    const ruvia::stop_group group{first->token(), second.token()};
    const auto combined = group.token();
    first.reset();
    EXPECT_TRUE(combined.stoppable());
    int calls = 0;
    ruvia::stop_registration registration;
    combined.register_callback(registration, [&calls] { ++calls; });
    EXPECT_TRUE(registration.registered());
    second.request_stop();
    EXPECT_TRUE(combined.stop_requested());
    EXPECT_EQ(calls, 1);
}

TEST(Ruvia, stop_token_outlives_its_source_across_threads) {
    for (const bool request_stop : {false, true}) {
        std::optional<ruvia::stop_source> source;
        std::thread([&] { source.emplace(); }).join();
        auto token = source->token();
        int calls = 0;
        ruvia::stop_registration registration;
        token.register_callback(registration, [&calls] { ++calls; });
        if (request_stop) {
            source->request_stop();
        }
        source.reset();
        EXPECT_EQ(token.stop_requested(), request_stop);
        EXPECT_EQ(token.stoppable(), request_stop);
        EXPECT_EQ(calls, request_stop ? 1 : 0);

        bool retained_state_valid = false;
        std::thread([retained = std::move(token), &registration, &retained_state_valid, request_stop]() mutable {
            retained_state_valid = retained.stop_requested() == request_stop &&
                                   retained.stoppable() == request_stop;
            retained = {};
            // The registration now owns the last reference, on a thread other
            // than the already-exited thread that created the source.
            registration.reset();
        }).join();
        EXPECT_TRUE(retained_state_valid);
        EXPECT_TRUE(!registration.registered());
        EXPECT_EQ(calls, request_stop ? 1 : 0);
    }
}

TEST(Ruvia, stop_registration_reset_waits_for_callback_running_on_another_thread) {
    ruvia::stop_source source;
    std::atomic_bool entered{false};
    std::atomic_bool released{false};
    std::atomic_bool returned{false};
    ruvia::stop_registration registration;
    source.token().register_callback(registration, [&] {
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
    EXPECT_TRUE(waited);
}

TEST(Ruvia, stop_registration_racing_with_stop_invokes_each_callback_once) {
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
        ruvia::stop_registration registration;
        sources[index].token().register_callback(
            registration, [&, index] { calls[index].fetch_add(1, std::memory_order_relaxed); });
        gate.arrive_and_wait();
        registration.reset();
        sources[index].request_stop();
        EXPECT_EQ(calls[index].load(std::memory_order_relaxed), 1);
    }
    stopper.join();
}

TEST(Ruvia, stop_token_registration_synchronous_race_leaves_registration_idle_and_reusable) {
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

    ruvia::stop_group first_group{first.token(), second.token()};
    auto token = first_group.token();
    state_type first_state{&first, nullptr, &registration, &moves, &calls};
    token.register_callback(registration, stop_on_move_without_reset(first_state));
    EXPECT_TRUE(!registration.registered());
    EXPECT_EQ(calls.load(std::memory_order_relaxed), 1);

    ruvia::stop_source fourth;
    moves.store(0, std::memory_order_relaxed);
    ruvia::stop_group second_group{fourth.token(), second.token()};
    auto second_token = second_group.token();
    state_type second_state{&fourth, &third, &registration, &moves, &calls};
    second_token.register_callback(registration, stop_on_move_without_reset(second_state));
    EXPECT_TRUE(registration.registered());
    EXPECT_EQ(calls.load(std::memory_order_relaxed), 2);
    third.request_stop();
    EXPECT_EQ(calls.load(std::memory_order_relaxed), 12);
}

TEST(Ruvia, stop_token_registration_reset_pending_before_stop_already_requested_suppresses_callback) {
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

    ruvia::stop_group group{first.token(), second.token()};
    auto token = group.token();
    token.register_callback(registration, reset_then_stop_on_move(state));
    EXPECT_TRUE(first.stop_requested());
    EXPECT_TRUE(!registration.registered());
    EXPECT_EQ(calls.load(std::memory_order_relaxed), 0);
    second.request_stop();
    EXPECT_EQ(calls.load(std::memory_order_relaxed), 0);
}

TEST(Ruvia, stop_group_can_be_destroyed_from_its_downstream_callback) {
    ruvia::stop_source first;
    ruvia::stop_source second;
    auto group = std::make_unique<ruvia::stop_group>(first.token(), second.token());
    auto token = group->token();
    int calls = 0;
    ruvia::stop_registration registration;
    token.register_callback(registration, [&] {
        ++calls;
        registration.reset();
        group.reset();
        second.request_stop();
    });
    first.request_stop();
    EXPECT_TRUE(!group);
    EXPECT_TRUE(token.stop_requested());
    EXPECT_EQ(calls, 1);
}

TEST(Ruvia, stop_group_destruction_waits_for_cross_thread_downstream_callback) {
    ruvia::stop_source first;
    ruvia::stop_source second;
    auto group = std::make_unique<ruvia::stop_group>(first.token(), second.token());
    std::atomic_bool entered{false};
    std::atomic_bool released{false};
    std::atomic_bool returned{false};
    auto registration = group->token().register_callback([&] {
        entered.store(true, std::memory_order_release);
        while (!released.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        returned.store(true, std::memory_order_release);
    });
    std::thread stopper([&] { first.request_stop(); });
    while (!entered.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    std::thread releaser([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        released.store(true, std::memory_order_release);
    });
    group.reset();
    const bool waited = returned.load(std::memory_order_acquire);
    releaser.join();
    stopper.join();
    EXPECT_TRUE(waited);
}

TEST(Ruvia, operation_options_replace_cancellation_and_keep_tighter_timeout) {
    ruvia::stop_source base;
    ruvia::stop_source replacement;
    const ruvia::operation_options original{
        .timeout_ = std::chrono::milliseconds(20), .stop_token_ = base.token()};
    auto inherited = ruvia::override_operation_options(original, {});
    auto replaced = ruvia::override_operation_options(inherited,
        {.timeout_ = std::chrono::milliseconds(50), .stop_token_ = replacement.token()});
    base.request_stop();
    EXPECT_TRUE(inherited.stop_token_.stop_requested());
    EXPECT_TRUE(!replaced.stop_token_.stop_requested());
    EXPECT_TRUE(replaced.timeout_ == ruvia::operation_timeout(std::chrono::milliseconds(20)));
    replacement.request_stop();
    EXPECT_TRUE(replaced.stop_token_.stop_requested());

    ruvia::stop_token retired;
    {
        ruvia::stop_source source;
        retired = source.token();
    }
    auto disabled = ruvia::override_operation_options(original, {.stop_token_ = retired});
    EXPECT_TRUE(disabled.stop_token_ == retired);
    EXPECT_TRUE(!disabled.stop_token_.stoppable());
}

TEST(Ruvia, equivalent_failures_compare_type_and_message) {
    const auto first = std::make_exception_ptr(std::runtime_error("redis connect timed out"));
    const auto same = std::make_exception_ptr(std::runtime_error("redis connect timed out"));
    // Same length, different bytes: a dangling what() comparison could see the
    // same freed storage for both and call them equal.
    const auto other = std::make_exception_ptr(std::runtime_error("redis connect timed in!"));
    const auto other_type = std::make_exception_ptr(std::logic_error("redis connect timed out"));
    const auto non_standard = std::make_exception_ptr(42);

    EXPECT_TRUE(ruvia::equivalent_failures(first, first));
    EXPECT_TRUE(ruvia::equivalent_failures(first, same));
    EXPECT_TRUE(!ruvia::equivalent_failures(first, other));
    EXPECT_TRUE(!ruvia::equivalent_failures(other, first));
    EXPECT_TRUE(!ruvia::equivalent_failures(first, other_type));
    EXPECT_TRUE(!ruvia::equivalent_failures(first, non_standard));
    EXPECT_TRUE(!ruvia::equivalent_failures(first, nullptr));
    EXPECT_TRUE(ruvia::equivalent_failures(non_standard, non_standard));
}

TEST(Ruvia, equivalent_failures_distinguish_long_messages) {
    // Messages too long for any small-buffer storage, differing only at the end.
    const std::string prefix(512, 'x');
    const auto left = std::make_exception_ptr(std::runtime_error(prefix + "a"));
    const auto right = std::make_exception_ptr(std::runtime_error(prefix + "b"));
    EXPECT_TRUE(!ruvia::equivalent_failures(left, right));
    EXPECT_TRUE(ruvia::equivalent_failures(left, std::make_exception_ptr(std::runtime_error(prefix + "a"))));
}

TEST(Ruvia, byte_views_preserve_octets_including_embedded_nul) {
    const char raw[] = {'a', '\0', '\xff'};
    const auto text = std::string_view(raw, sizeof(raw));
    const auto bytes_value = ruvia::as_bytes(text);
    EXPECT_EQ(bytes_value.size(), std::size_t{3});
    EXPECT_TRUE(bytes_value[0] == std::byte{'a'});
    EXPECT_TRUE(bytes_value[1] == std::byte{0});
    EXPECT_TRUE(bytes_value[2] == std::byte{0xff});
    EXPECT_TRUE(ruvia::as_chars(bytes_value).data() == text.data());
    EXPECT_EQ(ruvia::as_chars(bytes_value).size(), text.size());
}

TEST(Ruvia, empty_byte_views_round_trip) {
    EXPECT_TRUE(ruvia::as_bytes({}).empty());
    EXPECT_TRUE(ruvia::as_chars({}).empty());
}

TEST(Ruvia, byte_views_accept_named_owners_and_preserve_storage_addresses) {
    const std::string owned_text("abc", 3);
    const std::vector<std::byte> owned_bytes{std::byte{'x'}, std::byte{0}, std::byte{'z'}};
    const auto text_bytes = ruvia::as_bytes(owned_text);
    const auto chars_value = ruvia::as_chars(owned_bytes);
    EXPECT_TRUE(reinterpret_cast<const char*>(text_bytes.data()) == owned_text.data());
    EXPECT_TRUE(chars_value.data() == reinterpret_cast<const char*>(owned_bytes.data()));
    EXPECT_EQ(chars_value, std::string_view("x\0z", 3));
}

TEST(Ruvia, ip_address_parsing_handles_ipv4_ipv6_and_view_bounds) {
    const auto v4 = ruvia::parse_ip_address("192.0.2.1");
    EXPECT_TRUE(v4.has_value() && v4->is_v4());
    const auto v6 = ruvia::parse_ip_address("2001:0db8:1234:5678:90ab:cdef:1234:5678");
    EXPECT_TRUE(v6.has_value() && v6->is_v6());
    const auto scoped = ruvia::parse_ip_address("fe80::1%12");
    EXPECT_TRUE(scoped.has_value() && scoped->is_v6() && scoped->to_v6().scope_id() == 12);
    constexpr std::string_view storage = "192.0.2.1suffix";
    EXPECT_TRUE(ruvia::parse_ip_address(storage.substr(0, 9)).has_value());
    EXPECT_TRUE(!ruvia::parse_ip_address(storage).has_value());
}

TEST(Ruvia, ip_address_parsing_rejects_malformed_and_embedded_nul_input) {
    EXPECT_TRUE(!ruvia::parse_ip_address(std::string_view{}).has_value());
    EXPECT_TRUE(!ruvia::parse_ip_address("not an address").has_value());
    EXPECT_TRUE(!ruvia::parse_ip_address(std::string_view("192.0.2.1\0suffix", 16)).has_value());
}

TEST(Ruvia, ip_address_parsing_rejects_ports_brackets_and_partial_or_padded_ipv4) {
    for (const std::string_view text : {"192.0.2.1:80", "[::1]:80", "[::1]", "192.0.2", "01.2.3.4", "192.0.2.256",
             "192.0.2.1.", ".192.0.2.1", "192.0.2.1%1", "fe80::1%", "fe80::1%1%2", "::1 ", "1",
             "::ffff:01.2.3.4", "::ffff:192.0.2", "fe80::1%12:80", "fe80::1%12 ", "fe80::1%1]:80"}) {
        EXPECT_TRUE(!ruvia::parse_ip_address(text).has_value());
    }
    EXPECT_TRUE(ruvia::parse_ip_address("0.0.0.0").has_value());
    EXPECT_TRUE(ruvia::parse_ip_address("255.255.255.255").has_value());
    const auto mapped = ruvia::parse_ip_address("::ffff:192.0.2.1");
    EXPECT_TRUE(mapped.has_value() && mapped->is_v6());
}

TEST(Ruvia, ip_address_parsing_handles_long_input_storage) {
    const auto result_value = ruvia::parse_ip_address("fe80::1%" + std::string(1024, 'x'));
    // An unknown zone name is a platform decision; the storage must round-trip either way.
    EXPECT_TRUE(!result_value.has_value() || result_value->is_v6());
}

TEST(Ruvia, ip_address_parsing_rejects_invalid_ipv6_zone_identifiers) {
    for (const std::string_view text : {
             "fe80::1%0",
             "fe80::1%01",
             "fe80::1%-1",
             "fe80::1%+1",
             "fe80::1%12abc",
             "fe80::1%4294967296",
             "fe80::1%99999999999999999999",
             "fe80::1%nonexistent_ruvia_zone_id",
         }) {
        EXPECT_TRUE(!ruvia::parse_ip_address(text).has_value());
    }
}

namespace {

[[nodiscard]] bool is_utf8(std::string_view text) {
    std::size_t index = 0;
    while (index < text.size()) {
        const auto lead = static_cast<unsigned char>(text[index]);
        std::size_t length = 0;
        if (lead < 0x80U) {
            length = 1;
        } else if (lead >= 0xC2U && lead <= 0xDFU) {
            length = 2;
        } else if (lead >= 0xE0U && lead <= 0xEFU) {
            length = 3;
        } else if (lead >= 0xF0U && lead <= 0xF4U) {
            length = 4;
        } else {
            return false;
        }
        if (length > text.size() - index) {
            return false;
        }
        for (std::size_t offset = 1; offset < length; ++offset) {
            if ((static_cast<unsigned char>(text[index + offset]) & 0xC0U) != 0x80U) {
                return false;
            }
        }
        index += length;
    }
    return true;
}

}  // namespace

TEST(Ruvia, error_code_text_keeps_portable_category_messages) {
    const auto timed_out = std::make_error_code(std::errc::timed_out);
    EXPECT_EQ(std::string_view(ruvia::error_code_text(timed_out)), timed_out.message());

    const std::system_error generic(timed_out, "upstream");
    EXPECT_EQ(ruvia::system_error_text(generic), std::string_view(generic.what()));
}

TEST(Ruvia, error_code_text_describes_system_codes_in_utf8) {
    for (const auto& error : {std::error_code(1, std::system_category()),
             std::error_code(0x7FFFFFF0, std::system_category()),
             std::error_code(asio::error::connection_reset), std::error_code(asio::error::host_not_found)}) {
        const auto text = ruvia::error_code_text(error);
        EXPECT_TRUE(!text.empty());
        EXPECT_TRUE(is_utf8(text));
    }

    // The context before the OS message is kept.
    const std::system_error bind(std::error_code(1, std::system_category()), "bind");
    const auto text = ruvia::system_error_text(bind);
    EXPECT_TRUE(text.starts_with("bind: "));
    EXPECT_TRUE(text.size() > std::string_view("bind: ").size());
    EXPECT_TRUE(is_utf8(text));
}

namespace {

struct alignas(4096) retained_process_value final {
    std::array<std::byte, 4096> bytes_;
};

}  // namespace

TEST(Ruvia, collect_thread_heap_preserves_live_storage) {
    std::string retained(30000, 'r');
    const auto* const retained_address = retained.data();
    std::vector<std::byte> small(17, std::byte{0x33});
    {
        std::vector<std::byte> scratch(1024, std::byte{0xaa});
        (void)scratch;
    }

    ruvia::collect_thread_heap();
    ruvia::collect_thread_heap();

    EXPECT_TRUE(retained.data() == retained_address);
    EXPECT_EQ(retained.size(), std::size_t{30000});
    EXPECT_EQ(retained.find_first_not_of('r'), std::string::npos);
    EXPECT_TRUE(std::ranges::all_of(small, [](std::byte value) {
        return value == std::byte{0x33};
    }));
}

TEST(Ruvia, storage_survives_its_allocating_thread) {
    constexpr std::size_t text_bytes = 2 * 1024 * 1024;
    std::optional<std::string> text;
    std::optional<std::vector<retained_process_value>> values;
    std::thread([&] {
        text.emplace(text_bytes, 's');
        values.emplace(3);
        for (std::size_t index = 0; index < values->size(); ++index) {
            (*values)[index].bytes_.fill(static_cast<std::byte>(index + 1));
        }
    }).join();

    for (std::size_t index = 0; index < values->size(); ++index) {
        const auto& value = (*values)[index];
        EXPECT_EQ(reinterpret_cast<std::uintptr_t>(&value) % alignof(retained_process_value), std::uintptr_t{0});
        EXPECT_TRUE(std::ranges::all_of(value.bytes_, [index](std::byte byte) {
            return byte == static_cast<std::byte>(index + 1);
        }));
    }
    EXPECT_EQ(text->size(), text_bytes);
    EXPECT_EQ(text->find_first_not_of('s'), std::string::npos);

    bool preserved_after_growth = false;
    std::thread([&] {
        values->reserve(values->capacity() + 1);
        text->append(text->capacity() + 1, 't');
        preserved_after_growth = text->find_first_not_of('s') == text_bytes &&
                                 text->find_first_not_of('t', text_bytes) == std::string::npos;
        for (std::size_t index = 0; index < values->size(); ++index) {
            preserved_after_growth = preserved_after_growth &&
                                     std::ranges::all_of((*values)[index].bytes_, [index](std::byte byte) {
                                         return byte == static_cast<std::byte>(index + 1);
                                     });
        }
        text.reset();
        values.reset();
    }).join();
    EXPECT_TRUE(preserved_after_growth);
}

namespace ruvia::test::asio_task_tests {

struct alignas(4096) aligned_result final {
    explicit aligned_result(int value)
        : value_(std::make_unique<int>(value)),
          addresses_aligned_(is_aligned(this)) {}
    aligned_result(aligned_result&& other) noexcept
        : value_(std::move(other.value_)),
          addresses_aligned_(other.addresses_aligned_ && is_aligned(&other) && is_aligned(this)) {}

    std::unique_ptr<int> value_;
    bool addresses_aligned_;
    // Fill the alignment boundary explicitly; C4324 is an error under MSVC /WX.
    std::array<std::byte, 4096 - sizeof(std::unique_ptr<int>) - sizeof(bool)> padding_;

private:
    static bool is_aligned(const void* value) noexcept {
        return reinterpret_cast<std::uintptr_t>(value) % alignof(aligned_result) == 0;
    }
};

ruvia::task<aligned_result> make_value(ruvia::worker_ref worker_value,
    std::unique_ptr<aligned_result> result_value) {
    if (!worker_value.is_current()) {
        throw std::logic_error("task started outside its event loop");
    }
    // Keep the over-aligned object outside coroutine locals, whose alignment
    // is independently broken by GCC 13/14.
    co_return std::move(*result_value);
}

ruvia::task<void> complete_void(ruvia::worker_ref worker_value, bool& completed) {
    if (!worker_value.is_current()) {
        throw std::logic_error("task started outside its event loop");
    }
    completed = true;
    co_return;
}

ruvia::task<void> fail() {
    throw std::runtime_error("root task failure");
    co_return;
}

ruvia::task<int> answer() {
    co_return 7;
}

template <typename t_type>
// Not final: Asio executors keep an allocator as an empty base.
struct counting_allocator {
    using value_type = t_type;

    explicit counting_allocator(std::size_t& allocations) noexcept
        : allocations_(&allocations) {}
    template <typename other_type>
    counting_allocator(const counting_allocator<other_type>& other) noexcept
        : allocations_(other.allocations_) {}

    [[nodiscard]] t_type* allocate(std::size_t count) {
        ++*allocations_;
        return std::allocator<t_type>().allocate(count);
    }
    void deallocate(t_type* pointer, std::size_t count) noexcept {
        std::allocator<t_type>().deallocate(pointer, count);
    }
    template <typename other_type>
    friend bool operator==(const counting_allocator& left, const counting_allocator<other_type>& right) noexcept {
        return left.allocations_ == right.allocations_;
    }

    std::size_t* allocations_;
};

}  // namespace ruvia::test::asio_task_tests

TEST(Ruvia, AsioTaskLifecycle) {
    {
        auto ok_void = ruvia::asio_completion<void>::completed({});
        auto err_void = ruvia::asio_completion<void>::completed(std::make_error_code(std::errc::invalid_argument));
        auto ok_val = ruvia::asio_completion<int>::completed({}, 17);
        const auto& const_val = ok_val;
        EXPECT_TRUE(ok_void.succeeded());
        EXPECT_FALSE(ok_void.failed());
        EXPECT_FALSE(err_void.succeeded());
        EXPECT_TRUE(err_void.failed());
        EXPECT_TRUE(ok_val.succeeded());
        EXPECT_FALSE(ok_val.failed());
        EXPECT_EQ(const_val.result(), 17);
        EXPECT_EQ(ok_val.result(), 17);
        EXPECT_EQ(std::move(ok_val).take_result(), 17);
    }
    ruvia::event_loop_pool loops({.loop_count_ = 1});
    const auto loop = loops.loop(0);
    const auto worker_value = loop.worker();
    bool void_completed = false;

    auto value = loop.start(ruvia::test::asio_task_tests::make_value(
        worker_value, std::make_unique<ruvia::test::asio_task_tests::aligned_result>(42)));
    auto no_value = loop.start(ruvia::test::asio_task_tests::complete_void(worker_value, void_completed));
    auto failure = loop.start(ruvia::test::asio_task_tests::fail());
    std::promise<bool> same_loop_get_rejected;
    auto same_loop_get_result = same_loop_get_rejected.get_future();
    const auto probe_posted = loop.worker().post([&] {
        try {
            static_cast<void>(std::move(value).get());
            same_loop_get_rejected.set_value(false);
        } catch (const std::logic_error&) {
            same_loop_get_rejected.set_value(value.valid());
        }
    });
    ASSERT_TRUE(probe_posted.accepted());

    loops.start();
    bool valid = false;
    std::optional<ruvia::test::asio_task_tests::aligned_result> result_value;
    try {
        const auto same_loop_guard_held = same_loop_get_result.get();
        result_value.emplace(std::move(value).get());
        std::move(no_value).get();
        bool failure_observed = false;
        try {
            std::move(failure).get();
        } catch (const std::runtime_error& error) {
            failure_observed = std::string_view(error.what()) == "root task failure";
        }
        valid = same_loop_guard_held && void_completed && failure_observed;
    } catch (...) {
        valid = false;
    }
    loops.stop();
    loops.join();
    valid = valid && result_value && result_value->addresses_aligned_ &&
            result_value->value_ != nullptr && *result_value->value_ == 42;
    EXPECT_TRUE(valid);
}

TEST(Ruvia, AsioTaskAbandonedFailureReachesJoin) {
    ruvia::event_loop_pool abandoned_pool({.loop_count_ = 1});
    {
        auto unobserved = abandoned_pool.loop(0).start(ruvia::test::asio_task_tests::fail());
    }
    abandoned_pool.start();
    abandoned_pool.stop();
    try {
        abandoned_pool.join();
    } catch (const std::runtime_error& error) {
        EXPECT_EQ(std::string_view(error.what()), "root task failure");
        return;
    }
    ADD_FAILURE() << "Expected abandoned root task failure";
}

TEST(Ruvia, AsyncStartUsesHandlerExecutorAndAllocator) {
    asio::io_context io_context;
    std::size_t allocations = 0;
    std::optional<int> delivered;
    bool failure_delivered = false;
    ruvia::async_start(ruvia::test::asio_task_tests::answer(),
        asio::bind_allocator(ruvia::test::asio_task_tests::counting_allocator<int>(allocations),
            asio::bind_executor(io_context.get_executor(),
                [&delivered](ruvia::task_completion_result<int>&& result_value) {
                    if (result_value.index() == 0) {
                        delivered = std::get<0>(result_value);
                    }
                })));
    ruvia::async_start(ruvia::test::asio_task_tests::fail(),
        asio::bind_executor(io_context.get_executor(), [&failure_delivered](std::exception_ptr failure) {
            failure_delivered = failure != nullptr;
        }));
    // Both tasks complete inline; delivery waits for the handler's executor.
    EXPECT_FALSE(delivered.has_value());
    EXPECT_FALSE(failure_delivered);
    io_context.run();
    EXPECT_EQ(delivered, 7);
    EXPECT_TRUE(failure_delivered);
    EXPECT_GT(allocations, 0U);
}

TEST(Ruvia, EventLoopTryStartReturnsRejectedTask) {
    ruvia::event_loop_pool loops({.loop_count_ = 1, .queue_capacity_ = 1});
    const auto loop = loops.loop(0);
    auto accepted = loop.try_start(ruvia::test::asio_task_tests::answer());
    ASSERT_TRUE(accepted.accepted());
    EXPECT_EQ(accepted.rejected(), nullptr);
    auto root = std::move(accepted).take_value();

    auto full = loop.try_start(ruvia::test::asio_task_tests::answer());
    EXPECT_EQ(full.status(), ruvia::post_status::queue_full);
    ASSERT_NE(full.rejected(), nullptr);
    EXPECT_THROW(static_cast<void>(full.value()), std::logic_error);
    auto returned = std::move(full).take_rejected();
    try {
        static_cast<void>(loop.start(std::move(returned)));
        ADD_FAILURE() << "a full queue must reject start()";
    } catch (const ruvia::event_loop_start_rejected& error) {
        EXPECT_EQ(error.status(), ruvia::post_status::queue_full);
    }

    EXPECT_FALSE(root.ready());
    EXPECT_FALSE(root.wait_for(ruvia::operation_timeout(std::chrono::steady_clock::now())));
    EXPECT_FALSE(root.wait_for(ruvia::operation_timeout(std::chrono::steady_clock::time_point{})));
    EXPECT_FALSE(root.wait_for(std::chrono::milliseconds(1)));
    EXPECT_THROW(static_cast<void>(root.wait_for(std::chrono::milliseconds(0))), std::invalid_argument);
    EXPECT_THROW(static_cast<void>(root.wait_for(std::chrono::milliseconds(-1))), std::invalid_argument);
    loops.start();
    EXPECT_TRUE(root.wait_for(std::chrono::seconds(30)));
    EXPECT_TRUE(root.ready());
    EXPECT_EQ(std::move(root).get(), 7);
    EXPECT_FALSE(root.ready());

    loops.stop();
    loops.join();
    auto stopped = loop.try_start(ruvia::test::asio_task_tests::answer());
    EXPECT_EQ(stopped.status(), ruvia::post_status::worker_stopping);
    EXPECT_NE(stopped.rejected(), nullptr);
    try {
        static_cast<void>(loop.start(std::move(stopped).take_rejected()));
        ADD_FAILURE() << "a retired loop must reject start()";
    } catch (const ruvia::event_loop_start_rejected& error) {
        EXPECT_EQ(error.status(), ruvia::post_status::worker_stopping);
    }
}

TEST(Ruvia, worker_queue_yields_to_io_context_between_batches) {
    struct reposting_task final {
        const ruvia::worker_ref* worker_;
        ruvia::event_loop_attachment* attachment_;
        const bool* foreign_ran_;
        unsigned* runs_;
        void operator()() const {
            ++*runs_;
            if (!*foreign_ran_ && *runs_ < 1000) {
                static_cast<void>(worker_->post(*this));
            } else {
                attachment_->stop();
            }
        }
    };
    asio::io_context context;
    auto attachment = ruvia::attach_event_loop(context, {.queue_capacity_ = 1});
    const auto worker_value = attachment.loop().worker();
    bool foreign_ran{};
    unsigned runs{};
    EXPECT_TRUE(worker_value.post(reposting_task{&worker_value, &attachment, &foreign_ran, &runs}).accepted());
    asio::post(context, [&foreign_ran] { foreign_ran = true; });
    attachment.run();
    // The repost lands behind the plain handler queued after the first batch.
    EXPECT_TRUE(foreign_ran);
    EXPECT_EQ(runs, 2u);
}

TEST(Ruvia, worker_cancellation_registration_retires_before_slot_reuse) {
    struct owner final {
        std::uint64_t identity_{};
        unsigned accepted_{};
        unsigned stale_{};
        void cancel_operation_by_id(std::uint64_t id) noexcept {
            if (id == identity_) {
                ++accepted_;
            } else {
                ++stale_;
            }
        }
    } state;
    asio::io_context context;
    auto attachment = ruvia::attach_event_loop(context, {.queue_capacity_ = 1});
    const auto worker_value = attachment.loop().worker();
    auto target = ruvia::make_worker_cancellation_target(state, worker_value);
    using registration = ruvia::worker_cancellation_registration<ruvia::worker_cancellation_target<owner>>;
    using target_owner = std::shared_ptr<ruvia::worker_cancellation_target<owner>>;
    static_assert(std::is_constructible_v<registration, target_owner&, std::uint64_t&>);
    static_assert(std::is_constructible_v<registration, const target_owner&, std::uint64_t&>);
    static_assert(!std::is_constructible_v<registration, target_owner&&, std::uint64_t&>);
    static_assert(!std::is_constructible_v<registration, const target_owner&&, std::uint64_t&>);
    std::optional<registration> current;
    ruvia::stop_source old_stop;
    ruvia::stop_source new_stop;
    std::uint64_t old_id{};
    bool identity_cleared{};
    EXPECT_TRUE(worker_value.post([&] {
                                current.emplace(target, state.identity_);
                                old_id = current->id();
                                current->arm(old_stop.token());
                                // Foreign cancellation is queued behind this owner turn.
                                std::jthread requester([&] { old_stop.request_stop(); });
                                requester.join();
                                current.reset();
                                identity_cleared = state.identity_ == 0;
                                current.emplace(target, state.identity_);
                                current->arm(new_stop.token());
                            })
            .accepted());
    // Drain the ready handlers, including the queued foreign cancellation,
    // while the loop stays attached for the second turn.
    context.poll();
    EXPECT_TRUE(identity_cleared);
    EXPECT_TRUE(state.identity_ != 0 && state.identity_ != old_id);
    EXPECT_EQ(state.accepted_, 0u);
    EXPECT_EQ(state.stale_, 1u);
    EXPECT_TRUE(worker_value.post([&] {
                                new_stop.request_stop();
                                current.reset();
                                target->detach(state);
                                attachment.stop();
                            })
            .accepted());
    attachment.run();
    EXPECT_EQ(state.accepted_, 1u);
    EXPECT_EQ(state.identity_, std::uint64_t{0});
}

TEST(Ruvia, worker_cancellation_registration_handles_precancel_and_exception_unwind) {
    struct owner final {
        std::uint64_t identity_{};
        unsigned cancelled_{};
        void cancel_operation_by_id(std::uint64_t id) noexcept {
            if (id == identity_) {
                ++cancelled_;
            }
        }
    } state;
    asio::io_context context;
    auto attachment = ruvia::attach_event_loop(context, {.queue_capacity_ = 1});
    const auto worker_value = attachment.loop().worker();
    auto target = ruvia::make_worker_cancellation_target(state, worker_value);
    ruvia::stop_source source;
    source.request_stop();
    bool unwound = false;
    EXPECT_TRUE(worker_value.post([&] {
                                try {
                                    ruvia::worker_cancellation_registration registration(target, state.identity_);
                                    registration.arm(source.token());
                                    throw std::runtime_error("operation failed");
                                } catch (const std::runtime_error&) {
                                    unwound = state.identity_ == 0;
                                }
                                target->detach(state);
                                attachment.stop();
                            })
            .accepted());
    attachment.run();
    EXPECT_TRUE(unwound);
    EXPECT_EQ(state.cancelled_, 1u);
}

namespace worker_resume_test {

// Suspends and has the worker resume the coroutine, asked from the worker
// itself or from a foreign thread. The step is recorded negated once the
// request returned, which is before the resumption when it is never inline.
struct deferred_resume final {
    const ruvia::worker_ref& worker_;
    std::vector<int>& order_;
    int step_;
    bool foreign_{false};

    [[nodiscard]] bool await_ready() const noexcept {
        return false;
    }
    void await_suspend(std::coroutine_handle<> continuation) const {
        if (foreign_) {
            std::jthread([this, continuation] { worker_.defer_resume(continuation); }).join();
        } else {
            worker_.defer_resume(continuation);
        }
        order_.push_back(-step_);
    }
    void await_resume() const noexcept {}
};

ruvia::task<void> resume_twice(const ruvia::worker_ref& worker, std::vector<int>& order, int first, bool& on_worker) {
    order.push_back(first);
    co_await deferred_resume{worker, order, first};
    on_worker = on_worker && worker.is_current();
    order.push_back(first + 1);
    co_await deferred_resume{worker, order, first + 2, true};
    on_worker = on_worker && worker.is_current();
    order.push_back(first + 3);
}

[[nodiscard]] bool before(const std::vector<int>& order, int first, int second) {
    const auto left = std::ranges::find(order, first);
    const auto right = std::ranges::find(order, second);
    return left != order.end() && right != order.end() && left < right;
}

}  // namespace worker_resume_test

// A deferred resumption runs on the worker in a later turn, never inside the
// request, whether it was asked on the worker or from another thread, and
// requests from the worker's own thread resume in request order -- under the
// default event loop and the single-owner one production workers use.
TEST(Ruvia, worker_ref_defers_resumption_to_its_worker) {
    using worker_resume_test::before;
    for (const int hint : {ASIO_CONCURRENCY_HINT_DEFAULT, ASIO_CONCURRENCY_HINT_UNSAFE_IO}) {
        asio::io_context context(hint);
        auto attachment = ruvia::attach_event_loop(context, {.queue_capacity_ = 1});
        const auto worker_value = attachment.loop().worker();
        std::vector<int> order;
        bool on_worker = true;
        ruvia::test::run_worker_tasks(attachment,
            worker_resume_test::resume_twice(worker_value, order, 10, on_worker),
            worker_resume_test::resume_twice(worker_value, order, 20, on_worker));
        EXPECT_TRUE(on_worker);
        EXPECT_EQ(order.size(), std::size_t{10});
        for (const int first : {10, 20}) {
            EXPECT_TRUE(before(order, -first, first + 1));
            EXPECT_TRUE(before(order, -(first + 2), first + 3));
        }
        EXPECT_TRUE(before(order, 11, 21));
    }
}

namespace ruvia::test::move_only_function_tests {

struct counted_delete final {
    int* destroyed_;

    void operator()(int* value) const noexcept {
        ++*destroyed_;
        delete value;
    }
};

using callback = ruvia::move_only_function<int()>;
using payload = std::unique_ptr<int, counted_delete>;

struct reentry_state final {
    callback* destination_{nullptr};
    callback replacement_;
    int expected_current_{0};
    int reentries_{0};
};

struct replacing_target final {
    reentry_state* state_;
    payload value_;

    replacing_target(reentry_state* state, int value, int& destroyed)
        : state_(state),
          value_(new int(value), {&destroyed}) {}

    replacing_target(replacing_target&& other) noexcept
        : state_(std::exchange(other.state_, nullptr)),
          value_(std::move(other.value_)) {}

    ~replacing_target() {
        if (state_ != nullptr) {
            auto* const state = state_;
            ++state->reentries_;
            EXPECT_TRUE(static_cast<bool>(*state->destination_));
            if (*state->destination_) {
                EXPECT_EQ((*state->destination_)(), state->expected_current_);
            }
            *state->destination_ = std::move(state->replacement_);
        }
    }

    int operator()() const noexcept {
        return *value_;
    }
};

callback make_replacing_target(int value, int& destroyed, bool heap_target, reentry_state* state = nullptr) {
    if (heap_target) {
        return callback([target = replacing_target(state, value, destroyed), padding = std::array<char, 64>{}] {
            (void)padding;
            return target();
        });
    }
    return callback(replacing_target(state, value, destroyed));
}

struct destroy_owner_delete final {
    std::unique_ptr<callback>* owner_;
    int* destroyed_;

    void operator()(int* value) const noexcept {
        ++*destroyed_;
        delete value;
        owner_->reset();
    }
};

struct callback_lifetime final {
    bool* alive_;

    explicit callback_lifetime(bool& alive) noexcept
        : alive_(&alive) {
        alive = true;
    }

    callback_lifetime(callback_lifetime&& other) noexcept
        : alive_(std::exchange(other.alive_, nullptr)) {}

    ~callback_lifetime() {
        if (alive_ != nullptr) {
            *alive_ = false;
        }
    }
};

struct clear_reentry_target final {
    callback_lifetime lifetime_;
    reentry_state* state_;

    explicit clear_reentry_target(bool& alive, reentry_state* state = nullptr) noexcept
        : lifetime_(alive),
          state_(state) {}

    clear_reentry_target(clear_reentry_target&& other) noexcept
        : lifetime_(std::move(other.lifetime_)),
          state_(std::exchange(other.state_, nullptr)) {}

    ~clear_reentry_target() {
        if (auto* const state = std::exchange(state_, nullptr)) {
            ++state->reentries_;
            EXPECT_FALSE(static_cast<bool>(*state->destination_));
            *state->destination_ = std::move(state->replacement_);
        }
    }

    int operator()() const noexcept {
        return lifetime_.alive_ != nullptr && *lifetime_.alive_;
    }
};

}  // namespace ruvia::test::move_only_function_tests

TEST(Ruvia, move_assignment_extracts_source_owned_by_old_target) {
    using callback = ruvia::move_only_function<void()>;
    using payload = std::unique_ptr<int, ruvia::test::move_only_function_tests::counted_delete>;

    for (const bool heap_target : {false, true}) {
        SCOPED_TRACE(heap_target);
        int calls = 0;
        int destroyed = 0;
        auto nested = std::make_unique<callback>(
            [value = payload(new int(42), {&destroyed}), &calls] { calls += *value; });
        auto* const source = nested.get();
        callback destination;
        if (heap_target) {
            destination = callback([owned = std::move(nested), padding = std::array<char, 64>{}] {
                (void)owned;
                (void)padding;
            });
        } else {
            destination = callback([owned = std::move(nested)] { (void)owned; });
        }

        destination = std::move(*source);
        EXPECT_EQ(destroyed, 0);
        destination();
        EXPECT_EQ(calls, 42);
        destination = nullptr;
        EXPECT_EQ(destroyed, 1);
    }
}

TEST(Ruvia, move_assignment_reentrant_replacements_release_each_target) {
    using namespace ruvia::test::move_only_function_tests;

    for (const bool heap_target : {false, true}) {
        for (const int reentry_count : {1, 4}) {
            SCOPED_TRACE(heap_target);
            SCOPED_TRACE(reentry_count);
            int destroyed = 0;
            callback destination;
            std::array<reentry_state, 4> states;
            for (int index = 0; index < reentry_count; ++index) {
                states[index].destination_ = &destination;
                states[index].expected_current_ = index + 2;
                auto* const next = index + 2 < reentry_count ? &states[index + 2] : nullptr;
                states[index].replacement_ = make_replacing_target(index + 3, destroyed, heap_target, next);
            }
            destination = make_replacing_target(1, destroyed, heap_target, &states[0]);
            callback source = make_replacing_target(2, destroyed, heap_target, reentry_count > 1 ? &states[1] : nullptr);

            destination = std::move(source);

            EXPECT_FALSE(static_cast<bool>(source));
            ASSERT_TRUE(static_cast<bool>(destination));
            EXPECT_EQ(destination(), reentry_count + 2);
            EXPECT_EQ(destroyed, reentry_count + 1);
            for (int index = 0; index < reentry_count; ++index) {
                EXPECT_EQ(states[index].reentries_, 1);
                EXPECT_FALSE(static_cast<bool>(states[index].replacement_));
            }
            destination = nullptr;
            EXPECT_EQ(destroyed, reentry_count + 2);
        }
    }
}

TEST(Ruvia, move_assignment_retirement_can_destroy_destination_owner) {
    using namespace ruvia::test::move_only_function_tests;
    using owner_payload = std::unique_ptr<int, destroy_owner_delete>;

    for (const bool heap_target : {false, true}) {
        SCOPED_TRACE(heap_target);
        int destroyed = 0;
        auto owner = std::make_unique<callback>();
        if (heap_target) {
            *owner = callback([value = owner_payload(new int(1), {&owner, &destroyed}), padding = std::array<char, 64>{}] {
                (void)padding;
                return *value;
            });
        } else {
            *owner = callback([value = owner_payload(new int(1), {&owner, &destroyed})] { return *value; });
        }
        callback source = make_replacing_target(2, destroyed, heap_target);

        *owner = std::move(source);

        EXPECT_EQ(owner, nullptr);
        EXPECT_FALSE(static_cast<bool>(source));
        EXPECT_EQ(destroyed, 2);
    }
}

TEST(Ruvia, null_assignment_preserves_reentrant_replacement_lifetime) {
    using namespace ruvia::test::move_only_function_tests;

    for (const bool heap_target : {false, true}) {
        SCOPED_TRACE(heap_target);
        bool original_alive = false;
        bool replacement_alive = false;
        callback destination;
        reentry_state state;
        state.destination_ = &destination;
        state.replacement_ = callback(clear_reentry_target(replacement_alive));
        if (heap_target) {
            destination = callback([target = clear_reentry_target(original_alive, &state), padding = std::array<char, 64>{}] {
                (void)padding;
                return target();
            });
        } else {
            destination = callback(clear_reentry_target(original_alive, &state));
        }

        destination = nullptr;

        EXPECT_FALSE(original_alive);
        EXPECT_TRUE(replacement_alive);
        EXPECT_EQ(state.reentries_, 1);
        EXPECT_FALSE(static_cast<bool>(state.replacement_));
        ASSERT_TRUE(static_cast<bool>(destination));
        EXPECT_EQ(destination(), 1);
        destination = nullptr;
        EXPECT_FALSE(replacement_alive);
    }
}

TEST(Ruvia, noexcept_function_preserves_mutable_target_through_const_owner) {
    using callback = ruvia::move_only_function<int() noexcept>;
    const auto throwing = [] { return 0; };
    static_assert(!std::is_constructible_v<callback, decltype(throwing)>);
    static_assert(std::is_nothrow_invocable_r_v<int, const callback&>);

    callback source([count = std::make_unique<int>(4)]() mutable noexcept {
        return ++*count;
    });
    const callback destination(std::move(source));
    EXPECT_FALSE(source);
    EXPECT_EQ(destination(), 5);
    EXPECT_EQ(destination(), 6);
}

namespace {

using namespace std::chrono_literals;
constexpr auto wait_timeout = 5s;

struct spawned_task final {
    std::shared_ptr<std::promise<std::exception_ptr>> completion_;
    std::future<std::exception_ptr> result_;
};

template <typename runtime_type>
[[nodiscard]] spawned_task spawn(runtime_type& runtime, ruvia::task<void> task_value) {
    auto completion = std::make_shared<std::promise<std::exception_ptr>>();
    auto result_value = completion->get_future();
    asio::co_spawn(runtime.io_context(), ruvia::as_awaitable(std::move(task_value)),
        [completion](std::exception_ptr error) { completion->set_value(std::move(error)); });
    return {std::move(completion), std::move(result_value)};
}

[[nodiscard]] bool stop(ruvia::worker_runtime& runtime) {
    runtime.join();
    return runtime.take_failure() == nullptr;
}

[[nodiscard]] bool finish(spawned_task& task_value) {
    if (task_value.result_.wait_for(wait_timeout) != std::future_status::ready) {
        return false;
    }
    try {
        if (const auto failure = task_value.result_.get()) {
            std::rethrow_exception(failure);
        }
    } catch (...) {
        return false;
    }
    return true;
}

ruvia::task<void> close_notification(ruvia::worker_notification& notification) {
    notification.close();
    co_return;
}

ruvia::task<void> close_and_stop_attachment(ruvia::worker_notification& notification,
    ruvia::event_loop_attachment& attachment, asio::steady_timer& watchdog_value) {
    watchdog_value.cancel();
    notification.close();
    attachment.stop();
    co_return;
}

ruvia::task<void> wait_for_one(ruvia::worker_notification& notification,
    std::promise<void>* started, std::atomic<int>* result_value) {
    if (started != nullptr) {
        started->set_value();
    }
    const auto status = co_await notification.wait();
    if (result_value != nullptr) {
        result_value->store(status == ruvia::worker_notification_wait_status::notified ? 1 : 2,
            std::memory_order_release);
    }
}

ruvia::task<void> cold_wait(ruvia::worker_notification& notification) {
    static_cast<void>(co_await notification.wait());
}

ruvia::task<void> wait_close_and_stop_attachment(ruvia::worker_notification& notification,
    ruvia::event_loop_attachment& attachment, std::atomic<int>& result_value) {
    const auto status = co_await notification.wait();
    notification.close();
    attachment.stop();
    result_value.store(status == ruvia::worker_notification_wait_status::notified ? 1 : 2,
        std::memory_order_release);
}
ruvia::task<void> notify_on_worker(
    ruvia::worker_notification& notification, std::atomic<int>& result_value) {
    result_value.store(static_cast<int>(notification.notify()), std::memory_order_release);
    co_return;
}

ruvia::task<void> close_and_destroy_on_resume(
    std::unique_ptr<ruvia::worker_notification>& notification, std::promise<void>* started,
    std::atomic<int>& result_value) {
    if (started != nullptr) {
        started->set_value();
    }
    const auto status = co_await notification->wait();
    notification->close();
    notification.reset();
    result_value.store(status == ruvia::worker_notification_wait_status::notified ? 1 : 2,
        std::memory_order_release);
}

ruvia::task<void> wait_until_closed(ruvia::worker_notification& notification,
    std::promise<void>* started, std::atomic<int>& result_value) {
    if (started != nullptr) {
        started->set_value();
    }
    for (;;) {
        const auto status = co_await notification.wait();
        if (status == ruvia::worker_notification_wait_status::closed) {
            result_value.store(2, std::memory_order_release);
            co_return;
        }
    }
}

struct round_state final {
    std::mutex mutex_;
    std::condition_variable changed_;
    std::size_t completed_{0};

    [[nodiscard]] bool wait_for(
        std::size_t target, std::chrono::steady_clock::time_point deadline_value) {
        std::unique_lock lock(mutex_);
        return changed_.wait_until(lock, deadline_value, [&] { return completed_ >= target; });
    }
};

ruvia::task<void> wait_runtime_rounds(ruvia::worker_notification& notification,
    round_state& rounds, std::size_t target, bool& success) {
    for (std::size_t index = 0; index < target; ++index) {
        if (co_await notification.wait() != ruvia::worker_notification_wait_status::notified) {
            success = false;
            co_return;
        }
        {
            const std::lock_guard lock(rounds.mutex_);
            rounds.completed_ = index + 1;
        }
        rounds.changed_.notify_all();
    }
    success = true;
}

ruvia::task<void> wait_rounds(ruvia::worker_notification& notification, round_state& rounds,
    std::size_t target, bool& success) {
    for (std::size_t index = 0; index < target; ++index) {
        if (co_await notification.wait() != ruvia::worker_notification_wait_status::notified) {
            success = false;
            co_return;
        }
        {
            const std::lock_guard lock(rounds.mutex_);
            rounds.completed_ = index + 1;
        }
        rounds.changed_.notify_all();
    }
    notification.close();
    success = true;
}

struct producer_state final {
    std::atomic<std::size_t> published_{0};
    std::atomic<std::size_t> finished_{0};
    std::atomic<std::size_t> observed_{0};
    std::atomic<bool> early_closed_{false};
};

ruvia::task<void> wait_for_producers(
    ruvia::worker_notification& notification, producer_state& state_value, std::size_t producer_count) {
    while (state_value.finished_.load(std::memory_order_acquire) != producer_count) {
        if (co_await notification.wait() != ruvia::worker_notification_wait_status::notified) {
            co_return;
        }
    }
    state_value.observed_.store(state_value.published_.load(std::memory_order_acquire), std::memory_order_release);
}

ruvia::task<void> observe_close_and_timer(asio::io_context& io_context,
    ruvia::worker_notification& notification, std::atomic<bool>& timer_fired) {
    asio::steady_timer timer(io_context);
    timer.expires_after(20ms);
    auto first = co_await ruvia::async_asio<void>([&timer](auto handler) mutable {
        timer.async_wait(std::move(handler));
    });
    if (first.error_code()) {
        throw std::system_error(first.error_code());
    }

    notification.close();
    timer.expires_after(20ms);
    auto second = co_await ruvia::async_asio<void>([&timer](auto handler) mutable {
        timer.async_wait(std::move(handler));
    });
    if (second.error_code()) {
        throw std::system_error(second.error_code());
    }
    timer_fired.store(true, std::memory_order_release);
}

ruvia::task<void> probe_wrong_worker_wait(
    ruvia::worker_notification& notification, bool& rejected) {
    try {
        static_cast<void>(co_await notification.wait());
    } catch (const std::logic_error&) {
        rejected = true;
    }
}

ruvia::task<void> probe_concurrent_wait(
    ruvia::worker_notification& notification, bool& rejected) {
    try {
        static_cast<void>(co_await notification.wait());
    } catch (const std::logic_error&) {
        rejected = true;
    }
}

struct notify_during_registration final {
    ruvia::worker_notification& notification_;
    ruvia::worker_notification::wait_operation& wait_;

    bool await_ready() {
        const bool ready = wait_.await_ready();
        if (!ready) {
            static_cast<void>(notification_.notify());
        }
        return ready;
    }

    bool await_suspend(std::coroutine_handle<> continuation) {
        return wait_.await_suspend(continuation);
    }

    ruvia::worker_notification_wait_status await_resume() {
        return wait_.await_resume();
    }
};

ruvia::task<void> wait_for_registration_race(
    ruvia::worker_notification& notification, bool& notified) {
    notified = (co_await notify_during_registration{notification, notification.wait()}) ==
               ruvia::worker_notification_wait_status::notified;
    notification.close();
}

TEST(Ruvia, worker_notification_delivers_signal_during_wait_registration) {
    ruvia::worker_runtime worker;
    ruvia::worker_notification notification(worker);
    bool notified = false;
    auto waiting = spawn(worker, wait_for_registration_race(notification, notified));
    EXPECT_TRUE(worker.start());
    const bool completed = finish(waiting);
    if (!completed) {
        auto closer = spawn(worker, close_notification(notification));
        static_cast<void>(finish(closer));
        static_cast<void>(finish(waiting));
    }
    EXPECT_TRUE(completed);
    EXPECT_TRUE(notified);
    EXPECT_TRUE(stop(worker));
}

TEST(Ruvia, worker_notification_early_latch_cold_wait_and_repeated_reuse) {
    constexpr std::size_t round_count = 128;
    ruvia::worker_runtime worker;
    ruvia::worker_notification notification(worker);

    {
        auto unused_cold_wait = cold_wait(notification);
        static_cast<void>(unused_cold_wait);
    }
    EXPECT_EQ(notification.notify(), ruvia::worker_notification_status::notified);
    EXPECT_EQ(notification.notify(), ruvia::worker_notification_status::coalesced);

    round_state rounds;
    bool success = false;
    auto task_value = spawn(worker, wait_rounds(notification, rounds, round_count, success));
    EXPECT_TRUE(worker.start());

    std::thread producer_value([&] {
        const auto deadline_value = std::chrono::steady_clock::now() + wait_timeout;
        for (std::size_t index = 1; index < round_count; ++index) {
            std::unique_lock lock(rounds.mutex_);
            const bool ready = rounds.changed_.wait_until(lock, deadline_value,
                [&] { return rounds.completed_ >= index; });
            lock.unlock();
            if (!ready) {
                return;
            }
            const auto status = notification.notify();
            if (status == ruvia::worker_notification_status::closed) {
                return;
            }
        }
    });

    const bool completed = finish(task_value);
    producer_value.join();
    EXPECT_TRUE(completed);
    EXPECT_TRUE(success);
    EXPECT_EQ(rounds.completed_, round_count);
    EXPECT_TRUE(stop(worker));
}

TEST(Ruvia, worker_notification_borrows_attached_loop_on_unsafe_io_context) {
    constexpr std::size_t round_count = 4;
    asio::io_context io_context(ASIO_CONCURRENCY_HINT_UNSAFE_IO);
    auto attachment = ruvia::attach_event_loop(io_context, {.queue_capacity_ = 64});
    const auto loop = attachment.loop();
    ruvia::worker_notification notification(loop);

    bool close_rejected_off_worker = false;
    try {
        notification.close();
    } catch (const std::logic_error&) {
        close_rejected_off_worker = true;
    }
    EXPECT_TRUE(close_rejected_off_worker);

    {
        auto unused_cold_wait = cold_wait(notification);
        static_cast<void>(unused_cold_wait);
    }
    EXPECT_EQ(notification.notify(), ruvia::worker_notification_status::notified);
    EXPECT_EQ(notification.notify(), ruvia::worker_notification_status::coalesced);

    round_state rounds;
    bool rounds_succeeded = false;
    auto rounds_task = spawn(loop, wait_runtime_rounds(notification, rounds, round_count, rounds_succeeded));

    asio::steady_timer watchdog(io_context);
    watchdog.expires_after(20s);
    watchdog.async_wait([&](const asio::error_code& error) {
        if (!error) {
            notification.close();
            attachment.stop();
        }
    });

    std::promise<std::exception_ptr> runtime_exit_promise;
    auto runtime_exit = runtime_exit_promise.get_future();
    std::thread worker_value([&] {
        std::exception_ptr failure;
        try {
            attachment.run();
        } catch (...) {
            failure = std::current_exception();
        }
        runtime_exit_promise.set_value(std::move(failure));
    });

    bool rounds_advanced = true;
    for (std::size_t next_value = 1; next_value < round_count; ++next_value) {
        if (!rounds.wait_for(next_value, std::chrono::steady_clock::now() + wait_timeout)) {
            rounds_advanced = false;
            break;
        }
        EXPECT_EQ(notification.notify(), ruvia::worker_notification_status::notified);
    }
    const bool rounds_finished = finish(rounds_task);
    EXPECT_TRUE(rounds_advanced);
    EXPECT_TRUE(rounds_finished);
    if (rounds_finished) {
        EXPECT_TRUE(rounds_succeeded);
        EXPECT_EQ(rounds.completed_, round_count);
    }

    std::promise<void> wait_started;
    auto wait_started_future = wait_started.get_future();
    std::atomic<int> wait_result{0};
    auto pending_wait = spawn(loop, wait_for_one(notification, &wait_started, &wait_result));
    const bool entered = wait_started_future.wait_for(wait_timeout) == std::future_status::ready;

    auto shutdown = spawn(loop, close_and_stop_attachment(notification, attachment, watchdog));
    const bool shutdown_completed = finish(shutdown);
    const bool wait_completed = finish(pending_wait);
    EXPECT_TRUE(entered);
    EXPECT_TRUE(shutdown_completed);
    EXPECT_TRUE(wait_completed);
    EXPECT_EQ(wait_result.load(std::memory_order_acquire), 2);

    bool runtime_exited = runtime_exit.wait_for(wait_timeout) == std::future_status::ready;
    if (!runtime_exited) {
        attachment.stop();
    }
    EXPECT_TRUE(runtime_exited);
    if (runtime_exited) {
        const auto failure = runtime_exit.get();
        EXPECT_TRUE(failure == nullptr);
    } else {
        std::terminate();
    }
    worker_value.join();
    EXPECT_TRUE(!loop.worker().accepting());
}

TEST(Ruvia, worker_notification_many_producers_coalesce_without_lost_final_state) {
    constexpr std::size_t producer_count = 4;
    constexpr std::size_t notifications_per_producer = 10000;
    constexpr std::size_t expected = producer_count * notifications_per_producer;

    ruvia::worker_runtime worker;
    ruvia::worker_notification notification(worker);
    producer_state state;
    auto task_value = spawn(worker, wait_for_producers(notification, state, producer_count));
    EXPECT_TRUE(worker.start());

    std::vector<std::thread> producers;
    producers.reserve(producer_count);
    for (std::size_t producer_index = 0; producer_index < producer_count; ++producer_index) {
        producers.emplace_back([&] {
            for (std::size_t index = 0; index < notifications_per_producer; ++index) {
                state.published_.fetch_add(1, std::memory_order_release);
                if (notification.notify() == ruvia::worker_notification_status::closed) {
                    state.early_closed_.store(true, std::memory_order_release);
                    return;
                }
            }
            state.finished_.fetch_add(1, std::memory_order_release);
            static_cast<void>(notification.notify());
        });
    }

    const bool completed = finish(task_value);
    for (auto& producer : producers) {
        producer.join();
    }
    const bool observed_value = state.observed_.load(std::memory_order_acquire) == expected;
    auto closer = spawn(worker, close_notification(notification));
    const bool closed = finish(closer);

    EXPECT_TRUE(completed);
    EXPECT_TRUE(closed);
    EXPECT_TRUE(observed_value);
    EXPECT_TRUE(!state.early_closed_.load(std::memory_order_acquire));
    EXPECT_EQ(state.published_.load(std::memory_order_acquire), expected);
    EXPECT_EQ(notification.notify(), ruvia::worker_notification_status::closed);
    EXPECT_TRUE(stop(worker));
}

TEST(Ruvia, worker_notification_wait_checks_worker_and_rejects_concurrency) {
    ruvia::worker_runtime owner;
    ruvia::worker_runtime other;
    ruvia::worker_notification notification(owner);
    bool wrong_worker_rejected = false;
    bool concurrent_wait_rejected = false;
    std::promise<void> first_wait_started;
    std::future<void> first_wait_ready = first_wait_started.get_future();
    std::atomic<int> first_wait_result{0};

    EXPECT_TRUE(owner.start());
    EXPECT_TRUE(other.start());

    auto wrong = spawn(other, probe_wrong_worker_wait(notification, wrong_worker_rejected));
    const bool wrong_completed = finish(wrong);
    auto first = spawn(owner, wait_for_one(notification, &first_wait_started, &first_wait_result));
    const bool first_started = first_wait_ready.wait_for(wait_timeout) == std::future_status::ready;
    auto concurrent = spawn(owner, probe_concurrent_wait(notification, concurrent_wait_rejected));
    const bool concurrent_completed = finish(concurrent);
    auto closer = spawn(owner, close_notification(notification));
    const bool close_completed = finish(closer);
    const bool first_completed = finish(first);

    EXPECT_TRUE(wrong_completed);
    EXPECT_TRUE(wrong_worker_rejected);
    EXPECT_TRUE(first_started);
    EXPECT_TRUE(concurrent_completed);
    EXPECT_TRUE(concurrent_wait_rejected);
    EXPECT_TRUE(close_completed);
    EXPECT_TRUE(first_completed);
    EXPECT_EQ(first_wait_result.load(std::memory_order_acquire), 2);
    EXPECT_TRUE(stop(owner));
    EXPECT_TRUE(stop(other));
}

TEST(Ruvia, worker_notification_normal_resume_can_destroy_owner_immediately) {
    ruvia::worker_runtime worker;
    auto notification = std::make_unique<ruvia::worker_notification>(worker);
    std::promise<void> started;
    auto started_future = started.get_future();
    std::atomic<int> wait_result{0};
    std::atomic<int> notify_result{-1};
    auto waiting = spawn(worker, close_and_destroy_on_resume(notification, &started, wait_result));
    EXPECT_TRUE(worker.start());
    const bool entered = started_future.wait_for(wait_timeout) == std::future_status::ready;

    auto notifier = spawn(worker, notify_on_worker(*notification, notify_result));
    const bool notify_completed = finish(notifier);
    const bool wait_completed = finish(waiting);

    EXPECT_TRUE(entered);
    EXPECT_TRUE(notify_completed);
    EXPECT_TRUE(wait_completed);
    EXPECT_EQ(notify_result.load(std::memory_order_acquire), static_cast<int>(ruvia::worker_notification_status::notified));
    EXPECT_EQ(wait_result.load(std::memory_order_acquire), 1);
    EXPECT_TRUE(notification == nullptr);
    EXPECT_TRUE(stop(worker));
}

TEST(Ruvia, worker_notification_cancel_resume_can_destroy_owner_immediately) {
    ruvia::worker_runtime worker;
    auto notification = std::make_unique<ruvia::worker_notification>(worker);
    std::promise<void> started;
    auto started_future = started.get_future();
    std::atomic<int> wait_result{0};
    auto waiting = spawn(worker, close_and_destroy_on_resume(notification, &started, wait_result));
    EXPECT_TRUE(worker.start());
    const bool entered = started_future.wait_for(wait_timeout) == std::future_status::ready;

    auto closer = spawn(worker, close_notification(*notification));
    const bool close_completed = finish(closer);
    const bool wait_completed = finish(waiting);

    EXPECT_TRUE(entered);
    EXPECT_TRUE(close_completed);
    EXPECT_TRUE(wait_completed);
    EXPECT_EQ(wait_result.load(std::memory_order_acquire), 2);
    EXPECT_TRUE(notification == nullptr);
    EXPECT_TRUE(stop(worker));
}

TEST(Ruvia, worker_notification_pending_close_drains_and_worker_timer_survives) {
    ruvia::worker_runtime worker;
    ruvia::worker_notification notification(worker);
    std::promise<void> started;
    auto started_future = started.get_future();
    std::atomic<int> wait_result{0};
    auto waiting = spawn(worker, wait_for_one(notification, &started, &wait_result));
    EXPECT_TRUE(worker.start());
    const bool entered = started_future.wait_for(wait_timeout) == std::future_status::ready;
    std::atomic<bool> timer_fired{false};
    auto closer = spawn(worker, observe_close_and_timer(worker.io_context(), notification, timer_fired));

    const bool wait_completed = finish(waiting);
    const bool close_task_completed = finish(closer);
    EXPECT_TRUE(entered);
    EXPECT_TRUE(wait_completed);
    EXPECT_TRUE(close_task_completed);
    EXPECT_TRUE(timer_fired.load(std::memory_order_acquire));
    EXPECT_EQ(wait_result.load(std::memory_order_acquire), 2);
    EXPECT_TRUE(stop(worker));
}

TEST(Ruvia, worker_notification_close_races_producers_without_waiting_for_them) {
    constexpr std::size_t producer_count = 4;
    ruvia::worker_runtime worker;
    ruvia::worker_notification notification(worker);
    std::promise<void> wait_started;
    auto wait_started_future = wait_started.get_future();
    std::atomic<int> wait_result{0};
    auto waiting = spawn(worker, wait_until_closed(notification, &wait_started, wait_result));
    EXPECT_TRUE(worker.start());
    const bool entered = wait_started_future.wait_for(wait_timeout) == std::future_status::ready;

    std::atomic<std::size_t> ready{0};
    std::atomic<bool> begin{false};
    std::atomic<std::size_t> open_calls{0};
    std::atomic<std::size_t> closed_returns{0};
    std::vector<std::thread> producers;
    producers.reserve(producer_count);
    for (std::size_t index = 0; index < producer_count; ++index) {
        producers.emplace_back([&] {
            ready.fetch_add(1, std::memory_order_release);
            while (!begin.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            if (notification.notify() == ruvia::worker_notification_status::closed) {
                closed_returns.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            open_calls.fetch_add(1, std::memory_order_release);
            const auto deadline_value = std::chrono::steady_clock::now() + 4s;
            while (std::chrono::steady_clock::now() < deadline_value) {
                if (notification.notify() == ruvia::worker_notification_status::closed) {
                    closed_returns.fetch_add(1, std::memory_order_relaxed);
                    return;
                }
            }
        });
    }
    const auto producer_deadline = std::chrono::steady_clock::now() + wait_timeout;
    while (ready.load(std::memory_order_acquire) != producer_count &&
           std::chrono::steady_clock::now() < producer_deadline) {
        std::this_thread::yield();
    }
    const bool producers_ready = ready.load(std::memory_order_acquire) == producer_count;
    begin.store(true, std::memory_order_release);
    const auto call_deadline = std::chrono::steady_clock::now() + wait_timeout;
    while (open_calls.load(std::memory_order_acquire) != producer_count &&
           std::chrono::steady_clock::now() < call_deadline) {
        std::this_thread::yield();
    }
    const bool producers_notified_open = open_calls.load(std::memory_order_acquire) == producer_count;
    std::atomic<bool> timer_fired{false};
    auto closer = spawn(worker, observe_close_and_timer(worker.io_context(), notification, timer_fired));

    const bool wait_completed = finish(waiting);
    const bool close_task_completed = finish(closer);
    for (auto& producer : producers) {
        producer.join();
    }
    EXPECT_TRUE(entered);
    EXPECT_TRUE(producers_ready);
    EXPECT_TRUE(producers_notified_open);
    EXPECT_TRUE(wait_completed);
    EXPECT_TRUE(close_task_completed);
    EXPECT_TRUE(timer_fired.load(std::memory_order_acquire));
    EXPECT_EQ(wait_result.load(std::memory_order_acquire), 2);
    EXPECT_TRUE(closed_returns.load(std::memory_order_relaxed) > 0);
    EXPECT_EQ(notification.notify(), ruvia::worker_notification_status::closed);
    EXPECT_TRUE(stop(worker));
}

TEST(Ruvia, worker_notification_wakes_while_public_post_queue_is_full) {
    ruvia::worker_runtime worker_value({.queue_capacity_ = 1});
    ruvia::worker_notification notification(worker_value);
    const auto worker_ref_value = worker_value.worker();
    std::promise<void> wait_started;
    auto wait_started_future = wait_started.get_future();
    std::atomic<int> wait_result{0};
    auto waiting = spawn(worker_value, wait_for_one(notification, &wait_started, &wait_result));
    EXPECT_TRUE(worker_value.start());
    const bool entered = wait_started_future.wait_for(wait_timeout) == std::future_status::ready;

    std::mutex gate_mutex;
    std::condition_variable gate_changed;
    bool release_worker = false;
    std::promise<void> blocked;
    auto blocked_future = blocked.get_future();
    const auto blocker = worker_ref_value.post([&] {
        blocked.set_value();
        std::unique_lock lock(gate_mutex);
        static_cast<void>(gate_changed.wait_until(lock,
            std::chrono::steady_clock::now() + wait_timeout, [&] { return release_worker; }));
    });
    const bool blocker_entered = blocked_future.wait_for(wait_timeout) == std::future_status::ready;

    std::atomic<bool> queued_ran{false};
    const auto queued = worker_ref_value.post([&] { queued_ran.store(true, std::memory_order_release); });
    const auto full = worker_ref_value.post([] {});
    const auto notification_result = notification.notify();
    {
        const std::lock_guard lock(gate_mutex);
        release_worker = true;
    }
    gate_changed.notify_all();

    const bool wait_completed = finish(waiting);
    auto closer = spawn(worker_value, close_notification(notification));
    const bool close_completed = finish(closer);

    EXPECT_TRUE(entered);
    EXPECT_TRUE(blocker.accepted());
    EXPECT_TRUE(blocker_entered);
    EXPECT_TRUE(queued.accepted());
    EXPECT_EQ(full.status(), ruvia::post_status::queue_full);
    EXPECT_TRUE(notification_result == ruvia::worker_notification_status::notified ||
                notification_result == ruvia::worker_notification_status::coalesced);
    EXPECT_TRUE(wait_completed);
    EXPECT_TRUE(close_completed);
    EXPECT_TRUE(queued_ran.load(std::memory_order_acquire));
    EXPECT_EQ(wait_result.load(std::memory_order_acquire), 1);
    EXPECT_TRUE(stop(worker_value));
}

TEST(Ruvia, worker_notification_startup_validation_rolls_back) {
    ruvia::worker_runtime worker;
    {
        ruvia::worker_notification cold(worker);
        auto abandoned_wait = cold_wait(cold);
        static_cast<void>(abandoned_wait);
    }

    ruvia::worker_runtime stopped;
    stopped.join();
    bool stopped_runtime_rejected = false;
    try {
        ruvia::worker_notification invalid(stopped);
    } catch (const std::invalid_argument&) {
        stopped_runtime_rejected = true;
    }

    bool invalid_timeout_rejected = false;
    try {
        ruvia::worker_notification invalid(worker, {.startup_timeout_ = 0ms});
    } catch (const std::invalid_argument&) {
        invalid_timeout_rejected = true;
    }
    bool unbounded_timeout_rejected = false;
    try {
        ruvia::worker_notification invalid(worker, {.startup_timeout_ = 61s});
    } catch (const std::invalid_argument&) {
        unbounded_timeout_rejected = true;
    }

    EXPECT_TRUE(stopped_runtime_rejected);
    EXPECT_TRUE(invalid_timeout_rejected);
    EXPECT_TRUE(unbounded_timeout_rejected);
    EXPECT_TRUE(stop(worker));
}

TEST(Ruvia, worker_notification_borrows_event_loop) {
    static_assert(std::is_constructible_v<ruvia::worker_notification, ruvia::event_loop&>);
    static_assert(std::is_constructible_v<ruvia::worker_notification, const ruvia::event_loop&>);
    static_assert(!std::is_constructible_v<ruvia::worker_notification, ruvia::event_loop&&>);
    static_assert(!std::is_constructible_v<ruvia::worker_notification, const ruvia::event_loop&&>);
    static_assert(std::is_constructible_v<ruvia::worker_notification, ruvia::event_loop&, ruvia::worker_notification_options>);
    static_assert(std::is_constructible_v<ruvia::worker_notification, const ruvia::event_loop&, ruvia::worker_notification_options>);
    static_assert(!std::is_constructible_v<ruvia::worker_notification, ruvia::event_loop&&, ruvia::worker_notification_options>);
    static_assert(!std::is_constructible_v<ruvia::worker_notification, const ruvia::event_loop&&, ruvia::worker_notification_options>);
    asio::io_context io_context;
    auto attachment = ruvia::attach_event_loop(io_context, {.queue_capacity_ = 8});
    const auto loop = attachment.loop();
    ruvia::worker_notification notification(loop);
    EXPECT_EQ(notification.notify(), ruvia::worker_notification_status::notified);
    std::atomic<int> wait_result{0};
    auto waiting = spawn(
        loop, wait_close_and_stop_attachment(notification, attachment, wait_result));
    attachment.run();
    EXPECT_TRUE(finish(waiting));
    EXPECT_EQ(wait_result.load(std::memory_order_acquire), 1);
}

}  // namespace
