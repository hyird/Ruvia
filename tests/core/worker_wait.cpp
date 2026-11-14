#include "ruvia/core/worker_wait.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <concepts>
#include <coroutine>
#include <cstddef>
#include <exception>
#include <functional>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <optional>
#include <semaphore>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>

#include <asio/io_context.hpp>
#include <asio/post.hpp>

#include "ruvia/core/blocking_pool.h"
#include "ruvia/core/event_loop.h"
#include "ruvia/core/stop_token.h"
#include "ruvia/core/task.h"
#include "ruvia/core/task_scope.h"
#include "ruvia/core/worker_ref.h"

#include "worker_task_fixture.h"

namespace ruvia::test::worker_wait_tests {

// A worker-bound counting semaphore composed only from public building
// blocks: the shared state owns the wait slot and its shutdown registration,
// and each acquire owns a worker_single_wait_awaiter.
struct semaphore_state final {
    semaphore_state(worker_ref worker_value, std::size_t permits) noexcept :
        worker_(std::move(worker_value)),
        permits_(permits) {}

    [[nodiscard]] const worker_ref& worker() const noexcept {
        return worker_;
    }

    [[nodiscard]] std::unique_lock<std::mutex> lock_state() {
        return std::unique_lock(mutex_);
    }

    [[nodiscard]] worker_wait_slot<std::monostate, semaphore_state>& wait_slot() noexcept {
        return wait_slot_;
    }

    void worker_stopping() noexcept {
        auto lock = lock_state();
        stopping_ = true;
        wait_slot_.complete(worker_wait_status::worker_stopping);
    }

    worker_shutdown_registration shutdown_registration_;
    worker_ref worker_;
    std::mutex mutex_;
    std::size_t permits_;
    bool stopping_{false};
    worker_wait_slot<std::monostate, semaphore_state> wait_slot_;
};

static_assert(worker_wait_state<semaphore_state, std::monostate>);
static_assert(worker_shutdown_observer<semaphore_state>);

// The concept names the shutdown registration member, not just the observer.
struct unregistered_state final {
    [[nodiscard]] const worker_ref& worker() const noexcept;
    [[nodiscard]] std::unique_lock<std::mutex> lock_state();
    [[nodiscard]] worker_wait_slot<std::monostate, unregistered_state>& wait_slot() noexcept;
    void worker_stopping() noexcept;
    worker_shutdown_registration registration_;
};
static_assert(!worker_wait_state<unregistered_state, std::monostate>);

class worker_semaphore final {
public:
    class acquire_operation final {
    public:
        acquire_operation(acquire_operation&&) noexcept = default;
        acquire_operation(const acquire_operation&) = delete;
        acquire_operation& operator=(const acquire_operation&) = delete;
        acquire_operation& operator=(acquire_operation&&) = delete;
        void operator&() const = delete;

        [[nodiscard]] bool await_ready() {
            wait_.start("semaphore acquire must run on its worker");
            auto& state = wait_.state();
            auto lock = state.lock_state();
            if (state.stopping_) {
                wait_.complete_ready(worker_wait_status::worker_stopping);
                return true;
            }
            if (state.permits_ != 0) {
                --state.permits_;
                wait_.complete_ready(worker_wait_result<std::monostate>::with_value(std::monostate{}));
                return true;
            }
            return wait_.complete_or_publish("semaphore supports one pending acquire");
        }

        [[nodiscard]] bool await_suspend(std::coroutine_handle<> continuation) {
            return wait_.suspend(continuation);
        }

        [[nodiscard]] worker_wait_result<std::monostate> await_resume() {
            return wait_.take_result();
        }

    private:
        friend class worker_semaphore;

        acquire_operation(std::shared_ptr<semaphore_state> state_value, operation_options options) noexcept :
            wait_(std::move(state_value), std::move(options)) {}

        worker_single_wait_awaiter<std::monostate, semaphore_state> wait_;
    };

    worker_semaphore(const worker_ref& worker_value, std::size_t permits) :
        state_(make_worker_wait_state<semaphore_state>(worker_value, permits)) {}

    [[nodiscard]] acquire_operation acquire(operation_options options = {}) {
        return acquire_operation(state_, std::move(options));
    }

    // Any thread: hands the permit to the pending acquire, or banks it.
    void release() {
        auto lock = state_->lock_state();
        if (state_->wait_slot().occupied()) {
            state_->wait_slot().complete(worker_wait_result<std::monostate>::with_value(std::monostate{}));
        } else {
            ++state_->permits_;
        }
    }

private:
    std::shared_ptr<semaphore_state> state_;
};

struct semaphore_outcomes final {
    bool permit_{false};
    bool nonpositive_rejected_{false};
    bool timed_out_{false};
    bool deadline_passed_{false};
    bool cancelled_{false};
    bool released_{false};
};

task<void> exercise_semaphore(const worker_ref& worker_value, std::binary_semaphore& released, semaphore_outcomes& outcomes) {
    worker_semaphore semaphore(worker_value, 1);
    outcomes.permit_ = (co_await semaphore.acquire()).has_value();
    try {
        static_cast<void>(semaphore.acquire({.timeout_ = std::chrono::milliseconds(0)}));
    } catch (const std::invalid_argument&) {
        outcomes.nonpositive_rejected_ = true;
    }
    outcomes.timed_out_ = (co_await semaphore.acquire({.timeout_ = std::chrono::milliseconds(5)})).status() == worker_wait_status::timed_out;
    outcomes.deadline_passed_ = (co_await semaphore.acquire({.timeout_ = operation_timeout(std::chrono::steady_clock::time_point{})})).status() == worker_wait_status::timed_out;

    stop_source source;
    static_cast<void>(worker_value.post([&source] {
        source.request_stop();
    }));
    outcomes.cancelled_ = (co_await semaphore.acquire({.timeout_ = std::chrono::seconds(30), .stop_token_ = source.token()})).status() == worker_wait_status::cancelled;

    std::thread releaser([&] {
        released.acquire();
        semaphore.release();
    });
    static_cast<void>(worker_value.post([&released] {
        released.release();
    }));
    const auto handed_off = co_await semaphore.acquire({.timeout_ = std::chrono::seconds(30)});
    releaser.join();
    outcomes.released_ = handed_off.has_value();
}

task<void> wait_through_stop(event_loop_attachment& attachment, worker_semaphore& semaphore, bool& stopping) {
    asio::post(attachment.loop().executor(), [&attachment] {
        attachment.stop();
    });
    stopping = (co_await semaphore.acquire()).status() == worker_wait_status::worker_stopping;
}

TEST(Ruvia, PublicWorkerWaitBlocksComposeBoundSemaphore) {
    asio::io_context io_context;
    auto attachment = attach_event_loop(io_context, {.queue_capacity_ = 8});
    const auto worker_value = attachment.loop().worker();
    std::binary_semaphore released{0};
    semaphore_outcomes outcomes;
    run_worker_tasks(attachment, exercise_semaphore(worker_value, released, outcomes));
    EXPECT_TRUE(outcomes.permit_);
    EXPECT_TRUE(outcomes.nonpositive_rejected_);
    EXPECT_TRUE(outcomes.timed_out_);
    EXPECT_TRUE(outcomes.deadline_passed_);
    EXPECT_TRUE(outcomes.cancelled_);
    EXPECT_TRUE(outcomes.released_);
}

TEST(Ruvia, PublicWorkerWaitBlocksObserveWorkerStopping) {
    asio::io_context io_context;
    auto attachment = attach_event_loop(io_context, {.queue_capacity_ = 8});
    const auto worker_value = attachment.loop().worker();
    worker_semaphore semaphore(worker_value, 0);
    bool stopping = false;
    run_worker_tasks(attachment, wait_through_stop(attachment, semaphore, stopping));
    EXPECT_TRUE(stopping);
    EXPECT_THROW(worker_semaphore(worker_value, 0), std::runtime_error);
}

struct counting_observer final {
    void worker_stopping() noexcept {
        ++stopping_;
    }
    void worker_stopping_complete() noexcept {
        complete_after_stopping_ = stopping_ == 1;
    }

