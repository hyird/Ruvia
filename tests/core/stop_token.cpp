#include "stop_token.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <future>
#include <memory>
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
#include <asio/error.hpp>
#include <asio/io_context.hpp>

#include "asio_bridge.h"
#include "asio_ip_address.h"
#include "byte_view.h"
#include "event_loop.h"
#include "failure_report.h"
#include "move_only_function.h"
#include "operation_options.h"
#include "task.h"

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
    explicit callback_payload(callback_retirement_state& state) noexcept :
        state_(state) {}
    ~callback_payload() {
        ++state_.payload_destructions_;
    }

    callback_retirement_state& state_;
    int value_{42};
};

class reset_registration_when_source_retires final {
public:
    explicit reset_registration_when_source_retires(callback_retirement_state& state) :
        state_(&state),
        payload_(std::make_unique<callback_payload>(state)) {}
    reset_registration_when_source_retires(const reset_registration_when_source_retires&) = delete;
    reset_registration_when_source_retires& operator=(const reset_registration_when_source_retires&) = delete;
    reset_registration_when_source_retires(reset_registration_when_source_retires&& other) noexcept :
        state_(other.state_),
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
    source.token().register_callback(registration, stop_token_test::reset_registration_when_source_retires(state));
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
    explicit reset_registration_on_move(reset_registration_move_state& state_value) noexcept :
        state_(&state_value) {}

    reset_registration_on_move(const reset_registration_on_move&) = delete;
    reset_registration_on_move& operator=(const reset_registration_on_move&) = delete;

