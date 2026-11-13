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
#include <cstdio>
#include <exception>
#include <functional>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <semaphore>
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

#include "worker_task_fixture.h"

#ifndef _WIN32
#include <unistd.h>
#endif

#include <unordered_map>

#include <asio/detached.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/ip/udp.hpp>

#if defined(_WIN32)
#include <windows.h>
#endif

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

namespace worker_runtime_test {

using state = ruvia::worker_runtime_state;

// take_failure() marks it observed, so it is not offered on a const runtime.
template <typename runtime_type>
concept failure_readable_through_const = requires(const runtime_type& runtime_value) { runtime_value.take_failure(); };
static_assert(!failure_readable_through_const<ruvia::worker_runtime>);
static_assert(requires(ruvia::worker_runtime& runtime_value) { runtime_value.take_failure(); });

TEST(Ruvia, worker_runtime_hook_moves_can_query_failure_during_configuration) {
    ruvia::worker_runtime runtime;
    bool armed = false;
    bool started = false;
    struct hook final {
        ruvia::worker_runtime* runtime_;
        bool* armed_;
        bool* started_;

        hook(ruvia::worker_runtime& runtime_value, bool& armed_value, bool* started_value)
            : runtime_(&runtime_value),
              armed_(&armed_value),
              started_(started_value) {}
        hook(const hook&) = delete;
        hook(hook&& other) noexcept
            : runtime_(other.runtime_),
              armed_(other.armed_),
              started_(other.started_) {
            if (*armed_) {
                (void)runtime_->take_failure();
            }
        }
        void operator()() {
            if (started_ != nullptr) {
                *started_ = true;
                runtime_->mark_ready();
            } else {
                runtime_->finalize();
            }
        }
    };
    ruvia::worker_runtime_hooks hooks;
    hooks.startup_ = hook(runtime, armed, &started);
    hooks.stop_admission_ = hook(runtime, armed, nullptr);
    armed = true;
    runtime.configure(std::move(hooks));
    EXPECT_TRUE(runtime.start());
    EXPECT_TRUE(runtime.wait_until_ready());
    runtime.request_stop();
    runtime.join();
    EXPECT_TRUE(started);
}

TEST(Ruvia, worker_runtime_finalize_hook_move_can_reenter_finalize) {
    ruvia::worker_runtime runtime;
    bool armed = false;
    bool invoked = false;
    struct hook final {
        ruvia::worker_runtime* runtime_;
        bool* armed_;
        bool* invoked_;

        hook(ruvia::worker_runtime& owner, bool& armed_value, bool& invoked_value)
            : runtime_(&owner),
              armed_(&armed_value),
              invoked_(&invoked_value) {}
        hook(const hook&) = delete;
        hook(hook&& other) noexcept
            : runtime_(other.runtime_),
              armed_(other.armed_),
              invoked_(other.invoked_) {
            if (*armed_) {
                runtime_->finalize();
            }
        }
        ~hook() {
            if (*armed_) {
                runtime_->finalize();
            }
        }
        void operator()() {
            *invoked_ = true;
        }
    };
    runtime.configure({.finalize_ = hook(runtime, armed, invoked)});
    armed = true;
    runtime.finalize();
    runtime.join();
    EXPECT_TRUE(invoked);
}

TEST(Ruvia, worker_runtime_drains_accepted_work_before_start) {
    ruvia::worker_runtime runtime;
    const auto worker_value = runtime.worker();
    const auto caller = std::this_thread::get_id();
    bool owner_affine{};
    bool ran{};
    EXPECT_TRUE(worker_value.post([&] {
                                owner_affine = worker_value.is_current() && std::this_thread::get_id() != caller;
                                ran = true;
                            })
            .accepted());
    runtime.join();
    EXPECT_TRUE(ran);
    EXPECT_TRUE(owner_affine);
    EXPECT_EQ(runtime.state(), state::stopped);
    EXPECT_TRUE(!worker_value.valid());
    EXPECT_TRUE(!worker_value.post([] {}).accepted());
    EXPECT_TRUE(!runtime.start());
}

TEST(Ruvia, worker_runtime_cold_stop_cancels_io_and_reclaims_inputs_before_shutdown) {
    ruvia::worker_runtime runtime;
    const auto worker = runtime.worker();
    asio::steady_timer timer(runtime.io_context());
    // 上下文分配的控制块在 worker 释放强引用，在调用线程释放最后的弱引用。
    auto payload = std::allocate_shared<int>(asio::execution_context::allocator<int>(runtime.io_context()), 42);
    std::weak_ptr<int> lifetime = payload;
    bool completion_on_owner = false;
    bool shutdown_after_completion = false;
    timer.expires_after(std::chrono::hours(1));
    timer.async_wait([owned = std::move(payload), &worker, &completion_on_owner](const asio::error_code& error) {
        completion_on_owner = worker.is_current() && error == asio::error::operation_aborted && *owned == 42;
    });
    runtime.configure({
        .stop_admission_ = [&] {
            timer.cancel();
            runtime.finalize(); },
        .shutdown_ = [&] { shutdown_after_completion = worker.is_current() && completion_on_owner && lifetime.expired(); },
    });
    runtime.request_stop();
    EXPECT_TRUE(!worker.accepting());
    EXPECT_TRUE(!runtime.wait_until_ready());
    EXPECT_TRUE(!runtime.wait_until_serving());
    runtime.join();
    EXPECT_TRUE(completion_on_owner);
    EXPECT_TRUE(shutdown_after_completion);
    EXPECT_TRUE(lifetime.expired());
    EXPECT_EQ(runtime.state(), state::stopped);
}

TEST(Ruvia, worker_runtime_keeps_owner_until_external_producers_quiesce) {
    ruvia::worker_runtime runtime({.io_policy_ = ruvia::worker_io_policy::single_owner});
    const auto worker_value = runtime.worker();
    std::promise<void> ready;
    std::promise<void> admission_stopped;
    std::atomic<bool> stopped_on_owner{};
    bool finalized_on_owner{};
    bool shutdown_on_owner{};
    std::vector<int> phases;
    runtime.configure({
        .startup_ = [&] {
            phases.push_back(1);
            ready.set_value(); },
        .stop_admission_ = [&] {
            phases.push_back(2);
            stopped_on_owner.store(worker_value.is_current());
            admission_stopped.set_value(); },
        .finalize_ = [&] {
            phases.push_back(3);
            finalized_on_owner = worker_value.is_current(); },
        .shutdown_ = [&] {
            phases.push_back(4);
            shutdown_on_owner = worker_value.is_current(); },
    });
    EXPECT_TRUE(runtime.start());
    ready.get_future().get();
    runtime.request_stop();
    admission_stopped.get_future().get();
    EXPECT_TRUE(stopped_on_owner.load());
    EXPECT_EQ(runtime.state(), state::stopping);
    EXPECT_TRUE(worker_value.valid());
    EXPECT_TRUE(!worker_value.accepting());
    EXPECT_TRUE(!runtime.post_control([] {}));
    runtime.finalize();
    runtime.join();
    EXPECT_TRUE(finalized_on_owner);
    EXPECT_TRUE(shutdown_on_owner);
    EXPECT_EQ(phases, (std::vector<int>{1, 2, 3, 4}));
    EXPECT_TRUE(!worker_value.valid());
}

TEST(Ruvia, worker_runtime_failure_cancels_then_drains_io_before_shutdown) {
    ruvia::worker_runtime runtime;
    const auto worker_value = runtime.worker();
    asio::steady_timer timer(runtime.io_context());
    bool failure_on_owner{};
    bool completion_on_owner{};
    bool shutdown_after_completion{};
    std::exception_ptr reported;
    std::vector<int> phases;
    runtime.configure({
        .startup_ = [&] {
            timer.expires_after(std::chrono::hours(1));
            timer.async_wait([&](const asio::error_code& error) {
                completion_on_owner = worker_value.is_current() && error == asio::error::operation_aborted;
                phases.push_back(3);
            });
            throw std::runtime_error("owner initialization failed"); },
        .stop_admission_ = [&] {
            phases.push_back(2);
            timer.cancel();
            runtime.finalize(); },
        .failure_ = [&](std::exception_ptr error) noexcept {
            phases.push_back(1);
            failure_on_owner = worker_value.is_current();
            reported = error; },
        .shutdown_ = [&] {
            shutdown_after_completion = completion_on_owner && worker_value.is_current();
            phases.push_back(4); },
    });
    EXPECT_TRUE(runtime.start());
    runtime.join();
    EXPECT_TRUE(failure_on_owner);
    EXPECT_TRUE(shutdown_after_completion);
    EXPECT_EQ(reported, runtime.take_failure());
    EXPECT_EQ(phases, (std::vector<int>{1, 2, 3, 4}));
    EXPECT_TRUE(runtime.take_failure() != nullptr);
    EXPECT_TRUE(ruvia::test::throws_on([&] { (void)runtime.start(); }));
}

TEST(Ruvia, worker_runtime_concurrent_stop_and_join_share_one_barrier) {
    for (int iteration = 0; iteration != 32; ++iteration) {
        ruvia::worker_runtime runtime;
        std::atomic<int> stops{};
        std::atomic<int> shutdowns{};
        runtime.configure({
            .stop_admission_ = [&] {
                ++stops;
                runtime.finalize(); },
            .shutdown_ = [&] { ++shutdowns; },
        });
        if (iteration % 2 == 0) {
            EXPECT_TRUE(runtime.start());
        }
        std::thread first([&] {
            runtime.request_stop();
            runtime.join();
        });
        std::thread second([&] {
            runtime.request_stop();
            runtime.join();
        });
        first.join();
        second.join();
        EXPECT_EQ(stops.load(), 1);
        EXPECT_EQ(shutdowns.load(), 1);
        EXPECT_EQ(runtime.state(), state::stopped);
    }
}

TEST(Ruvia, worker_runtime_finalize_before_start_retires_on_owner) {
    ruvia::worker_runtime runtime;
    const auto worker_value = runtime.worker();
    bool startup_ran{};
    bool stop_ran{};
    bool finalizer_ran{};
    bool shutdown_ran{};
    runtime.configure({
        .startup_ = [&] { startup_ran = true; },
        .stop_admission_ = [&] { stop_ran = worker_value.is_current(); },
        .finalize_ = [&] { finalizer_ran = worker_value.is_current(); },
        .shutdown_ = [&] { shutdown_ran = worker_value.is_current(); },
    });
    runtime.finalize();
    runtime.join();
    EXPECT_TRUE(!startup_ran);
    EXPECT_TRUE(stop_ran);
    EXPECT_TRUE(finalizer_ran);
    EXPECT_TRUE(shutdown_ran);
}

TEST(Ruvia, worker_runtime_rejects_self_join_without_losing_cleanup) {
    ruvia::worker_runtime runtime;
    bool rejected{};
    runtime.configure({
        .startup_ = [&] { rejected = ruvia::test::throws_on([&] { runtime.join(); }); },
    });
    EXPECT_TRUE(runtime.start());
    runtime.join();
    EXPECT_TRUE(rejected);
    EXPECT_EQ(runtime.state(), state::stopped);
}

TEST(Ruvia, worker_runtime_cold_shutdown_can_repeat_stop_and_rejects_self_join) {
    ruvia::worker_runtime runtime;
    int shutdowns = 0;
    bool self_join_rejected = false;
    bool admission_closed = false;
    runtime.configure({.shutdown_ = [&] {
        ++shutdowns;
        runtime.request_stop();
        admission_closed = !runtime.worker().accepting();
        self_join_rejected = ruvia::test::throws_on([&] { runtime.join(); });
    }});
    runtime.request_stop();
    runtime.join();
    EXPECT_EQ(shutdowns, 1);
    EXPECT_TRUE(self_join_rejected);
    EXPECT_TRUE(admission_closed);
    EXPECT_EQ(runtime.state(), state::stopped);
}

TEST(Ruvia, worker_runtime_serves_only_after_owner_published_readiness) {
    ruvia::worker_runtime runtime;
    const auto worker_value = runtime.worker();
    std::atomic<bool> served_on_owner{};
    std::atomic<int> serves{};
    runtime.configure({
        .startup_ = [&] {
            // Readiness may follow asynchronous startup work on the owner.
            EXPECT_TRUE(worker_value.post([&] { runtime.mark_ready(); }).accepted()); },
        .serve_ = [&] {
            ++serves;
            served_on_owner.store(worker_value.is_current()); },
    });
    EXPECT_TRUE(runtime.start());
    EXPECT_TRUE(runtime.wait_until_ready());
    EXPECT_EQ(serves.load(), 0);
    runtime.request_serve();
    runtime.request_serve();
    EXPECT_TRUE(runtime.wait_until_serving());
    EXPECT_EQ(serves.load(), 1);
    EXPECT_TRUE(served_on_owner.load());
    runtime.join();
    EXPECT_TRUE(runtime.take_failure() == nullptr);
}

TEST(Ruvia, worker_runtime_stop_before_readiness_aborts_startup) {
    ruvia::worker_runtime runtime;
    bool served{};
    runtime.configure({
        .serve_ = [&] { served = true; },
    });
    EXPECT_TRUE(runtime.start());
    runtime.request_stop();
    EXPECT_TRUE(!runtime.wait_until_ready());
    EXPECT_TRUE(!runtime.wait_until_serving());
    runtime.join();
    EXPECT_TRUE(!served);

    ruvia::worker_runtime unlaunched;
    int shutdowns = 0;
    unlaunched.configure({.shutdown_ = [&] { ++shutdowns; }});
    unlaunched.request_stop();
    unlaunched.request_stop();
    EXPECT_TRUE(!unlaunched.wait_until_ready());
    EXPECT_TRUE(!unlaunched.wait_until_serving());
    unlaunched.join();
    EXPECT_EQ(shutdowns, 1);
}

TEST(Ruvia, worker_runtime_first_failure_wakes_waiters_and_reaches_hook_once) {
    ruvia::worker_runtime runtime;
    std::atomic<int> reports{};
    runtime.configure({
        .startup_ = [&] {
            (void)runtime.fail(std::make_exception_ptr(std::runtime_error("first")));
            (void)runtime.fail(std::make_exception_ptr(std::runtime_error("second"))); },
        .failure_ = [&](std::exception_ptr) noexcept { ++reports; },
    });
    EXPECT_TRUE(runtime.start());
    std::string message;
    try {
        (void)runtime.wait_until_ready();
    } catch (const std::runtime_error& failure) {
        message = failure.what();
    }
    runtime.join();
    EXPECT_EQ(message, "first");
    EXPECT_EQ(reports.load(), 1);
    EXPECT_EQ(runtime.state(), state::stopped);
}

TEST(Ruvia, worker_runtime_serve_failure_fails_the_runtime) {
    ruvia::worker_runtime runtime;
    runtime.configure({
        .startup_ = [&] { runtime.mark_ready(); },
        .serve_ = [] { throw std::runtime_error("serve failed"); },
    });
    EXPECT_TRUE(runtime.start());
    EXPECT_TRUE(runtime.wait_until_ready());
    runtime.request_serve();
    EXPECT_TRUE(ruvia::test::throws_on([&] { (void)runtime.wait_until_serving(); }));
    runtime.join();
    EXPECT_TRUE(runtime.take_failure() != nullptr);
}

TEST(Ruvia, worker_group_coordinates_startup_serving_and_retirement) {
    ruvia::worker_runtime first;
    ruvia::worker_runtime second;
    std::mutex order_mutex;
    std::vector<std::string> order;
    const auto record = [&](std::string_view event) {
        const std::lock_guard lock(order_mutex);
        order.emplace_back(event);
    };
    std::atomic<int> ready_count{};
    first.configure({
        .startup_ = [&] {
            ++ready_count;
            first.mark_ready(); },
        .serve_ = [&] { record("serve first"); },
    });
    second.configure({
        .startup_ = [&] {
            ++ready_count;
            second.mark_ready(); },
        .serve_ = [&] { record("serve second"); },
    });
    {
        ruvia::worker_group group;
        group.add(first);
        group.add(second);
        EXPECT_TRUE(group.start());
        EXPECT_EQ(ready_count.load(), 2);
        EXPECT_TRUE(group.serve());
        group.request_stop();
        EXPECT_TRUE(group.join() == nullptr);
    }
    EXPECT_EQ(order.size(), 2u);
    EXPECT_EQ(first.state(), state::stopped);
    EXPECT_EQ(second.state(), state::stopped);
}

TEST(Ruvia, worker_group_stop_before_start_launches_nothing) {
    ruvia::worker_runtime member;
    std::atomic<bool> started{};
    member.configure({.startup_ = [&] { started.store(true); }});
    ruvia::worker_group group;
    group.add(member);
    group.request_stop();
    EXPECT_TRUE(group.stop_requested());
    EXPECT_TRUE(!group.start());
    EXPECT_TRUE(group.join() == nullptr);
    EXPECT_TRUE(!started.load());
}

TEST(Ruvia, worker_group_cold_shutdown_hooks_observe_and_reenter_stop) {
    ruvia::worker_runtime first;
    ruvia::worker_runtime second;
    ruvia::worker_group group;
    int shutdowns = 0;
    bool observed_stop = true;
    for (auto* member : {&first, &second}) {
        member->configure({.shutdown_ = [&] {
            observed_stop = observed_stop && group.stop_requested();
            group.request_stop();
            ++shutdowns;
        }});
        group.add(*member);
    }
    group.request_stop();
    group.request_stop();
    EXPECT_TRUE(group.join() == nullptr);
    EXPECT_TRUE(observed_stop);
    EXPECT_EQ(shutdowns, 2);
    EXPECT_EQ(first.state(), state::stopped);
    EXPECT_EQ(second.state(), state::stopped);
}

TEST(Ruvia, worker_group_stop_during_startup_aborts_readiness_and_retires_members) {
    ruvia::worker_runtime first;
    ruvia::worker_runtime second;
    ruvia::worker_group group;
    bool second_served = false;
    bool second_retired = false;
    first.configure({.startup_ = [&] {
        group.request_stop();
        first.mark_ready();
    }});
    second.configure({
        .serve_ = [&] { second_served = true; },
        .shutdown_ = [&] {
            second_retired = group.stop_requested() && second.worker().is_current();
            group.request_stop(); },
    });
    group.add(first);
    group.add(second);
    EXPECT_TRUE(!group.start());
    EXPECT_TRUE(group.join() == nullptr);
    EXPECT_TRUE(!second_served);
    EXPECT_TRUE(second_retired);
    EXPECT_EQ(first.state(), state::stopped);
    EXPECT_EQ(second.state(), state::stopped);
}

TEST(Ruvia, worker_group_cold_retirement_waits_for_producer_shutdown) {
    ruvia::worker_runtime producer;
    ruvia::worker_runtime consumer;
    ruvia::worker_group group;
    std::binary_semaphore shutdown_started{0};
    std::binary_semaphore finish_shutdown{0};
    std::atomic<bool> shutdown_finished{};
    bool consumer_after_producer = false;
    bool admissions_closed = false;
    producer.configure({.shutdown_ = [&] {
        group.request_stop();
        shutdown_started.release();
        finish_shutdown.acquire();
        shutdown_finished.store(true);
    }});
    consumer.configure({.shutdown_ = [&] {
        consumer_after_producer = shutdown_finished.load() && producer.state() == state::stopped;
    }});
    group.add(producer);
    group.add(consumer);
    std::thread observer([&] {
        shutdown_started.acquire();
        group.request_stop();
        admissions_closed = !producer.worker().accepting() && !consumer.worker().accepting();
        finish_shutdown.release();
    });
    const auto failure = group.join();
    observer.join();
    EXPECT_TRUE(failure == nullptr);
    EXPECT_TRUE(admissions_closed);
    EXPECT_TRUE(consumer_after_producer);
    EXPECT_EQ(consumer.state(), state::stopped);
}

TEST(Ruvia, worker_group_external_failure_preserves_cause_while_hook_reenters_stop) {
    ruvia::worker_runtime producer;
    ruvia::worker_runtime consumer;
    ruvia::worker_group group;
    std::binary_semaphore failure_started{0};
    std::binary_semaphore finish_failure{0};
    bool stop_observed = false;
    bool accepted_work_ran = false;
    bool shutdown_on_owner = false;
    const auto error = std::make_exception_ptr(std::runtime_error("external producer failure"));
    producer.configure({
        .failure_ = [&](std::exception_ptr reported) noexcept {
            stop_observed = group.stop_requested() && reported == error;
            group.request_stop();
            failure_started.release();
            finish_failure.acquire(); },
        .shutdown_ = [&] { shutdown_on_owner = producer.worker().is_current(); },
    });
    group.add(producer);
    group.add(consumer);
    EXPECT_TRUE(producer.worker().post([&] { accepted_work_ran = true; }).accepted());
    std::thread reporter([&] { static_cast<void>(producer.fail(error)); });
    failure_started.acquire();
    group.request_stop();
    const bool admissions_closed = !producer.worker().accepting() && !consumer.worker().accepting();
    std::string message;
    try {
        (void)group.start();
    } catch (const std::runtime_error& failure) {
        message = failure.what();
    }
    finish_failure.release();
    reporter.join();
    const auto failure = group.join();
    EXPECT_TRUE(stop_observed);
    EXPECT_TRUE(admissions_closed);
    EXPECT_EQ(message, "external producer failure");
    EXPECT_EQ(failure, error);
    EXPECT_TRUE(accepted_work_ran);
    EXPECT_TRUE(shutdown_on_owner);
    EXPECT_EQ(consumer.state(), state::stopped);
}

TEST(Ruvia, worker_group_rethrows_the_first_member_failure) {
    ruvia::worker_runtime healthy;
    ruvia::worker_runtime failing;
    healthy.configure({.startup_ = [&] { healthy.mark_ready(); }});
    failing.configure({.startup_ = [] { throw std::runtime_error("member startup failed"); }});
    ruvia::worker_group group;
    group.add(healthy);
    group.add(failing);
    std::string message;
    try {
        (void)group.start();
    } catch (const std::runtime_error& failure) {
        message = failure.what();
    }
    EXPECT_EQ(message, "member startup failed");
    const auto failure = group.join();
    EXPECT_TRUE(failure != nullptr);
    EXPECT_EQ(healthy.state(), state::stopped);
}

TEST(Ruvia, worker_group_join_returns_the_callers_primary_failure) {
    ruvia::worker_runtime failing;
    failing.configure({.startup_ = [] { throw std::runtime_error("member startup failed"); }});
    ruvia::worker_group group;
    group.add(failing);
    bool start_threw = false;
    try {
        (void)group.start();
    } catch (const std::runtime_error&) {
        start_threw = true;
    }
    const auto primary = std::make_exception_ptr(std::logic_error("start hook failed"));
    EXPECT_TRUE(start_threw);
    EXPECT_TRUE(group.join(primary) == primary);
    EXPECT_TRUE(group.join() == primary);
    EXPECT_EQ(failing.state(), state::stopped);
}

TEST(Ruvia, worker_group_serve_failure_closes_every_members_admission) {
    ruvia::worker_runtime producer;
    ruvia::worker_runtime consumer;
    std::atomic<int> failure_reports{};
    std::atomic<bool> consumer_served{};
    producer.configure({
        .startup_ = [&] { producer.mark_ready(); },
        .serve_ = [&] {
            while (!consumer_served.load()) {
                std::this_thread::yield();
            }
            throw std::runtime_error("producer serve failed"); },
        .failure_ = [&](std::exception_ptr) noexcept { ++failure_reports; },
    });
    consumer.configure({
        .startup_ = [&] { consumer.mark_ready(); },
        .serve_ = [&] { consumer_served.store(true); },
        // The consumer stays stopping until its producer joins.
        .stop_admission_ = [] {},
    });
    ruvia::worker_group group;
    group.add(consumer);
    group.add(producer);
    const bool started = group.start();
    std::string message;
    try {
        (void)group.serve();
    } catch (const std::runtime_error& failure) {
        message = failure.what();
    }
    const bool group_stopping = group.stop_requested();
    const auto producer_state = producer.state();
    const auto consumer_state = consumer.state();
    const bool producer_accepting = producer.worker().accepting();
    const bool consumer_accepting = consumer.worker().accepting();
    const bool consumer_accepted_work = consumer.worker().post([] {}).accepted();
    consumer.finalize();
    const auto failure = group.join();

    EXPECT_TRUE(started);
    EXPECT_TRUE(consumer_served.load());
    EXPECT_EQ(message, "producer serve failed");
    EXPECT_TRUE(group_stopping);
    EXPECT_TRUE(producer_state == state::stopping || producer_state == state::stopped);
    EXPECT_EQ(consumer_state, state::stopping);
    EXPECT_TRUE(!producer_accepting && !consumer_accepting);
    EXPECT_TRUE(!consumer_accepted_work);
    EXPECT_TRUE(failure != nullptr);
    EXPECT_EQ(failure_reports.load(), 1);
    EXPECT_EQ(producer.state(), state::stopped);
    EXPECT_EQ(consumer.state(), state::stopped);
}

TEST(Ruvia, worker_group_member_exit_wakes_an_earlier_asynchronous_startup) {
    for (const bool with_failure : {false, true}) {
        ruvia::worker_runtime pending;
        ruvia::worker_runtime failing;
        asio::steady_timer startup_timer(pending.io_context());
        std::promise<void> startup_waiting;
        auto startup_waiting_future = startup_waiting.get_future();
        std::atomic<bool> startup_cancelled{};
        std::atomic<int> failure_reports{};
        std::weak_ptr<int> weak_payload;
        pending.configure({
            .startup_ = [&] {
            const auto payload = std::make_shared<int>(42);
            weak_payload = payload;
            startup_timer.expires_after(std::chrono::hours(1));
            startup_timer.async_wait([&, payload](const asio::error_code& error) {
                startup_cancelled.store(error == asio::error::operation_aborted && *payload == 42);
                if (!error) {
                    pending.mark_ready();
                }
            });
            startup_waiting.set_value(); },
            .stop_admission_ = [&] {
            startup_timer.cancel();
            pending.finalize(); },
        });
        failing.configure({
            .startup_ = [&] {
            startup_waiting_future.wait();
            if (with_failure) {
                throw std::runtime_error("peer startup failed");
            }
            failing.request_stop(); },
            .failure_ = [&](std::exception_ptr) noexcept { ++failure_reports; },
        });
        ruvia::worker_group group;
        group.add(pending);
        group.add(failing);
        std::promise<void> start_returned;
        auto start_returned_future = start_returned.get_future();
        std::atomic<bool> watchdog_fired{};
        std::thread watchdog([&] {
            if (start_returned_future.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
                watchdog_fired.store(true);
                group.request_stop();
            }
        });
        std::string message;
        bool started = false;
        try {
            started = group.start();
        } catch (const std::runtime_error& failure) {
            message = failure.what();
        }
        start_returned.set_value();
        watchdog.join();
        const bool group_stopping = group.stop_requested();
        const bool pending_accepting = pending.worker().accepting();
        const auto failure = group.join();

        EXPECT_TRUE(!watchdog_fired.load());
        EXPECT_TRUE(!started);
        EXPECT_EQ(message, with_failure ? "peer startup failed" : "worker stopped before the group was ready");
        EXPECT_TRUE(group_stopping);
        EXPECT_TRUE(!pending_accepting);
        EXPECT_EQ(failure != nullptr, with_failure);
        EXPECT_TRUE(startup_cancelled.load());
        EXPECT_TRUE(weak_payload.expired());
        EXPECT_EQ(failure_reports.load(), with_failure ? 1 : 0);
        EXPECT_EQ(pending.state(), state::stopped);
        EXPECT_EQ(failing.state(), state::stopped);
    }
}

TEST(Ruvia, worker_group_membership_is_exclusive_and_closes_at_launch_or_stop) {
    ruvia::worker_runtime member;
    ruvia::worker_runtime second;
    ruvia::worker_runtime third;
    member.configure({.startup_ = [&] { member.mark_ready(); }});
    ruvia::worker_group group;
    group.add(member);
    const bool duplicate_rejected = ruvia::test::throws_on([&] { group.add(member); });
    bool other_group_rejected = false;
    bool stopped_group_rejected = false;
    {
        ruvia::worker_group other;
        other_group_rejected = ruvia::test::throws_on([&] { other.add(member); });
        other.add(second);
        other.request_stop();
        stopped_group_rejected = ruvia::test::throws_on([&] { other.add(third); });
        EXPECT_TRUE(other.join() == nullptr);
    }
    const bool started = group.start();
    const bool launched_group_rejected = ruvia::test::throws_on([&] { group.add(third); });
    const auto failure = group.join();
    const auto repeated_failure = group.join();
    // Rejected additions leave the new member available to its own owner.
    ruvia::worker_group third_group;
    third_group.add(third);
    EXPECT_TRUE(third_group.join() == nullptr);
    EXPECT_TRUE(duplicate_rejected);
    EXPECT_TRUE(other_group_rejected);
    EXPECT_TRUE(stopped_group_rejected);
    EXPECT_TRUE(started);
    EXPECT_TRUE(launched_group_rejected);
    EXPECT_TRUE(failure == nullptr && repeated_failure == failure);
}

TEST(Ruvia, worker_group_destruction_releases_observations_and_preserves_runtime_owners) {
    ruvia::worker_runtime member;
    ruvia::worker_runtime peer;
    std::atomic<int> failure_reports{};
    member.configure({
        .startup_ = [&] { member.mark_ready(); },
        .failure_ = [&](std::exception_ptr) noexcept { ++failure_reports; },
    });
    peer.configure({.startup_ = [&] { peer.mark_ready(); }});
    {
        ruvia::worker_group cold_owner;
        cold_owner.add(member);
    }
    {
        ruvia::worker_group group;
        group.add(member);
        group.add(peer);
        EXPECT_TRUE(group.start());
        EXPECT_TRUE(group.serve());
    }
    const bool members_still_running = member.state() == state::running && peer.state() == state::running;
    const auto error = std::make_exception_ptr(std::runtime_error("independent runtime failure"));
    const bool failure_recorded = member.fail(error);
    const bool peer_accepting = peer.worker().post([] {}).accepted();
    member.join();
    peer.join();
    EXPECT_TRUE(members_still_running);
    EXPECT_TRUE(failure_recorded);
    EXPECT_TRUE(peer_accepting);
    EXPECT_EQ(failure_reports.load(), 1);
    EXPECT_EQ(member.take_failure(), error);
}

TEST(Ruvia, worker_group_failure_hook_can_release_completed_member_observations) {
    ruvia::worker_runtime failing;
    ruvia::worker_runtime peer;
    auto group = std::make_unique<ruvia::worker_group>();
    int failure_reports = 0;
    bool stop_observed = false;
    failing.configure({.failure_ = [&](std::exception_ptr) noexcept {
        ++failure_reports;
        stop_observed = group->stop_requested() && !peer.worker().accepting();
        // Member observations have retired before invoking the public hook.
        group.reset();
    }});
    group->add(failing);
    group->add(peer);
    const auto error = std::make_exception_ptr(std::runtime_error("externally reported failure"));
    const bool recorded = failing.fail(error);
    failing.join();
    peer.join();
    EXPECT_TRUE(recorded);
    EXPECT_TRUE(stop_observed);
    EXPECT_TRUE(group == nullptr);
    EXPECT_EQ(failure_reports, 1);
    EXPECT_EQ(failing.take_failure(), error);
}

ruvia::task<void> hold_retirement(ruvia::worker_signal& gate_value, std::promise<void>& ready) {
    ready.set_value();
    co_await gate_value.wait();
}

struct cleanup_payload final {
    cleanup_payload(const ruvia::worker_ref& worker_value, bool& destroyed)
        : worker_(worker_value),
          destroyed_(destroyed) {}
    ~cleanup_payload() {
        destroyed_ = worker_.is_current();
    }
    const ruvia::worker_ref& worker_;
    bool& destroyed_;
};

TEST(Ruvia, event_loop_cleanup_drains_owned_inputs_after_business_admission_closes) {
    ruvia::event_loop_pool pool({.loop_count_ = 1});
    const auto loop = pool.loop(0);
    const auto worker_value = loop.worker();
    ruvia::worker_signal gate(worker_value);
    std::promise<void> retirement_started;
    auto stop_callback_value = loop.on_stop([&] { return hold_retirement(gate, retirement_started); });
    pool.start();
    pool.stop();
    retirement_started.get_future().get();
    bool ran{};
    bool inputs_destroyed_on_owner{};
    EXPECT_TRUE(!loop.worker().accepting());
    EXPECT_TRUE(loop.defer_cleanup([payload = std::make_unique<cleanup_payload>(worker_value, inputs_destroyed_on_owner), &gate, &ran, &worker_value] {
        ran = worker_value.is_current();
        gate.notify();
    }));
    pool.join();
    EXPECT_TRUE(ran);
    EXPECT_TRUE(inputs_destroyed_on_owner);
    EXPECT_TRUE(!loop.defer_cleanup([] {}));
}

TEST(Ruvia, event_loop_cleanup_can_be_drained_without_starting_business_workers) {
    ruvia::event_loop_pool pool({.loop_count_ = 1});
    const auto loop = pool.loop(0);
    const auto worker_value = loop.worker();
    bool ran{};
    EXPECT_TRUE(loop.defer_cleanup([&] { ran = worker_value.is_current(); }));
    pool.join();
    EXPECT_TRUE(ran);
    EXPECT_TRUE(!loop.worker().valid());
}

TEST(Ruvia, event_loop_draining_post_can_schedule_defer_cleanup_after_stop) {
    ruvia::event_loop_pool pool({.loop_count_ = 1});
    const auto loop = pool.loop(0);
    const auto worker_value = loop.worker();
    bool post_ran = false;
    bool cleanup_scheduled = false;
    bool cleanup_ran = false;
    EXPECT_EQ(loop.worker().post([loop, worker_value, &post_ran, &cleanup_scheduled, &cleanup_ran] {
        post_ran = worker_value.is_current();
        cleanup_scheduled = loop.defer_cleanup([worker_value, &cleanup_ran] {
            cleanup_ran = worker_value.is_current();
        });
    }),
        ruvia::post_status::accepted);
    pool.stop();
    pool.join();
    EXPECT_TRUE(post_ran);
    EXPECT_TRUE(cleanup_scheduled);
    EXPECT_TRUE(cleanup_ran);
}

TEST(Ruvia, worker_runtime_retains_control_payload_until_owner_completion) {
    ruvia::worker_runtime runtime;
    std::promise<void> initialized;
    std::promise<void> release_owner;
    auto release = release_owner.get_future();
    runtime.configure({.startup_ = [&] {
        initialized.set_value();
        release.wait();
    }});
    EXPECT_TRUE(runtime.start());
    initialized.get_future().get();
    auto payload_value = std::make_shared<int>(7);
    std::weak_ptr<int> lifetime = payload_value;
    bool consumed{};
    EXPECT_TRUE(runtime.post_control([owned = std::move(payload_value), &consumed] { consumed = *owned == 7; }));
    EXPECT_TRUE(!lifetime.expired());
    runtime.request_stop();
    release_owner.set_value();
    runtime.join();
    EXPECT_TRUE(consumed);
    EXPECT_TRUE(lifetime.expired());
}

TEST(Ruvia, worker_runtime_and_group_reject_repeated_start_and_invalid_serve) {
    {
        ruvia::worker_runtime stopped_runtime;
        stopped_runtime.request_stop();
        EXPECT_TRUE(!stopped_runtime.start());
        EXPECT_TRUE(ruvia::test::throws_on([&] { (void)stopped_runtime.start(); }));
        stopped_runtime.join();
    }
    {
        ruvia::worker_runtime member;
        member.configure({.startup_ = [&] { member.mark_ready(); }});
        ruvia::worker_group group;
        group.add(member);
        EXPECT_TRUE(ruvia::test::throws_on([&] { (void)group.serve(); }));
        EXPECT_TRUE(group.start());
        EXPECT_TRUE(group.serve());
        EXPECT_TRUE(ruvia::test::throws_on([&] { (void)group.serve(); }));
        group.request_stop();
        EXPECT_TRUE(ruvia::test::throws_on([&] { (void)group.start(); }));
        std::promise<std::exception_ptr> other_joined;
        std::thread joiner([&] { other_joined.set_value(group.join()); });
        EXPECT_TRUE(group.join() == nullptr);
        EXPECT_TRUE(other_joined.get_future().get() == nullptr);
        joiner.join();
    }
}

ruvia::task<int> sleep_then_finish(ruvia::worker_ref worker_value, std::atomic_bool& finished) {
    // Worker shutdown cuts the sleep short; the root still finishes normally.
    static_cast<void>(co_await ruvia::sleep_for(worker_value, std::chrono::seconds(30)));
    finished = true;
    co_return 11;
}

TEST(Ruvia, WorkerRuntimeLoopGatesStopAdmissionOnItsRoots) {
    ruvia::worker_runtime runtime;
    std::atomic_bool root_finished{false};
    std::atomic_bool admitted_after_root{false};
    std::atomic_bool admission_ran{false};
    runtime.configure({
        .startup_ = [&] { runtime.mark_ready(); },
        .stop_admission_ =
            [&] {
                admitted_after_root = root_finished.load();
                admission_ran = true;
                runtime.finalize();
            },
    });
    const auto loop = runtime.loop();
    EXPECT_TRUE(loop.valid());
    EXPECT_EQ(loop.worker().id(), runtime.worker().id());
    EXPECT_EQ(runtime.loop().worker().id(), loop.worker().id());
    ASSERT_TRUE(runtime.start());
    ASSERT_TRUE(runtime.wait_until_ready());
    auto root = loop.start(sleep_then_finish(loop.worker(), root_finished));
    runtime.request_stop();
    runtime.join();
    EXPECT_TRUE(admission_ran);
    EXPECT_TRUE(admitted_after_root);
    EXPECT_EQ(std::move(root).get(), 11);
    EXPECT_FALSE(loop.valid());
    EXPECT_EQ(runtime.take_failure(), nullptr);
}

TEST(Ruvia, WorkerRuntimeLoopReportsFailuresToItsRuntime) {
    ruvia::worker_runtime runtime;
    runtime.configure({.startup_ = [&] { runtime.mark_ready(); }});
    const auto loop = runtime.loop();
    ASSERT_TRUE(runtime.start());
    ASSERT_TRUE(runtime.wait_until_ready());
    loop.report_failure(std::make_exception_ptr(std::runtime_error("loop failure")));
    runtime.join();
    EXPECT_NE(runtime.take_failure(), nullptr);
    // A stopped runtime refuses new roots through its loop.
    auto refused = loop.try_start([]() -> ruvia::task<void> { co_return; }());
    EXPECT_EQ(refused.status(), ruvia::post_status::worker_stopping);
    EXPECT_NE(refused.rejected(), nullptr);
}

TEST(Ruvia, WorkerRefPostPayloadReturnsRefusedPayload) {
    ruvia::event_loop_pool loops({.loop_count_ = 1, .queue_capacity_ = 1});
    const auto worker_value = loops.loop(0).worker();
    std::promise<int> ran;
    auto accepted = worker_value.post_payload(std::make_unique<int>(3),
        [&ran](std::unique_ptr<int>&& value) -> ruvia::move_only_function<void()> {
            return [value = std::move(value), &ran] { ran.set_value(*value); };
        });
    EXPECT_TRUE(accepted.accepted());
    EXPECT_EQ(accepted.rejected(), nullptr);
    bool wrapped_refused = false;
    auto refused = worker_value.post_payload(std::make_unique<int>(4),
        [&wrapped_refused](std::unique_ptr<int>&&) -> ruvia::move_only_function<void()> {
            wrapped_refused = true;
            return [] {};
        });
    EXPECT_EQ(refused.status(), ruvia::post_status::queue_full);
    EXPECT_FALSE(wrapped_refused);
    ASSERT_NE(refused.rejected(), nullptr);
    EXPECT_EQ(**refused.rejected(), 4);
    auto unbound = ruvia::worker_ref{}.post_payload(std::make_unique<int>(5),
        [](std::unique_ptr<int>&&) -> ruvia::move_only_function<void()> { return [] {}; });
    EXPECT_EQ(unbound.status(), ruvia::post_status::worker_stopping);
    EXPECT_EQ(*std::move(unbound).take_rejected(), 5);
    loops.start();
    auto future = ran.get_future();
    ASSERT_EQ(future.wait_for(std::chrono::seconds(10)), std::future_status::ready);
    EXPECT_EQ(future.get(), 3);
    loops.stop();
    loops.join();
}

}  // namespace worker_runtime_test