    worker_shutdown_registration registration_;
    int stopping_{0};
    bool complete_after_stopping_{false};
};

TEST(Ruvia, WorkerShutdownRegistrationRunsBothPhasesOnce) {
    auto observer = std::make_shared<counting_observer>();
    auto dropped = std::make_shared<counting_observer>();
    {
        asio::io_context io_context;
        auto attachment = attach_event_loop(io_context, {.queue_capacity_ = 8});
        const auto worker_value = attachment.loop().worker();
        EXPECT_EQ(worker_value.register_shutdown(observer, &counting_observer::registration_), worker_registration_status::registered);
        EXPECT_THROW(static_cast<void>(worker_value.register_shutdown(observer, &counting_observer::registration_)), std::logic_error);
        EXPECT_EQ(worker_value.register_shutdown(dropped, &counting_observer::registration_), worker_registration_status::registered);
        // Destroying the owner unlinks it before the worker stops.
        dropped.reset();
        attachment.stop();
        io_context.run();
        EXPECT_EQ(worker_value.register_shutdown(std::make_shared<counting_observer>(), &counting_observer::registration_), worker_registration_status::worker_stopping);
    }
    EXPECT_EQ(observer->stopping_, 1);
    EXPECT_TRUE(observer->complete_after_stopping_);
    EXPECT_EQ(worker_ref{}.register_shutdown(observer, &counting_observer::registration_), worker_registration_status::worker_stopping);
}

TEST(Ruvia, WorkerCompletionLaneBypassesFullQueue) {
    asio::io_context io_context;
    auto attachment = attach_event_loop(io_context, {.queue_capacity_ = 1});
    const auto worker_value = attachment.loop().worker();
    int ran = 0;
    ASSERT_TRUE(worker_value
            .post([&ran] {
        ++ran;
    }).accepted());
    ASSERT_EQ(worker_value
                  .post([&ran] {
        ++ran;
    }).status(),
        post_status::queue_full);
    EXPECT_TRUE(worker_value.defer_completion([&ran] {
        ran += 10;
    }));
    EXPECT_THROW(static_cast<void>(worker_value.defer_completion(move_only_function<void()>{})), std::invalid_argument);
    io_context.run_one();
    io_context.poll();
    EXPECT_EQ(ran, 11);
    attachment.stop();
    io_context.run();
    bool destroyed = false;
    struct destruction_flag final {
        bool* destroyed_;
        destruction_flag(const destruction_flag&) = delete;
        destruction_flag(destruction_flag&& other) noexcept :
            destroyed_(std::exchange(other.destroyed_, nullptr)) {}
        explicit destruction_flag(bool& destroyed) noexcept :
            destroyed_(&destroyed) {}
        ~destruction_flag() {
            if (destroyed_ != nullptr) {
                *destroyed_ = true;
            }
        }
    };
    EXPECT_FALSE(worker_value.defer_completion([flag = destruction_flag(destroyed)] {}));
    EXPECT_TRUE(destroyed);
    EXPECT_FALSE(worker_ref{}.defer_completion([] {}));
}

TEST(Ruvia, WorkerWaitResultFactoriesFixCategory) {
    const auto value = worker_wait_result<int>::with_value(7);
    EXPECT_EQ(value.status(), worker_wait_status::value);
    EXPECT_EQ(value.value(), 7);
    const auto timed_out = worker_wait_result<int>::with_status(worker_wait_status::timed_out);
    EXPECT_EQ(timed_out.status(), worker_wait_status::timed_out);
    EXPECT_FALSE(timed_out.has_value());
    EXPECT_THROW(static_cast<void>(worker_wait_result<int>::with_status(worker_wait_status::value)), std::invalid_argument);
}

TEST(Ruvia, BlockingResultFactoriesFixCategory) {
    auto value = blocking_result<int>::with_value(5);
    EXPECT_TRUE(value.completed());
    EXPECT_EQ(std::move(value).take_value(), 5);
    const auto void_value = blocking_result<void>::with_value();
    EXPECT_TRUE(void_value.completed());
    const auto failed = blocking_result<int>::with_error(std::make_exception_ptr(std::runtime_error("failed")));
    EXPECT_TRUE(failed.failed());
    EXPECT_EQ(failed.status(), blocking_status::completed);
    EXPECT_THROW(static_cast<void>(failed.value()), std::runtime_error);
    const auto rejected = blocking_result<int>::with_status(blocking_status::queue_full);
    EXPECT_EQ(rejected.status(), blocking_status::queue_full);
    EXPECT_THROW(static_cast<void>(rejected.value()), blocking_operation_rejected);
    EXPECT_THROW(static_cast<void>(blocking_result<int>::with_status(blocking_status::completed)), std::invalid_argument);
    EXPECT_THROW(static_cast<void>(blocking_result<int>::with_error(nullptr)), std::invalid_argument);
}

}  // namespace ruvia::test::worker_wait_tests

namespace ruvia::test::one_shot_tests {

struct payload_state final {
    std::function<void()> reenter_;
    int destroyed_{};
    bool query_on_move_{};
    bool throw_on_move_{};
};

struct payload final {
    payload() = default;
    explicit payload(payload_state& state) :
        value_(std::make_unique<int>(42)),
        state_(&state) {}
    payload(const payload&) = delete;
    payload& operator=(const payload&) = delete;
    payload(payload&& other) :
        state_(other.state_) {
        if (state_ != nullptr) {
            if (state_->query_on_move_) {
                state_->reenter_();
            }
            if (state_->throw_on_move_) {
                throw std::runtime_error("payload move failed");
            }
        }
        value_ = std::move(other.value_);
    }
    ~payload() {
        if (value_) {
            value_.reset();
            ++state_->destroyed_;
            state_->reenter_();
        }
    }