    reset_registration_on_move(reset_registration_on_move&& other) noexcept :
        state_(other.state_) {
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
    source.token().register_callback(registration, [&calls] {
        calls.fetch_add(1, std::memory_order_relaxed);
    });
    EXPECT_TRUE(registration.registered());
    source.request_stop();
    source.request_stop();
    EXPECT_EQ(calls.load(std::memory_order_relaxed), 1);
}

TEST(Ruvia, stop_token_registration_can_be_reset) {
    ruvia::stop_source source;
    int calls = 0;
    ruvia::stop_registration registration;
    source.token().register_callback(registration, [&calls] {
        ++calls;
    });
    registration.reset();
    source.request_stop();
    EXPECT_EQ(calls, 0);
}

TEST(Ruvia, stop_token_registration_after_stop_runs_immediately) {
    ruvia::stop_source source;
    source.request_stop();
    int calls = 0;
    ruvia::stop_registration registration;
    source.token().register_callback(registration, [&calls] {
        ++calls;
    });
    EXPECT_TRUE(!registration.registered());
    EXPECT_EQ(calls, 1);
}

TEST(Ruvia, stop_token_registration_reuse_replaces_previous_callback) {
    ruvia::stop_source source;
    int first_calls = 0;
    int second_calls = 0;
    ruvia::stop_registration registration;
    source.token().register_callback(registration, [&first_calls] {
        ++first_calls;
    });
    source.token().register_callback(registration, [&second_calls] {
        ++second_calls;
    });
    EXPECT_TRUE(registration.registered());
    source.request_stop();
    EXPECT_EQ(first_calls, 0);
    EXPECT_EQ(second_calls, 1);
}

TEST(Ruvia, stop_token_replacement_preserves_entry_state_when_retirement_clears_token) {
    struct clear_token final {
        clear_token(ruvia::stop_token& token, ruvia::stop_source* source) noexcept :
            token_(token),
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

        token.register_callback(registration, [&calls] {
            ++calls;
        });
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
        explicit destroy_token(std::unique_ptr<ruvia::stop_token>& owner) noexcept :
            owner_(owner) {}
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

    token->register_callback(registration, [&calls] {
        ++calls;
    });
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
    reset_registration_move_state state_value{&first, &registration, &moves, &calls, move_into_callback_state, nullptr, nullptr};
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
    reset_registration_move_state state_value{&first, &registration, &moves, &calls, move_into_callback_state, &registration_paused, &stop_completed};
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
    auto registration = group.token().register_callback([&calls] {
        ++calls;
    });
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
    group.token().register_callback(registration, [&calls] {
        ++calls;
    });
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
    nested.register_callback(registration, [&calls] {
        ++calls;
    });
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
        auto registration = group.token().register_callback([&calls] {
            ++calls;
        });
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
    combined.register_callback(registration, [&calls] {
        ++calls;
    });
    EXPECT_TRUE(registration.registered());
    second.request_stop();
    EXPECT_TRUE(combined.stop_requested());
    EXPECT_EQ(calls, 1);
}

TEST(Ruvia, stop_token_outlives_its_source_across_threads) {
    for (const bool request_stop : {false, true}) {
        std::optional<ruvia::stop_source> source;
        std::thread([&] {
            source.emplace();
        }).join();
        auto token = source->token();
        int calls = 0;
        ruvia::stop_registration registration;
        token.register_callback(registration, [&calls] {
            ++calls;
        });
        if (request_stop) {
            source->request_stop();
        }
        source.reset();
        EXPECT_EQ(token.stop_requested(), request_stop);
        EXPECT_EQ(token.stoppable(), request_stop);
        EXPECT_EQ(calls, request_stop ? 1 : 0);

        bool retained_state_valid = false;
        std::thread([retained = std::move(token), &registration, &retained_state_valid, request_stop]() mutable {
            retained_state_valid = retained.stop_requested() == request_stop && retained.stoppable() == request_stop;
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
    std::thread stopper([&source] {
        source.request_stop();
    });
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
        sources[index].token().register_callback(registration, [&, index] {
            calls[index].fetch_add(1, std::memory_order_relaxed);
        });
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

        explicit stop_on_move_without_reset(state_type& state_value) noexcept :
            state_(&state_value) {}
        stop_on_move_without_reset(const stop_on_move_without_reset&) = delete;
        stop_on_move_without_reset& operator=(const stop_on_move_without_reset&) = delete;
        stop_on_move_without_reset(stop_on_move_without_reset&& other) noexcept :
            state_(other.state_) {
            if (state_->moves_->fetch_add(1, std::memory_order_relaxed) + 1 == move_into_callback_state) {
                state_->source_->request_stop();
            }
        }
        stop_on_move_without_reset& operator=(stop_on_move_without_reset&&) = delete;

        void operator()() const noexcept {
            state_->calls_->fetch_add(1, std::memory_order_relaxed);
            if (state_->reuse_source_ != nullptr) {
                auto* calls_ptr = state_->calls_;
                state_->reuse_source_->token().register_callback(*state_->registration_, [calls_ptr] {
                    calls_ptr->fetch_add(10, std::memory_order_relaxed);
                });
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

        explicit reset_then_stop_on_move(state_type& state_value) noexcept :
            state_(&state_value) {}
        reset_then_stop_on_move(const reset_then_stop_on_move&) = delete;
        reset_then_stop_on_move& operator=(const reset_then_stop_on_move&) = delete;
        reset_then_stop_on_move(reset_then_stop_on_move&& other) noexcept :
            state_(other.state_) {
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
    std::thread stopper([&] {
        first.request_stop();
    });
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
    const ruvia::operation_options original{.timeout_ = std::chrono::milliseconds(20), .stop_token_ = base.token()};
    auto inherited = ruvia::override_operation_options(original, {});
    auto replaced = ruvia::override_operation_options(inherited, {.timeout_ = std::chrono::milliseconds(50), .stop_token_ = replacement.token()});
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
    for (const std::string_view text : {"192.0.2.1:80", "[::1]:80", "[::1]", "192.0.2", "01.2.3.4", "192.0.2.256", "192.0.2.1.", ".192.0.2.1", "192.0.2.1%1", "fe80::1%", "fe80::1%1%2", "::1 ", "1", "::ffff:01.2.3.4", "::ffff:192.0.2", "fe80::1%12:80", "fe80::1%12 ", "fe80::1%1]:80"}) {
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
    for (const auto& error : {std::error_code(1, std::system_category()), std::error_code(0x7FFFFFF0, std::system_category()), std::error_code(asio::error::connection_reset), std::error_code(asio::error::host_not_found)}) {
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
        preserved_after_growth = text->find_first_not_of('s') == text_bytes && text->find_first_not_of('t', text_bytes) == std::string::npos;
        for (std::size_t index = 0; index < values->size(); ++index) {
            preserved_after_growth = preserved_after_growth && std::ranges::all_of((*values)[index].bytes_, [index](std::byte byte) {
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
    explicit aligned_result(int value) :
        value_(std::make_unique<int>(value)),
        addresses_aligned_(is_aligned(this)) {}
    aligned_result(aligned_result&& other) noexcept :
        value_(std::move(other.value_)),
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

ruvia::task<aligned_result> make_value(ruvia::worker_ref worker_value, std::unique_ptr<aligned_result> result_value) {
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

    explicit counting_allocator(std::size_t& allocations) noexcept :
        allocations_(&allocations) {}
    template <typename other_type>
    counting_allocator(const counting_allocator<other_type>& other) noexcept :
        allocations_(other.allocations_) {}

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

    auto value = loop.start(ruvia::test::asio_task_tests::make_value(worker_value, std::make_unique<ruvia::test::asio_task_tests::aligned_result>(42)));
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
    valid = valid && result_value && result_value->addresses_aligned_ && result_value->value_ != nullptr && *result_value->value_ == 42;
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
    ruvia::async_start(ruvia::test::asio_task_tests::answer(), asio::bind_allocator(ruvia::test::asio_task_tests::counting_allocator<int>(allocations), asio::bind_executor(io_context.get_executor(), [&delivered](ruvia::task_completion_result<int>&& result_value) {
        if (result_value.index() == 0) {
            delivered = std::get<0>(result_value);
        }
    })));
    ruvia::async_start(ruvia::test::asio_task_tests::fail(), asio::bind_executor(io_context.get_executor(), [&failure_delivered](std::exception_ptr failure) {
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