namespace ruvia::test::task_scope_tests {

ruvia::task<void> increment(ruvia::worker_ref worker_value, int& value) {
    static_cast<void>(co_await ruvia::sleep_for(worker_value, std::chrono::milliseconds(1)));
    ++value;
    co_return;
}

ruvia::task<void> fail_worker_runtime() {
    throw std::runtime_error("child failed");
    co_return;
}

ruvia::task<void> no_op() {
    co_return;
}

ruvia::task<void> observe_join(ruvia::task_scope::join_operation join, bool& completed) {
    co_await std::move(join);
    completed = true;
}

ruvia::task<void> join_and_destroy_on_failure(std::optional<ruvia::task_scope>& primary,
    bool& completed_after_node_release) {
    bool caught = false;
    try {
        co_await primary->join();
    } catch (const std::runtime_error& error) {
        caught = std::string_view(error.what()) == "child failed";
    }
    completed_after_node_release = caught && primary->size() == 0;
    primary.reset();
}

ruvia::task<void> verify_stop_callback_reentrant_join_waits_for_finish(
    ruvia::worker_ref worker_value, bool& success) {
    std::optional<ruvia::task_scope> primary(std::in_place, worker_value);
    ruvia::task_scope observer(worker_value);
    bool active_during_stop_callback = false;
    bool completed_after_node_release = false;
    ruvia::stop_registration registration;
    primary->token().register_callback(registration, [&] {
        active_during_stop_callback = primary.has_value() && primary->size() == 1;
        observer.spawn(join_and_destroy_on_failure(
            primary, completed_after_node_release));
    });
    primary->spawn(fail_worker_runtime());
    while (primary.has_value()) {
        static_cast<void>(co_await ruvia::sleep_for(worker_value, std::chrono::milliseconds(1)));
    }
    co_await observer.join();
    success = active_during_stop_callback && completed_after_node_release &&
              !primary.has_value();
}

ruvia::task<void> wait_for_stop(ruvia::worker_ref worker_value, ruvia::stop_token token, bool& stopped) {
    stopped = co_await ruvia::sleep_for(worker_value, std::chrono::hours(1), std::move(token)) == ruvia::timer_sleep_result::stop_requested;
}

ruvia::task<void> exercise(ruvia::worker_ref worker_value, bool& success) {
    {
        ruvia::task_scope empty_scope(worker_value);
        if (!empty_scope.empty()) {
            co_return;
        }
        {
            auto discarded_cold_join = empty_scope.join();
            static_cast<void>(discarded_cold_join);
        }
        co_await empty_scope.join();
        bool second_join_rejected = false;
        try {
            co_await empty_scope.join();
        } catch (const std::logic_error&) {
            second_join_rejected = true;
        }
        bool spawn_after_join_rejected = false;
        try {
            empty_scope.spawn(no_op());
        } catch (const std::logic_error&) {
            spawn_after_join_rejected = true;
        }
        if (!second_join_rejected || !spawn_after_join_rejected) {
            co_return;
        }
    }

    {
        ruvia::task_scope empty_task_scope(worker_value);
        auto moved_from = no_op();
        auto retained = std::move(moved_from);
        static_cast<void>(retained);
        bool empty_task_rejected = false;
        try {
            empty_task_scope.spawn(std::move(moved_from));
        } catch (const std::logic_error&) {
            empty_task_rejected = true;
        }
        co_await empty_task_scope.join();
        if (!empty_task_rejected || empty_task_scope.size() != 0) {
            co_return;
        }
    }

    {
        int completed = 0;
        ruvia::task_scope cold_join_scope(worker_value);
        cold_join_scope.spawn(increment(worker_value, completed));
        auto first_join = cold_join_scope.join();
        std::optional<ruvia::task_scope::join_operation> second_join;
        bool spawn_before_join_started = false;
        try {
            second_join.emplace(cold_join_scope.join());
        } catch (const std::logic_error&) {
        }
        try {
            cold_join_scope.spawn(increment(worker_value, completed));
            spawn_before_join_started = true;
        } catch (const std::logic_error&) {
        }
        co_await std::move(first_join);
        bool repeated_start_rejected = false;
        if (second_join) {
            try {
                co_await std::move(*second_join);
            } catch (const std::logic_error&) {
                repeated_start_rejected = true;
            }
        }
        if (!spawn_before_join_started || !repeated_start_rejected || completed != 2) {
            co_return;
        }
    }

    {
        int completed = 0;
        ruvia::task_scope running_scope(worker_value);
        auto first_join = running_scope.join();
        auto competing_join = running_scope.join();
        running_scope.spawn(increment(worker_value, completed));
        bool join_completed = false;
        ruvia::task_scope observer(worker_value);
        observer.spawn(observe_join(std::move(first_join), join_completed));
        bool competing_join_rejected = false;
        try {
            co_await std::move(competing_join);
        } catch (const std::logic_error&) {
            competing_join_rejected = true;
        }
        bool spawn_during_join_rejected = false;
        try {
            running_scope.spawn(no_op());
        } catch (const std::logic_error&) {
            spawn_during_join_rejected = true;
        }
        co_await observer.join();
        if (!join_completed || !competing_join_rejected || !spawn_during_join_rejected || completed != 1) {
            co_return;
        }
    }

    {
        ruvia::stop_token retained;
        int notifications = 0;
        {
            ruvia::task_scope scope(worker_value);
            const auto& observed = scope;
            retained = observed.token();
            ruvia::stop_registration registration;
            retained.register_callback(registration, [&] { ++notifications; });
            scope.request_stop();
            scope.request_stop();
            if (observed.token() != retained || !observed.stop_requested() ||
                !retained.stop_requested() || notifications != 1) {
                co_return;
            }
        }
        if (!retained.stop_requested() || notifications != 1) {
            co_return;
        }
    }

    {
        ruvia::task_scope cancelled_scope(worker_value);
        {
            auto discarded_join = cancelled_scope.join();
            static_cast<void>(discarded_join);
        }
        bool stopped = false;
        cancelled_scope.spawn(wait_for_stop(worker_value, cancelled_scope.token(), stopped));
        auto join = cancelled_scope.join();
        cancelled_scope.request_stop();
        co_await std::move(join);
        if (!stopped || !cancelled_scope.stop_requested() || !cancelled_scope.empty() || cancelled_scope.size() != 0) {
            co_return;
        }
    }

    {
        // A scope whose children all completed may be destroyed without a
        // join; an unobserved failure is reported instead of lost.
        int completed = 0;
        {
            ruvia::task_scope unjoined_scope(worker_value);
            unjoined_scope.spawn(increment(worker_value, completed));
            unjoined_scope.spawn(fail_worker_runtime());
            // Completion is delivered after the timer pass that resumed the
            // child, so wait until every child has been retired.
            while (unjoined_scope.size() != 0) {
                static_cast<void>(co_await ruvia::sleep_for(worker_value, std::chrono::milliseconds(1)));
            }
            if (!unjoined_scope.stop_requested()) {
                co_return;
            }
        }
        if (completed != 1) {
            co_return;
        }
    }
    {
        bool scope_clean_on_retire = true;
        struct unjoined_scope_holder final {
            explicit unjoined_scope_holder(const ruvia::worker_ref& w, bool& clean)
                : clean_(clean),
                  scope_(w) {}
            ~unjoined_scope_holder() {
                if (!scope_.empty()) {
                    clean_ = false;
                }
            }
            bool& clean_;
            ruvia::task_scope scope_;
        };
        auto holder = std::make_shared<unjoined_scope_holder>(worker_value, scope_clean_on_retire);
        auto child_with_holder = [worker_value](std::shared_ptr<unjoined_scope_holder> owned, int delay_ms) -> ruvia::task<void> {
            if (delay_ms > 0) {
                static_cast<void>(co_await ruvia::sleep_for(worker_value, std::chrono::milliseconds(delay_ms)));
            }
            static_cast<void>(owned);
            co_return;
        };
        for (int index = 0; index < 16; ++index) {
            holder->scope_.spawn(child_with_holder(holder, index % 3));
        }
        std::weak_ptr<unjoined_scope_holder> weak_holder = holder;
        holder.reset();
        while (!weak_holder.expired()) {
            static_cast<void>(co_await ruvia::sleep_for(worker_value, std::chrono::milliseconds(1)));
        }
        if (!scope_clean_on_retire) {
            co_return;
        }
    }

    {
        ruvia::task_scope completed_failure_scope(worker_value);
        completed_failure_scope.spawn(fail_worker_runtime());
        static_cast<void>(co_await ruvia::sleep_for(worker_value, std::chrono::milliseconds(1)));
        bool completed_failure_observed = false;
        try {
            co_await completed_failure_scope.join();
        } catch (const std::runtime_error& error) {
            completed_failure_observed = std::string_view(error.what()) == "child failed";
        }
        if (!completed_failure_observed) {
            co_return;
        }
    }

    int calls = 0;
    ruvia::task_scope scope(worker_value);
    scope.spawn(increment(worker_value, calls));
    scope.spawn(fail_worker_runtime());
    try {
        co_await scope.join();
    } catch (const std::runtime_error& error) {
        success = calls == 1 && scope.size() == 0 && scope.stop_requested() &&
                  std::string_view(error.what()) == "child failed";
    }
}

#ifndef _WIN32
ruvia::task<void> fail_distinct() {
    throw std::logic_error("second child failed");
    co_return;
}

ruvia::task<void> join_distinct_failures(ruvia::worker_ref worker_value, bool& first_rethrown) {
    ruvia::task_scope scope(worker_value);
    scope.spawn(fail_worker_runtime());
    scope.spawn(fail_worker_runtime());
    scope.spawn(fail_distinct());
    try {
        co_await scope.join();
    } catch (const std::runtime_error& error) {
        first_rethrown = std::string_view(error.what()) == "child failed";
    }
}

// Redirects stderr, where the unhandled-failure diagnostic writes, into a
// temporary file for the guard's lifetime.
class stderr_capture final {
public:
    stderr_capture()
        : sink_(std::tmpfile()),
          saved_(::dup(STDERR_FILENO)) {
        std::fflush(stderr);
        if (sink_ != nullptr && saved_ >= 0) {
            redirected_ = ::dup2(::fileno(sink_), STDERR_FILENO) >= 0;
        }
    }
    stderr_capture(const stderr_capture&) = delete;
    stderr_capture& operator=(const stderr_capture&) = delete;
    ~stderr_capture() {
        restore();
        if (sink_ != nullptr) {
            std::fclose(sink_);
        }
    }