    std::unique_ptr<int> value_;
    payload_state* state_{};
};

task<void> exercise_payload_retirement(worker_ref worker, bool& success) {
    for (const auto consume : {false, true}) {
        payload_state state;
        auto [completion, receiver] = make_one_shot<payload>(worker);
        auto observed = one_shot_complete_status::completed;
        state.reenter_ = [&] {
            receiver.close();
            observed = completion.complete(payload{}).status();
        };
        if (!completion.complete(payload(state)).accepted()) {
            co_return;
        }
        if (consume) {
            state.query_on_move_ = true;
            {
                const auto result = co_await receiver.wait();
                if (!result.has_value() || *result.value().value_ != 42 || observed != one_shot_complete_status::already_completed) {
                    co_return;
                }
            }
        } else {
            receiver.close();
            if (observed != one_shot_complete_status::receiver_closed) {
                co_return;
            }
        }
        const auto closed = co_await receiver.wait();
        if (closed.status() != worker_wait_status::closed || state.destroyed_ != 1) {
            co_return;
        }
    }
    {
        payload_state state;
        auto [completion, receiver] = make_one_shot<payload>(worker);
        state.reenter_ = [&] {
            receiver.close();
        };
        if (!completion.complete(payload(state)).accepted()) {
            co_return;
        }
        state.throw_on_move_ = true;
        bool failed = false;
        try {
            static_cast<void>(co_await receiver.wait());
        } catch (const std::runtime_error&) {
            failed = true;
        }
        state.throw_on_move_ = false;
        if (!failed || state.destroyed_ != 1 || (co_await receiver.wait()).status() != worker_wait_status::closed) {
            co_return;
        }
    }
    {
        payload_state state;
        auto [completion, receiver] = make_one_shot<payload>(worker);
        state.reenter_ = [&] {
            receiver.close();
        };
        receiver.close();
        state.query_on_move_ = true;
        {
            auto rejected = completion.complete(payload(state));
            if (rejected != one_shot_complete_status::receiver_closed || rejected == one_shot_complete_status::completed || rejected.rejected() == nullptr || !rejected.rejected()->value_) {
                co_return;
            }
            auto taken = std::move(rejected).take_rejected();
            if (rejected.rejected() != nullptr || !taken.value_ || *taken.value_ != 42) {
                co_return;
            }
        }
        if (state.destroyed_ != 1) {
            co_return;
        }
    }
    success = true;
}

bool test_completion_move_can_close_receiver(bool stop_worker) {
    asio::io_context io;
    auto attachment = attach_event_loop(io, {.queue_capacity_ = 8});
    payload_state state;
    auto [completion, receiver] = make_one_shot<payload>(attachment.loop().worker());
    std::binary_semaphore requested{0};
    std::binary_semaphore closed{0};
    bool observe = true;
    bool closed_during_move = false;
    const auto close = [&] {
        if (stop_worker) {
            attachment.stop();
        } else {
            receiver.close();
        }
    };
    state.query_on_move_ = true;
    state.reenter_ = [&] {
        if (std::exchange(observe, false)) {
            requested.release();
            closed_during_move = closed.try_acquire_for(std::chrono::milliseconds(300));
            if (closed_during_move) {
                close();
            }
        }
    };
    std::thread closer([&] {
        requested.acquire();
        close();
        closed.release();
    });
    bool rejected = false;
    {
        const auto result = completion.complete(payload(state));
        closer.join();
        rejected = result.status() == (stop_worker ? one_shot_complete_status::worker_stopping : one_shot_complete_status::receiver_closed) && result.rejected() != nullptr && result.rejected()->value_ && *result.rejected()->value_ == 42;
    }
    receiver.close();
    return rejected && closed_during_move && state.destroyed_ == 1;
}

task<void> verify_payload_value(one_shot_receiver<payload>& receiver, bool& success) {
    const auto result = co_await receiver.wait();
    success = result.has_value() && result.value().value_ && *result.value().value_ == 42;
}

bool test_concurrent_completion_reservation(bool throw_first) {
    asio::io_context io;
    auto attachment = attach_event_loop(io, {.queue_capacity_ = 8});
    payload_state first_state;
    payload_state retry_state;
    auto [completion, receiver] = make_one_shot<payload>(attachment.loop().worker());
    std::binary_semaphore entered{0};
    std::binary_semaphore proceed{0};
    std::binary_semaphore second_finished{0};
    bool observe = true;
    first_state.query_on_move_ = true;
    first_state.throw_on_move_ = throw_first;
    first_state.reenter_ = [&] {
        if (std::exchange(observe, false)) {
            entered.release();
            proceed.acquire();
        }
    };
    retry_state.reenter_ = [] {};
    bool first_completed = false;
    bool first_failed = false;
    std::thread first([&] {
        try {
            first_completed = completion.complete(payload(first_state)).accepted();
        } catch (const std::runtime_error&) {
            first_failed = true;
        }
    });
    entered.acquire();
    one_shot_complete_status second_status = one_shot_complete_status::completed;
    std::thread second([&] {
        second_status = completion.complete(payload{}).status();
        second_finished.release();
    });
    const bool concurrent = second_finished.try_acquire_for(std::chrono::milliseconds(300));
    proceed.release();
    first.join();
    second.join();
    const bool retried = !throw_first || completion.complete(payload(retry_state)).accepted();
    if ((!first_completed && !retried) || (!first_completed && !throw_first)) {
        receiver.close();
    }
    bool received = false;
    ruvia::test::run_worker_tasks(attachment, verify_payload_value(receiver, received));
    return concurrent && second_status == one_shot_complete_status::in_progress && first_completed == !throw_first && first_failed == throw_first && retried && received;
}

task<void> wait_for_cancellation(one_shot_receiver<payload>& receiver, stop_token token, bool& cancelled) {
    const auto result = co_await receiver.wait({.stop_token_ = std::move(token)});
    cancelled = result.status() == worker_wait_status::cancelled;
}

task<void> exercise_cancellation_during_completion(worker_ref worker, bool& success) {
    payload_state state;
    auto [completion, receiver] = make_one_shot<payload>(worker);
    std::binary_semaphore entered{0};
    std::binary_semaphore proceed{0};
    std::binary_semaphore cancelled_before_release{0};
    bool observe = true;
    state.query_on_move_ = true;
    state.reenter_ = [&] {
        if (std::exchange(observe, false)) {
            entered.release();
            proceed.acquire();
        }
    };
    bool completed = false;
    std::thread producer([&] {
        completed = completion.complete(payload(state)).accepted();
    });
    entered.acquire();
    bool cancellation_won = false;
    std::thread release([&] {
        cancellation_won = cancelled_before_release.try_acquire_for(std::chrono::milliseconds(300));
        proceed.release();
    });
    stop_source source;
    bool cancelled = false;
    task_scope scope(worker);
    scope.spawn(wait_for_cancellation(receiver, source.token(), cancelled));
    source.request_stop();
    cancelled_before_release.release();
    release.join();
    producer.join();
    if (!completed) {
        receiver.close();
    }
    co_await scope.join();
    const auto result = co_await receiver.wait();
    success = cancellation_won && cancelled && completed && result.has_value() && result.value().value_ && *result.value().value_ == 42;
}

}  // namespace ruvia::test::one_shot_tests

namespace {

class throwing_move final {
public:
    explicit throwing_move(int value) noexcept :
        value_(value) {}

    throwing_move(const throwing_move&) = delete;
    throwing_move& operator=(const throwing_move&) = delete;
    // This fixture intentionally models a move that can throw.
    throwing_move(throwing_move&& other) noexcept(false) {
        if (throw_on_move_) {
            throw std::runtime_error("requested move failure");
        }
        value_ = std::exchange(other.value_, 0);
    }

    [[nodiscard]] int value() const noexcept {
        return value_;
    }

    static inline bool throw_on_move_{false};

private:
    int value_{0};
};

ruvia::task<void> wait_for_value(ruvia::one_shot_receiver<int>& receiver, int expected, bool& success) {
    const auto result_value = co_await receiver.wait({.timeout_ = std::chrono::seconds(1)});
    success = result_value.status() == ruvia::worker_wait_status::value && result_value.has_value() && result_value.value() == expected;
}

ruvia::task<void> wait_for_worker_stopping(ruvia::event_loop_attachment& attachment, ruvia::one_shot_receiver<int>& receiver, bool& success) {
    asio::post(attachment.loop().executor(), [&attachment] {
        attachment.stop();
    });
    const auto result_value = co_await receiver.wait();
    success = result_value.status() == ruvia::worker_wait_status::worker_stopping && !result_value.has_value();
}

ruvia::task<void> settled_outcomes_survive_worker_stopping(ruvia::event_loop_attachment& attachment, ruvia::one_shot_completion<int> delivered_completion, ruvia::one_shot_receiver<int>& delivered_receiver, ruvia::one_shot_completion<int> closed_completion, ruvia::one_shot_receiver<int>& closed_receiver, bool& success) {
    const bool completed = delivered_completion.complete(1).accepted();
    const auto delivered = co_await delivered_receiver.wait();
    closed_receiver.close();
    attachment.stop();
    const auto duplicate = delivered_completion.complete(2);
    const auto after_consumed = co_await delivered_receiver.wait();
    const auto after_close = closed_completion.complete(3);
    success = completed && delivered.has_value() && delivered.value() == 1 && duplicate.status() == ruvia::one_shot_complete_status::already_completed && after_consumed.status() == ruvia::worker_wait_status::closed && after_close.status() == ruvia::one_shot_complete_status::receiver_closed;
}

ruvia::task<void> wait_until_closed(ruvia::one_shot_receiver<int>& receiver, bool& success) {
    const auto result_value = co_await receiver.wait({.timeout_ = std::chrono::seconds(1)});
    success = result_value.status() == ruvia::worker_wait_status::closed;
}

ruvia::one_shot_receiver<int>::wait_operation make_cold_wait_after_receiver_close(ruvia::worker_ref worker_value, bool timed, bool completed = false) {
    auto [completion, receiver] = ruvia::make_one_shot<int>(std::move(worker_value));
    if (completed) {
        static_cast<void>(completion.complete(7));
    }
    if (timed) {
        return receiver.wait({.timeout_ = std::chrono::seconds(1)});
    }
    return receiver.wait();
}

ruvia::task<void> verify_cold_wait_operations(ruvia::one_shot_receiver<int>::wait_operation wait, ruvia::one_shot_receiver<int>::wait_operation timed_wait, ruvia::one_shot_receiver<int>::wait_operation completed_wait, ruvia::one_shot_receiver<int>::wait_operation completed_timed_wait, bool& success) {
    const auto cold_closed = co_await std::move(wait);
    const auto timed_cold_closed = co_await std::move(timed_wait);
    const auto completed_cold_closed = co_await std::move(completed_wait);
    const auto completed_timed_cold_closed = co_await std::move(completed_timed_wait);
    success = cold_closed.status() == ruvia::worker_wait_status::closed && timed_cold_closed.status() == ruvia::worker_wait_status::closed && completed_cold_closed.status() == ruvia::worker_wait_status::closed && completed_timed_cold_closed.status() == ruvia::worker_wait_status::closed;
}

ruvia::task<void> exercise(ruvia::worker_ref worker_value, bool& success) {
    {
        auto [completion, receiver] = ruvia::make_one_shot<throwing_move>(worker_value);
        bool move_failed = false;
        throwing_move::throw_on_move_ = true;
        try {
            static_cast<void>(completion.complete(throwing_move(5)));
        } catch (const std::runtime_error&) {
            move_failed = true;
        }
        throwing_move::throw_on_move_ = false;
        if (!move_failed || !completion.complete(throwing_move(6)).accepted()) {
            co_return;
        }
        const auto result_value = co_await receiver.wait();
        if (!result_value.has_value() || result_value.value().value() != 6) {
            co_return;
        }
    }

    {
        auto [completion, receiver] = ruvia::make_one_shot<int>(worker_value);
        const auto completed = completion.complete(7);
        const auto duplicate = completion.complete(8);
        if (!completed.accepted() || duplicate.status() != ruvia::one_shot_complete_status::already_completed || duplicate.rejected() == nullptr || *duplicate.rejected() != 8) {
            co_return;
        }
        const auto result_value = co_await receiver.wait();
        if (!result_value.has_value() || result_value.value() != 7) {
            co_return;
        }
    }

    {
        auto [completion, receiver] = ruvia::make_one_shot<int>(worker_value);
        const auto timeout = co_await receiver.wait({.timeout_ = std::chrono::milliseconds(1)});
        if (timeout.status() != ruvia::worker_wait_status::timed_out || !completion.complete(9).accepted()) {
            co_return;
        }
        const auto late = co_await receiver.wait();
        if (!late.has_value() || late.value() != 9) {
            co_return;
        }
    }

    {
        ruvia::stop_source source;
        auto [completion, receiver] = ruvia::make_one_shot<int>(worker_value);
        static_cast<void>(worker_value.post([&source] {
            source.request_stop();
        }));
        const auto cancelled = co_await receiver.wait({.timeout_ = std::chrono::seconds(1), .stop_token_ = source.token()});
        if (cancelled.status() != ruvia::worker_wait_status::cancelled || !completion.complete(11).accepted()) {
            co_return;
        }
        const auto late = co_await receiver.wait();
        if (!late.has_value() || late.value() != 11) {
            co_return;
        }
    }

    {
        ruvia::stop_source source;
        auto [completion, receiver] = ruvia::make_one_shot<int>(worker_value);
        bool first_cancelled = false;
        ruvia::task_scope scope(worker_value);
        scope.spawn([](ruvia::one_shot_receiver<int>& rx, ruvia::stop_token token, bool& cancelled) -> ruvia::task<void> {
            const auto outcome = co_await rx.wait({.stop_token_ = std::move(token)});
            cancelled = outcome.status() == ruvia::worker_wait_status::cancelled;
        }(receiver, source.token(), first_cancelled));
        source.request_stop();
        if (!completion.complete(88).accepted()) {
            receiver.close();
            co_await scope.join();
            co_return;
        }
        co_await scope.join();
        const auto preserved = co_await receiver.wait();
        if (!first_cancelled || !preserved.has_value() || preserved.value() != 88) {
            co_return;
        }
    }
    {
        ruvia::stop_source source;
        source.request_stop();
        auto [completion, receiver] = ruvia::make_one_shot<int>(worker_value);
        const auto cancelled = co_await receiver.wait({.timeout_ = std::chrono::seconds(1), .stop_token_ = source.token()});
        if (cancelled.status() != ruvia::worker_wait_status::cancelled || !completion.complete(12).accepted()) {
            co_return;
        }
        const auto late = co_await receiver.wait();
        if (!late.has_value() || late.value() != 12) {
            co_return;
        }
    }

    {
        auto [completion, receiver] = ruvia::make_one_shot<int>(worker_value);
        receiver.close();
        const auto closed = co_await receiver.wait();
        const auto rejected = completion.complete(10);
        if (closed.status() != ruvia::worker_wait_status::closed || rejected.status() != ruvia::one_shot_complete_status::receiver_closed || rejected.rejected() == nullptr || *rejected.rejected() != 10) {
            co_return;
        }
    }

    {
        auto [completion, receiver] = ruvia::make_one_shot<std::shared_ptr<int>>(worker_value);
        auto value = std::make_shared<int>(14);
        const std::weak_ptr<int> value_lifetime(value);
        auto wait = receiver.wait();
        if (!completion.complete(std::move(value)).accepted()) {
            co_return;
        }
        receiver.close();
        if (!value_lifetime.expired()) {
            co_return;
        }
        const auto closed = co_await std::move(wait);
        const auto rejected = completion.complete(std::make_shared<int>(15));
        if (closed.status() != ruvia::worker_wait_status::closed || closed.has_value() || rejected.status() != ruvia::one_shot_complete_status::receiver_closed || rejected.rejected() == nullptr || **rejected.rejected() != 15) {
            co_return;
        }
    }

    {
        auto [completion, receiver] = ruvia::make_one_shot<int>(worker_value);
        bool pending_closed = false;
        ruvia::task_scope scope(worker_value);
        scope.spawn(wait_until_closed(receiver, pending_closed));
        bool duplicate_rejected = false;
        try {
            static_cast<void>(co_await receiver.wait());
        } catch (const std::logic_error&) {
            duplicate_rejected = true;
        }
        receiver.close();
        co_await scope.join();
        if (!pending_closed || !duplicate_rejected || completion.complete(13).status() != ruvia::one_shot_complete_status::receiver_closed) {
            co_return;
        }
    }

    auto [completion, receiver] = ruvia::make_one_shot<int>(worker_value);
    auto active_receiver = std::move(receiver);
    ruvia::one_shot_completion<int> empty_completion;
    if (empty_completion.valid() || !completion.valid() || receiver.valid() || !active_receiver.valid()) {
        co_return;
    }
    ruvia::task_scope scope(worker_value);
    scope.spawn(wait_for_value(active_receiver, 42, success));
    const bool completed = completion.complete(42).accepted();
    if (!completed) {
        active_receiver.close();
    }
    co_await scope.join();
    success = success && completed;
}

}  // namespace