    // Restores stderr and returns everything written while redirected.
    [[nodiscard]] std::string finish() {
        restore();
        std::string text;
        if (sink_ == nullptr) {
            return text;
        }
        std::rewind(sink_);
        char chunk[256];
        std::size_t read_bytes = 0;
        while ((read_bytes = std::fread(chunk, 1, sizeof(chunk), sink_)) != 0) {
            text.append(chunk, read_bytes);
        }
        return text;
    }

private:
    void restore() noexcept {
        std::fflush(stderr);
        if (redirected_) {
            static_cast<void>(::dup2(saved_, STDERR_FILENO));
            redirected_ = false;
        }
        if (saved_ >= 0) {
            ::close(saved_);
            saved_ = -1;
        }
    }

    std::FILE* sink_;
    int saved_;
    bool redirected_{false};
};

// join rethrows the first child failure; a later, distinct one must still reach
// the unhandled-failure diagnostic, and one equivalent to the first must not
// be reported again.
bool verify_additional_child_failures_are_reported() {
    asio::io_context io_context;
    auto attachment = ruvia::attach_event_loop(io_context, {.queue_capacity_ = 8});
    bool first_rethrown = false;
    stderr_capture capture;
    ruvia::test::run_worker_tasks(attachment, join_distinct_failures(attachment.loop().worker(), first_rethrown));
    const auto text = capture.finish();
    return first_rethrown &&
           text.find("ruvia: additional task scope child failed: second child failed") != std::string_view::npos &&
           text.find("additional task scope child failed: child failed") == std::string_view::npos;
}
#endif

}  // namespace ruvia::test::task_scope_tests

TEST(Ruvia, TaskScopeLifecycle) {
    ruvia::stop_token retained_token;
    {
        ruvia::stop_source source;
        retained_token = source.token();
        source.request_stop();
    }
    EXPECT_TRUE(retained_token.stop_requested());

    asio::io_context io_context;
    auto attachment = ruvia::attach_event_loop(io_context, {.queue_capacity_ = 8});
    const auto worker_value = attachment.loop().worker();
    bool success = false;
    bool reentrant_stop_join_safe = false;
    ruvia::test::run_worker_tasks(attachment,
        ruvia::test::task_scope_tests::exercise(worker_value, success),
        ruvia::test::task_scope_tests::verify_stop_callback_reentrant_join_waits_for_finish(
            worker_value, reentrant_stop_join_safe));
    bool additional_failures_reported = true;
#ifndef _WIN32
    additional_failures_reported = ruvia::test::task_scope_tests::verify_additional_child_failures_are_reported();
#endif
    EXPECT_TRUE(success);
    EXPECT_TRUE(reentrant_stop_join_safe);
    EXPECT_TRUE(additional_failures_reported);
}

namespace ruvia::test::runtime_worker_tests {

static_assert(std::is_constructible_v<worker_signal, worker_ref&>);
static_assert(std::is_constructible_v<worker_signal, const worker_ref&>);
static_assert(!std::is_constructible_v<worker_signal, worker_ref&&>);
static_assert(!std::is_constructible_v<worker_signal, const worker_ref&&>);
static_assert(std::is_constructible_v<task_scope, worker_ref&>);
static_assert(std::is_constructible_v<task_scope, const worker_ref&>);
static_assert(!std::is_constructible_v<task_scope, worker_ref&&>);
static_assert(!std::is_constructible_v<task_scope, const worker_ref&&>);

struct root_assignment_result final {
    explicit root_assignment_result(int value, std::function<void()>* on_retire = nullptr,
        std::unique_ptr<root_task<root_assignment_result>> child = {}) noexcept
        : value_(value),
          on_retire_(on_retire),
          child_(std::move(child)) {}
    root_assignment_result(root_assignment_result&& other) noexcept
        : value_(other.value_),
          on_retire_(std::exchange(other.on_retire_, nullptr)),
          child_(std::move(other.child_)) {}
    root_assignment_result(const root_assignment_result&) = delete;
    root_assignment_result& operator=(const root_assignment_result&) = delete;
    root_assignment_result& operator=(root_assignment_result&&) = delete;
    ~root_assignment_result() {
        if (on_retire_ != nullptr && *on_retire_) {
            auto callback = std::exchange(*on_retire_, {});
            callback();
        }
    }

    int value_;
    std::function<void()>* on_retire_;
    std::unique_ptr<root_task<root_assignment_result>> child_;
};

task<root_assignment_result> make_root_assignment_result(root_assignment_result value, bool fail = false) {
    if (fail) {
        throw std::runtime_error("expected root task failure");
    }
    co_return std::move(value);
}

TEST(Ruvia, WorkerRootMoveAssignmentCapturesSourceBeforeRetiringOldResult) {
    std::function<void()> retirement;
    event_loop_pool loops({.loop_count_ = 1, .queue_capacity_ = 8});
    const auto loop = loops.loop(0);
    auto source = loop.start(make_root_assignment_result(root_assignment_result(42)));
    auto replacement = loop.start(make_root_assignment_result(root_assignment_result(99)));
    auto destination = loop.start(make_root_assignment_result(root_assignment_result(7, &retirement)));
    loops.start();
    source.wait();
    replacement.wait();
    destination.wait();
    loops.stop();
    loops.join();

    bool retired = false;
    retirement = [&] {
        retired = true;
        source = std::move(replacement);
    };
    destination = std::move(source);
    EXPECT_TRUE(retired);
    EXPECT_EQ(std::move(destination).get().value_, 42);
    ASSERT_TRUE(source.valid());
    EXPECT_EQ(std::move(source).get().value_, 99);
}

TEST(Ruvia, WorkerRootMoveAssignmentCanTakeSourceOwnedByOldResult) {
    for (const bool fail : {false, true}) {
        SCOPED_TRACE(fail);
        std::function<void()> retirement;
        event_loop_pool loops({.loop_count_ = 1, .queue_capacity_ = 8});
        const auto loop = loops.loop(0);
        auto child = std::make_unique<root_task<root_assignment_result>>(
            loop.start(make_root_assignment_result(root_assignment_result(42), fail)));
        auto* const source = child.get();
        auto destination = loop.start(make_root_assignment_result(
            root_assignment_result(7, &retirement, std::move(child))));
        loops.start();
        source->wait();
        destination.wait();
        loops.stop();
        loops.join();

        bool retired = false;
        retirement = [&] { retired = true; };
        destination = std::move(*source);
        EXPECT_TRUE(retired);
        if (fail) {
            EXPECT_THROW(static_cast<void>(std::move(destination).get()), std::runtime_error);
        } else {
            EXPECT_EQ(std::move(destination).get().value_, 42);
        }
        EXPECT_FALSE(destination.valid());
    }
}

}  // namespace ruvia::test::runtime_worker_tests

namespace {

struct payload_tracker final {
    void record_acquire() noexcept {
        ++allocations_;
        ++live_;
    }

    void record_release() noexcept {
        ++deallocations_;
        --live_;
    }

    [[nodiscard]] std::size_t allocation_count() const noexcept {
        return allocations_;
    }

    [[nodiscard]] std::size_t deallocation_count() const noexcept {
        return deallocations_;
    }

    [[nodiscard]] std::size_t live_allocations() const noexcept {
        return live_;
    }

    std::size_t allocations_{0};
    std::size_t deallocations_{0};
    std::size_t live_{0};
};

struct tracked_bytes final {
    tracked_bytes(payload_tracker* tracker, std::size_t count, char fill)
        : tracker_(tracker),
          bytes_(count, fill) {
        if (tracker_ != nullptr) {
            tracker_->record_acquire();
        }
    }

    tracked_bytes(const tracked_bytes& other)
        : tracker_(other.tracker_),
          bytes_(other.bytes_) {
        if (tracker_ != nullptr && !bytes_.empty()) {
            tracker_->record_acquire();
        }
    }

    tracked_bytes(tracked_bytes&& other) noexcept
        : tracker_(std::exchange(other.tracker_, nullptr)),
          bytes_(std::move(other.bytes_)) {}

    tracked_bytes& operator=(const tracked_bytes&) = delete;
    tracked_bytes& operator=(tracked_bytes&&) = delete;

    ~tracked_bytes() {
        if (tracker_ != nullptr && !bytes_.empty()) {
            tracker_->record_release();
        }
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return bytes_.size();
    }

    [[nodiscard]] char front() const noexcept {
        return bytes_.front();
    }

    [[nodiscard]] char back() const noexcept {
        return bytes_.back();
    }

    payload_tracker* tracker_{nullptr};
    std::vector<char> bytes_;
};

struct post_payload final {
    explicit post_payload(payload_tracker* resource)
        : bytes_(resource, 4096, 'x') {}
    post_payload(post_payload&&) noexcept = default;
    post_payload(const post_payload&) = delete;
    tracked_bytes bytes_;
};

struct reentrant_post_state final {
    ruvia::event_loop* loop_;
    std::unique_ptr<ruvia::event_loop_pool>* pool_;
    std::promise<void>* completion_;
    std::atomic_bool* payload_valid_;

    void shut_down_loop() noexcept {
        *loop_ = ruvia::event_loop{};
        pool_->reset();
    }
};

struct inline_reentrant_post_callable final {
    explicit inline_reentrant_post_callable(reentrant_post_state& state_value) noexcept
        : state_(&state_value) {
        payload_.fill('x');
    }
    inline_reentrant_post_callable(const inline_reentrant_post_callable& other) noexcept
        : state_(other.state_),
          payload_(other.payload_) {
        state_->shut_down_loop();
    }
    inline_reentrant_post_callable(inline_reentrant_post_callable&& other) noexcept
        : state_(other.state_),
          payload_(other.payload_) {
        state_->shut_down_loop();
    }
    ~inline_reentrant_post_callable() {
        payload_.fill('\0');
    }

    void operator()() {
        state_->payload_valid_->store(payload_.size() == 16 && payload_.front() == 'x' &&
                                          payload_.back() == 'x',
            std::memory_order_release);
        state_->completion_->set_value();
    }

    reentrant_post_state* state_;
    std::array<char, 16> payload_{};
};

struct heap_reentrant_post_callable final {
    explicit heap_reentrant_post_callable(reentrant_post_state& state_value,
        payload_tracker* resource)
        : state_(&state_value),
          payload_(resource, 4096, 'x') {}
    heap_reentrant_post_callable(const heap_reentrant_post_callable& other)
        : state_(other.state_),
          payload_(other.payload_) {
        state_->shut_down_loop();
    }
    heap_reentrant_post_callable(heap_reentrant_post_callable&& other) noexcept
        : state_(other.state_),
          payload_(std::move(other.payload_)) {
        state_->shut_down_loop();
    }
    ~heap_reentrant_post_callable() = default;

    void operator()() {
        state_->payload_valid_->store(payload_.size() == 4096 && payload_.front() == 'x' &&
                                          payload_.back() == 'x',
            std::memory_order_release);
        state_->completion_->set_value();
    }