TEST(Ruvia, OneShotLifecycle) {
    bool success = false;
    bool cold_wait_operations_safe = false;
    bool payload_retirement_safe = false;
    bool cancellation_during_completion = false;
    {
        asio::io_context io_context;
        auto attachment = ruvia::attach_event_loop(io_context, {.queue_capacity_ = 8});
        const auto worker_value = attachment.loop().worker();
        auto cold_wait = make_cold_wait_after_receiver_close(worker_value, false);
        auto timed_cold_wait = make_cold_wait_after_receiver_close(worker_value, true);
        auto completed_cold_wait = make_cold_wait_after_receiver_close(worker_value, false, true);
        auto completed_timed_cold_wait = make_cold_wait_after_receiver_close(worker_value, true, true);
        ruvia::test::run_worker_tasks(attachment, exercise(worker_value, success), verify_cold_wait_operations(std::move(cold_wait), std::move(timed_cold_wait), std::move(completed_cold_wait), std::move(completed_timed_cold_wait), cold_wait_operations_safe), ruvia::test::one_shot_tests::exercise_payload_retirement(worker_value, payload_retirement_safe), ruvia::test::one_shot_tests::exercise_cancellation_during_completion(worker_value, cancellation_during_completion));
    }

    bool worker_stopping = false;
    {
        asio::io_context io_context;
        auto attachment = ruvia::attach_event_loop(io_context, {.queue_capacity_ = 8});
        const auto worker_value = attachment.loop().worker();
        auto [completion, receiver] = ruvia::make_one_shot<int>(worker_value);
        ruvia::test::run_worker_tasks(attachment, wait_for_worker_stopping(attachment, receiver, worker_stopping));
    }

    bool settled_outcomes_kept = false;
    {
        asio::io_context io_context;
        auto attachment = ruvia::attach_event_loop(io_context, {.queue_capacity_ = 8});
        const auto worker_value = attachment.loop().worker();
        auto [delivered_completion, delivered_receiver] = ruvia::make_one_shot<int>(worker_value);
        auto [closed_completion, closed_receiver] = ruvia::make_one_shot<int>(worker_value);
        ruvia::test::run_worker_tasks(attachment, settled_outcomes_survive_worker_stopping(attachment, std::move(delivered_completion), delivered_receiver, std::move(closed_completion), closed_receiver, settled_outcomes_kept));
    }

    bool cross_thread_value = false;
    bool cross_thread_completed = false;
    {
        asio::io_context io_context;
        auto attachment = ruvia::attach_event_loop(io_context, {.queue_capacity_ = 8});
        const auto worker_value = attachment.loop().worker();
        auto [completion, receiver] = ruvia::make_one_shot<int>(worker_value);
        std::binary_semaphore receiver_scheduled{0};
        asio::post(io_context, [&receiver_scheduled] {
            receiver_scheduled.release();
        });
        std::thread completer([&] {
            receiver_scheduled.acquire();
            cross_thread_completed = completion.complete(77).accepted();
        });
        ruvia::test::run_worker_tasks(attachment, wait_for_value(receiver, 77, cross_thread_value));
        completer.join();
    }
    EXPECT_TRUE(success);
    EXPECT_TRUE(cold_wait_operations_safe);
    EXPECT_TRUE(payload_retirement_safe);
    EXPECT_TRUE(worker_stopping);
    EXPECT_TRUE(settled_outcomes_kept);
    EXPECT_TRUE(cross_thread_value);
    EXPECT_TRUE(cross_thread_completed);
    EXPECT_TRUE(cancellation_during_completion);
}

TEST(Ruvia, OneShotCompletionPinsStateDuringReentrantRebinding) {
    using namespace ruvia::test::one_shot_tests;
    for (const bool fail_move : {false, true}) {
        SCOPED_TRACE(fail_move);
        asio::io_context io;
        auto attachment = ruvia::attach_event_loop(io, {.queue_capacity_ = 8});
        payload_state state;
        auto [completion, receiver] = ruvia::make_one_shot<payload>(attachment.loop().worker());
        std::optional<ruvia::one_shot_receiver<payload>> owner(std::move(receiver));
        owner->close();
        bool first_move = true;
        bool state_pinned = false;
        state.query_on_move_ = true;
        state.throw_on_move_ = fail_move;
        state.reenter_ = [&] {
            if (std::exchange(first_move, false)) {
                owner.reset();
                completion = {};
                state_pinned = !completion.valid();
            }
        };

        if (fail_move) {
            EXPECT_THROW(static_cast<void>(completion.complete(payload(state))), std::runtime_error);
        } else {
            const auto result = completion.complete(payload(state));
            EXPECT_EQ(result.status(), ruvia::one_shot_complete_status::receiver_closed);
            ASSERT_NE(result.rejected(), nullptr);
            ASSERT_NE(result.rejected()->value_, nullptr);
            EXPECT_EQ(*result.rejected()->value_, 42);
        }
        EXPECT_TRUE(state_pinned);
        EXPECT_FALSE(completion.valid());
        EXPECT_EQ(state.destroyed_, 1);
    }
}

TEST(Ruvia, OneShotCompletionMoveCanCloseReceiver) {
    EXPECT_TRUE(ruvia::test::one_shot_tests::test_completion_move_can_close_receiver(false));
}

TEST(Ruvia, OneShotCompletionMoveCanStopWorker) {
    EXPECT_TRUE(ruvia::test::one_shot_tests::test_completion_move_can_close_receiver(true));
}

TEST(Ruvia, OneShotConcurrentCompletionReservation) {
    EXPECT_TRUE(ruvia::test::one_shot_tests::test_concurrent_completion_reservation(false));
}

TEST(Ruvia, OneShotConcurrentCompletionReservationAfterThrow) {
    EXPECT_TRUE(ruvia::test::one_shot_tests::test_concurrent_completion_reservation(true));
}

TEST(Ruvia, WorkerDeliveryFailedExtractionRetainsRejectedPayload) {
    using namespace ruvia::test::one_shot_tests;
    payload_state state;
    state.reenter_ = [] {};
    {
        auto result = ruvia::one_shot_completion<payload>{}.complete(payload(state));
        state.throw_on_move_ = true;
        EXPECT_THROW(static_cast<void>(std::move(result).take_rejected()), std::runtime_error);
        state.throw_on_move_ = false;
        EXPECT_EQ(state.destroyed_, 0);
        EXPECT_EQ(result.status(), ruvia::one_shot_complete_status::receiver_closed);
        ASSERT_NE(result.rejected(), nullptr);
        ASSERT_NE(result.rejected()->value_, nullptr);
        EXPECT_EQ(*result.rejected()->value_, 42);
        const auto extracted = std::move(result).take_rejected();
        ASSERT_NE(extracted.value_, nullptr);
        EXPECT_EQ(*extracted.value_, 42);
        EXPECT_EQ(result.rejected(), nullptr);
        EXPECT_EQ(result.status(), ruvia::one_shot_complete_status::receiver_closed);
        EXPECT_THROW(static_cast<void>(std::move(result).take_rejected()), std::logic_error);
    }
    EXPECT_EQ(state.destroyed_, 1);
}

TEST(Ruvia, WorkerDeliveryExtractionDuringUnwindCommitsOnlyOnSuccess) {
    using namespace ruvia::test::one_shot_tests;
    using result_type = ruvia::one_shot_complete_result<payload>;
    struct extract_on_unwind final {
        result_type& result_;
        bool& succeeded_;
        bool& failed_;
        ~extract_on_unwind() {
            try {
                const auto value = std::move(result_).take_rejected();
                succeeded_ = value.value_ && *value.value_ == 42;
            } catch (const std::runtime_error&) {
                failed_ = true;
            }
        }
    };
    for (const bool fail_move : {false, true}) {
        SCOPED_TRACE(fail_move);
        payload_state state;
        state.reenter_ = [] {};
        {
            auto result = ruvia::one_shot_completion<payload>{}.complete(payload(state));
            state.throw_on_move_ = fail_move;
            bool succeeded = false;
            bool failed = false;
            try {
                const extract_on_unwind guard{result, succeeded, failed};
                throw std::runtime_error("outer failure");
            } catch (const std::runtime_error&) {
            }
            state.throw_on_move_ = false;
            EXPECT_EQ(succeeded, !fail_move);
            EXPECT_EQ(failed, fail_move);
            if (fail_move) {
                EXPECT_EQ(state.destroyed_, 0);
                ASSERT_NE(result.rejected(), nullptr);
                const auto retry = std::move(result).take_rejected();
                ASSERT_NE(retry.value_, nullptr);
                EXPECT_EQ(*retry.value_, 42);
            }
            EXPECT_EQ(result.rejected(), nullptr);
            EXPECT_EQ(result.status(), ruvia::one_shot_complete_status::receiver_closed);
        }
        EXPECT_EQ(state.destroyed_, 1);
    }
}

TEST(Ruvia, WorkerDeliveryRejectedAccessBypassesPayloadAddressOperator) {
    struct non_addressable final {
        int value_;
        non_addressable* operator&() = delete;
        const non_addressable* operator&() const = delete;
    };
    auto result = ruvia::one_shot_completion<non_addressable>{}.complete(non_addressable{17});
    const auto& constant = result;
    ASSERT_NE(result.rejected(), nullptr);
    ASSERT_NE(constant.rejected(), nullptr);
    EXPECT_EQ(constant.rejected(), result.rejected());
    EXPECT_EQ(result.rejected()->value_, 17);
    EXPECT_EQ(constant.rejected()->value_, 17);
    const auto extracted = std::move(result).take_rejected();
    EXPECT_EQ(extracted.value_, 17);
    EXPECT_EQ(result.rejected(), nullptr);
    EXPECT_EQ(constant.rejected(), nullptr);
}