    reentrant_post_state* state_;
    tracked_bytes payload_;
};

struct reentrant_worker_post_state final {
    std::optional<ruvia::worker_ref> worker_;
    std::unique_ptr<ruvia::event_loop_pool> pool_;
    bool retired_{false};
    bool payload_valid_{false};
    bool erase_on_destroy_{false};

    void retire() noexcept {
        if (retired_) {
            return;
        }
        retired_ = true;
        worker_.reset();
        pool_.reset();
    }
};

struct inline_worker_callable final {
    explicit inline_worker_callable(reentrant_worker_post_state& state_value) noexcept
        : state_(&state_value) {
        payload_.fill('i');
    }
    inline_worker_callable(const inline_worker_callable&) = delete;
    inline_worker_callable(inline_worker_callable&& other) noexcept
        : state_(other.state_),
          payload_(other.payload_) {
        state_->retire();
    }
    void operator()() {
        state_->payload_valid_ = payload_.front() == 'i' && payload_.back() == 'i';
    }
    reentrant_worker_post_state* state_;
    std::array<char, 16> payload_{};
};

struct heap_worker_callable final {
    explicit heap_worker_callable(reentrant_worker_post_state& state_value, payload_tracker* resource)
        : state_(&state_value),
          payload_(resource, 4096, 'h') {}
    heap_worker_callable(const heap_worker_callable& other)
        : state_(other.state_),
          payload_(other.payload_) {
        state_->retire();
    }
    heap_worker_callable(heap_worker_callable&&) noexcept = default;
    void operator()() {
        state_->payload_valid_ = payload_.size() == 4096 && payload_.front() == 'h' &&
                                 payload_.back() == 'h';
    }
    reentrant_worker_post_state* state_;
    tracked_bytes payload_;
};

struct erased_worker_callable final {
    explicit erased_worker_callable(reentrant_worker_post_state& state_value) noexcept
        : state_(&state_value) {
        payload_.fill('e');
    }
    erased_worker_callable(const erased_worker_callable&) = delete;
    erased_worker_callable(erased_worker_callable&& other) noexcept
        : state_(other.state_),
          payload_(other.payload_) {}
    ~erased_worker_callable() {
        if (state_->erase_on_destroy_) {
            state_->retire();
        }
    }
    void operator()() {
        state_->payload_valid_ = payload_.front() == 'e' && payload_.back() == 'e';
    }
    reentrant_worker_post_state* state_;
    std::array<char, 16> payload_{};
};

bool test_worker_ref_callable_lifetime() {
    const auto run_rejected = [](ruvia::post_outcome<> rejected, bool& payload_valid) {
        if (rejected != ruvia::post_status::worker_stopping || rejected.rejected() == nullptr) {
            return false;
        }
        auto task_value = std::move(rejected).take_rejected();
        ruvia::event_loop_pool recovery({.loop_count_ = 1, .queue_capacity_ = 1});
        if (recovery.loop(0).worker().post(std::move(task_value)) != ruvia::post_status::accepted) {
            return false;
        }
        recovery.start();
        recovery.join();
        return payload_valid;
    };

    {
        reentrant_worker_post_state state;
        state.pool_ = std::make_unique<ruvia::event_loop_pool>(
            ruvia::event_loop_pool_options{.loop_count_ = 1, .queue_capacity_ = 1});
        state.worker_.emplace(state.pool_->loop(0).worker());
        std::optional<inline_worker_callable> input;
        input.emplace(state);
        auto rejected = state.worker_->post(std::move(*input));
        if (!state.retired_ || state.worker_ || state.pool_ || !run_rejected(std::move(rejected), state.payload_valid_)) {
            return false;
        }
    }

    payload_tracker resource;
    {
        reentrant_worker_post_state state;
        state.pool_ = std::make_unique<ruvia::event_loop_pool>(
            ruvia::event_loop_pool_options{.loop_count_ = 1, .queue_capacity_ = 1});
        state.worker_.emplace(state.pool_->loop(0).worker());
        std::optional<heap_worker_callable> input;
        input.emplace(state, &resource);
        auto rejected = state.worker_->post(*input);
        input.reset();
        if (!state.retired_ || state.worker_ || state.pool_ ||
            !run_rejected(std::move(rejected), state.payload_valid_) ||
            resource.allocation_count() != resource.deallocation_count()) {
            return false;
        }
    }

    {
        reentrant_worker_post_state state;
        state.pool_ = std::make_unique<ruvia::event_loop_pool>(
            ruvia::event_loop_pool_options{.loop_count_ = 1, .queue_capacity_ = 1});
        state.worker_.emplace(state.pool_->loop(0).worker());
        ruvia::move_only_function<void()> input{erased_worker_callable(state)};
        state.erase_on_destroy_ = true;
        auto rejected = state.worker_->post(std::move(input));
        if (!state.retired_ || state.worker_ || state.pool_ ||
            !run_rejected(std::move(rejected), state.payload_valid_)) {
            return false;
        }
    }
    if (resource.allocation_count() != resource.deallocation_count()) {
        return false;
    }

    struct throw_on_copy final {
        explicit throw_on_copy(payload_tracker* resource_value, const std::atomic<bool>& fail)
            : bytes_(resource_value, 4096, 'c'),
              fail_(fail) {}
        throw_on_copy(const throw_on_copy& other)
            : bytes_(other.bytes_),
              fail_(other.fail_) {
            if (fail_.load(std::memory_order_relaxed)) {
                throw std::runtime_error("copy failed");
            }
        }
        throw_on_copy(throw_on_copy&&) noexcept = default;
        void operator()() const {}
        tracked_bytes bytes_;
        const std::atomic<bool>& fail_;
    };
    struct throw_on_move final {
        explicit throw_on_move(payload_tracker* resource_value, const std::atomic<bool>& fail)
            : bytes_(resource_value, 4096, 'm'),
              fail_(fail) {}
        throw_on_move(throw_on_move&& other)
            : bytes_(std::move(other.bytes_)),
              fail_(other.fail_) {
            if (fail_.load(std::memory_order_relaxed)) {
                throw std::runtime_error("move failed");
            }
        }
        void operator()() const {}
        tracked_bytes bytes_;
        const std::atomic<bool>& fail_;
    };

    asio::io_context exception_context;
    auto exception_attachment = ruvia::attach_event_loop(exception_context, {.queue_capacity_ = 1});
    const auto exception_worker = exception_attachment.loop().worker();
    payload_tracker exception_resource;
    // Keep failure injection observable at runtime, including under whole-program optimization.
    std::atomic<bool> fail_construction{true};
    {
        throw_on_copy copy_input(&exception_resource, fail_construction);
        throw_on_move move_input(&exception_resource, fail_construction);
        bool copy_threw = false;
        bool move_threw = false;
        try {
            static_cast<void>(exception_worker.post(copy_input));
        } catch (const std::runtime_error&) {
            copy_threw = true;
        }
        try {
            static_cast<void>(exception_worker.post(std::move(move_input)));
        } catch (const std::runtime_error&) {
            move_threw = true;
        }
        fail_construction.store(false, std::memory_order_relaxed);
        const auto after_exception = exception_worker.post(copy_input);
        exception_context.poll();
        if (!copy_threw || !move_threw || !exception_worker.valid() || !exception_worker.accepting() ||
            after_exception != ruvia::post_status::accepted) {
            return false;
        }
    }
    if (exception_resource.allocation_count() != exception_resource.deallocation_count()) {
        return false;
    }

    struct worker_reentry_state final {
        std::optional<ruvia::worker_ref>* source_;
        std::optional<asio::io_context>* context_;
        ruvia::event_loop_attachment* attachment_;
        bool detach_;
        bool retired_{false};
        bool payload_valid_{false};
        payload_tracker* resource_;

        void retire() noexcept {
            if (retired_) {
                return;
            }
            retired_ = true;
            source_->reset();
            if (detach_) {
                // No root task or cleanup is outstanding, so destroying the
                // context retires and detaches the worker synchronously.
                context_->reset();
            } else {
                attachment_->stop();
            }
        }
    };
    struct reentrant_worker_callable final {
        explicit reentrant_worker_callable(worker_reentry_state& state_value)
            : state_(&state_value),
              payload_(state_value.resource_, 4096, 'v') {}
        reentrant_worker_callable(const reentrant_worker_callable& other)
            : state_(other.state_),
              payload_(other.payload_) {
            state_->retire();
        }
        reentrant_worker_callable(reentrant_worker_callable&& other) noexcept
            : state_(other.state_),
              payload_(std::move(other.payload_)) {
            state_->retire();
        }
        void operator()() {
            state_->payload_valid_ = payload_.size() == 4096 && payload_.front() == 'v' &&
                                     payload_.back() == 'v';
        }
        worker_reentry_state* state_;
        tracked_bytes payload_;
    };

    const auto exercise_reentry = [](bool detach, bool use_copy) {
        payload_tracker reentry_resource;
        std::optional<asio::io_context> context;
        context.emplace();
        auto attachment = ruvia::attach_event_loop(*context, {.queue_capacity_ = 1});
        const auto worker_value = attachment.loop().worker();
        std::optional<ruvia::worker_ref> source;
        source.emplace(worker_value);
        worker_reentry_state state_value{.source_ = &source,
            .context_ = &context,
            .attachment_ = &attachment,
            .detach_ = detach,
            .resource_ = &reentry_resource};
        std::optional<reentrant_worker_callable> input;
        input.emplace(state_value);
        auto rejected = use_copy ? source->post(*input) : source->post(std::move(*input));
        if (!state_value.retired_ || source || worker_value.valid() == detach ||
            worker_value.accepting() || rejected != ruvia::post_status::worker_stopping ||
            rejected.rejected() == nullptr) {
            return false;
        }
        auto task_value = std::move(rejected).take_rejected();
        ruvia::event_loop_pool recovery({.loop_count_ = 1, .queue_capacity_ = 1});
        if (recovery.loop(0).worker().post(std::move(task_value)) != ruvia::post_status::accepted) {
            return false;
        }
        recovery.start();
        recovery.join();
        input.reset();
        return state_value.payload_valid_ && reentry_resource.allocation_count() == reentry_resource.deallocation_count();
    };
    if (!exercise_reentry(false, true) || !exercise_reentry(true, false)) {
        return false;
    }

    struct reserved_move_state final {
        const ruvia::worker_ref* worker_;
        ruvia::event_loop_attachment* attachment_;
        payload_tracker* resource_;
        int moves_{0};
        bool reservation_observed_{false};
        bool ran_{false};
    };
    struct inline_reserved_move_callable final {
        explicit inline_reserved_move_callable(reserved_move_state& state_value)
            : state_(&state_value),
              payload_(new char[16]) {
            state_->resource_->record_acquire();
            std::fill_n(payload_.get(), 16, 'r');
        }
        inline_reserved_move_callable(inline_reserved_move_callable&& other) noexcept
            : state_(other.state_),
              payload_(std::move(other.payload_)) {
            // This move transfers the callable into its already reserved node;
            // abandoning that node may cause additional cleanup moves after this point.
            if (++state_->moves_ == 3) {
                state_->reservation_observed_ =
                    state_->worker_->post([] {}) == ruvia::post_status::queue_full;
                state_->attachment_->stop();
            }
        }
        ~inline_reserved_move_callable() {
            if (payload_ != nullptr) {
                state_->resource_->record_release();
            }
        }
        void operator()() {
            state_->ran_ = payload_ != nullptr && payload_[0] == 'r' && payload_[15] == 'r';
        }

        reserved_move_state* state_;
        std::unique_ptr<char[]> payload_;
    };

    // Stopping inside the reserved move keeps the admitted callback. Draining
    // the context runs it; destroying the unrun context detaches the worker and
    // releases it without invoking it.
    const auto exercise_reserved_move = [](bool detach) {
        payload_tracker reserved_resource;
        std::optional<asio::io_context> context;
        context.emplace();
        auto attachment = ruvia::attach_event_loop(*context, {.queue_capacity_ = 1});
        const auto worker_value = attachment.loop().worker();
        reserved_move_state state_value{
            .worker_ = &worker_value, .attachment_ = &attachment, .resource_ = &reserved_resource};
        inline_reserved_move_callable input(state_value);
        const auto submitted = worker_value.post(std::move(input));
        if (submitted != ruvia::post_status::accepted || !state_value.reservation_observed_ ||
            reserved_resource.allocation_count() != 1) {
            return false;
        }
        if (detach) {
            context.reset();
            if (state_value.ran_ || worker_value.valid() || reserved_resource.deallocation_count() != 1) {
                return false;
            }
        } else {
            context->run();
            if (!state_value.ran_ || worker_value.valid() || reserved_resource.deallocation_count() != 1) {
                return false;
            }
        }
        return reserved_resource.allocation_count() == reserved_resource.deallocation_count();
    };
    if (!exercise_reserved_move(false) || !exercise_reserved_move(true)) {
        return false;
    }

    constexpr std::size_t repeated_posts = 64;
    payload_tracker repeated_resource;
    asio::io_context context;
    auto attachment = ruvia::attach_event_loop(context, {.queue_capacity_ = repeated_posts + 2});
    const auto worker_value = attachment.loop().worker();
    bool accepted_payload_valid = false;
    if (!worker_value.valid() || !worker_value.accepting() ||
        worker_value.post([] {}) != ruvia::post_status::accepted) {
        return false;
    }
    auto accepted = worker_value.post([payload = post_payload(&repeated_resource), &accepted_payload_valid]() mutable {
        accepted_payload_valid = payload.bytes_.size() == 4096 && payload.bytes_.front() == 'x' &&
                                 payload.bytes_.back() == 'x';
    });
    if (accepted != ruvia::post_status::accepted) {
        return false;
    }
    for (std::size_t index = 0; index < repeated_posts; ++index) {
        auto posted = worker_value.post([payload = post_payload(&repeated_resource)]() mutable {
            if (payload.bytes_.size() != 4096 || payload.bytes_.front() != 'x') {
                std::terminate();
            }
        });
        if (!posted.accepted()) {
            return false;
        }
    }
    // Stop drains every accepted post before the attachment retires.
    attachment.stop();
    context.run();
    if (!accepted_payload_valid || repeated_resource.allocation_count() != repeated_resource.deallocation_count()) {
        return false;
    }

    asio::io_context full_context;
    auto full_attachment = ruvia::attach_event_loop(full_context, {.queue_capacity_ = 1});
    const auto full_worker = full_attachment.loop().worker();
    payload_tracker rejected_resource;
    bool retained_payload_valid = false;
    if (full_worker.post([] {}) != ruvia::post_status::accepted) {
        return false;
    }
    auto full = full_worker.post([payload = post_payload(&rejected_resource), &retained_payload_valid]() mutable {
        retained_payload_valid = payload.bytes_.size() == 4096 && payload.bytes_.front() == 'x' &&
                                 payload.bytes_.back() == 'x';
    });
    if (full != ruvia::post_status::queue_full || full.rejected() == nullptr) {
        return false;
    }
    auto retained = std::move(full).take_rejected();
    for (int attempt_value = 0; attempt_value < 3; ++attempt_value) {
        auto again = full_worker.post(std::move(retained));
        if (again != ruvia::post_status::queue_full || again.rejected() == nullptr ||
            rejected_resource.allocation_count() == rejected_resource.deallocation_count()) {
            return false;
        }
        retained = std::move(again).take_rejected();
    }
    full_context.poll();
    const auto retried = full_worker.post(std::move(retained));
    if (retried != ruvia::post_status::accepted) {
        return false;
    }
    full_attachment.stop();
    full_context.run();
    if (!retained_payload_valid || rejected_resource.allocation_count() != rejected_resource.deallocation_count()) {
        return false;
    }

    asio::io_context closed_context;
    auto closed_attachment = ruvia::attach_event_loop(closed_context, {.queue_capacity_ = 1});
    const auto closed_worker = closed_attachment.loop().worker();
    closed_attachment.stop();
    if (!closed_worker.valid() || closed_worker.accepting() ||
        closed_worker.post([] {}) != ruvia::post_status::worker_stopping) {
        return false;
    }
    closed_attachment.run();
    if (closed_worker.valid() || closed_worker.accepting() || closed_attachment.valid()) {
        return false;
    }
    ruvia::worker_ref invalid;
    return !invalid.valid() && !invalid.accepting() &&
           invalid.post([] {}) == ruvia::post_status::worker_stopping &&
           repeated_resource.allocation_count() > 0 &&
           repeated_resource.allocation_count() == repeated_resource.deallocation_count();
}

bool test_post_factory_keeps_endpoint_through_owner_retirement() {
    auto context = std::make_unique<asio::io_context>();
    std::optional<ruvia::event_loop_attachment> attachment;
    attachment.emplace(ruvia::attach_event_loop(*context, {.queue_capacity_ = 1}));
    auto worker = attachment->loop().worker();
    auto payload = std::make_shared<int>(42);
    const std::weak_ptr<int> observed = payload;
    bool ran = false;
    const auto status = worker.post_factory(
        [owned = std::move(payload), &attachment, &worker, &ran]() mutable -> ruvia::move_only_function<void()> {
            attachment.reset();
            worker = ruvia::worker_ref{};
            return [owned = std::move(owned), &ran] { ran = *owned == 42; };
        });
    // Destroying the unrun context detaches the worker, which abandons the
    // admitted callback and releases its owned inputs.
    context.reset();
    return status == ruvia::post_status::accepted && !attachment && !worker.valid() &&
           !ran && observed.expired();
}

bool test_post_factory_exception_releases_admission() {
    asio::io_context context;
    auto attachment = ruvia::attach_event_loop(context, {.queue_capacity_ = 1});
    const auto worker_value = attachment.loop().worker();
    const auto expected = std::make_error_code(std::errc::argument_out_of_domain);
    bool propagated = false;
    try {
        static_cast<void>(worker_value.post_factory(
            [&expected]() -> ruvia::move_only_function<void()> { throw std::system_error(expected); }));
    } catch (const std::system_error& error) {
        propagated = error.code() == expected;
    }
    bool ran = false;
    const auto status = worker_value.post_factory(
        [&ran]() -> ruvia::move_only_function<void()> { return [&ran] { ran = true; }; });
    attachment.stop();
    context.run();
    return propagated && status == ruvia::post_status::accepted && ran;
}

bool test_post_factory_close_drains_admitted_callback() {
    asio::io_context context;
    auto attachment = ruvia::attach_event_loop(context, {.queue_capacity_ = 1});
    const auto worker_value = attachment.loop().worker();
    int calls = 0;
    const auto status = worker_value.post_factory([&]() -> ruvia::move_only_function<void()> {
        attachment.stop();
        return [&calls] { ++calls; };
    });
    bool rejected_factory_called = false;
    const auto rejected = worker_value.post_factory([&]() -> ruvia::move_only_function<void()> {
        rejected_factory_called = true;
        return [] {};
    });
    context.run();
    return status == ruvia::post_status::accepted && calls == 1 &&
           rejected == ruvia::post_status::worker_stopping && !rejected_factory_called;
}

struct queue_destructor_state final {
    const ruvia::worker_ref* worker_{nullptr};
    ruvia::event_loop_attachment* attachment_{nullptr};
    int destroyed_{0};
    bool ran_{false};
};

class queue_destructor_callback final {
public:
    explicit queue_destructor_callback(queue_destructor_state& state_value) noexcept
        : state_(&state_value) {}
    queue_destructor_callback(const queue_destructor_callback&) = delete;
    queue_destructor_callback(queue_destructor_callback&&) noexcept = default;

    ~queue_destructor_callback() {
        static_cast<void>(state_->worker_->post([] {}));
        ++state_->destroyed_;
    }

    void operator()() const {
        state_->ran_ = true;
        state_->attachment_->stop();
    }

private:
    queue_destructor_state* state_;
};

struct cancellation_observation final {
    unsigned calls_{0};
    std::uint64_t operation_id_{0};
    bool on_worker_{false};
    bool owner_destroyed_{false};
};

struct cancellation_owner final {
    const ruvia::worker_ref& worker_;
    cancellation_observation& observed_;

    ~cancellation_owner() {
        observed_.owner_destroyed_ = true;
    }

    void cancel_operation_by_id(std::uint64_t operation_id) noexcept {
        ++observed_.calls_;
        observed_.operation_id_ = operation_id;
        observed_.on_worker_ = worker_.is_current();
    }
};

bool test_cancellation_reaches_worker_with_saturated_or_closed_queue(bool close) {
    asio::io_context context;
    auto attachment = ruvia::attach_event_loop(context, {.queue_capacity_ = 1});
    const auto worker_value = attachment.loop().worker();
    cancellation_observation observed;
    cancellation_owner owner_value{worker_value, observed};
    auto queue = ruvia::make_worker_cancellation_target(owner_value, worker_value);
    ruvia::stop_source source;
    ruvia::stop_registration registration;
    source.token().register_callback(registration, ruvia::worker_cancellation_post(queue.get(), 7));
    unsigned normal_calls = 0;
    const auto accepted = worker_value.post([&normal_calls] { ++normal_calls; });
    if (!accepted.accepted()) {
        return false;
    }
    if (close) {
        attachment.stop();
    }
    const auto rejected = worker_value.post([] {});
    if (rejected.status() != (close ? ruvia::post_status::worker_stopping : ruvia::post_status::queue_full)) {
        return false;
    }
    source.request_stop();
    if (observed.calls_ != 0) {
        return false;
    }
    // Retirement drains the queued post and the deferred cancellation first.
    attachment.stop();
    attachment.run();
    queue->detach(owner_value);
    return observed.calls_ == 1 && observed.operation_id_ == 7 && observed.on_worker_ &&
           normal_calls == 1;
}

bool test_queued_cancellation_releases_queue_after_owner_retirement() {
    asio::io_context context;
    auto attachment = ruvia::attach_event_loop(context, {.queue_capacity_ = 1});
    const auto worker_value = attachment.loop().worker();
    cancellation_observation observed;
    auto owner_value = std::make_unique<cancellation_owner>(worker_value, observed);
    auto queue = ruvia::make_worker_cancellation_target(*owner_value, worker_value);
    std::weak_ptr weak_queue(queue);
    ruvia::stop_source source;
    ruvia::stop_registration registration;
    source.token().register_callback(registration, ruvia::worker_cancellation_post(queue.get(), 9));
    source.request_stop();
    registration.reset();
    queue->detach(*owner_value);
    owner_value.reset();
    queue.reset();
    const bool retained_by_post = !weak_queue.expired();
    attachment.stop();
    attachment.run();
    return retained_by_post && observed.owner_destroyed_ && observed.calls_ == 0 &&
           weak_queue.expired();
}

bool test_cancellation_after_endpoint_detach_does_not_touch_retired_owner() {
    asio::io_context context;
    auto attachment = ruvia::attach_event_loop(context, {.queue_capacity_ = 1});
    const auto worker_value = attachment.loop().worker();
    cancellation_observation observed;
    auto owner_value = std::make_unique<cancellation_owner>(worker_value, observed);
    auto queue = ruvia::make_worker_cancellation_target(*owner_value, worker_value);
    ruvia::stop_source source;
    ruvia::stop_registration registration;
    source.token().register_callback(registration, ruvia::worker_cancellation_post(queue.get(), 11));
    queue->detach(*owner_value);
    owner_value.reset();
    // Retiring the attachment detaches the endpoint before the stop request.
    attachment.stop();
    attachment.run();
    if (worker_value.valid()) {
        return false;
    }
    source.request_stop();
    context.restart();
    return observed.owner_destroyed_ && observed.calls_ == 0 && context.run() == 0;
}

bool test_attachment_escaped_worker_detaches_on_retirement() {
    asio::io_context context;
    std::optional<ruvia::worker_ref> escaped_worker;
    {
        auto attachment = ruvia::attach_event_loop(context, {.queue_capacity_ = 8});
        const auto loop = attachment.loop();
        if (&loop.io_context() != &context || !loop.worker().valid()) {
            return false;
        }
        escaped_worker.emplace(loop.worker());
    }
    asio::post(context, [] {});
    context.run();
    return escaped_worker && !escaped_worker->valid();
}

bool test_queue_callable_destruction_can_inspect_worker() {
    asio::io_context context;
    auto attachment = ruvia::attach_event_loop(context);
    const auto worker_value = attachment.loop().worker();
    queue_destructor_state state_value{.worker_ = &worker_value, .attachment_ = &attachment};
    const auto submitted = worker_value.post(queue_destructor_callback(state_value));
    if (!submitted.accepted()) {
        return false;
    }
    attachment.run();
    return state_value.ran_ && state_value.destroyed_ > 0;
}

bool test_event_loop_post_borrow_and_rejected_callable_ownership() {
    constexpr std::size_t repeated_posts = 64;
    payload_tracker resource;
    std::atomic_bool invalid_payload_valid{false};
    std::atomic_size_t repeated_valid{0};
    std::atomic_size_t repeated_invalid{0};
    std::promise<void> repeated_completed;
    auto repeated_completion = repeated_completed.get_future();
    ruvia::event_loop_pool accepting({.loop_count_ = 1, .queue_capacity_ = repeated_posts + 1});
    const auto loop = accepting.loop(0);

    auto invalid = ruvia::event_loop{}.worker().post(
        [payload = post_payload(&resource), &invalid_payload_valid]() mutable {
            invalid_payload_valid.store(payload.bytes_.size() == 4096 && payload.bytes_.front() == 'x' &&
                                            payload.bytes_.back() == 'x',
                std::memory_order_release);
        });
    if (invalid != ruvia::post_status::worker_stopping || invalid.rejected() == nullptr ||
        resource.allocation_count() == resource.deallocation_count()) {
        return false;
    }
    auto invalid_task = std::move(invalid).take_rejected();
    if (resource.allocation_count() == resource.deallocation_count() ||
        loop.worker().post(std::move(invalid_task)) != ruvia::post_status::accepted) {
        return false;
    }

    for (std::size_t i = 0; i < repeated_posts; ++i) {
        auto posted = accepting.loop(0).worker().post([payload = post_payload(&resource), i, &repeated_valid,
                                                          &repeated_invalid, &repeated_completed]() mutable {
            if (payload.bytes_.size() == 4096 && payload.bytes_.front() == 'x' &&
                payload.bytes_.back() == 'x') {
                repeated_valid.fetch_add(1, std::memory_order_relaxed);
            } else {
                repeated_invalid.fetch_add(1, std::memory_order_relaxed);
            }
            if (i + 1 == repeated_posts) {
                repeated_completed.set_value();
            }
        });
        if (!posted.accepted()) {
            return false;
        }
    }
    accepting.start();
    repeated_completion.wait();
    accepting.stop();
    accepting.join();
    if (!invalid_payload_valid.load(std::memory_order_acquire) ||
        repeated_valid.load(std::memory_order_relaxed) != repeated_posts ||
        repeated_invalid.load(std::memory_order_relaxed) != 0 ||
        resource.allocation_count() != resource.deallocation_count()) {
        return false;
    }

    ruvia::event_loop closed;
    {
        ruvia::event_loop_pool stopped({.loop_count_ = 1, .queue_capacity_ = 1});
        closed = stopped.loop(0);
        stopped.join();
    }
    std::atomic_bool closed_payload_valid{false};
    auto rejected = closed.worker().post(
        [payload = post_payload(&resource), &closed_payload_valid]() mutable {
            closed_payload_valid.store(payload.bytes_.size() == 4096 && payload.bytes_.front() == 'x' &&
                                           payload.bytes_.back() == 'x',
                std::memory_order_release);
        });
    if (rejected != ruvia::post_status::worker_stopping || rejected.rejected() == nullptr ||
        resource.allocation_count() == resource.deallocation_count()) {
        return false;
    }
    auto retry = std::move(rejected).take_rejected();
    if (resource.allocation_count() == resource.deallocation_count()) {
        return false;
    }
    std::promise<void> recovered;
    auto recovered_future = recovered.get_future();
    ruvia::event_loop_pool recovery({.loop_count_ = 1, .queue_capacity_ = 1});
    auto recovered_post = recovery.loop(0).worker().post(
        [task = std::move(retry), &recovered]() mutable {
            task();
            recovered.set_value();
        });
    if (!recovered_post.accepted()) {
        return false;
    }
    recovery.start();
    recovered_future.wait();
    recovery.stop();
    recovery.join();
    return closed_payload_valid.load(std::memory_order_acquire) &&
           resource.allocation_count() == resource.deallocation_count();
}

bool test_event_loop_post_protects_reentrant_inline_move() {
    std::promise<void> completed;
    auto completion = completed.get_future();
    std::atomic_bool payload_valid{false};
    auto original_pool = std::make_unique<ruvia::event_loop_pool>(
        ruvia::event_loop_pool_options{.loop_count_ = 1, .queue_capacity_ = 1});
    auto loop = original_pool->loop(0);
    reentrant_post_state state_value{.loop_ = &loop,
        .pool_ = &original_pool,
        .completion_ = &completed,
        .payload_valid_ = &payload_valid};
    std::optional<inline_reentrant_post_callable> input;
    input.emplace(state_value);

    auto rejected = loop.worker().post(std::move(*input));
    if (rejected != ruvia::post_status::worker_stopping || rejected.rejected() == nullptr ||
        original_pool != nullptr || loop.worker().valid()) {
        return false;
    }
    auto retry = std::move(rejected).take_rejected();
    input.reset();
    ruvia::event_loop_pool recovery({.loop_count_ = 1, .queue_capacity_ = 1});
    if (recovery.loop(0).worker().post(std::move(retry)) != ruvia::post_status::accepted) {
        return false;
    }
    recovery.start();
    if (completion.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
        recovery.stop();
        recovery.join();
        return false;
    }
    recovery.stop();
    recovery.join();
    return payload_valid.load(std::memory_order_acquire);
}

bool test_event_loop_post_protects_reentrant_heap_copy() {
    payload_tracker resource;
    std::promise<void> completed;
    auto completion = completed.get_future();
    std::atomic_bool payload_valid{false};
    auto original_pool = std::make_unique<ruvia::event_loop_pool>(
        ruvia::event_loop_pool_options{.loop_count_ = 1, .queue_capacity_ = 1});
    auto loop = original_pool->loop(0);
    reentrant_post_state state_value{.loop_ = &loop,
        .pool_ = &original_pool,
        .completion_ = &completed,
        .payload_valid_ = &payload_valid};
    std::optional<heap_reentrant_post_callable> input;
    input.emplace(state_value, &resource);
    const auto callable_storage = resource.allocation_count() - resource.deallocation_count();
    if (callable_storage == 0) {
        return false;
    }

    auto rejected = loop.worker().post(*input);
    if (rejected != ruvia::post_status::worker_stopping || rejected.rejected() == nullptr ||
        original_pool != nullptr || loop.worker().valid()) {
        return false;
    }
    input.reset();
    if (resource.allocation_count() - resource.deallocation_count() != callable_storage) {
        return false;
    }
    auto retry = std::move(rejected).take_rejected();
    if (resource.allocation_count() - resource.deallocation_count() != callable_storage) {
        return false;
    }
    ruvia::event_loop_pool recovery({.loop_count_ = 1, .queue_capacity_ = 1});
    if (recovery.loop(0).worker().post(std::move(retry)) != ruvia::post_status::accepted) {
        return false;
    }
    recovery.start();
    if (completion.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
        recovery.stop();
        recovery.join();
        return false;
    }
    recovery.stop();
    recovery.join();
    return payload_valid.load(std::memory_order_acquire) &&
           resource.allocation_count() == resource.deallocation_count();
}

bool test_post_outcome_invariants_and_empty_callbacks() {
    ruvia::event_loop_pool loops({.loop_count_ = 1, .queue_capacity_ = 1});
    const auto loop = loops.loop(0);
    // Results come only from real deliveries: an accepted one has nothing to return.
    auto accepted = loop.worker().post([] {});
    bool accepted_take_rejected = false;
    try {
        static_cast<void>(std::move(accepted).take_rejected());
    } catch (const std::logic_error&) {
        accepted_take_rejected = true;
    }
    auto full = loop.worker().post([] {});
    bool full_returns_task = full.status() == ruvia::post_status::queue_full && full.rejected() != nullptr &&
                             static_cast<bool>(std::move(full).take_rejected()) && full.rejected() == nullptr;

    bool empty_post = false;
    using null_callback_type = void (*)();
    try {
        static_cast<void>(loop.worker().post(static_cast<null_callback_type>(nullptr)));
    } catch (const std::invalid_argument&) {
        empty_post = true;
    }
    bool empty_stop_callback = false;
    using null_stop_callback_type = ruvia::move_only_function<ruvia::task<void>()>;
    try {
        static_cast<void>(loop.on_stop(null_stop_callback_type{}));
    } catch (const std::invalid_argument&) {
        empty_stop_callback = true;
    }
    loops.join();
    return accepted.accepted() && accepted_take_rejected && full_returns_task && empty_post && empty_stop_callback;
}

ruvia::task<void> wait_for_signal(ruvia::worker_signal& signal, bool& resumed,
    std::size_t& remaining, ruvia::event_loop_attachment& attachment) {
    {
        auto discarded_cold_wait = signal.wait();
        static_cast<void>(discarded_cold_wait);
    }
    co_await signal.wait();
    resumed = true;
    if (--remaining == 0) {
        attachment.stop();
    }
}

bool test_worker_signal_is_worker_affine() {
    bool invalid_worker_rejected = false;
    ruvia::worker_ref invalid_worker;
    try {
        ruvia::worker_signal invalid(invalid_worker);
    } catch (const std::invalid_argument&) {
        invalid_worker_rejected = true;
    }

    asio::io_context io_context;
    auto attachment = ruvia::attach_event_loop(io_context);
    const auto worker_ref_value = attachment.loop().worker();
    ruvia::worker_signal first_signal(worker_ref_value);
    ruvia::worker_signal second_signal(worker_ref_value);
    const bool worker_borrowed = &first_signal.worker() == &worker_ref_value;
    bool wait_creation_rejected = false;
    try {
        static_cast<void>(first_signal.wait());
    } catch (const std::logic_error&) {
        wait_creation_rejected = true;
    }
    // notify() shares wait()'s affinity contract: a wrong thread throws, and
    // the signal stays usable (no pending wake is recorded).
    bool notify_rejected = false;
    try {
        first_signal.notify();
    } catch (const std::logic_error&) {
        notify_rejected = true;
    }
    bool first_resumed = false;
    bool second_resumed = false;
    std::size_t remaining = 2;
    asio::co_spawn(io_context,
        ruvia::as_awaitable(
            wait_for_signal(first_signal, first_resumed, remaining, attachment)),
        asio::detached);
    asio::co_spawn(io_context,
        ruvia::as_awaitable(
            wait_for_signal(second_signal, second_resumed, remaining, attachment)),
        asio::detached);
    asio::post(io_context, [&] {
        first_signal.notify();
        second_signal.notify();
    });
    io_context.run();
    return invalid_worker_rejected && worker_borrowed && wait_creation_rejected && notify_rejected && first_resumed &&
           second_resumed;
}

ruvia::task<void> wait_signal_once(ruvia::worker_signal& signal, bool& resumed) {
    co_await signal.wait();
    resumed = true;
}

ruvia::task<void> exercise_signal_pending_latch(
    ruvia::worker_signal& signal, ruvia::event_loop_attachment& attachment, bool& success) {
    signal.notify();
    signal.notify();
    {
        auto cold_wait = signal.wait();
        static_cast<void>(cold_wait);
    }
    auto completed_wait = signal.wait();
    co_await completed_wait;
    bool repeated_wait_rejected = false;
    try {
        co_await completed_wait;
    } catch (const std::logic_error&) {
        repeated_wait_rejected = true;
    }

    bool resumed = false;
    ruvia::task_scope scope(signal.worker());
    scope.spawn(wait_signal_once(signal, resumed));
    const bool second_wait_suspended = !resumed;
    signal.notify();
    signal.notify();
    co_await scope.join();
    co_await signal.wait();
    success = second_wait_suspended && resumed && repeated_wait_rejected;
    attachment.stop();
}

bool test_worker_signal_pending_latch_survives_cold_wait_discard_and_scheduled_wake() {
    asio::io_context context;
    auto attachment = ruvia::attach_event_loop(context);
    const auto worker_value = attachment.loop().worker();
    ruvia::worker_signal signal(worker_value);
    bool success = false;
    asio::co_spawn(context,
        ruvia::as_awaitable(exercise_signal_pending_latch(signal, attachment, success)),
        asio::detached);
    context.run();
    return success;
}

ruvia::task<void> release_signal_after_wait(const ruvia::worker_ref& worker,
    ruvia::event_loop_attachment& attachment, bool& success) {
    for (const bool deferred : {false, true}) {
        auto signal = std::make_unique<ruvia::worker_signal>(worker);
        auto wait = signal->wait();
        if (deferred) {
            auto loop = attachment.loop();
            asio::post(loop.io_context(), [&signal] { signal->notify(); });
        } else {
            signal->notify();
        }
        co_await wait;
        signal.reset();
    }
    success = true;
    attachment.stop();
}

bool test_completed_signal_wait_releases_owner() {
    asio::io_context context;
    auto attachment = ruvia::attach_event_loop(context);
    const auto worker = attachment.loop().worker();
    bool success = false;
    asio::co_spawn(context, ruvia::as_awaitable(release_signal_after_wait(worker, attachment, success)), asio::detached);
    context.run();
    return success;
}

bool test_worker_signal_has_no_arbitrary_waiter_limit() {
    constexpr std::size_t waiter_count = 16;
    asio::io_context io_context;
    auto attachment = ruvia::attach_event_loop(io_context);
    const auto worker_ref_value = attachment.loop().worker();
    ruvia::worker_signal signal(worker_ref_value);
    std::array<bool, waiter_count> resumed{};
    std::size_t remaining = waiter_count;
    for (std::size_t index = 0; index < resumed.size(); ++index) {
        asio::co_spawn(io_context,
            ruvia::as_awaitable(
                wait_for_signal(signal, resumed[index], remaining, attachment)),
            asio::detached);
    }

    asio::post(io_context, [&] { signal.notify(); });
    io_context.run();
    for (const bool value : resumed) {
        if (!value) {
            return false;
        }
    }
    return true;
}

ruvia::task<void> start_cold_signal_wait(ruvia::worker_signal::wait_operation cold_wait, bool& rejected) {
    try {
        co_await std::move(cold_wait);
    } catch (const std::logic_error&) {
        rejected = true;
    }
}

bool test_worker_signal_rechecks_affinity_when_cold_wait_starts() {
    asio::io_context owner_context;
    asio::io_context other_context;
    auto owner_attachment = ruvia::attach_event_loop(owner_context);
    auto other_attachment = ruvia::attach_event_loop(other_context);
    const auto owner_worker = owner_attachment.loop().worker();
    ruvia::worker_signal signal(owner_worker);
    std::optional<ruvia::worker_signal::wait_operation> cold_wait;

    asio::post(owner_context, [&] {
        cold_wait.emplace(signal.wait());
        owner_attachment.stop();
    });
    owner_context.run();
    if (!cold_wait.has_value()) {
        return false;
    }

    bool rejected = false;
    asio::co_spawn(other_context,
        ruvia::as_awaitable(start_cold_signal_wait(std::move(*cold_wait), rejected)),
        asio::detached);
    asio::post(other_context, [&] { other_attachment.stop(); });
    other_context.run();
    cold_wait.reset();
    return rejected;
}

bool test_dispatch_and_affinity() {
    ruvia::event_loop_pool loops({.loop_count_ = 2, .queue_capacity_ = 4});
    const auto first = loops.loop(0);
    const auto second = loops.loop(1);
    if (!first.worker().valid() || first.worker().id() == 0 || first.worker().id() == second.worker().id() ||
        first.worker().is_current()) {
        return false;
    }
    constexpr std::string_view key = "device-42";
    if (loops.loop_for(key).worker().id() != loops.loop_for(key).worker().id()) {
        return false;
    }
    if (&first.io_context() != &first.executor().context()) {
        return false;
    }
    asio::ip::tcp::socket tcp(first.worker().executor());
    asio::ip::udp::socket udp(first.worker().executor());
    if (&tcp.get_executor().context() != &first.io_context() ||
        &udp.get_executor().context() != &first.io_context()) {
        return false;
    }

    std::promise<bool> completed;
    auto result_value = completed.get_future();
    std::atomic_bool stop_callback_ran{false};
    std::atomic_bool stop_callback_on_loop{false};
    auto stop_registration_value = first.on_stop([&]() -> ruvia::task<void> {
        stop_callback_on_loop = first.worker().is_current();
        stop_callback_ran = true;
        co_return;
    });
    auto move_only = std::make_unique<int>(42);
    if (first.worker().post([loop = first, value = std::move(move_only),
                                completed = std::move(completed)]() mutable {
            completed.set_value(loop.worker().is_current() && *value == 42);
        }) != ruvia::post_status::accepted) {
        return false;
    }

    loops.start();
    const bool success = result_value.get();
    loops.stop();
    loops.join();
    return success && stop_registration_value.valid() && stop_callback_ran && stop_callback_on_loop &&
           first.worker().post([] {}) == ruvia::post_status::worker_stopping;
}

bool test_bounded_queue() {
    ruvia::event_loop_pool loops({.loop_count_ = 1, .queue_capacity_ = 2});
    const auto worker_value = loops.loop(0).worker();
    std::atomic<int> calls{0};
    std::promise<void> completed;
    auto result_value = completed.get_future();
    std::promise<void> retried;
    auto retried_result = retried.get_future();

    if (worker_value.post([&] { ++calls; }) != ruvia::post_status::accepted || worker_value.post([&] {
            ++calls;
            completed.set_value();
        }) != ruvia::post_status::accepted) {
        return false;
    }
    auto rejected = worker_value.post([value = std::make_unique<int>(9), &calls, &retried] {
        calls.fetch_add(*value);
        retried.set_value();
    });
    if (rejected != ruvia::post_status::queue_full || rejected.rejected() == nullptr) {
        return false;
    }

    loops.start();
    result_value.get();
    if (worker_value.post(std::move(rejected).take_rejected()) != ruvia::post_status::accepted) {
        return false;
    }
    retried_result.get();
    loops.stop();
    loops.join();
    bool recovered_after_stop = false;
    auto stopped = worker_value.post([&recovered_after_stop] { recovered_after_stop = true; });
    if (stopped != ruvia::post_status::worker_stopping || stopped.rejected() == nullptr) {
        return false;
    }
    auto stopped_task = std::move(stopped).take_rejected();
    stopped_task();
    return calls.load() == 11 && recovered_after_stop;
}

bool test_external_event_loop_attachment() {
    asio::io_context io_context;
    {
        auto attachment = ruvia::attach_event_loop(io_context, {.queue_capacity_ = 4});
        const auto loop = attachment.loop();
        if (!attachment.valid() || !loop.worker().valid() || &loop.io_context() != &io_context) {
            return false;
        }

        asio::ip::tcp::socket tcp(loop.worker().executor());
        asio::ip::udp::socket udp(loop.worker().executor());
        if (&tcp.get_executor().context() != &io_context ||
            &udp.get_executor().context() != &io_context) {
            return false;
        }

        bool duplicate_rejected = false;
        try {
            auto duplicate = ruvia::attach_event_loop(io_context);
        } catch (const std::invalid_argument&) {
            duplicate_rejected = true;
        }
        if (!duplicate_rejected) {
            return false;
        }

        std::promise<bool> completed;
        auto result_value = completed.get_future();
        std::atomic_bool stop_callback_ran{false};
        std::atomic_bool stop_callback_on_loop{false};
        auto stop_registration_value = loop.on_stop([&]() -> ruvia::task<void> {
            stop_callback_on_loop = loop.worker().is_current();
            stop_callback_ran = true;
            co_return;
        });
        if (loop.worker().post([loop, completed = std::move(completed)]() mutable {
                completed.set_value(loop.worker().is_current());
            }) != ruvia::post_status::accepted) {
            return false;
        }

        std::thread external_thread([&] { attachment.run(); });
        bool dispatched_on_external_thread = false;
        try {
            dispatched_on_external_thread = result_value.get();
            attachment.stop();
            external_thread.join();
        } catch (...) {
            attachment.stop();
            if (external_thread.joinable()) {
                external_thread.join();
            }
            throw;
        }
        if (!dispatched_on_external_thread || !stop_registration_value.valid() || !stop_callback_ran ||
            !stop_callback_on_loop || loop.worker().valid() ||
            loop.worker().post([] {}) != ruvia::post_status::worker_stopping) {
            return false;
        }
    }

    io_context.restart();
    {
        auto attachment = ruvia::attach_event_loop(io_context);
        attachment.stop();
        attachment.run();
    }

    io_context.restart();
    try {
        auto invalid = ruvia::attach_event_loop(io_context, {.queue_capacity_ = 0});
    } catch (const std::invalid_argument&) {
        return true;
    }
    return false;
}

ruvia::task<void> finish_shutdown_cleanup(
    ruvia::worker_ref worker_value, std::atomic_bool& finished) {
    static_cast<void>(co_await ruvia::sleep_for(worker_value, std::chrono::milliseconds(1)));
    finished.store(true, std::memory_order_release);
    co_return;
}

bool test_pool_reported_failure_stops_every_loop_and_join_rethrows() {
    ruvia::event_loop_pool loops({.loop_count_ = 2, .queue_capacity_ = 4});
    const auto first = loops.loop(0);
    const auto second = loops.loop(1);
    std::atomic_bool first_cleanup_finished{false};
    std::atomic_bool second_cleanup_finished{false};
    auto first_cleanup = first.on_stop([&]() -> ruvia::task<void> {
        co_await finish_shutdown_cleanup(first.worker(), first_cleanup_finished);
    });
    auto second_cleanup = second.on_stop([&]() -> ruvia::task<void> {
        co_await finish_shutdown_cleanup(second.worker(), second_cleanup_finished);
    });
    std::promise<void> reported;
    auto reported_result = reported.get_future();
    if (first.worker().post([first, &reported] {
            first.report_failure(std::make_exception_ptr(std::runtime_error("reported runtime failure")));
            reported.set_value();
        }) != ruvia::post_status::accepted) {
        return false;
    }
    loops.start();
    if (reported_result.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
        loops.stop();
        loops.join();
        return false;
    }
    const bool all_closed = !first.worker().accepting() && !second.worker().accepting();
    bool original_failure_rethrown = false;
    try {
        loops.join();
    } catch (const std::runtime_error& error) {
        original_failure_rethrown = std::string_view(error.what()) == "reported runtime failure";
    }
    return first_cleanup.valid() && second_cleanup.valid() && all_closed &&
           first_cleanup_finished.load(std::memory_order_acquire) &&
           second_cleanup_finished.load(std::memory_order_acquire) && original_failure_rethrown;
}

struct failure_report_gate final {
    std::mutex mutex_;
    std::condition_variable changed_;
    bool entered_{false};
    bool released_{false};
    std::atomic_size_t calls_{0};
};

class gated_failure final : public std::exception {
public:
    explicit gated_failure(failure_report_gate& gate_value) noexcept
        : gate_(gate_value) {}