namespace {

ruvia::task<void> exercise_lease_and_close(ruvia::pool_lease_scheduler& scheduler, asio::io_context& io_context, bool& success) {
    {
        auto discarded_cold_acquire = scheduler.acquire();
        static_cast<void>(discarded_cold_acquire);
    }
    const auto first = co_await scheduler.acquire();
    if (!first.has_value()) {
        co_return;
    }
    const auto index = first.value();
    scheduler.release(index);

    const auto reacquired = co_await scheduler.acquire();
    if (!reacquired.has_value() || reacquired.value() != index) {
        co_return;
    }

    bool handoff_released = false;
    asio::post(io_context, [&scheduler, &handoff_released, index] {
        scheduler.release(index);
        handoff_released = true;
    });
    const auto handed_off = co_await scheduler.acquire();
    if (!handed_off.has_value() || handed_off.value() != index) {
        co_return;
    }

    asio::post(io_context, [&scheduler] {
        (void)scheduler.close();
    });
    const auto waiting_at_close = co_await scheduler.acquire();
    if (!handoff_released || waiting_at_close.status() != ruvia::worker_wait_status::closed || !scheduler.closing() || scheduler.close()) {
        co_return;
    }
    scheduler.release(index);
    const auto after_close = co_await scheduler.acquire();
    success = after_close.status() == ruvia::worker_wait_status::closed;
    auto moved_acquire = scheduler.acquire();
    auto target_acquire = std::move(moved_acquire);
    static_cast<void>(target_acquire);
    bool moved_from_rejected = false;
    try {
        (void)co_await std::move(moved_acquire);
    } catch (const std::logic_error&) {
        moved_from_rejected = true;
    }
    if (!moved_from_rejected) {
        success = false;
        co_return;
    }
}

ruvia::task<void> exercise_worker_bound_acquire_timeout(ruvia::pool_lease_scheduler& scheduler, bool& success) {
    bool zero_rejected = false;
    bool negative_rejected = false;
    try {
        static_cast<void>(scheduler.acquire({.timeout_ = std::chrono::milliseconds(0)}));
    } catch (const std::invalid_argument&) {
        zero_rejected = true;
    }
    try {
        static_cast<void>(scheduler.acquire({.timeout_ = std::chrono::milliseconds(-5)}));
    } catch (const std::invalid_argument&) {
        negative_rejected = true;
    }
    const auto passed_deadline = co_await scheduler.acquire({.timeout_ = ruvia::operation_timeout(std::chrono::steady_clock::time_point{})});
    const auto result_value = co_await scheduler.acquire({.timeout_ = std::chrono::milliseconds(1)});
    const auto deadline_value = co_await scheduler.acquire({.timeout_ = ruvia::operation_timeout(std::chrono::steady_clock::now() + std::chrono::milliseconds(1))});
    success = zero_rejected && negative_rejected && passed_deadline.status() == ruvia::worker_wait_status::timed_out && result_value.status() == ruvia::worker_wait_status::timed_out && deadline_value.status() == ruvia::worker_wait_status::timed_out;
}

ruvia::task<void> exercise_saturated_acquire_timeout(ruvia::pool_lease_scheduler& scheduler, asio::io_context& io_context, bool& success) {
    asio::post(io_context, [&scheduler] {
        (void)scheduler.close();
    });
    const auto result_value = co_await scheduler.acquire({.timeout_ = std::chrono::milliseconds::max()});
    // A maximum positive timeout is effectively unbounded: its worker timer
    // must not wrap into the past and time out before the close notification.
    success = result_value.status() == ruvia::worker_wait_status::closed;
}

ruvia::task<void> exercise_acquire_cancellation(ruvia::pool_lease_scheduler& scheduler, asio::io_context& io_context, const ruvia::worker_ref& worker_value, bool& success) {
    ruvia::stop_source source;
    asio::post(io_context, [&scheduler, &source] {
        source.request_stop();
        (void)scheduler.close();
    });
    const auto result_value = co_await scheduler.acquire({.stop_token_ = source.token()});
    // Cancellation is committed before request_stop() returns. A same-stack
    // close must not replace it with closed while resumption is deferred.
    success = result_value.status() == ruvia::worker_wait_status::cancelled;
}

ruvia::task<void> observe_waiter(ruvia::pool_lease_scheduler& scheduler, std::optional<ruvia::timeout_duration> timeout, ruvia::stop_token token, std::optional<ruvia::worker_wait_status>& status) {
    const auto result = co_await scheduler.acquire({.timeout_ = timeout, .stop_token_ = std::move(token)});
    status = result.status();
    if (result.has_value()) {
        scheduler.release(result.value());
    }
}

ruvia::task<void> expire_then_release(ruvia::pool_lease_scheduler& scheduler, std::size_t slot, ruvia::stop_source& cancellation, std::optional<ruvia::worker_wait_status>& status) {
    const auto result = co_await scheduler.acquire({.timeout_ = std::chrono::milliseconds(1)});
    status = result.status();
    cancellation.request_stop();
    scheduler.release(slot);
}

ruvia::task<void> exercise_timeout_between_cancel_and_release(const ruvia::worker_ref& worker, bool& success) {
    ruvia::pool_lease_scheduler scheduler(1, worker);
    const auto held = co_await scheduler.acquire();
    if (!held.has_value()) {
        co_return;
    }
    ruvia::stop_source cancellation;
    std::optional<ruvia::worker_wait_status> cancelled;
    std::optional<ruvia::worker_wait_status> timed_out;
    std::optional<ruvia::worker_wait_status> leased;
    ruvia::task_scope scope(worker);
    scope.spawn(observe_waiter(scheduler, std::nullopt, cancellation.token(), cancelled));
    scope.spawn(expire_then_release(scheduler, held.value(), cancellation, timed_out));
    scope.spawn(observe_waiter(scheduler, std::chrono::seconds(30), {}, leased));
    co_await scope.join();
    success = cancelled == ruvia::worker_wait_status::cancelled && timed_out == ruvia::worker_wait_status::timed_out && leased == ruvia::worker_wait_status::value;
}

ruvia::task<void> exercise_passed_deadline_does_not_steal_same_turn_release(const ruvia::worker_ref& worker_value, bool& success) {
    ruvia::pool_lease_scheduler scheduler(1, worker_value);
    const ruvia::operation_timeout passed(std::chrono::steady_clock::time_point{});
    const ruvia::operation_timeout now(std::chrono::steady_clock::now());
    const auto first = co_await scheduler.acquire({.timeout_ = passed});
    if (!first.has_value()) {
        co_return;
    }
    bool zero_timed_out = false;
    bool negative_timed_out = false;
    ruvia::task_scope scope(worker_value);
    scope.spawn([&]() -> ruvia::task<void> {
        const auto outcome = co_await scheduler.acquire({.timeout_ = now});
        zero_timed_out = outcome.status() == ruvia::worker_wait_status::timed_out;
    }());
    scope.spawn([&]() -> ruvia::task<void> {
        const auto outcome = co_await scheduler.acquire({.timeout_ = passed});
        negative_timed_out = outcome.status() == ruvia::worker_wait_status::timed_out;
    }());
    scheduler.release(first.value());
    co_await scope.join();
    const auto reacquired = co_await scheduler.acquire({.timeout_ = passed});
    if (!reacquired.has_value() || reacquired.value() != first.value()) {
        co_return;
    }
    scheduler.release(reacquired.value());
    success = zero_timed_out && negative_timed_out;
}
ruvia::task<void> exercise_repeated_lifecycle(ruvia::pool_lease_scheduler& scheduler, asio::io_context& io_context, std::optional<ruvia::worker_wait_result<std::size_t>>& retained_result, bool& success) {
    for (std::size_t i = 0; i < 32; ++i) {
        auto discarded_cold_acquire = scheduler.acquire();
        static_cast<void>(discarded_cold_acquire);
    }

    const auto held = co_await scheduler.acquire();
    if (!held.has_value()) {
        co_return;
    }
    for (std::size_t i = 0; i < 32; ++i) {
        scheduler.release(held.value());
        const auto acquired = co_await scheduler.acquire();
        if (!acquired.has_value() || acquired.value() != held.value()) {
            co_return;
        }
        retained_result.emplace(acquired);
    }

    for (std::size_t i = 0; i < 32; ++i) {
        const auto timed_out = co_await scheduler.acquire({.timeout_ = std::chrono::milliseconds(1)});
        if (timed_out.status() != ruvia::worker_wait_status::timed_out) {
            co_return;
        }

        ruvia::stop_source source;
        asio::post(io_context, [&source] {
            source.request_stop();
        });
        const auto cancelled = co_await scheduler.acquire({.stop_token_ = source.token()});
        if (cancelled.status() != ruvia::worker_wait_status::cancelled) {
            co_return;
        }
    }

    scheduler.release(held.value());
    success = retained_result.has_value() && retained_result->has_value() && retained_result->value() == held.value();
}

ruvia::task<void> observe_timed_out_acquire(ruvia::pool_lease_acquire pending, bool& timed_out) {
    const auto result_value = co_await std::move(pending);
    timed_out = result_value.status() == ruvia::worker_wait_status::timed_out;
}

bool test_scheduler_owns_timeout_for_lazy_acquires() {
    bool success = true;
    for (const bool with_stop_token : {false, true}) {
        asio::io_context context;
        auto attachment = ruvia::attach_event_loop(context, {.queue_capacity_ = 8});
        {
            ruvia::pool_lease_scheduler scheduler(0, attachment.loop().worker());
            std::optional<ruvia::pool_lease_acquire> pending;
            {
                std::optional<ruvia::timeout_duration> timeout = std::chrono::milliseconds(1);
                if (with_stop_token) {
                    pending.emplace(scheduler.acquire({.timeout_ = timeout, .stop_token_ = ruvia::stop_source{}.token()}));
                } else {
                    pending.emplace(scheduler.acquire({.timeout_ = timeout}));
                }
                timeout.reset();
            }
            bool timed_out = false;
            auto prepared = std::move(*pending);
            pending.reset();
            ruvia::test::run_worker_tasks(attachment, observe_timed_out_acquire(std::move(prepared), timed_out));
            success = success && timed_out;
            (void)scheduler.close();
        }
    }
    return success;
}

ruvia::task<void> observe_worker_acquire(ruvia::pool_lease_acquire pending, const ruvia::worker_ref& worker_value, ruvia::stop_source& source, bool& cancelled) {
    static_cast<void>(worker_value.post([&source] {
        source.request_stop();
    }));
    const auto result_value = co_await std::move(pending);
    cancelled = result_value.status() == ruvia::worker_wait_status::cancelled;
}

bool test_scheduler_retains_worker_binding_for_lazy_acquires() {
    asio::io_context context;
    auto attachment = ruvia::attach_event_loop(context, {.queue_capacity_ = 8});
    const auto worker_value = attachment.loop().worker();
    bool success = true;
    {
        std::optional<ruvia::pool_lease_scheduler> scheduler;
        std::optional<ruvia::pool_lease_acquire> pending;
        ruvia::stop_source source;
        {
            auto temporary_worker = worker_value;
            scheduler.emplace(0, temporary_worker);
            pending.emplace(scheduler->acquire({.stop_token_ = source.token()}));
        }
        // An unstarted operation releases its reservation before its scheduler.
        // The scheduler needs a live worker, so this runs before the loop retires.
        {
            ruvia::pool_lease_scheduler temporary_scheduler(1, worker_value);
            auto discarded = temporary_scheduler.acquire({.stop_token_ = ruvia::stop_source{}.token()});
            static_cast<void>(discarded);
        }
        bool cancelled = false;
        auto prepared = std::move(*pending);
        pending.reset();
        ruvia::test::run_worker_tasks(attachment, observe_worker_acquire(std::move(prepared), worker_value, source, cancelled));
        success = success && cancelled;
    }
    return success;
}

ruvia::task<void> exercise_repeated_test(asio::io_context& io_context, const ruvia::worker_ref& worker_value, bool& success) {
    std::optional<ruvia::worker_wait_result<std::size_t>> retained_result;
    {
        ruvia::pool_lease_scheduler scheduler(1, worker_value);
        co_await exercise_repeated_lifecycle(scheduler, io_context, retained_result, success);
    }
}

}  // namespace