    const char* what() const noexcept override {
        gate_.calls_.fetch_add(1, std::memory_order_relaxed);
        std::unique_lock lock(gate_.mutex_);
        gate_.entered_ = true;
        gate_.changed_.notify_all();
        gate_.changed_.wait(lock, [this] { return gate_.released_; });
        return "gated first pool failure";
    }

private:
    failure_report_gate& gate_;
};

class counted_failure final : public std::exception {
public:
    explicit counted_failure(std::shared_ptr<std::atomic_size_t> calls) noexcept
        : calls_(std::move(calls)) {}

    const char* what() const noexcept override {
        calls_->fetch_add(1, std::memory_order_relaxed);
        return "handed-off pool failure";
    }

private:
    std::shared_ptr<std::atomic_size_t> calls_;
};

ruvia::task<void> report_pool_root_failure(ruvia::event_loop loop, failure_report_gate& gate_value) {
    std::exception_ptr failure;
    try {
        throw gated_failure(gate_value);
    } catch (...) {
        failure = std::current_exception();
    }
    loop.report_failure(std::move(failure));
    co_return;
}

bool test_pool_failure_handoff_during_destructor_report() {
    failure_report_gate gate;
    auto second_failure_calls = std::make_shared<std::atomic_size_t>(0);
    std::exception_ptr second_failure;
    try {
        throw counted_failure(second_failure_calls);
    } catch (...) {
        second_failure = std::current_exception();
    }
    auto pool = std::make_unique<ruvia::event_loop_pool>(
        ruvia::event_loop_pool_options{.loop_count_ = 1, .queue_capacity_ = 1});
    const auto loop = pool->loop(0);
    auto root = loop.start(report_pool_root_failure(loop, gate));
    pool->start();
    root.wait();
    std::move(root).get();

    std::thread destroyer([&] { pool.reset(); });
    bool first_report_entered = false;
    {
        std::unique_lock lock(gate.mutex_);
        first_report_entered = gate.changed_.wait_for(
            lock, std::chrono::seconds(5), [&] { return gate.entered_; });
    }
    if (first_report_entered) {
        std::thread reporter([&] {
            loop.report_failure(second_failure);
        });
        reporter.join();
    }
    {
        const std::lock_guard lock(gate.mutex_);
        gate.released_ = true;
    }
    gate.changed_.notify_all();
    destroyer.join();

    return first_report_entered && !pool && gate.calls_.load(std::memory_order_relaxed) == 1 &&
           second_failure_calls->load(std::memory_order_relaxed) == 1;
}

bool test_pool_reports_distinct_failure_from_another_loop() {
    auto second_failure_calls = std::make_shared<std::atomic_size_t>(0);
    std::exception_ptr second_failure;
    try {
        throw counted_failure(second_failure_calls);
    } catch (...) {
        second_failure = std::current_exception();
    }
    ruvia::event_loop_pool loops({.loop_count_ = 2, .queue_capacity_ = 1});
    loops.loop(0).report_failure(std::make_exception_ptr(std::runtime_error("first loop failure")));
    loops.loop(1).report_failure(second_failure);
    bool first_rethrown = false;
    try {
        loops.join();
    } catch (const std::runtime_error& error) {
        first_rethrown = std::string_view(error.what()) == "first loop failure";
    }
    return first_rethrown && second_failure_calls->load(std::memory_order_relaxed) == 1;
}

bool test_worker_run_reports_failure_after_delivered_one() {
    auto second_failure_calls = std::make_shared<std::atomic_size_t>(0);
    asio::io_context io_context;
    auto attachment = ruvia::attach_event_loop(io_context);
    asio::post(io_context, [] { throw std::runtime_error("first handler failed"); });
    asio::post(io_context, [second_failure_calls] { throw counted_failure(second_failure_calls); });
    bool first_rethrown = false;
    try {
        attachment.run();
    } catch (const std::runtime_error& error) {
        first_rethrown = std::string_view(error.what()) == "first handler failed";
    }
    return first_rethrown && second_failure_calls->load(std::memory_order_relaxed) == 1;
}

bool test_pool_failure_report_works_after_queue_closure() {
    ruvia::event_loop_pool loops({.loop_count_ = 1, .queue_capacity_ = 1});
    const auto loop = loops.loop(0);
    std::atomic_bool queued_work_ran{false};
    if (loop.worker().post([&] { queued_work_ran.store(true, std::memory_order_release); }) !=
        ruvia::post_status::accepted) {
        return false;
    }
    loops.stop();
    loop.report_failure(std::make_exception_ptr(std::runtime_error("late runtime failure")));
    bool failure_rethrown = false;
    try {
        loops.join();
    } catch (const std::runtime_error& error) {
        failure_rethrown = std::string_view(error.what()) == "late runtime failure";
    }
    return failure_rethrown && queued_work_ran.load(std::memory_order_acquire) && !loop.worker().valid();
}

bool test_attachment_report_failure_keeps_native_run_ownership() {
    const auto run = [](bool wrapped_run, bool receive_failure) {
        asio::io_context io_context;
        const auto expected = std::make_exception_ptr(std::runtime_error("attached report"));
        std::exception_ptr received;
        int reports = 0;
        ruvia::event_loop_attachment_options options;
        if (receive_failure) {
            options.failure_ = [&](std::exception_ptr failure) {
                received = std::move(failure);
                ++reports;
            };
        }
        auto attachment = ruvia::attach_event_loop(io_context, std::move(options));
        const auto loop = attachment.loop();
        std::atomic_bool cleanup_finished{false};
        auto cleanup = loop.on_stop([&]() -> ruvia::task<void> {
            co_await finish_shutdown_cleanup(loop.worker(), cleanup_finished);
        });
        bool report_returned = false;
        bool unrelated_work_ran = false;
        asio::post(io_context, [&] {
            loop.report_failure(expected);
            report_returned = true;
            asio::post(io_context, [&] { unrelated_work_ran = true; });
        });
        bool native_run_threw = false;
        try {
            if (wrapped_run) {
                attachment.run();
            } else {
                io_context.run();
            }
        } catch (...) {
            native_run_threw = true;
        }
        return cleanup.valid() && report_returned && unrelated_work_ran &&
               cleanup_finished.load(std::memory_order_acquire) && !native_run_threw && !loop.worker().valid() &&
               reports == (receive_failure ? 1 : 0) && received == (receive_failure ? expected : nullptr);
    };
    return run(true, false) && run(false, false) && run(true, true) && run(false, true);
}

ruvia::task<void> failed_abandoned_attachment_root() {
    throw std::runtime_error("abandoned attachment root failed");
    co_return;
}

bool test_abandoned_attachment_root_failure_retires(bool wrapped_run) {
    asio::io_context io_context;
    int root_reports = 0;
    int stop_reports = 0;
    bool reports_on_runner = true;
    const auto runner = std::this_thread::get_id();
    auto attachment = ruvia::attach_event_loop(io_context, {
                                                               .failure_ = [&](std::exception_ptr failure) {
                                                                   reports_on_runner = reports_on_runner && std::this_thread::get_id() == runner;
                                                                   try {
                                                                       std::rethrow_exception(failure);
                                                                   } catch (const std::runtime_error& error) {
                                                                       if (std::string_view(error.what()) == "abandoned attachment root failed") {
                                                                           ++root_reports;
                                                                       } else if (std::string_view(error.what()) == "attachment stop failed") {
                                                                           ++stop_reports;
                                                                       }
                                                                   }
                                                               },
                                                           });
    const auto loop = attachment.loop();
    std::atomic_bool cleanup_finished{false};
    auto cleanup = loop.on_stop([&]() -> ruvia::task<void> {
        co_await finish_shutdown_cleanup(loop.worker(), cleanup_finished);
        throw std::runtime_error("attachment stop failed");
    });
    std::atomic_bool unrelated_work_ran{false};
    asio::post(io_context, [&] { unrelated_work_ran.store(true, std::memory_order_release); });
    {
        auto root = loop.start(failed_abandoned_attachment_root());
    }

    if (wrapped_run) {
        attachment.run();
    } else {
        io_context.run();
    }

    bool context_access_rejected = false;
    try {
        static_cast<void>(loop.io_context());
    } catch (const std::logic_error&) {
        context_access_rejected = true;
    }
    return unrelated_work_ran.load(std::memory_order_acquire) && !attachment.valid() &&
           !loop.worker().valid() && context_access_rejected && root_reports == 1 && stop_reports == 1 &&
           reports_on_runner && cleanup.valid() && cleanup_finished.load(std::memory_order_acquire);
}

bool test_attachment_run_failure_retires_through_async_cleanup() {
    asio::io_context io_context;
    auto attachment = ruvia::attach_event_loop(io_context);
    const auto loop = attachment.loop();
    bool cleanup_ran = false;
    auto cleanup = loop.on_stop([&]() -> ruvia::task<void> {
        cleanup_ran = true;
        co_return;
    });
    asio::post(io_context, [] { throw std::runtime_error("attached handler failed"); });
    bool failure_propagated = false;
    try {
        attachment.run();
    } catch (const std::runtime_error& error) {
        failure_propagated = std::string_view(error.what()) == "attached handler failed";
    }
    return cleanup.valid() && cleanup_ran && failure_propagated && !loop.worker().valid();
}

bool test_external_attachment_retains_state_until_context_cleanup() {
    asio::io_context io_context;
    std::mutex gate_mutex;
    std::condition_variable gate_changed;
    bool entered = false;
    bool release = false;
    std::atomic_bool stop_callback_ran{false};
    ruvia::event_loop_stop_registration stop_registration;
    std::thread external_thread;

    {
        auto attachment = ruvia::attach_event_loop(io_context, {.queue_capacity_ = 4});
        stop_registration = attachment.loop().on_stop([&]() -> ruvia::task<void> {
            stop_callback_ran.store(true, std::memory_order_release);
            co_return;
        });
        asio::post(io_context, [&] {
            {
                const std::lock_guard lock(gate_mutex);
                entered = true;
            }
            gate_changed.notify_one();
            std::unique_lock lock(gate_mutex);
            gate_changed.wait(lock, [&] { return release; });
        });
        external_thread = std::thread([&] { io_context.run(); });
        {
            std::unique_lock lock(gate_mutex);
            gate_changed.wait(lock, [&] { return entered; });
        }
    }

    bool duplicate_rejected_while_cleanup_is_pending = false;
    try {
        auto duplicate = ruvia::attach_event_loop(io_context);
    } catch (const std::invalid_argument&) {
        duplicate_rejected_while_cleanup_is_pending = true;
    }

    {
        const std::lock_guard lock(gate_mutex);
        release = true;
    }
    gate_changed.notify_one();
    external_thread.join();
    stop_registration.reset();

    bool reattached_after_cleanup = false;
    try {
        io_context.restart();
        auto replacement = ruvia::attach_event_loop(io_context);
        replacement.stop();
        replacement.run();
        reattached_after_cleanup = true;
    } catch (...) {
    }
    return duplicate_rejected_while_cleanup_is_pending &&
           stop_callback_ran.load(std::memory_order_acquire) && reattached_after_cleanup;
}

bool test_external_attachment_handles_context_destruction() {
    auto io_context = std::make_unique<asio::io_context>();
    {
        // Register the timer service before Ruvia's external-context service;
        // shutdown must remain safe regardless of Asio service registration
        // order.
        asio::steady_timer timer(*io_context);
    }
    auto attachment = ruvia::attach_event_loop(*io_context);
    const auto loop = attachment.loop();
    io_context.reset();

    bool io_context_rejected = false;
    try {
        static_cast<void>(loop.io_context());
    } catch (const std::logic_error&) {
        io_context_rejected = true;
    }
    bool executor_rejected = false;
    try {
        static_cast<void>(loop.executor());
    } catch (const std::logic_error&) {
        executor_rejected = true;
    }
    bool worker_executor_rejected = false;
    try {
        static_cast<void>(loop.worker().executor());
    } catch (const std::logic_error&) {
        worker_executor_rejected = true;
    }
    const bool post_rejected = loop.worker().post([] {}) == ruvia::post_status::worker_stopping;
    attachment.stop();
    return io_context_rejected && executor_rejected && worker_executor_rejected && post_rejected && !attachment.valid() &&
           !loop.worker().valid();
}

bool test_stopped_attachment_with_queued_post_handles_context_destruction() {
    auto marker = std::make_shared<int>(0);
    const std::weak_ptr<int> observed = marker;
    {
        asio::io_context io_context;
        auto attachment = ruvia::attach_event_loop(io_context);
        // A plain post holds no retirement lease, so stop() parks its idle
        // finalization behind it; destroying the unrun context abandons both.
        if (attachment.loop().worker().post([marker = std::move(marker)] {}) != ruvia::post_status::accepted) {
            return false;
        }
        attachment.stop();
    }
    return observed.expired();
}

bool test_join_stops_running_pool() {
    ruvia::event_loop_pool loops({.loop_count_ = 1, .queue_capacity_ = 1});
    const auto loop = loops.loop(0);
    loops.start();
    loops.join();
    return !loop.worker().accepting() && loop.worker().post([] {}) == ruvia::post_status::worker_stopping;
}

bool test_failure_propagation() {
    ruvia::event_loop_pool loops({.loop_count_ = 1, .queue_capacity_ = 1});
    const auto loop = loops.loop(0);
    std::atomic_bool stop_callback_ran{false};
    std::atomic_bool stop_callback_on_loop{false};
    auto stop_registration_value = loop.on_stop([&]() -> ruvia::task<void> {
        stop_callback_on_loop = loop.worker().is_current();
        stop_callback_ran = true;
        co_return;
    });
    if (loop.worker().post([] { throw std::runtime_error("posted task failed"); }) !=
        ruvia::post_status::accepted) {
        return false;
    }
    loops.start();
    try {
        loops.join();
    } catch (const std::runtime_error& error) {
        return stop_registration_value.valid() && stop_callback_ran && stop_callback_on_loop &&
               std::string_view(error.what()) == "posted task failed";
    }
    return false;
}

bool test_join_before_start_drains_on_owners() {
    ruvia::event_loop_pool loops({.loop_count_ = 2, .queue_capacity_ = 1});
    const auto first = loops.loop(0);
    const auto second = loops.loop(1);
    std::atomic<unsigned> task_calls{0};
    std::atomic<unsigned> stop_calls{0};
    std::atomic_bool tasks_on_owners{true};
    std::atomic_bool stops_on_owners{true};

    auto first_stop = first.on_stop([&]() -> ruvia::task<void> {
        if (!first.worker().is_current()) {
            stops_on_owners.store(false, std::memory_order_relaxed);
        }
        stop_calls.fetch_add(1, std::memory_order_relaxed);
        co_return;
    });
    auto second_stop = second.on_stop([&]() -> ruvia::task<void> {
        if (!second.worker().is_current()) {
            stops_on_owners.store(false, std::memory_order_relaxed);
        }
        stop_calls.fetch_add(1, std::memory_order_relaxed);
        co_return;
    });
    if (first.worker().post([&] {
            if (!first.worker().is_current()) {
                tasks_on_owners.store(false, std::memory_order_relaxed);
            }
            task_calls.fetch_add(1, std::memory_order_relaxed);
        }) != ruvia::post_status::accepted ||
        second.worker().post([&] {
            if (!second.worker().is_current()) {
                tasks_on_owners.store(false, std::memory_order_relaxed);
            }
            task_calls.fetch_add(1, std::memory_order_relaxed);
        }) != ruvia::post_status::accepted) {
        return false;
    }

    loops.join();
    const bool rejected_after_join = first.worker().post([] {}) == ruvia::post_status::worker_stopping &&
                                     second.worker().post([] {}) == ruvia::post_status::worker_stopping;
    return first_stop.valid() && second_stop.valid() && rejected_after_join &&
           task_calls.load(std::memory_order_relaxed) == 2 &&
           stop_calls.load(std::memory_order_relaxed) == 2 &&
           tasks_on_owners.load(std::memory_order_relaxed) &&
           stops_on_owners.load(std::memory_order_relaxed);
}

bool test_stop_before_start_propagates_failure() {
    ruvia::event_loop_pool loops({.loop_count_ = 1, .queue_capacity_ = 1});
    const auto loop = loops.loop(0);
    std::atomic_bool stop_on_owner{false};
    auto stop_registration_value = loop.on_stop([&]() -> ruvia::task<void> {
        stop_on_owner.store(loop.worker().is_current(), std::memory_order_release);
        co_return;
    });
    if (loop.worker().post([] { throw std::runtime_error("pre-start task failed"); }) !=
        ruvia::post_status::accepted) {
        return false;
    }

    loops.stop();
    try {
        loops.join();
    } catch (const std::runtime_error& error) {
        return stop_registration_value.valid() && stop_on_owner.load(std::memory_order_acquire) &&
               std::string_view(error.what()) == "pre-start task failed";
    }
    return false;
}

bool test_join_rejects_pool_worker() {
    ruvia::event_loop_pool loops({.loop_count_ = 1, .queue_capacity_ = 1});
    const auto loop = loops.loop(0);
    std::promise<bool> completed;
    auto result_value = completed.get_future();
    if (loop.worker().post([&] {
            bool rejected = false;
            try {
                loops.join();
            } catch (const std::logic_error& error) {
                rejected = std::string_view(error.what()) ==
                           "cannot join an event loop pool from one of its workers";
            }
            completed.set_value(rejected && loop.worker().is_current());
        }) != ruvia::post_status::accepted) {
        return false;
    }

    loops.start();
    const bool rejected = result_value.get();
    loops.stop();
    loops.join();
    return rejected;
}

bool test_executor_failure_drains_shutdown_on_owners() {
    struct abandon_probe final {
        explicit abandon_probe(std::atomic_bool& destroyed) noexcept
            : destroyed_(&destroyed) {}
        ~abandon_probe() {
            destroyed_->store(true, std::memory_order_release);
        }
        std::atomic_bool* destroyed_;
    };

    ruvia::event_loop_pool loops({.loop_count_ = 2, .queue_capacity_ = 2});
    const auto failed_loop = loops.loop(0);
    const auto peer_loop = loops.loop(1);
    std::atomic<unsigned> failed_stop_calls{0};
    std::atomic<unsigned> peer_stop_calls{0};
    std::atomic_bool failed_stop_on_owner{false};
    std::atomic_bool peer_stop_on_owner{false};
    std::atomic_bool shutdown_continuation_drained{false};
    std::atomic_bool abandoned_queue_ran{false};
    std::atomic_bool abandoned_queue_destroyed{false};

    auto failed_stop = failed_loop.on_stop([&]() -> ruvia::task<void> {
        failed_stop_on_owner.store(failed_loop.worker().is_current(), std::memory_order_release);
        failed_stop_calls.fetch_add(1, std::memory_order_relaxed);
        asio::post(failed_loop.io_context(),
            [] { throw std::runtime_error("secondary shutdown handler failed"); });
        asio::post(failed_loop.io_context(), [&] {
            shutdown_continuation_drained.store(failed_loop.worker().is_current(), std::memory_order_release);
        });
        co_return;
    });
    auto peer_stop = peer_loop.on_stop([&]() -> ruvia::task<void> {
        peer_stop_on_owner.store(peer_loop.worker().is_current(), std::memory_order_release);
        peer_stop_calls.fetch_add(1, std::memory_order_relaxed);
        co_return;
    });

    asio::post(failed_loop.io_context(), [] { throw std::runtime_error("executor handler failed"); });
    if (failed_loop.worker().post([probe = std::make_unique<abandon_probe>(abandoned_queue_destroyed),
                                      &abandoned_queue_ran] {
            abandoned_queue_ran.store(true, std::memory_order_release);
        }) != ruvia::post_status::accepted) {
        return false;
    }

    loops.start();
    try {
        loops.join();
    } catch (const std::runtime_error& error) {
        return failed_stop.valid() && peer_stop.valid() &&
               std::string_view(error.what()) == "executor handler failed" &&
               failed_stop_calls.load(std::memory_order_relaxed) == 1 &&
               peer_stop_calls.load(std::memory_order_relaxed) == 1 &&
               failed_stop_on_owner.load(std::memory_order_acquire) &&
               peer_stop_on_owner.load(std::memory_order_acquire) &&
               shutdown_continuation_drained.load(std::memory_order_acquire) &&
               !abandoned_queue_ran.load(std::memory_order_acquire) &&
               abandoned_queue_destroyed.load(std::memory_order_acquire);
    }
    return false;
}

bool test_expired_loop() {
    ruvia::event_loop loop;
    {
        ruvia::event_loop_pool loops({.loop_count_ = 1, .queue_capacity_ = 1});
        loop = loops.loop(0);
    }
    bool context_rejected = false;
    try {
        static_cast<void>(loop.io_context());
    } catch (const std::logic_error&) {
        context_rejected = true;
    }
    return !loop.worker().valid() && !loop.worker().accepting() && context_rejected &&
           loop.worker().post([] {}) == ruvia::post_status::worker_stopping;
}

bool test_escaped_worker_ref_becomes_detached_endpoint() {
    ruvia::worker_ref worker;
    ruvia::worker_id_type live_id = 0;
    {
        ruvia::event_loop_pool loops({.loop_count_ = 1, .queue_capacity_ = 1});
        worker = loops.loop(0).worker();
        live_id = worker.id();
        if (!worker.valid() || live_id == 0) {
            return false;
        }
    }

    return !worker.valid() && !worker.accepting() && !worker.is_current() && worker.id() == 0 &&
           worker.post([] {}) == ruvia::post_status::worker_stopping;
}

bool test_abandoned_root_task_completes_when_queue_drain_fails() {
    ruvia::event_loop_pool loops({.loop_count_ = 1, .queue_capacity_ = 8});
    const auto loop = loops.loop(0);
    if (loop.worker().post([] { throw std::runtime_error("boom"); }) != ruvia::post_status::accepted) {
        return false;
    }

    auto root = loop.start([]() -> ruvia::task<int> { co_return 42; }());
    loops.start();

    bool join_threw = false;
    try {
        loops.join();
    } catch (const std::runtime_error& error) {
        join_threw = std::string_view(error.what()) == "boom";
    } catch (...) {
    }
    if (!join_threw) {
        return false;
    }

    try {
        static_cast<void>(std::move(root).get());
        return false;
    } catch (const ruvia::event_loop_start_rejected& error) {
        return error.status() == ruvia::post_status::worker_stopping;
    } catch (...) {
        return false;
    }
}

// A stop callback runs after every caller that could have received its
// exception is gone. Dropping it would make a failed cleanup invisible, so the
// pool records it as its first failure and join() rethrows it.
bool test_stop_callback_factory_failure_does_not_skip_other_callbacks() {
    ruvia::event_loop_pool loops({.loop_count_ = 1, .queue_capacity_ = 4});
    const auto loop = loops.loop(0);
    std::atomic_bool coroutine_callback_ran{false};
    auto factory_failure = loop.on_stop([]() -> ruvia::task<void> {
        throw std::runtime_error("stop factory failed");
    });
    auto coroutine_callback = loop.on_stop([&]() -> ruvia::task<void> {
        coroutine_callback_ran.store(true, std::memory_order_release);
        co_return;
    });

    loops.start();
    loops.stop();
    bool rethrown = false;
    try {
        loops.join();
    } catch (const std::runtime_error& error) {
        rethrown = std::string_view(error.what()) == "stop factory failed";
    } catch (...) {
    }
    return rethrown && coroutine_callback_ran.load(std::memory_order_acquire) &&
           factory_failure.valid() && coroutine_callback.valid();
}

bool test_stop_callback_failure_reaches_join() {
    ruvia::event_loop_pool loops({.loop_count_ = 1, .queue_capacity_ = 2});
    const auto loop = loops.loop(0);
    std::atomic<unsigned> stop_calls{0};
    auto stop_registration_value = loop.on_stop([&]() -> ruvia::task<void> {
        stop_calls.fetch_add(1, std::memory_order_relaxed);
        throw std::runtime_error("stop callback failed");
        co_return;
    });

    loops.start();
    loops.stop();

    bool rethrown = false;
    try {
        loops.join();
    } catch (const std::runtime_error& error) {
        rethrown = std::string_view(error.what()) == "stop callback failed";
    } catch (...) {
    }
    return rethrown && stop_calls.load(std::memory_order_relaxed) == 1;
}

bool test_async_stop_callbacks_start_together_and_keep_captures_alive() {
    struct callback final {
        std::atomic<unsigned>* started_;
        std::atomic<unsigned>* finished_;
        ruvia::worker_signal* signal_;
        bool notifier_;
        int value_;
        int* observed_;

        ruvia::task<void> operator()() {
            started_->fetch_add(1, std::memory_order_release);
            if (notifier_) {
                signal_->notify();
            } else {
                co_await signal_->wait();
            }
            *observed_ += value_;
            finished_->fetch_add(1, std::memory_order_release);
            co_return;
        }
    };

    ruvia::event_loop_pool loops({.loop_count_ = 1, .queue_capacity_ = 4});
    const auto loop = loops.loop(0);
    const auto worker_value = loop.worker();
    std::atomic<unsigned> started{0};
    std::atomic<unsigned> finished{0};
    ruvia::worker_signal signal(worker_value);
    int observed_value = 0;
    auto first = loop.on_stop(callback{&started, &finished, &signal, false, 1, &observed_value});
    auto second = loop.on_stop(callback{&started, &finished, &signal, true, 2, &observed_value});
    loops.start();
    loops.stop();
    const auto deadline_value = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (started.load(std::memory_order_acquire) != 2 &&
           std::chrono::steady_clock::now() < deadline_value) {
        std::this_thread::yield();
    }
    first.reset();
    second.reset();
    loops.join();
    return started.load(std::memory_order_acquire) == 2 &&
           finished.load(std::memory_order_acquire) == 2 && observed_value == 3;
}

bool test_reset_stop_registration_cancels_deferred_unstarted_callback() {
    asio::io_context io_context;
    auto attachment = ruvia::attach_event_loop(io_context);
    bool ran = false;
    auto reg = attachment.loop().on_stop([&]() -> ruvia::task<void> {
        ran = true;
        co_return;
    });
    attachment.stop();
    reg.reset();
    attachment.run();
    return !ran && !reg.valid();
}

bool test_retired_stop_registration_cancels_deferred_callback(bool destroy) {
    asio::io_context io_context;
    auto attachment = ruvia::attach_event_loop(io_context);
    bool ran = false;
    auto capture = std::make_shared<int>(42);
    const std::weak_ptr<int> lifetime(capture);
    std::optional<ruvia::event_loop_stop_registration> registration;
    registration.emplace(attachment.loop().on_stop(
        [capture = std::move(capture), &ran]() -> ruvia::task<void> {
            ran = *capture == 42;
            co_return;
        }));
    attachment.stop();
    if (destroy) {
        registration.reset();
    } else {
        *registration = ruvia::event_loop_stop_registration{};
    }
    const bool released_before_run = lifetime.expired();
    attachment.run();
    return released_before_run && !ran && (!registration || !registration->valid());
}

bool test_move_stop_registration_preserves_incoming_callback() {
    asio::io_context io_context;
    auto attachment = ruvia::attach_event_loop(io_context);
    int observed = 0;
    auto old_capture = std::make_shared<int>(1);
    const std::weak_ptr<int> old_lifetime(old_capture);
    auto destination = attachment.loop().on_stop(
        [capture = std::move(old_capture), &observed]() -> ruvia::task<void> {
            observed += *capture;
            co_return;
        });
    auto new_capture = std::make_shared<int>(10);
    const std::weak_ptr<int> new_lifetime(new_capture);
    auto source = attachment.loop().on_stop(
        [capture = std::move(new_capture), &observed]() -> ruvia::task<void> {
            observed += *capture;
            co_return;
        });
    attachment.stop();
    destination = std::move(source);
    auto& alias = destination;
    destination = std::move(alias);
    auto transferred = std::move(destination);
    const bool ownership_transferred = !source.valid() && !destination.valid() && transferred.valid() &&
                                       old_lifetime.expired() && !new_lifetime.expired();
    attachment.run();
    return ownership_transferred && observed == 10 && new_lifetime.expired();
}

bool test_stop_registration_reentrant_retirement_preserves_replacement() {
    asio::io_context io_context;
    auto attachment = ruvia::attach_event_loop(io_context);
    int observed = 0;
    ruvia::event_loop_stop_registration destination;
    auto source = std::make_unique<ruvia::event_loop_stop_registration>(
        attachment.loop().on_stop([&]() -> ruvia::task<void> {
            observed += 10;
            co_return;
        }));
    auto* owned_source = source.get();
    auto replacement = attachment.loop().on_stop([&]() -> ruvia::task<void> {
        observed += 100;
        co_return;
    });
    struct callback final {
        std::unique_ptr<ruvia::event_loop_stop_registration> source_;
        ruvia::event_loop_stop_registration* destination_;
        ruvia::event_loop_stop_registration* replacement_;
        int* observed_;
        callback(std::unique_ptr<ruvia::event_loop_stop_registration> source,
            ruvia::event_loop_stop_registration& destination,
            ruvia::event_loop_stop_registration& replacement,
            int& observed)
            : source_(std::move(source)),
              destination_(&destination),
              replacement_(&replacement),
              observed_(&observed) {}
        callback(callback&&) noexcept = default;
        ~callback() {
            if (source_) {
                *destination_ = std::move(*replacement_);
            }
        }
        ruvia::task<void> operator()() {
            *observed_ += 1;
            co_return;
        }
    };
    destination = attachment.loop().on_stop(callback(std::move(source), destination, replacement, observed));
    attachment.stop();
    destination = std::move(*owned_source);
    const bool replacement_transferred = destination.valid() && !replacement.valid();
    attachment.run();
    return replacement_transferred && observed == 100;
}

bool test_retired_started_stop_registration_keeps_capture(bool destroy, bool fail) {
    ruvia::event_loop_pool loops({.loop_count_ = 1, .queue_capacity_ = 4});
    const auto loop = loops.loop(0);
    ruvia::worker_signal resume(loop.worker());
    bool retained_while_suspended = false;
    bool finished = false;
    int observed = 0;
    auto capture = std::make_shared<int>(42);
    const std::weak_ptr<int> lifetime(capture);
    std::optional<ruvia::event_loop_stop_registration> registration;
    registration.emplace(loop.on_stop(
        [&, capture = std::move(capture)]() -> ruvia::task<void> {
            asio::post(loop.executor(), [&] {
                if (destroy) {
                    registration.reset();
                } else {
                    *registration = ruvia::event_loop_stop_registration{};
                }
                retained_while_suspended = !finished && !lifetime.expired();
                resume.notify();
            });
            co_await resume.wait();
            observed = *capture;
            finished = true;
            if (fail) {
                throw std::runtime_error("retired stop callback");
            }
        }));
    loops.start();
    loops.stop();
    bool reported_failure = false;
    try {
        loops.join();
    } catch (const std::runtime_error& error) {
        if (std::string_view(error.what()) != "retired stop callback") {
            return false;
        }
        reported_failure = true;
    }
    return retained_while_suspended && finished && observed == 42 && lifetime.expired() &&
           reported_failure == fail && (!registration || !registration->valid());
}

bool test_stop_registration_move_can_stop_attachment() {
    asio::io_context io_context;
    auto attachment = ruvia::attach_event_loop(io_context);
    bool armed = false;
    bool ran = false;
    struct callback final {
        ruvia::event_loop_attachment* attachment_;
        bool* armed_;
        bool* ran_;
        callback(ruvia::event_loop_attachment& owner, bool& enabled, bool& invoked)
            : attachment_(&owner),
              armed_(&enabled),
              ran_(&invoked) {}
        callback(const callback&) = delete;
        callback(callback&& other) noexcept
            : attachment_(other.attachment_),
              armed_(other.armed_),
              ran_(other.ran_) {
            if (*armed_) {
                attachment_->stop();
            }
        }
        ~callback() {
            if (*armed_) {
                attachment_->stop();
            }
        }
        ruvia::task<void> operator()() {
            *ran_ = true;
            co_return;
        }
    };
    auto reg = attachment.loop().on_stop(callback(attachment, armed, ran));
    armed = true;
    reg.reset();
    attachment.run();
    return !ran && !reg.valid();
}

ruvia::task<void> wait_for_shutdown_timer(const ruvia::worker_ref& worker_value, bool& child_done) {
    static_cast<void>(co_await ruvia::sleep_for(worker_value, std::chrono::hours(1)));
    child_done = true;
    co_return;
}

ruvia::task<void> nested_shutdown_scope(const ruvia::worker_ref& worker_value, bool& child_done,
    bool& middle_done) {
    ruvia::task_scope scope(worker_value);
    scope.spawn(wait_for_shutdown_timer(worker_value, child_done));
    co_await scope.join();
    middle_done = true;
}

ruvia::task<void> root_shutdown_scope(const ruvia::worker_ref& worker_value,
    std::promise<void>& started, bool& child_done, bool& middle_done, bool& root_done) {
    ruvia::task_scope scope(worker_value);
    scope.spawn(nested_shutdown_scope(worker_value, child_done, middle_done));
    started.set_value();
    co_await scope.join();
    root_done = true;
}

struct root_get_move_probe final {
    std::optional<ruvia::root_task<root_get_move_probe>>* owner_{};
    std::optional<ruvia::root_task<root_get_move_probe>>* replacement_{};
    std::atomic_bool* trigger_{};
    bool* valid_during_move_{};

    root_get_move_probe() = default;
    root_get_move_probe(std::optional<ruvia::root_task<root_get_move_probe>>* root_owner,
        std::optional<ruvia::root_task<root_get_move_probe>>* root_replacement,
        std::atomic_bool* move_trigger, bool* observed_validity)
        : owner_(root_owner),
          replacement_(root_replacement),
          trigger_(move_trigger),
          valid_during_move_(observed_validity) {}
    root_get_move_probe(root_get_move_probe&& other) noexcept
        : owner_(other.owner_),
          replacement_(other.replacement_),
          trigger_(other.trigger_),
          valid_during_move_(other.valid_during_move_) {
        if (trigger_ != nullptr && trigger_->exchange(false, std::memory_order_acq_rel)) {
            *valid_during_move_ = owner_->value().valid();
            if (replacement_ != nullptr) {
                *owner_ = std::move(*replacement_);
            }
        }
    }
    root_get_move_probe& operator=(root_get_move_probe&&) = delete;
    root_get_move_probe(const root_get_move_probe&) = delete;
    root_get_move_probe& operator=(const root_get_move_probe&) = delete;
};

ruvia::task<root_get_move_probe> return_root_get_move_probe(root_get_move_probe value) {
    co_return std::move(value);
}

struct root_heap_result final {
    explicit root_heap_result(payload_tracker* resource)
        : tracker_(resource),
          values_(32, 42),
          elements_(values_.data()) {
        if (tracker_ != nullptr) {
            tracker_->record_acquire();
        }
    }
    root_heap_result(root_heap_result&& other) noexcept
        : tracker_(std::exchange(other.tracker_, nullptr)),
          values_(std::move(other.values_)),
          elements_(other.elements_) {}
    root_heap_result(const root_heap_result&) = delete;
    ~root_heap_result() {
        if (tracker_ != nullptr) {
            tracker_->record_release();
        }
    }
    payload_tracker* tracker_{nullptr};
    std::vector<int> values_;
    const int* elements_{nullptr};
};

ruvia::task<root_heap_result> make_root_heap_result(payload_tracker* resource) {
    co_return root_heap_result(resource);
}

bool test_root_task_result_moves_outside_lock_and_preserves_reentrant_owner() {
    ruvia::event_loop_pool loops({.loop_count_ = 1, .queue_capacity_ = 8});
    const auto loop = loops.loop(0);
    std::optional<ruvia::root_task<root_get_move_probe>> root;
    std::optional<ruvia::root_task<root_get_move_probe>> replacement;
    std::atomic_bool trigger{false};
    bool valid_during_move = true;
    root.emplace(loop.start(return_root_get_move_probe(
        root_get_move_probe{&root, &replacement, &trigger, &valid_during_move})));
    replacement.emplace(loop.start(return_root_get_move_probe(
        root_get_move_probe{&root, nullptr, &trigger, &valid_during_move})));
    loops.start();
    root->wait();
    replacement->wait();
    trigger.store(true, std::memory_order_release);
    static_cast<void>(std::move(*root).get());
    const bool replacement_survived = root->valid();
    if (replacement_survived) {
        static_cast<void>(std::move(*root).get());
    }
    loops.join();
    return !valid_during_move && replacement_survived && !root->valid();
}

ruvia::task<const int> immutable_root_result(const ruvia::worker_ref& worker_value) {
    co_return worker_value.is_current() ? 17 : -1;
}

bool test_root_task_delivers_const_result() {
    ruvia::event_loop_pool loops({.loop_count_ = 1});
    const auto worker_value = loops.loop(0).worker();
    auto root = loops.loop(0).start(immutable_root_result(worker_value));
    loops.start();
    const auto value = std::move(root).get();
    loops.stop();
    loops.join();
    return value == 17 && !root.valid();
}

bool test_root_task_result_owns_heap_storage_past_pool_retirement() {
    ruvia::event_loop_pool loops({.loop_count_ = 1, .queue_capacity_ = 4});
    const auto loop = loops.loop(0);
    payload_tracker resource;
    auto root = loop.start(make_root_heap_result(&resource));
    loops.start();
    root.wait();
    loops.stop();
    loops.join();
    bool retained = false;
    {
        auto result_value = std::move(root).get();
        const bool payload_retained = result_value.values_.data() == result_value.elements_;
        const bool values_valid = result_value.values_.size() == 32 &&
                                  std::all_of(result_value.values_.begin(), result_value.values_.end(),
                                      [](int value) { return value == 42; });
        retained = values_valid && payload_retained && resource.live_allocations() > 0;
    }
    return retained && resource.live_allocations() == 0 &&
           resource.allocation_count() == resource.deallocation_count();
}

bool test_root_tasks_join_nested_scopes_during_stop() {
    ruvia::event_loop_pool loops({.loop_count_ = 1, .queue_capacity_ = 8});
    const auto loop = loops.loop(0);
    const auto worker_value = loop.worker();
    std::promise<void> started;
    auto started_result = started.get_future();
    bool child_done = false;
    bool middle_done = false;
    bool root_done = false;
    auto root = loop.start(root_shutdown_scope(
        worker_value, started, child_done, middle_done, root_done));
    loops.start();
    if (started_result.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
        loops.stop();
        loops.join();
        return false;
    }
    loops.stop();
    loops.join();
    try {
        std::move(root).get();
    } catch (...) {
        return false;
    }
    return child_done && middle_done && root_done;
}

bool test_draining_post_can_schedule_defer_cleanup_during_stop() {
    ruvia::event_loop_pool pool({.loop_count_ = 1, .queue_capacity_ = 4});
    const auto loop = pool.loop(0);
    const auto worker_value = loop.worker();
    bool post_ran = false;
    bool cleanup_scheduled = false;
    bool cleanup_ran = false;
    if (loop.worker().post([loop, worker_value, &post_ran, &cleanup_scheduled, &cleanup_ran] {
            post_ran = worker_value.is_current();
            cleanup_scheduled = loop.defer_cleanup([worker_value, &cleanup_ran] {
                cleanup_ran = worker_value.is_current();
            });
        }) != ruvia::post_status::accepted) {
        return false;
    }
    pool.stop();
    pool.join();
    return post_ran && cleanup_scheduled && cleanup_ran;
}

}  // namespace