namespace ruvia::test::lease_handoff {

struct handoff_state final {
    std::size_t depth_{};
    std::size_t peak_{};
    std::size_t completed_{};
    bool ordered_{true};
};

task<void> release_immediately(pool_lease_scheduler& scheduler, handoff_state& state, std::size_t index) {
    const auto result = co_await scheduler.acquire();
    if (!result.has_value()) {
        state.ordered_ = false;
        co_return;
    }
    state.ordered_ = state.ordered_ && state.completed_ == index;
    ++state.completed_;
    ++state.depth_;
    state.peak_ = std::max(state.peak_, state.depth_);
    scheduler.release(result.value());
    --state.depth_;
}

task<void> exercise_long_chain(const worker_ref& worker, bool& success) {
    constexpr std::size_t waiter_count = 512;
    pool_lease_scheduler scheduler(1, worker);
    task_scope scope(worker);
    handoff_state state;
    const auto held = co_await scheduler.acquire();
    for (std::size_t index = 0; index != waiter_count; ++index) {
        scope.spawn(release_immediately(scheduler, state, index));
    }
    scheduler.release(held.value());
    co_await scope.join();
    success = state.peak_ == 1 && state.depth_ == 0 && state.completed_ == waiter_count && state.ordered_;
}

task<void> exercise_committed_handoff(const worker_ref& worker, bool& success) {
    pool_lease_scheduler scheduler(1, worker);
    task_scope scope(worker);
    stop_source cancellation;
    std::optional<worker_wait_status> handed_off;
    std::optional<worker_wait_status> closed;
    const auto held = co_await scheduler.acquire();
    scope.spawn(observe_waiter(scheduler, std::chrono::seconds(30), cancellation.token(), handed_off));
    scope.spawn(observe_waiter(scheduler, std::nullopt, {}, closed));
    scheduler.release(held.value());
    const bool handoff_deferred = !handed_off.has_value();
    cancellation.request_stop();
    (void)scheduler.close();
    co_await scope.join();
    success = handoff_deferred && handed_off == worker_wait_status::value && closed == worker_wait_status::closed;
}

task<void> exercise_close_defers_resumption(const worker_ref& worker, bool& success) {
    pool_lease_scheduler scheduler(0, worker);
    task_scope scope(worker);
    std::optional<worker_wait_status> first;
    std::optional<worker_wait_status> second;
    scope.spawn(observe_waiter(scheduler, std::nullopt, {}, first));
    scope.spawn(observe_waiter(scheduler, std::chrono::seconds(30), {}, second));
    const bool closed_now = scheduler.close();
    // Waiters observe closure only after the closing caller's turn.
    const bool resumed_inline = first.has_value() || second.has_value();
    co_await scope.join();
    success = closed_now && !resumed_inline && first == worker_wait_status::closed && second == worker_wait_status::closed;
}

}  // namespace ruvia::test::lease_handoff

namespace {

// Like channel and one_shot waits, a worker that stops completes queued and
// saturated acquires as worker_stopping, while a free slot is still leased.
ruvia::task<void> record_acquire_status(ruvia::pool_lease_scheduler& scheduler, ruvia::operation_options options, ruvia::worker_wait_status& status) {
    status = (co_await scheduler.acquire(std::move(options))).status();
}

ruvia::task<void> exercise_acquire_observes_worker_stop(ruvia::event_loop_attachment& attachment, const ruvia::worker_ref& worker_value, bool& success) {
    ruvia::pool_lease_scheduler scheduler(1, worker_value);
    const auto held = co_await scheduler.acquire();
    if (!held.has_value()) {
        co_return;
    }
    auto queued = ruvia::worker_wait_status::value;
    auto timed = ruvia::worker_wait_status::value;
    ruvia::task_scope scope(worker_value);
    scope.spawn(record_acquire_status(scheduler, {}, queued));
    scope.spawn(record_acquire_status(scheduler, {.timeout_ = std::chrono::seconds(30)}, timed));
    static_cast<void>(worker_value.post([&attachment] {
        attachment.stop();
    }));
    co_await scope.join();
    const auto saturated = co_await scheduler.acquire({.timeout_ = std::chrono::seconds(30)});
    scheduler.release(held.value());
    const auto free_slot = co_await scheduler.acquire();
    success = queued == ruvia::worker_wait_status::worker_stopping && timed == ruvia::worker_wait_status::worker_stopping && saturated.status() == ruvia::worker_wait_status::worker_stopping && free_slot.has_value();
    if (free_slot.has_value()) {
        scheduler.release(free_slot.value());
    }
}

}  // namespace

TEST(Ruvia, PoolLeaseSchedulerLifecycle) {
    asio::io_context io_context;
    auto attachment = ruvia::attach_event_loop(io_context, {.queue_capacity_ = 8});
    const auto worker_value = attachment.loop().worker();
    ruvia::pool_lease_scheduler lease_scheduler(1, worker_value);
    ruvia::pool_lease_scheduler saturated_timeout_scheduler(0, worker_value);
    ruvia::pool_lease_scheduler worker_timeout_scheduler(0, worker_value);
    ruvia::pool_lease_scheduler cancellation_scheduler(0, worker_value);
    bool lease_success = false;
    bool worker_timeout_success = false;
    bool nonpositive_timeout_success = false;
    bool saturated_timeout_success = false;
    bool cancellation_success = false;
    bool repeated_lifecycle_success = false;
    bool mixed_waiters_success = false;
    bool handoff_chain_success = false;
    bool committed_handoff_success = false;
    bool close_deferred_success = false;
    ruvia::test::run_worker_tasks(attachment, ruvia::test::lease_handoff::exercise_long_chain(worker_value, handoff_chain_success), ruvia::test::lease_handoff::exercise_committed_handoff(worker_value, committed_handoff_success), ruvia::test::lease_handoff::exercise_close_defers_resumption(worker_value, close_deferred_success), exercise_lease_and_close(lease_scheduler, io_context, lease_success), exercise_worker_bound_acquire_timeout(worker_timeout_scheduler, worker_timeout_success), exercise_passed_deadline_does_not_steal_same_turn_release(worker_value, nonpositive_timeout_success), exercise_saturated_acquire_timeout(saturated_timeout_scheduler, io_context, saturated_timeout_success), exercise_timeout_between_cancel_and_release(worker_value, mixed_waiters_success), exercise_acquire_cancellation(cancellation_scheduler, io_context, worker_value, cancellation_success), exercise_repeated_test(io_context, worker_value, repeated_lifecycle_success));
    EXPECT_TRUE(lease_success);
    EXPECT_TRUE(worker_timeout_success);
    EXPECT_TRUE(nonpositive_timeout_success);
    EXPECT_TRUE(saturated_timeout_success);
    EXPECT_TRUE(cancellation_success);
    EXPECT_TRUE(repeated_lifecycle_success);
    EXPECT_TRUE(mixed_waiters_success);
    EXPECT_TRUE(handoff_chain_success);
    EXPECT_TRUE(committed_handoff_success);
    EXPECT_TRUE(close_deferred_success);
}

TEST(Ruvia, SchedulerOwnsTimeoutForLazyAcquires) {
    EXPECT_TRUE(test_scheduler_owns_timeout_for_lazy_acquires());
}

TEST(Ruvia, SchedulerRetainsWorkerBindingForLazyAcquires) {
    EXPECT_TRUE(test_scheduler_retains_worker_binding_for_lazy_acquires());
}

TEST(Ruvia, PoolLeaseAcquireCompletesWithWorkerStopping) {
    asio::io_context io_context;
    auto attachment = ruvia::attach_event_loop(io_context, {.queue_capacity_ = 8});
    const auto worker_value = attachment.loop().worker();
    bool success = false;
    ruvia::test::run_worker_tasks(attachment, exercise_acquire_observes_worker_stop(attachment, worker_value, success));
    EXPECT_TRUE(success);
}