TEST(Ruvia, WorkerPostBorrowAndRejectedCallableOwnership) {
    EXPECT_TRUE(test_event_loop_post_borrow_and_rejected_callable_ownership());
}
TEST(Ruvia, WorkerPostProtectsReentrantInlineMove) {
    EXPECT_TRUE(test_event_loop_post_protects_reentrant_inline_move());
}
TEST(Ruvia, WorkerPostProtectsReentrantHeapCopy) {
    EXPECT_TRUE(test_event_loop_post_protects_reentrant_heap_copy());
}
TEST(Ruvia, WorkerRefCallableLifetime) {
    EXPECT_TRUE(test_worker_ref_callable_lifetime());
}
TEST(Ruvia, WorkerPostFactoryKeepsEndpointThroughOwnerRetirement) {
    EXPECT_TRUE(test_post_factory_keeps_endpoint_through_owner_retirement());
}
TEST(Ruvia, WorkerPostFactoryExceptionReleasesAdmission) {
    EXPECT_TRUE(test_post_factory_exception_releases_admission());
}
TEST(Ruvia, WorkerPostFactoryCloseDrainsAdmittedCallback) {
    EXPECT_TRUE(test_post_factory_close_drains_admitted_callback());
}
TEST(Ruvia, WorkerPostOutcomeInvariantsAndEmptyCallbacks) {
    EXPECT_TRUE(test_post_outcome_invariants_and_empty_callbacks());
}
TEST(Ruvia, WorkerAttachmentEscapedRefDetachesOnRetirement) {
    EXPECT_TRUE(test_attachment_escaped_worker_detaches_on_retirement());
}
TEST(Ruvia, WorkerQueueCallableDestructionCanInspectWorker) {
    EXPECT_TRUE(test_queue_callable_destruction_can_inspect_worker());
}
TEST(Ruvia, WorkerSignalIsWorkerAffine) {
    EXPECT_TRUE(test_worker_signal_is_worker_affine());
}
TEST(Ruvia, WorkerSignalPendingLatchSurvivesDiscardAndWake) {
    EXPECT_TRUE(test_worker_signal_pending_latch_survives_cold_wait_discard_and_scheduled_wake());
}
TEST(Ruvia, WorkerCompletedSignalWaitReleasesOwner) {
    EXPECT_TRUE(test_completed_signal_wait_releases_owner());
}
TEST(Ruvia, WorkerSignalHasNoWaiterLimit) {
    EXPECT_TRUE(test_worker_signal_has_no_arbitrary_waiter_limit());
}
TEST(Ruvia, WorkerSignalRechecksColdWaitAffinity) {
    EXPECT_TRUE(test_worker_signal_rechecks_affinity_when_cold_wait_starts());
}
TEST(Ruvia, WorkerDispatchAndAffinity) {
    EXPECT_TRUE(test_dispatch_and_affinity());
}
TEST(Ruvia, WorkerBoundedQueue) {
    EXPECT_TRUE(test_bounded_queue());
}
TEST(Ruvia, WorkerCancellationWithFullQueue) {
    EXPECT_TRUE(test_cancellation_reaches_worker_with_saturated_or_closed_queue(false));
}
TEST(Ruvia, WorkerCancellationWithClosedQueue) {
    EXPECT_TRUE(test_cancellation_reaches_worker_with_saturated_or_closed_queue(true));
}
TEST(Ruvia, WorkerQueuedCancellationAfterOwnerRetirement) {
    EXPECT_TRUE(test_queued_cancellation_releases_queue_after_owner_retirement());
}
TEST(Ruvia, WorkerCancellationAfterEndpointDetach) {
    EXPECT_TRUE(test_cancellation_after_endpoint_detach_does_not_touch_retired_owner());
}
TEST(Ruvia, WorkerExternalEventLoopAttachment) {
    EXPECT_TRUE(test_external_event_loop_attachment());
}
TEST(Ruvia, WorkerAttachmentRunFailureRetiresThroughAsyncCleanup) {
    EXPECT_TRUE(test_attachment_run_failure_retires_through_async_cleanup());
}
TEST(Ruvia, WorkerAbandonedAttachmentFailureRetiresNativeRun) {
    EXPECT_TRUE(test_abandoned_attachment_root_failure_retires(false));
}
TEST(Ruvia, WorkerAbandonedAttachmentFailureRetiresAttachmentRun) {
    EXPECT_TRUE(test_abandoned_attachment_root_failure_retires(true));
}
TEST(Ruvia, WorkerPoolReportedFailureStopsEveryLoopAndJoinRethrows) {
    EXPECT_TRUE(test_pool_reported_failure_stops_every_loop_and_join_rethrows());
}
TEST(Ruvia, WorkerPoolFailureHandoffDuringDestructorReport) {
    EXPECT_TRUE(test_pool_failure_handoff_during_destructor_report());
}
TEST(Ruvia, WorkerPoolReportsDistinctFailureFromAnotherLoop) {
    EXPECT_TRUE(test_pool_reports_distinct_failure_from_another_loop());
}
TEST(Ruvia, WorkerRunReportsFailureAfterDeliveredOne) {
    EXPECT_TRUE(test_worker_run_reports_failure_after_delivered_one());
}
TEST(Ruvia, WorkerPoolFailureReportWorksAfterQueueClosure) {
    EXPECT_TRUE(test_pool_failure_report_works_after_queue_closure());
}
TEST(Ruvia, WorkerAttachmentReportFailurePreservesNativeRunOwnership) {
    EXPECT_TRUE(test_attachment_report_failure_keeps_native_run_ownership());
}
TEST(Ruvia, WorkerAttachmentOnlyReportsUnconsumedRootFailures) {
    asio::io_context context;
    int reports = 0;
    std::thread::id reporting_thread;
    std::exception_ptr received;
    auto attachment = ruvia::attach_event_loop(context, {
                                                            .failure_ = [&](std::exception_ptr failure) {
                                                                ++reports;
                                                                reporting_thread = std::this_thread::get_id();
                                                                received = std::move(failure);
                                                            },
                                                        });
    const auto loop = attachment.loop();
    {
        auto consumed = loop.start(failed_abandoned_attachment_root());
        context.poll();
        EXPECT_THROW(std::move(consumed).get(), std::runtime_error);
    }
    EXPECT_EQ(reports, 0);
    EXPECT_TRUE(loop.worker().accepting());

    auto abandoned = std::make_optional(loop.start(failed_abandoned_attachment_root()));
    context.poll();
    std::thread::id abandoning_thread;
    std::thread reporter([&] {
        abandoning_thread = std::this_thread::get_id();
        abandoned.reset();
    });
    reporter.join();
    EXPECT_EQ(reports, 1);
    EXPECT_EQ(reporting_thread, abandoning_thread);
    EXPECT_NE(received, nullptr);
    EXPECT_FALSE(loop.worker().accepting());
    context.run();
    EXPECT_FALSE(attachment.valid());
}

TEST(Ruvia, WorkerAttachmentFailureReceiverSurvivesReentrantOwnerDestructionAndThrow) {
    asio::io_context context;
    std::optional<ruvia::event_loop_attachment> attachment;
    std::optional<ruvia::event_loop> loop;
    auto lifetime = std::make_shared<int>(42);
    const std::weak_ptr<int> observed = lifetime;
    auto diagnostic_calls = std::make_shared<std::atomic_size_t>(0);
    const auto first = std::make_exception_ptr(std::runtime_error("first attached failure"));
    const auto nested = std::make_exception_ptr(std::runtime_error("nested attached failure"));
    int reports = 0;
    bool target_survived = false;
    bool unrelated_work_ran = false;
    std::atomic_bool cleanup_finished{false};
    attachment.emplace(ruvia::attach_event_loop(context, {
                                                             .failure_ = [&, owned = std::make_unique<int>(42), lifetime = std::move(lifetime)](std::exception_ptr failure) {
                                                                 ++reports;
                                                                 if (failure == first) {
                                                                     loop->report_failure(nested);
                                                                     target_survived = !attachment && !loop && !observed.expired() && *owned == *lifetime;
                                                                 } else {
                                                                     EXPECT_EQ(failure, nested);
                                                                     attachment.reset();
                                                                     loop.reset();
                                                                     throw counted_failure(diagnostic_calls);
                                                                 }
                                                             },
                                                         }));
    loop.emplace(attachment->loop());
    const auto worker = loop->worker();
    auto cleanup = loop->on_stop([&]() -> ruvia::task<void> {
        co_await finish_shutdown_cleanup(worker, cleanup_finished);
    });
    asio::post(context, [&] {
        loop->report_failure(first);
        asio::post(context, [&] { unrelated_work_ran = true; });
    });
    attachment->run();
    EXPECT_EQ(reports, 2);
    EXPECT_TRUE(target_survived);
    EXPECT_EQ(diagnostic_calls->load(std::memory_order_relaxed), 1);
    EXPECT_TRUE(cleanup_finished.load(std::memory_order_acquire));
    EXPECT_TRUE(unrelated_work_ran);
    EXPECT_FALSE(worker.valid());
    EXPECT_TRUE(observed.expired());
}

TEST(Ruvia, WorkerExternalAttachmentRetainsStateUntilCleanup) {
    EXPECT_TRUE(test_external_attachment_retains_state_until_context_cleanup());
}
TEST(Ruvia, WorkerExternalAttachmentHandlesContextDestruction) {
    EXPECT_TRUE(test_external_attachment_handles_context_destruction());
}
TEST(Ruvia, WorkerStoppedAttachmentQueuedPostHandlesContextDestruction) {
    EXPECT_TRUE(test_stopped_attachment_with_queued_post_handles_context_destruction());
}
TEST(Ruvia, WorkerJoinStopsRunningPool) {
    EXPECT_TRUE(test_join_stops_running_pool());
}
TEST(Ruvia, WorkerFailurePropagation) {
    EXPECT_TRUE(test_failure_propagation());
}
TEST(Ruvia, WorkerJoinBeforeStartDrainsOnOwners) {
    EXPECT_TRUE(test_join_before_start_drains_on_owners());
}
TEST(Ruvia, WorkerStopBeforeStartPropagatesFailure) {
    EXPECT_TRUE(test_stop_before_start_propagates_failure());
}
TEST(Ruvia, WorkerJoinRejectsPoolWorker) {
    EXPECT_TRUE(test_join_rejects_pool_worker());
}
TEST(Ruvia, WorkerExecutorFailureDrainsShutdownOnOwners) {
    EXPECT_TRUE(test_executor_failure_drains_shutdown_on_owners());
}
TEST(Ruvia, WorkerExpiredLoop) {
    EXPECT_TRUE(test_expired_loop());
}
TEST(Ruvia, WorkerEscapedRefDetaches) {
    EXPECT_TRUE(test_escaped_worker_ref_becomes_detached_endpoint());
}
TEST(Ruvia, WorkerAbandonedRootTaskCompletesWhenQueueDrainFails) {
    EXPECT_TRUE(test_abandoned_root_task_completes_when_queue_drain_fails());
}
TEST(Ruvia, WorkerStopCallbackFailureReachesJoin) {
    EXPECT_TRUE(test_stop_callback_failure_reaches_join());
}
TEST(Ruvia, WorkerStopCallbackFactoryFailureKeepsOtherCallbacksRunning) {
    EXPECT_TRUE(test_stop_callback_factory_failure_does_not_skip_other_callbacks());
}
TEST(Ruvia, WorkerAsyncStopCallbacksStartTogetherAndKeepCapturesAlive) {
    EXPECT_TRUE(test_async_stop_callbacks_start_together_and_keep_captures_alive());
}
TEST(Ruvia, WorkerResetStopRegistrationCancelsDeferredUnstartedCallback) {
    EXPECT_TRUE(test_reset_stop_registration_cancels_deferred_unstarted_callback());
}
TEST(Ruvia, WorkerDestroyedStopRegistrationCancelsDeferredCallback) {
    EXPECT_TRUE(test_retired_stop_registration_cancels_deferred_callback(true));
}
TEST(Ruvia, WorkerOverwrittenStopRegistrationCancelsDeferredCallback) {
    EXPECT_TRUE(test_retired_stop_registration_cancels_deferred_callback(false));
}
TEST(Ruvia, WorkerMovedStopRegistrationPreservesIncomingCallback) {
    EXPECT_TRUE(test_move_stop_registration_preserves_incoming_callback());
}
TEST(Ruvia, WorkerStopRegistrationReentrantRetirementPreservesReplacement) {
    EXPECT_TRUE(test_stop_registration_reentrant_retirement_preserves_replacement());
}
TEST(Ruvia, WorkerDestroyedStopRegistrationKeepsStartedCallbackAlive) {
    EXPECT_TRUE(test_retired_started_stop_registration_keeps_capture(true, false));
}
TEST(Ruvia, WorkerOverwrittenStopRegistrationKeepsStartedCallbackAlive) {
    EXPECT_TRUE(test_retired_started_stop_registration_keeps_capture(false, false));
}
TEST(Ruvia, WorkerDestroyedStopRegistrationPreservesStartedCallbackFailure) {
    EXPECT_TRUE(test_retired_started_stop_registration_keeps_capture(true, true));
}
TEST(Ruvia, WorkerOverwrittenStopRegistrationPreservesStartedCallbackFailure) {
    EXPECT_TRUE(test_retired_started_stop_registration_keeps_capture(false, true));
}
TEST(Ruvia, WorkerStopRegistrationMoveCanStopAttachment) {
    EXPECT_TRUE(test_stop_registration_move_can_stop_attachment());
}
TEST(Ruvia, WorkerRootTasksJoinNestedScopesDuringStop) {
    EXPECT_TRUE(test_root_tasks_join_nested_scopes_during_stop());
}
TEST(Ruvia, WorkerRootResultMovesAreReentrantAndPreserveOwner) {
    EXPECT_TRUE(test_root_task_result_moves_outside_lock_and_preserves_reentrant_owner());
}
TEST(Ruvia, WorkerRootResultOwnsHeapStoragePastPoolRetirement) {
    EXPECT_TRUE(test_root_task_result_owns_heap_storage_past_pool_retirement());
}
TEST(Ruvia, WorkerRootTaskDeliversConstResult) {
    EXPECT_TRUE(test_root_task_delivers_const_result());
}
TEST(Ruvia, WorkerDrainingPostCanScheduleDeferCleanupDuringStop) {
    EXPECT_TRUE(test_draining_post_can_schedule_defer_cleanup_during_stop());
}

#if defined(_WIN32)
TEST(Ruvia, WorkerNativeThreadWaitDoesNotWaitForRunningOrColdCleanup) {
    ruvia::event_loop_pool pool({.loop_count_ = 1});
    pool.start();
    const auto caller = pool.loop(0);
    for (const bool started : {false, true}) {
        SCOPED_TRACE(started);
        ruvia::worker_runtime runtime;
        std::promise<void> entered;
        auto entered_future = entered.get_future();
        std::promise<void> release;
        const auto released = release.get_future().share();
        struct release_shutdown final {
            std::promise<void>* release_;
            ~release_shutdown() {
                if (release_) {
                    release_->set_value();
                }
            }
        } unblock{&release};
        runtime.configure({.startup_ = [&] { runtime.mark_ready(); },
            .shutdown_ = [&] {
                entered.set_value();
                (void)released.wait_for(std::chrono::seconds(10)); }});
        if (started) {
            ASSERT_TRUE(runtime.start());
            ASSERT_TRUE(runtime.wait_until_ready());
        }
        const auto prepare = [&]() -> ruvia::task<std::uintptr_t> {
            co_return co_await runtime.prepare_thread_join(caller.worker());
        };
        const auto native = caller.start(prepare()).get();
        ASSERT_NE(native, std::uintptr_t{0});
        const std::unique_ptr<void, decltype(&::CloseHandle)> wait_descriptor(
            reinterpret_cast<HANDLE>(native), &::CloseHandle);
        ASSERT_EQ(entered_future.wait_for(std::chrono::seconds(5)), std::future_status::ready);
        EXPECT_EQ(::WaitForSingleObject(wait_descriptor.get(), 0), WAIT_TIMEOUT);
        release.set_value();
        unblock.release_ = nullptr;
        ASSERT_EQ(::WaitForSingleObject(wait_descriptor.get(), 5000), WAIT_OBJECT_0);
        runtime.join();
        EXPECT_EQ(runtime.state(), ruvia::worker_runtime_state::stopped);
        EXPECT_EQ(caller.start(prepare()).get(), std::uintptr_t{0});
    }
    pool.join();
}
#endif
