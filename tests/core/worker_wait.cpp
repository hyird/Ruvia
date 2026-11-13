#include "ruvia/core/worker_wait.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <concepts>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <future>
#include <initializer_list>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <ratio>
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

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/post.hpp>

#include "ruvia/core/event_loop.h"
#include "ruvia/core/task.h"
#include "ruvia/core/task_scope.h"
#include "ruvia/core/worker_ref.h"

#include "worker_task_fixture.h"

namespace ruvia::test::worker_wait_tests {

// A worker-bound counting semaphore composed only from public building
// blocks: the shared state owns the wait slot and its shutdown registration,
// and each acquire owns a worker_single_wait_awaiter.
struct semaphore_state final {
    semaphore_state(worker_ref worker_value, std::size_t permits) noexcept
        : worker_(std::move(worker_value)),
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

        acquire_operation(std::shared_ptr<semaphore_state> state_value, operation_options options) noexcept
            : wait_(std::move(state_value), std::move(options)) {}

        worker_single_wait_awaiter<std::monostate, semaphore_state> wait_;
    };

    worker_semaphore(const worker_ref& worker_value, std::size_t permits)
        : state_(make_worker_wait_state<semaphore_state>(worker_value, permits)) {}

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

task<void> exercise_semaphore(
    const worker_ref& worker_value, std::binary_semaphore& released, semaphore_outcomes& outcomes) {
    worker_semaphore semaphore(worker_value, 1);
    outcomes.permit_ = (co_await semaphore.acquire()).has_value();
    try {
        static_cast<void>(semaphore.acquire({.timeout_ = std::chrono::milliseconds(0)}));
    } catch (const std::invalid_argument&) {
        outcomes.nonpositive_rejected_ = true;
    }
    outcomes.timed_out_ =
        (co_await semaphore.acquire({.timeout_ = std::chrono::milliseconds(5)})).status() == worker_wait_status::timed_out;
    outcomes.deadline_passed_ =
        (co_await semaphore.acquire({.timeout_ = operation_timeout(std::chrono::steady_clock::time_point{})})).status() ==
        worker_wait_status::timed_out;

    stop_source source;
    static_cast<void>(worker_value.post([&source] { source.request_stop(); }));
    outcomes.cancelled_ =
        (co_await semaphore.acquire({.timeout_ = std::chrono::seconds(30), .stop_token_ = source.token()})).status() ==
        worker_wait_status::cancelled;

    std::thread releaser([&] {
        released.acquire();
        semaphore.release();
    });
    static_cast<void>(worker_value.post([&released] { released.release(); }));
    const auto handed_off = co_await semaphore.acquire({.timeout_ = std::chrono::seconds(30)});
    releaser.join();
    outcomes.released_ = handed_off.has_value();
}

task<void> wait_through_stop(event_loop_attachment& attachment, worker_semaphore& semaphore, bool& stopping) {
    asio::post(attachment.loop().executor(), [&attachment] { attachment.stop(); });
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
        EXPECT_EQ(worker_value.register_shutdown(observer, &counting_observer::registration_),
            worker_registration_status::registered);
        EXPECT_THROW(static_cast<void>(worker_value.register_shutdown(observer, &counting_observer::registration_)),
            std::logic_error);
        EXPECT_EQ(worker_value.register_shutdown(dropped, &counting_observer::registration_),
            worker_registration_status::registered);
        // Destroying the owner unlinks it before the worker stops.
        dropped.reset();
        attachment.stop();
        io_context.run();
        EXPECT_EQ(worker_value.register_shutdown(std::make_shared<counting_observer>(), &counting_observer::registration_),
            worker_registration_status::worker_stopping);
    }
    EXPECT_EQ(observer->stopping_, 1);
    EXPECT_TRUE(observer->complete_after_stopping_);
    EXPECT_EQ(worker_ref{}.register_shutdown(observer, &counting_observer::registration_),
        worker_registration_status::worker_stopping);
}

TEST(Ruvia, WorkerCompletionLaneBypassesFullQueue) {
    asio::io_context io_context;
    auto attachment = attach_event_loop(io_context, {.queue_capacity_ = 1});
    const auto worker_value = attachment.loop().worker();
    int ran = 0;
    ASSERT_TRUE(worker_value.post([&ran] { ++ran; }).accepted());
    ASSERT_EQ(worker_value.post([&ran] { ++ran; }).status(), post_status::queue_full);
    EXPECT_TRUE(worker_value.defer_completion([&ran] { ran += 10; }));
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
        destruction_flag(destruction_flag&& other) noexcept
            : destroyed_(std::exchange(other.destroyed_, nullptr)) {}
        explicit destruction_flag(bool& destroyed) noexcept
            : destroyed_(&destroyed) {}
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
    explicit payload(payload_state& state)
        : value_(std::make_unique<int>(42)),
          state_(&state) {}
    payload(const payload&) = delete;
    payload& operator=(const payload&) = delete;
    payload(payload&& other)
        : state_(other.state_) {
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
                if (!result.has_value() || *result.value().value_ != 42 ||
                    observed != one_shot_complete_status::already_completed) {
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
        state.reenter_ = [&] { receiver.close(); };
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
        if (!failed || state.destroyed_ != 1 ||
            (co_await receiver.wait()).status() != worker_wait_status::closed) {
            co_return;
        }
    }
    {
        payload_state state;
        auto [completion, receiver] = make_one_shot<payload>(worker);
        state.reenter_ = [&] { receiver.close(); };
        receiver.close();
        state.query_on_move_ = true;
        {
            auto rejected = completion.complete(payload(state));
            if (rejected != one_shot_complete_status::receiver_closed ||
                rejected == one_shot_complete_status::completed ||
                rejected.rejected() == nullptr || !rejected.rejected()->value_) {
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
        rejected = result.status() == (stop_worker ? one_shot_complete_status::worker_stopping
                                                   : one_shot_complete_status::receiver_closed) &&
                   result.rejected() != nullptr && result.rejected()->value_ &&
                   *result.rejected()->value_ == 42;
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
    return concurrent && second_status == one_shot_complete_status::in_progress &&
           first_completed == !throw_first && first_failed == throw_first && retried && received;
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
    std::thread producer([&] { completed = completion.complete(payload(state)).accepted(); });
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
    success = cancellation_won && cancelled && completed && result.has_value() &&
              result.value().value_ && *result.value().value_ == 42;
}

}  // namespace ruvia::test::one_shot_tests

namespace {

class throwing_move final {
public:
    explicit throwing_move(int value) noexcept
        : value_(value) {}

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
    success = result_value.status() == ruvia::worker_wait_status::value && result_value.has_value() &&
              result_value.value() == expected;
}

ruvia::task<void> wait_for_worker_stopping(ruvia::event_loop_attachment& attachment,
    ruvia::one_shot_receiver<int>& receiver, bool& success) {
    asio::post(attachment.loop().executor(), [&attachment] { attachment.stop(); });
    const auto result_value = co_await receiver.wait();
    success = result_value.status() == ruvia::worker_wait_status::worker_stopping && !result_value.has_value();
}

ruvia::task<void> settled_outcomes_survive_worker_stopping(ruvia::event_loop_attachment& attachment,
    ruvia::one_shot_completion<int> delivered_completion, ruvia::one_shot_receiver<int>& delivered_receiver,
    ruvia::one_shot_completion<int> closed_completion, ruvia::one_shot_receiver<int>& closed_receiver,
    bool& success) {
    const bool completed = delivered_completion.complete(1).accepted();
    const auto delivered = co_await delivered_receiver.wait();
    closed_receiver.close();
    attachment.stop();
    const auto duplicate = delivered_completion.complete(2);
    const auto after_consumed = co_await delivered_receiver.wait();
    const auto after_close = closed_completion.complete(3);
    success = completed && delivered.has_value() && delivered.value() == 1 &&
              duplicate.status() == ruvia::one_shot_complete_status::already_completed &&
              after_consumed.status() == ruvia::worker_wait_status::closed &&
              after_close.status() == ruvia::one_shot_complete_status::receiver_closed;
}

ruvia::task<void> wait_until_closed(ruvia::one_shot_receiver<int>& receiver, bool& success) {
    const auto result_value = co_await receiver.wait({.timeout_ = std::chrono::seconds(1)});
    success = result_value.status() == ruvia::worker_wait_status::closed;
}

ruvia::one_shot_receiver<int>::wait_operation make_cold_wait_after_receiver_close(
    ruvia::worker_ref worker_value, bool timed, bool completed = false) {
    auto [completion, receiver] = ruvia::make_one_shot<int>(std::move(worker_value));
    if (completed) {
        static_cast<void>(completion.complete(7));
    }
    if (timed) {
        return receiver.wait({.timeout_ = std::chrono::seconds(1)});
    }
    return receiver.wait();
}

ruvia::task<void> verify_cold_wait_operations(ruvia::one_shot_receiver<int>::wait_operation wait,
    ruvia::one_shot_receiver<int>::wait_operation timed_wait,
    ruvia::one_shot_receiver<int>::wait_operation completed_wait,
    ruvia::one_shot_receiver<int>::wait_operation completed_timed_wait, bool& success) {
    const auto cold_closed = co_await std::move(wait);
    const auto timed_cold_closed = co_await std::move(timed_wait);
    const auto completed_cold_closed = co_await std::move(completed_wait);
    const auto completed_timed_cold_closed = co_await std::move(completed_timed_wait);
    success = cold_closed.status() == ruvia::worker_wait_status::closed &&
              timed_cold_closed.status() == ruvia::worker_wait_status::closed &&
              completed_cold_closed.status() == ruvia::worker_wait_status::closed &&
              completed_timed_cold_closed.status() == ruvia::worker_wait_status::closed;
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
        if (!completed.accepted() ||
            duplicate.status() != ruvia::one_shot_complete_status::already_completed ||
            duplicate.rejected() == nullptr || *duplicate.rejected() != 8) {
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
        if (timeout.status() != ruvia::worker_wait_status::timed_out ||
            !completion.complete(9).accepted()) {
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
        static_cast<void>(worker_value.post([&source] { source.request_stop(); }));
        const auto cancelled = co_await receiver.wait({.timeout_ = std::chrono::seconds(1), .stop_token_ = source.token()});
        if (cancelled.status() != ruvia::worker_wait_status::cancelled ||
            !completion.complete(11).accepted()) {
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
        if (cancelled.status() != ruvia::worker_wait_status::cancelled ||
            !completion.complete(12).accepted()) {
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
        if (closed.status() != ruvia::worker_wait_status::closed ||
            rejected.status() != ruvia::one_shot_complete_status::receiver_closed ||
            rejected.rejected() == nullptr || *rejected.rejected() != 10) {
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
        if (closed.status() != ruvia::worker_wait_status::closed || closed.has_value() ||
            rejected.status() != ruvia::one_shot_complete_status::receiver_closed ||
            rejected.rejected() == nullptr || **rejected.rejected() != 15) {
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
        if (!pending_closed || !duplicate_rejected ||
            completion.complete(13).status() != ruvia::one_shot_complete_status::receiver_closed) {
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
        ruvia::test::run_worker_tasks(attachment, exercise(worker_value, success),
            verify_cold_wait_operations(std::move(cold_wait), std::move(timed_cold_wait),
                std::move(completed_cold_wait), std::move(completed_timed_cold_wait),
                cold_wait_operations_safe),
            ruvia::test::one_shot_tests::exercise_payload_retirement(worker_value, payload_retirement_safe),
            ruvia::test::one_shot_tests::exercise_cancellation_during_completion(worker_value, cancellation_during_completion));
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
        ruvia::test::run_worker_tasks(attachment,
            settled_outcomes_survive_worker_stopping(attachment, std::move(delivered_completion), delivered_receiver,
                std::move(closed_completion), closed_receiver, settled_outcomes_kept));
    }

    bool cross_thread_value = false;
    bool cross_thread_completed = false;
    {
        asio::io_context io_context;
        auto attachment = ruvia::attach_event_loop(io_context, {.queue_capacity_ = 8});
        const auto worker_value = attachment.loop().worker();
        auto [completion, receiver] = ruvia::make_one_shot<int>(worker_value);
        std::binary_semaphore receiver_scheduled{0};
        asio::post(io_context, [&receiver_scheduled] { receiver_scheduled.release(); });
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

ruvia::task<void> exercise_lease_and_close(
    ruvia::pool_lease_scheduler& scheduler, asio::io_context& io_context, bool& success) {
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

    asio::post(io_context, [&scheduler] { (void)scheduler.close(); });
    const auto waiting_at_close = co_await scheduler.acquire();
    if (!handoff_released ||
        waiting_at_close.status() != ruvia::worker_wait_status::closed ||
        !scheduler.closing() || scheduler.close()) {
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

ruvia::task<void> exercise_worker_bound_acquire_timeout(
    ruvia::pool_lease_scheduler& scheduler, bool& success) {
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
    success = zero_rejected && negative_rejected &&
              passed_deadline.status() == ruvia::worker_wait_status::timed_out &&
              result_value.status() == ruvia::worker_wait_status::timed_out &&
              deadline_value.status() == ruvia::worker_wait_status::timed_out;
}

ruvia::task<void> exercise_saturated_acquire_timeout(
    ruvia::pool_lease_scheduler& scheduler, asio::io_context& io_context, bool& success) {
    asio::post(io_context, [&scheduler] { (void)scheduler.close(); });
    const auto result_value = co_await scheduler.acquire({.timeout_ = std::chrono::milliseconds::max()});
    // A maximum positive timeout is effectively unbounded: its worker timer
    // must not wrap into the past and time out before the close notification.
    success = result_value.status() == ruvia::worker_wait_status::closed;
}

ruvia::task<void> exercise_acquire_cancellation(ruvia::pool_lease_scheduler& scheduler,
    asio::io_context& io_context, const ruvia::worker_ref& worker_value, bool& success) {
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

ruvia::task<void> observe_waiter(ruvia::pool_lease_scheduler& scheduler,
    std::optional<ruvia::timeout_duration> timeout, ruvia::stop_token token,
    std::optional<ruvia::worker_wait_status>& status) {
    const auto result = co_await scheduler.acquire({.timeout_ = timeout, .stop_token_ = std::move(token)});
    status = result.status();
    if (result.has_value()) {
        scheduler.release(result.value());
    }
}

ruvia::task<void> expire_then_release(ruvia::pool_lease_scheduler& scheduler, std::size_t slot,
    ruvia::stop_source& cancellation, std::optional<ruvia::worker_wait_status>& status) {
    const auto result = co_await scheduler.acquire({.timeout_ = std::chrono::milliseconds(1)});
    status = result.status();
    cancellation.request_stop();
    scheduler.release(slot);
}

ruvia::task<void> exercise_timeout_between_cancel_and_release(
    const ruvia::worker_ref& worker, bool& success) {
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
    success = cancelled == ruvia::worker_wait_status::cancelled &&
              timed_out == ruvia::worker_wait_status::timed_out &&
              leased == ruvia::worker_wait_status::value;
}

ruvia::task<void> exercise_passed_deadline_does_not_steal_same_turn_release(
    const ruvia::worker_ref& worker_value, bool& success) {
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
ruvia::task<void> exercise_repeated_lifecycle(ruvia::pool_lease_scheduler& scheduler,
    asio::io_context& io_context,
    std::optional<ruvia::worker_wait_result<std::size_t>>& retained_result, bool& success) {
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
        asio::post(io_context, [&source] { source.request_stop(); });
        const auto cancelled = co_await scheduler.acquire({.stop_token_ = source.token()});
        if (cancelled.status() != ruvia::worker_wait_status::cancelled) {
            co_return;
        }
    }

    scheduler.release(held.value());
    success = retained_result.has_value() && retained_result->has_value() &&
              retained_result->value() == held.value();
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

ruvia::task<void> observe_worker_acquire(ruvia::pool_lease_acquire pending,
    const ruvia::worker_ref& worker_value, ruvia::stop_source& source, bool& cancelled) {
    static_cast<void>(worker_value.post([&source] { source.request_stop(); }));
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
        ruvia::test::run_worker_tasks(
            attachment, observe_worker_acquire(std::move(prepared), worker_value, source, cancelled));
        success = success && cancelled;
    }
    return success;
}

ruvia::task<void> exercise_repeated_test(asio::io_context& io_context,
    const ruvia::worker_ref& worker_value, bool& success) {
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

task<void> release_immediately(
    pool_lease_scheduler& scheduler, handoff_state& state, std::size_t index) {
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
    success = state.peak_ == 1 && state.depth_ == 0 &&
              state.completed_ == waiter_count && state.ordered_;
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
    success = handoff_deferred && handed_off == worker_wait_status::value &&
              closed == worker_wait_status::closed;
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
    success = closed_now && !resumed_inline && first == worker_wait_status::closed &&
              second == worker_wait_status::closed;
}

}  // namespace ruvia::test::lease_handoff

namespace {

// Like channel and one_shot waits, a worker that stops completes queued and
// saturated acquires as worker_stopping, while a free slot is still leased.
ruvia::task<void> record_acquire_status(ruvia::pool_lease_scheduler& scheduler, ruvia::operation_options options,
    ruvia::worker_wait_status& status) {
    status = (co_await scheduler.acquire(std::move(options))).status();
}

ruvia::task<void> exercise_acquire_observes_worker_stop(
    ruvia::event_loop_attachment& attachment, const ruvia::worker_ref& worker_value, bool& success) {
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
    static_cast<void>(worker_value.post([&attachment] { attachment.stop(); }));
    co_await scope.join();
    const auto saturated = co_await scheduler.acquire({.timeout_ = std::chrono::seconds(30)});
    scheduler.release(held.value());
    const auto free_slot = co_await scheduler.acquire();
    success = queued == ruvia::worker_wait_status::worker_stopping &&
              timed == ruvia::worker_wait_status::worker_stopping &&
              saturated.status() == ruvia::worker_wait_status::worker_stopping && free_slot.has_value();
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
    ruvia::test::run_worker_tasks(attachment,
        ruvia::test::lease_handoff::exercise_long_chain(worker_value, handoff_chain_success),
        ruvia::test::lease_handoff::exercise_committed_handoff(worker_value, committed_handoff_success),
        ruvia::test::lease_handoff::exercise_close_defers_resumption(worker_value, close_deferred_success),
        exercise_lease_and_close(lease_scheduler, io_context, lease_success),
        exercise_worker_bound_acquire_timeout(worker_timeout_scheduler, worker_timeout_success),
        exercise_passed_deadline_does_not_steal_same_turn_release(
            worker_value, nonpositive_timeout_success),
        exercise_saturated_acquire_timeout(saturated_timeout_scheduler, io_context, saturated_timeout_success),
        exercise_timeout_between_cancel_and_release(worker_value, mixed_waiters_success),
        exercise_acquire_cancellation(cancellation_scheduler, io_context, worker_value, cancellation_success),
        exercise_repeated_test(io_context, worker_value, repeated_lifecycle_success));
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

namespace {

template <typename worker_type, typename... token_types>
concept can_sleep_for = requires(worker_type&& worker, token_types&&... tokens) {
    ruvia::sleep_for(std::forward<worker_type>(worker), std::chrono::milliseconds(1),
        std::forward<token_types>(tokens)...);
};

template <typename worker_type, typename... token_types>
concept can_sleep_until = requires(worker_type&& worker, token_types&&... tokens) {
    ruvia::sleep_until(std::forward<worker_type>(worker), std::chrono::steady_clock::time_point{},
        std::forward<token_types>(tokens)...);
};

static_assert(can_sleep_for<ruvia::worker_ref&>);
static_assert(can_sleep_for<const ruvia::worker_ref&>);
static_assert(!can_sleep_for<ruvia::worker_ref&&>);
static_assert(!can_sleep_for<const ruvia::worker_ref&&>);
static_assert(can_sleep_for<const ruvia::worker_ref&, ruvia::stop_token>);
static_assert(!can_sleep_for<const ruvia::worker_ref&&, ruvia::stop_token>);
static_assert(can_sleep_until<ruvia::worker_ref&>);
static_assert(can_sleep_until<const ruvia::worker_ref&>);
static_assert(!can_sleep_until<ruvia::worker_ref&&>);
static_assert(!can_sleep_until<const ruvia::worker_ref&&>);
static_assert(can_sleep_until<const ruvia::worker_ref&, ruvia::stop_token>);
static_assert(!can_sleep_until<const ruvia::worker_ref&&, ruvia::stop_token>);

bool timer_duration_conversions_preserve_integer_ticks_and_saturate() {
    using namespace std::chrono;
    for (const nanoseconds value : {nanoseconds(9'000'000'000'000'000'001LL),
             nanoseconds(-9'000'000'000'000'000'001LL), nanoseconds(1), nanoseconds(-1)}) {
        if (ruvia::saturating_duration_cast(value) != duration_cast<steady_clock::duration>(value)) {
            return false;
        }
    }
    const milliseconds coarse(9'000'000'000'001LL);
    using picoseconds_type = duration<std::int64_t, std::pico>;
    const picoseconds_type fine(9'223'000'000'000'001'000LL);
    if (ruvia::saturating_duration_cast(coarse) != duration_cast<steady_clock::duration>(coarse) ||
        ruvia::saturating_duration_cast(fine) != duration_cast<steady_clock::duration>(fine) ||
        ruvia::saturating_duration_cast(seconds::max()) != steady_clock::duration::max() ||
        ruvia::saturating_duration_cast(seconds::min()) != steady_clock::duration::min() ||
        ruvia::saturating_duration_cast(duration<std::uint64_t, std::nano>((std::numeric_limits<std::uint64_t>::max)())) != steady_clock::duration::max()) {
        return false;
    }
    return ruvia::ceil_milliseconds(duration_cast<steady_clock::duration>(nanoseconds(9'000'000'000'000'000'001LL))) == milliseconds(9'000'000'000'001LL) &&
           ruvia::ceil_milliseconds(duration_cast<steady_clock::duration>(milliseconds(1))) == milliseconds(1) &&
           ruvia::ceil_milliseconds(duration_cast<steady_clock::duration>(nanoseconds(1))) == milliseconds(1) &&
           ruvia::ceil_milliseconds(steady_clock::duration::min()) == milliseconds::zero();
}

bool timer_immediate_shutdown_works() {
    asio::io_context io_context;
    for (int attempt_value = 0; attempt_value < 32; ++attempt_value) {
        io_context.restart();
        auto attachment = ruvia::attach_event_loop(io_context, {.queue_capacity_ = 2});
        const auto worker_value = attachment.loop().worker();
        ruvia::worker_timer_registration registration;
        std::promise<void> stopped;
        auto stopped_ready = stopped.get_future();
        asio::post(io_context, [&] {
            (worker_value).schedule_timer(registration, std::chrono::steady_clock::now() + std::chrono::hours(1), [](ruvia::worker_timer_outcome) {});
            attachment.stop();
            asio::post(io_context, [&] {
                stopped.set_value();
            });
        });
        std::thread worker_thread([&] { attachment.run(); });
        stopped_ready.get();
        worker_thread.join();
    }
    return true;
}

bool off_worker_cancellation_can_race_with_timer_shutdown() {
    for (int attempt_value = 0; attempt_value < 64; ++attempt_value) {
        asio::io_context io_context;
        auto attachment = ruvia::attach_event_loop(io_context, {.queue_capacity_ = 8});
        const auto worker_value = attachment.loop().worker();
        ruvia::worker_timer_registration registration;
        ruvia::worker_timer_cancellation cancellation;
        std::barrier start(2);
        std::atomic_int completions{0};

        asio::post(io_context, [&] {
            (worker_value).schedule_timer(registration, std::chrono::steady_clock::now() + std::chrono::hours(1), [&](ruvia::worker_timer_outcome outcome) {
                if (outcome == ruvia::worker_timer_outcome::cancelled) {
                    completions.fetch_add(1, std::memory_order_relaxed);
                }
            });
            cancellation = registration.cancellation();
            start.arrive_and_wait();
            attachment.stop();
        });

        std::thread worker_thread([&] { attachment.run(); });
        std::thread cancelling_thread([&] {
            start.arrive_and_wait();
            for (int call = 0; call < 64; ++call) {
                cancellation.cancel();
            }
        });
        cancelling_thread.join();
        worker_thread.join();
        if (completions.load(std::memory_order_relaxed) != 1) {
            return false;
        }
    }
    return true;
}

bool off_worker_cancellation_after_context_stop_does_not_expire_later() {
    asio::io_context io_context;
    auto attachment = ruvia::attach_event_loop(io_context, {.queue_capacity_ = 2});
    const auto worker_value = attachment.loop().worker();
    auto registration = std::make_unique<ruvia::worker_timer_registration>();
    bool expired = false;
    bool cancelled = false;

    asio::post(io_context, [&] {
        (worker_value).schedule_timer(*registration, std::chrono::steady_clock::now() + std::chrono::milliseconds(1), [&](ruvia::worker_timer_outcome outcome) {
            expired = outcome == ruvia::worker_timer_outcome::expired;
            cancelled = outcome == ruvia::worker_timer_outcome::cancelled;
        });
        io_context.stop();
    });
    attachment.run();

    // Destroying a registration outside the stopped context quietly cancels
    // its callback, including when the caller later restarts the context.
    registration.reset();

    io_context.restart();
    io_context.run_for(std::chrono::milliseconds(20));
    attachment.stop();
    return !expired && !cancelled;
}

bool off_worker_cancellation_before_queued_stop_timers_does_not_invoke_callback() {
    asio::io_context io_context;
    auto attachment = ruvia::attach_event_loop(io_context, {.queue_capacity_ = 4});
    const auto worker_value = attachment.loop().worker();
    auto registration = std::make_unique<ruvia::worker_timer_registration>();
    bool invoked = false;

    asio::post(io_context, [&] {
        (worker_value).schedule_timer(*registration, std::chrono::steady_clock::now() + std::chrono::hours(1), [&invoked](ruvia::worker_timer_outcome) { invoked = true; });
        io_context.stop();
    });
    attachment.run();

    attachment.stop();
    registration.reset();

    io_context.restart();
    attachment.run();
    return !invoked;
}

bool explicit_timer_cancellation_notifies_after_quiet_destruction() {
    asio::io_context io_context;
    auto attachment = ruvia::attach_event_loop(io_context, {.queue_capacity_ = 2});
    const auto worker_value = attachment.loop().worker();
    auto registration = std::make_unique<ruvia::worker_timer_registration>();
    bool cancelled = false;

    asio::post(io_context, [&] {
        (worker_value).schedule_timer(*registration, std::chrono::steady_clock::now() + std::chrono::hours(1), [&cancelled](ruvia::worker_timer_outcome outcome) {
            cancelled = outcome == ruvia::worker_timer_outcome::cancelled;
        });
        registration->cancel_and_notify();
        registration.reset();
        io_context.stop();
    });
    attachment.run();

    io_context.restart();
    while (io_context.poll() != 0) {
    }
    attachment.stop();
    return cancelled;
}

bool on_worker_token_cancellation_followed_by_owner_cancel_withdraws_callback() {
    asio::io_context io_context;
    auto attachment = ruvia::attach_event_loop(io_context, {.queue_capacity_ = 4});
    const auto worker_value = attachment.loop().worker();
    struct timer_owner final {
        ruvia::worker_timer_registration registration_;
    };
    auto owner = std::make_unique<timer_owner>();
    // The posted cancellation turn runs only after this handler returns, so a
    // callback that runs at all runs after the owner was destroyed.
    bool callback_ran = false;
    auto capture = std::make_shared<int>(1);
    std::weak_ptr<int> weak_capture = capture;

    asio::post(io_context, [&] {
        static_cast<void>(worker_value.schedule_timer(owner->registration_,
            std::chrono::steady_clock::now(),
            [&callback_ran, captured = std::move(capture)](ruvia::worker_timer_outcome) {
                static_cast<void>(captured);
                callback_ran = true;
            }));
        auto cancellation = owner->registration_.cancellation();
        cancellation.cancel();
        owner->registration_.cancel();
        owner.reset();
        attachment.stop();
    });
    attachment.run();
    return !callback_ran && weak_capture.expired();
}

ruvia::task<void> mark_after_sleep(
    ruvia::worker_ref worker_value, bool& completed, bool& reported_elapsed) {
    reported_elapsed = co_await ruvia::sleep_for(worker_value, std::chrono::hours(1)) ==
                       ruvia::timer_sleep_result::elapsed;
    completed = true;
}

ruvia::task<void> exercise(ruvia::event_loop_attachment& attachment,
    ruvia::worker_ref worker_value, bool& success) {
    const bool first_sleep_elapsed = co_await ruvia::sleep_for(worker_value, std::chrono::milliseconds(1)) ==
                                     ruvia::timer_sleep_result::elapsed;

    bool expired = false;
    bool cancelled = false;
    ruvia::worker_timer_registration expired_timer;
    (worker_value).schedule_timer(expired_timer, std::chrono::steady_clock::now(), [&expired](ruvia::worker_timer_outcome outcome) {
        expired = outcome == ruvia::worker_timer_outcome::expired;
    });
    ruvia::worker_timer_registration cancelled_timer;
    (worker_value).schedule_timer(cancelled_timer, std::chrono::steady_clock::now() + std::chrono::hours(1), [&cancelled](ruvia::worker_timer_outcome outcome) {
        cancelled = outcome == ruvia::worker_timer_outcome::cancelled;
    });
    bool quiet_invoked = false;
    ruvia::worker_timer_registration quiet_timer;
    (worker_value).schedule_timer(quiet_timer, std::chrono::steady_clock::now() + std::chrono::hours(1), [&quiet_invoked](ruvia::worker_timer_outcome) {
        quiet_invoked = true;
    });
    if (!expired_timer.registered() || !cancelled_timer.registered() || !quiet_timer.registered()) {
        co_return;
    }
    cancelled_timer.cancel_and_notify();
    // The owner-side cancel() never delivers the completion.
    quiet_timer.cancel();
    static_cast<void>(co_await ruvia::sleep_for(worker_value, std::chrono::milliseconds(1)));

    bool cancelled_sleep_resumed = false;
    bool cancelled_sleep_reported_elapsed = true;
    ruvia::task_scope scope(worker_value);
    scope.spawn(mark_after_sleep(worker_value, cancelled_sleep_resumed, cancelled_sleep_reported_elapsed));
    attachment.stop();
    co_await scope.join();
    // A normal sleep reports elapsed; a shutdown-cancelled sleep resumes but
    // reports not-elapsed so a periodic loop can stop instead of re-sleeping.
    success = expired && cancelled && !quiet_invoked && first_sleep_elapsed && cancelled_sleep_resumed &&
              !cancelled_sleep_reported_elapsed;
}

}  // namespace

// A framework-provided wait that ignores the caller's stop token is a hole in
// every deadline built on that token. These pin that sleep_for's stoppable
// overload closes it, and that stopping it does not depend on the timer having
// been long enough to notice.
ruvia::task<void> exercise_stoppable_sleep(const ruvia::worker_ref& worker_value, bool& success) {
    success = false;

    // Not stopped: the sleep runs to completion and reports elapsed.
    ruvia::stop_source idle;
    if (co_await ruvia::sleep_for(worker_value, std::chrono::milliseconds(1), idle.token()) !=
        ruvia::timer_sleep_result::elapsed) {
        co_return;
    }
    if (co_await ruvia::sleep_until(worker_value,
            std::chrono::steady_clock::now() + std::chrono::milliseconds(1), idle.token()) !=
        ruvia::timer_sleep_result::elapsed) {
        co_return;
    }

    // Already stopped before the call: must not suspend for the full duration.
    ruvia::stop_source stopped;
    stopped.request_stop();
    const auto before = std::chrono::steady_clock::now();
    if (co_await ruvia::sleep_for(worker_value, std::chrono::seconds(30), stopped.token()) !=
        ruvia::timer_sleep_result::stop_requested) {
        co_return;
    }
    if (std::chrono::steady_clock::now() - before > std::chrono::seconds(5)) {
        co_return;
    }

    // Stopped while suspended: the deferred cancel has to cut a sleep that is
    // already parked on the timer queue, which is the case the whole overload
    // exists for.
    ruvia::stop_source inflight;
    static_cast<void>(worker_value.post([&inflight] { inflight.request_stop(); }));
    const auto parked = std::chrono::steady_clock::now();
    if (co_await ruvia::sleep_for(worker_value, std::chrono::seconds(30), inflight.token()) !=
        ruvia::timer_sleep_result::stop_requested) {
        co_return;
    }
    if (std::chrono::steady_clock::now() - parked > std::chrono::seconds(5)) {
        co_return;
    }

    // An unbounded delay saturates instead of overflowing into a negative
    // duration that would report elapsed without ever suspending.
    ruvia::stop_source unbounded;
    static_cast<void>(worker_value.post([&unbounded] { unbounded.request_stop(); }));
    if (co_await ruvia::sleep_for(worker_value, std::chrono::hours::max(), unbounded.token()) !=
        ruvia::timer_sleep_result::stop_requested) {
        co_return;
    }

    // Cancellation from another thread must resume the sleeping task with the
    // public stop_requested outcome.
    ruvia::stop_source cross_thread;
    std::thread stopper([&cross_thread] {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        cross_thread.request_stop();
    });
    const auto cross_thread_result =
        co_await ruvia::sleep_for(worker_value, std::chrono::seconds(30), cross_thread.token());
    stopper.join();
    if (cross_thread_result != ruvia::timer_sleep_result::stop_requested) {
        co_return;
    }

    // A stop that arrives long after the sleep already finished must be a
    // no-op, not a use-after-free of the awaiter's timer registration.
    ruvia::stop_source late;
    if (co_await ruvia::sleep_for(worker_value, std::chrono::milliseconds(1), late.token()) !=
        ruvia::timer_sleep_result::elapsed) {
        co_return;
    }
    late.request_stop();
    static_cast<void>(co_await ruvia::sleep_for(worker_value, std::chrono::milliseconds(5)));

    success = true;
}

bool stoppable_sleep_works() {
    // Keep token cancellation independent of the shutdown cases above.
    asio::io_context io_context;
    auto attachment = ruvia::attach_event_loop(io_context, {.queue_capacity_ = 8});
    const auto worker_value = attachment.loop().worker();
    bool success = false;
    asio::co_spawn(io_context,
        ruvia::as_awaitable(exercise_stoppable_sleep(worker_value, success)),
        [&attachment](std::exception_ptr failure) {
            attachment.stop();
            if (failure) {
                std::rethrow_exception(failure);
            }
        });
    attachment.run();
    attachment.stop();
    return success;
}

ruvia::task<void> receive_through_timer_stop(
    ruvia::channel_receiver<int>& receiver, ruvia::worker_wait_status& status) {
    const auto result_value = co_await receiver.receive({.timeout_ = std::chrono::hours(1)});
    status = result_value.status();
}

ruvia::task<void> acquire_after_timer_stop(
    ruvia::pool_lease_scheduler& pool, std::optional<ruvia::worker_wait_result<std::size_t>>& result) {
    result.emplace(co_await pool.acquire({.timeout_ = std::chrono::milliseconds(1)}));
}

// Once the worker stops, every timed primitive reports its shutdown result
// instead of throwing, whether it was pending or starts later.
ruvia::task<void> exercise_after_timer_stop(ruvia::event_loop_attachment& attachment, bool& success) {
    success = false;
    const auto loop = attachment.loop();
    const auto& worker_value = loop.worker();
    auto [sender, receiver] = ruvia::make_channel<int>(worker_value, {.capacity_ = 1});
    auto [completion, one_shot_receiver] = ruvia::make_one_shot<int>(worker_value);

    auto pending_status = ruvia::worker_wait_status::value;
    {
        ruvia::task_scope scope(worker_value);
        scope.spawn(receive_through_timer_stop(receiver, pending_status));
        // Let the receive park on its deadline before the worker stops.
        static_cast<void>(co_await ruvia::sleep_for(worker_value, std::chrono::milliseconds(1)));
        attachment.stop();
        co_await scope.join();
    }
    if (pending_status != ruvia::worker_wait_status::worker_stopping) {
        co_return;
    }

    if (co_await ruvia::sleep_for(worker_value, std::chrono::hours(1)) !=
        ruvia::timer_sleep_result::stop_requested) {
        co_return;
    }
    ruvia::stop_source idle;
    if (co_await ruvia::sleep_for(worker_value, std::chrono::hours(1), idle.token()) !=
        ruvia::timer_sleep_result::stop_requested) {
        co_return;
    }
    if ((co_await receiver.receive({.timeout_ = std::chrono::hours(1)})).status() !=
        ruvia::worker_wait_status::worker_stopping) {
        co_return;
    }
    if ((co_await one_shot_receiver.wait({.timeout_ = std::chrono::hours(1)})).status() !=
        ruvia::worker_wait_status::worker_stopping) {
        co_return;
    }

    ruvia::worker_timer_registration registration;
    bool invoked = false;
    const auto schedule_status = worker_value.schedule_timer(registration,
        std::chrono::steady_clock::now() + std::chrono::hours(1),
        [&invoked](ruvia::worker_timer_outcome) { invoked = true; });
    if (schedule_status != ruvia::worker_timer_schedule_status::worker_stopping || registration.registered() ||
        invoked) {
        co_return;
    }

    // A free slot is still leased; a saturated timed acquire reports the
    // shutdown like every other timed primitive.
    ruvia::pool_lease_scheduler pool(1, worker_value);
    const auto held = co_await pool.acquire();
    if (!held.has_value()) {
        co_return;
    }
    std::optional<ruvia::worker_wait_result<std::size_t>> waiting_result;
    {
        ruvia::task_scope scope(worker_value);
        scope.spawn(acquire_after_timer_stop(pool, waiting_result));
        co_await scope.join();
    }
    pool.release(held.value());
    success = waiting_result.has_value() && waiting_result->status() == ruvia::worker_wait_status::worker_stopping;
}

bool timed_primitives_report_timer_stop() {
    asio::io_context io_context;
    auto attachment = ruvia::attach_event_loop(io_context, {.queue_capacity_ = 16});
    bool success = false;
    ruvia::test::run_worker_tasks(attachment, exercise_after_timer_stop(attachment, success));
    return success;
}

// An expired registration is released by its worker and may be rescheduled on
// another; a still-pending one is rejected until it is cancelled.
bool expired_registration_reschedules_on_another_worker() {
    asio::io_context first_context;
    asio::io_context second_context;
    auto first = ruvia::attach_event_loop(first_context, {.queue_capacity_ = 8});
    auto second = ruvia::attach_event_loop(second_context, {.queue_capacity_ = 8});
    const auto first_worker = first.loop().worker();
    const auto second_worker = second.loop().worker();
    ruvia::worker_timer_registration expiring;
    ruvia::worker_timer_registration pending;
    bool first_expired = false;
    bool second_expired = false;
    bool pending_rejected = false;
    bool cancelled_rescheduled = false;
    const auto drive = [](asio::io_context& context) {
        context.restart();
        context.run_for(std::chrono::milliseconds(20));
    };
    const auto retire = [](ruvia::event_loop_attachment& attachment, asio::io_context& context) {
        attachment.stop();
        context.restart();
        attachment.run();
    };

    const auto first_post = first_worker.post([&] {
        static_cast<void>(first_worker.schedule_timer(expiring, std::chrono::steady_clock::now(),
            [&first_expired](ruvia::worker_timer_outcome outcome) {
                first_expired = outcome == ruvia::worker_timer_outcome::expired;
            }));
        static_cast<void>(first_worker.schedule_timer(pending,
            std::chrono::steady_clock::now() + std::chrono::hours(1), [](ruvia::worker_timer_outcome) {}));
    });
    if (!first_post.accepted()) {
        return false;
    }
    drive(first_context);

    const auto second_post = second_worker.post([&] {
        try {
            static_cast<void>(second_worker.schedule_timer(pending,
                std::chrono::steady_clock::now(), [](ruvia::worker_timer_outcome) {}));
        } catch (const std::logic_error&) {
            pending_rejected = true;
        }
        static_cast<void>(second_worker.schedule_timer(expiring, std::chrono::steady_clock::now(),
            [&second_expired](ruvia::worker_timer_outcome outcome) {
                second_expired = outcome == ruvia::worker_timer_outcome::expired;
            }));
    });
    if (!second_post.accepted()) {
        return false;
    }
    drive(second_context);

    // Cancellation from this thread is applied on the first worker.
    pending.cancel();
    drive(first_context);
    const auto rescheduled_post = second_worker.post([&] {
        cancelled_rescheduled = second_worker.schedule_timer(pending,
                                    std::chrono::steady_clock::now() + std::chrono::hours(1),
                                    [](ruvia::worker_timer_outcome) {}) ==
                                ruvia::worker_timer_schedule_status::scheduled;
        pending.cancel();
    });
    if (!rescheduled_post.accepted()) {
        return false;
    }
    drive(second_context);
    retire(first, first_context);
    retire(second, second_context);
    return first_expired && second_expired && pending_rejected && cancelled_rescheduled;
}

bool timer_callback_failure_preserves_remaining_operations() {
    asio::io_context io_context;
    auto attachment = ruvia::attach_event_loop(io_context, {.queue_capacity_ = 8});
    const auto worker_value = attachment.loop().worker();
    ruvia::worker_timer_registration failing_timer;
    ruvia::worker_timer_registration remaining_timer;
    ruvia::worker_timer_registration cancelled_timer;
    bool remaining_expired = false;
    bool rescheduled_expired = false;
    bool cancellation_observed = false;
    std::weak_ptr<int> failed_capture;
    std::weak_ptr<int> completed_capture;
    std::weak_ptr<int> cancelled_capture;

    asio::post(io_context, [&] {
        const auto deadline = std::chrono::steady_clock::now();
        auto owned = std::make_shared<int>(1);
        failed_capture = owned;
        static_cast<void>(worker_value.schedule_timer(failing_timer, deadline,
            [owned = std::move(owned)](ruvia::worker_timer_outcome) {
                throw std::runtime_error("timer completion failure");
            }));
        owned = std::make_shared<int>(2);
        completed_capture = owned;
        static_cast<void>(worker_value.schedule_timer(remaining_timer, deadline,
            [owned = std::move(owned), &remaining_expired](ruvia::worker_timer_outcome outcome) {
                remaining_expired = outcome == ruvia::worker_timer_outcome::expired;
            }));
        owned = std::make_shared<int>(3);
        cancelled_capture = owned;
        static_cast<void>(worker_value.schedule_timer(cancelled_timer, deadline + std::chrono::hours(1),
            [owned = std::move(owned), &cancellation_observed](ruvia::worker_timer_outcome outcome) {
                cancellation_observed = outcome == ruvia::worker_timer_outcome::cancelled;
            }));
    });

    bool failure_observed = false;
    try {
        io_context.run_for(std::chrono::milliseconds(20));
    } catch (const std::runtime_error& error) {
        failure_observed = std::string_view(error.what()) == "timer completion failure";
    }
    asio::post(io_context, [&] {
        cancelled_timer.cancel_and_notify();
        static_cast<void>(worker_value.schedule_timer(failing_timer, std::chrono::steady_clock::now(),
            [&rescheduled_expired](ruvia::worker_timer_outcome outcome) {
                rescheduled_expired = outcome == ruvia::worker_timer_outcome::expired;
            }));
    });
    io_context.run_for(std::chrono::milliseconds(20));
    const bool success = failure_observed && remaining_expired && rescheduled_expired &&
                         cancellation_observed && failed_capture.expired() && completed_capture.expired() &&
                         cancelled_capture.expired();
    attachment.stop();
    io_context.restart();
    attachment.run();
    return success;
}

// A completion that re-arms an already-due deadline must not keep one timer
// pass running: other handlers, such as I/O and stop controls, run between
// rounds.
bool self_rescheduling_due_timer_yields_to_other_handlers() {
    constexpr int round_limit = 256;
    asio::io_context io_context;
    auto attachment = ruvia::attach_event_loop(io_context, {.queue_capacity_ = 8});
    const auto worker_value = attachment.loop().worker();
    ruvia::worker_timer_registration timer;
    int rounds = 0;
    int rounds_before_handler = -1;
    std::function<void(ruvia::worker_timer_outcome)> tick;
    tick = [&](ruvia::worker_timer_outcome outcome) {
        if (outcome != ruvia::worker_timer_outcome::expired) {
            return;
        }
        if (++rounds == 1) {
            asio::post(io_context, [&] {
                rounds_before_handler = rounds;
                io_context.stop();
            });
        }
        if (rounds < round_limit && rounds_before_handler < 0) {
            static_cast<void>(worker_value.schedule_timer(timer, std::chrono::steady_clock::now(),
                [&tick](ruvia::worker_timer_outcome next) { tick(next); }));
        }
    };
    asio::post(io_context, [&] {
        static_cast<void>(worker_value.schedule_timer(timer, std::chrono::steady_clock::now(),
            [&tick](ruvia::worker_timer_outcome next) { tick(next); }));
    });
    io_context.run();
    attachment.stop();
    io_context.restart();
    attachment.run();
    return rounds_before_handler >= 1 && rounds_before_handler < round_limit;
}

bool self_rescheduling_due_timer_preserves_other_expirations() {
    asio::io_context io_context;
    auto attachment = ruvia::attach_event_loop(io_context, {.queue_capacity_ = 8});
    const auto worker = attachment.loop().worker();
    ruvia::worker_timer_registration repeating;
    ruvia::worker_timer_registration other;
    const auto earlier = std::chrono::steady_clock::now() - std::chrono::seconds(2);
    std::string order;
    int rounds = 0;
    std::function<void(ruvia::worker_timer_outcome)> tick;
    tick = [&](ruvia::worker_timer_outcome outcome) {
        if (outcome != ruvia::worker_timer_outcome::expired) {
            return;
        }
        order += 'A';
        if (++rounds < 3) {
            static_cast<void>(worker.schedule_timer(repeating, earlier,
                [&](ruvia::worker_timer_outcome next) { tick(next); }));
        } else {
            // The last expected expiration retires the loop so run() returns.
            attachment.stop();
        }
    };
    asio::post(io_context, [&] {
        static_cast<void>(worker.schedule_timer(repeating, earlier,
            [&](ruvia::worker_timer_outcome next) { tick(next); }));
        static_cast<void>(worker.schedule_timer(other, earlier + std::chrono::seconds(1),
            [&](ruvia::worker_timer_outcome outcome) {
                if (outcome == ruvia::worker_timer_outcome::expired) {
                    order += 'B';
                }
            }));
    });
    io_context.run();
    return order == "ABAA";
}

struct stop_during_move_state final {
    ruvia::event_loop_attachment* attachment_;
    int trigger_;
    int moves_{};
    int completions_{};
    bool stopped_{};
    bool cancelled_{};
};

struct stop_during_move_callback final {
    stop_during_move_state* state_;
    explicit stop_during_move_callback(stop_during_move_state& value) noexcept
        : state_(&value) {}
    stop_during_move_callback(const stop_during_move_callback&) = delete;
    stop_during_move_callback(stop_during_move_callback&& other) noexcept
        : state_(other.state_) {
        if (++state_->moves_ == state_->trigger_) {
            state_->stopped_ = true;
            state_->attachment_->stop();
        }
    }
    void operator()(ruvia::worker_timer_outcome outcome) {
        ++state_->completions_;
        state_->cancelled_ = outcome == ruvia::worker_timer_outcome::cancelled;
    }
};

// stop() cancels the worker's timers on its thread after it returns, so the
// registration is inspected once a sleep proves the timer queue has stopped.
ruvia::task<void> schedule_with_stop_during_callback_move(
    ruvia::event_loop_attachment& attachment, stop_during_move_state& current, bool& success) {
    const auto loop = attachment.loop();
    const auto& worker = loop.worker();
    ruvia::worker_timer_registration registration;
    ruvia::move_only_function<void(ruvia::worker_timer_outcome)> completion{stop_during_move_callback(current)};
    current.moves_ = 0;
    const auto result = worker.schedule_timer(
        registration, std::chrono::steady_clock::now() + std::chrono::hours(1), std::move(completion));
    if (!current.stopped_) {
        attachment.stop();
    }
    static_cast<void>(co_await ruvia::sleep_for(worker, std::chrono::hours(1)));
    bool reusable = false;
    try {
        reusable = worker.schedule_timer(registration, std::chrono::steady_clock::now(),
                       [](ruvia::worker_timer_outcome) {}) ==
                   ruvia::worker_timer_schedule_status::worker_stopping;
    } catch (const std::logic_error&) {
    }
    success = reusable &&
              ((result == ruvia::worker_timer_schedule_status::worker_stopping && current.completions_ == 0) ||
                  (result == ruvia::worker_timer_schedule_status::scheduled &&
                      current.completions_ == 1 && current.cancelled_));
    registration.cancel();
}

bool timer_registration_survives_stop_during_callback_move() {
    for (int trigger = 1; trigger <= 12; ++trigger) {
        asio::io_context io_context;
        auto attachment = ruvia::attach_event_loop(io_context, {.queue_capacity_ = 8});
        stop_during_move_state current{&attachment, trigger};
        bool success = false;
        ruvia::test::run_worker_tasks(attachment, schedule_with_stop_during_callback_move(attachment, current, success));
        if (!success) {
            return false;
        }
    }
    return true;
}

bool timer_registration_rejects_reentrant_acquire() {
    struct state final {
        const ruvia::worker_ref* worker_;
        ruvia::worker_timer_registration* registration_;
        int trigger_;
        int moves_{};
        bool attempted_{};
        bool inner_accepted_{};
        bool inner_rejected_{};
    };
    struct callback final {
        state* state_;
        explicit callback(state& value) noexcept
            : state_(&value) {}
        callback(const callback&) = delete;
        callback(callback&& other) noexcept
            : state_(other.state_) {
            if (++state_->moves_ != state_->trigger_) {
                return;
            }
            state_->attempted_ = true;
            try {
                state_->inner_accepted_ = state_->worker_->schedule_timer(*state_->registration_,
                                              std::chrono::steady_clock::now() + std::chrono::hours(1),
                                              [](ruvia::worker_timer_outcome) {}) == ruvia::worker_timer_schedule_status::scheduled;
            } catch (const std::logic_error&) {
                state_->inner_rejected_ = true;
            }
        }
        void operator()(ruvia::worker_timer_outcome) {}
    };
    for (int trigger = 1; trigger <= 12; ++trigger) {
        asio::io_context io_context;
        auto attachment = ruvia::attach_event_loop(io_context, {.queue_capacity_ = 8});
        const auto loop = attachment.loop();
        const auto& worker = loop.worker();
        ruvia::worker_timer_registration registration;
        state current{&worker, &registration, trigger};
        bool success = false;
        asio::post(io_context, [&] {
            ruvia::move_only_function<void(ruvia::worker_timer_outcome)> completion{callback(current)};
            current.moves_ = 0;
            bool outer_accepted = false;
            bool outer_rejected = false;
            try {
                outer_accepted = worker.schedule_timer(registration,
                                     std::chrono::steady_clock::now() + std::chrono::hours(1), std::move(completion)) ==
                                 ruvia::worker_timer_schedule_status::scheduled;
            } catch (const std::logic_error&) {
                outer_rejected = true;
            }
            success = current.attempted_
                          ? (current.inner_accepted_ && outer_rejected) ||
                                (outer_accepted && current.inner_rejected_)
                          : outer_accepted;
            registration.cancel();
            attachment.stop();
        });
        io_context.run();
        if (!success) {
            return false;
        }
    }
    return true;
}

TEST(Ruvia, TimerDurationConversionsPreserveIntegerTicksAndSaturate) {
    EXPECT_TRUE(timer_duration_conversions_preserve_integer_ticks_and_saturate());
}

TEST(Ruvia, timer_fractional_duration_conversion_preserves_rounding_and_bounds) {
    using target_type = std::chrono::steady_clock::duration;
    using period_type = std::ratio_multiply<target_type::period, std::ratio<3, 5>>;
    using signed_type = std::chrono::duration<std::int64_t, period_type>;
    using unsigned_type = std::chrono::duration<std::uint64_t, period_type>;
    for (const auto& [input, expected] : {std::pair<std::int64_t, std::int64_t>{0, 0},
             {1, 1}, {4, 3}, {5, 3}, {-1, 0}, {-4, -2}, {-5, -3},
             {9'000'000'000'000'000'003LL, 5'400'000'000'000'000'002LL},
             {-9'000'000'000'000'000'003LL, -5'400'000'000'000'000'001LL}}) {
        SCOPED_TRACE(input);
        EXPECT_EQ(ruvia::saturating_duration_cast(signed_type(input)).count(), expected);
    }
    EXPECT_EQ(ruvia::saturating_duration_cast(unsigned_type(10'000'000'000'000'000'002ULL)).count(), 6'000'000'000'000'000'002LL);
    EXPECT_EQ(ruvia::saturating_duration_cast(unsigned_type::max()), target_type::max());
    EXPECT_EQ(ruvia::saturating_duration_cast(signed_type::min()).count(), -5'534'023'222'112'865'484LL);
}

TEST(Ruvia, timer_fractional_duration_conversion_avoids_intermediate_overflow) {
    using target_type = std::chrono::steady_clock::duration;
    using period_type = std::ratio_multiply<target_type::period, std::ratio<9'000'000'001LL, 9'000'000'000LL>>;
    using signed_type = std::chrono::duration<std::int64_t, period_type>;
    using unsigned_type = std::chrono::duration<std::uint64_t, period_type>;
    EXPECT_EQ(ruvia::saturating_duration_cast(signed_type(8'999'999'999LL)).count(), 9'000'000'000LL);
    EXPECT_EQ(ruvia::saturating_duration_cast(signed_type(-8'999'999'999LL)).count(), -8'999'999'999LL);
    EXPECT_EQ(ruvia::saturating_duration_cast(signed_type::max()), target_type::max());
    EXPECT_EQ(ruvia::saturating_duration_cast(signed_type::min()), target_type::min());
    EXPECT_EQ(ruvia::saturating_duration_cast(unsigned_type::max()), target_type::max());
}

TEST(Ruvia, timer_floating_duration_underflow_keeps_positive_minimum_tick) {
    using input_type = std::chrono::duration<long double, std::micro>;
    using target_type = std::chrono::steady_clock::duration;
    using fine_type = std::chrono::duration<long double, std::ratio_divide<target_type::period, std::kilo>>;
    const auto tiny = (std::numeric_limits<long double>::denorm_min)();
    ASSERT_GT(tiny, 0.0L);
    EXPECT_EQ(ruvia::ceil_milliseconds(input_type(tiny)), std::chrono::milliseconds(1));
    EXPECT_EQ(ruvia::saturating_duration_cast<std::chrono::milliseconds>(input_type(tiny)), std::chrono::milliseconds(1));
    EXPECT_EQ(ruvia::saturating_duration_cast(fine_type(tiny)), target_type(1));
    EXPECT_EQ(ruvia::saturating_duration_cast<std::chrono::milliseconds>(input_type(-tiny)), std::chrono::milliseconds::zero());
    EXPECT_EQ(ruvia::ceil_milliseconds(input_type(-tiny)), std::chrono::milliseconds::zero());
    EXPECT_EQ(ruvia::saturating_duration_cast<std::chrono::milliseconds>(input_type::zero()), std::chrono::milliseconds::zero());
}

TEST(Ruvia, TimerImmediateShutdownWorks) {
    EXPECT_TRUE(timer_immediate_shutdown_works());
}
TEST(Ruvia, TimerOffWorkerCancellationCanRaceWithShutdown) {
    EXPECT_TRUE(off_worker_cancellation_can_race_with_timer_shutdown());
}
TEST(Ruvia, TimerOffWorkerCancellationAfterContextStopDoesNotExpireLater) {
    EXPECT_TRUE(off_worker_cancellation_after_context_stop_does_not_expire_later());
}
TEST(Ruvia, TimerOffWorkerCancellationBeforeQueuedStopDoesNotInvokeCallback) {
    EXPECT_TRUE(off_worker_cancellation_before_queued_stop_timers_does_not_invoke_callback());
}
TEST(Ruvia, TimerExplicitCancellationNotifiesAfterQuietDestruction) {
    EXPECT_TRUE(explicit_timer_cancellation_notifies_after_quiet_destruction());
}
TEST(Ruvia, TimerOnWorkerTokenCancellationThenOwnerCancelWithdrawsCallback) {
    EXPECT_TRUE(on_worker_token_cancellation_followed_by_owner_cancel_withdraws_callback());
}
TEST(Ruvia, TimerStoppableSleepWorks) {
    EXPECT_TRUE(stoppable_sleep_works());
}
TEST(Ruvia, TimerTimedPrimitivesReportStop) {
    EXPECT_TRUE(timed_primitives_report_timer_stop());
}
TEST(Ruvia, TimerExpiredRegistrationReschedulesOnAnotherWorker) {
    EXPECT_TRUE(expired_registration_reschedules_on_another_worker());
}
TEST(Ruvia, TimerCallbackFailurePreservesRemainingOperations) {
    EXPECT_TRUE(timer_callback_failure_preserves_remaining_operations());
}
TEST(Ruvia, TimerRegistrationSurvivesStopDuringCallbackMove) {
    EXPECT_TRUE(timer_registration_survives_stop_during_callback_move());
}
TEST(Ruvia, TimerRegistrationRejectsReentrantAcquire) {
    EXPECT_TRUE(timer_registration_rejects_reentrant_acquire());
}
TEST(Ruvia, TimerSelfReschedulingYieldsToOtherHandlers) {
    EXPECT_TRUE(self_rescheduling_due_timer_yields_to_other_handlers());
}
TEST(Ruvia, TimerSelfReschedulingPreservesOtherExpirations) {
    EXPECT_TRUE(self_rescheduling_due_timer_preserves_other_expirations());
}

TEST(Ruvia, WorkerTimerLifecycle) {
    asio::io_context io_context;
    auto attachment = ruvia::attach_event_loop(io_context, {.queue_capacity_ = 8});
    const auto worker_value = attachment.loop().worker();
    bool success = false;
    asio::co_spawn(io_context, ruvia::as_awaitable(exercise(attachment, worker_value, success)),
        asio::detached);
    attachment.run();
    attachment.stop();
    EXPECT_TRUE(success);
}

namespace {

template <typename left_type, typename right_type>
concept timeout_orderable = requires(left_type left, right_type right) { left < right; };

template <typename left_type, typename right_type>
concept timeout_equatable = requires(left_type left, right_type right) { left == right; };

static_assert(std::totally_ordered<ruvia::timeout_duration>);
static_assert(!timeout_orderable<ruvia::timeout_duration, std::chrono::nanoseconds>);
static_assert(!timeout_orderable<std::chrono::nanoseconds, ruvia::timeout_duration>);
static_assert(!timeout_equatable<ruvia::timeout_duration, std::chrono::nanoseconds>);
static_assert(!timeout_equatable<std::chrono::nanoseconds, ruvia::timeout_duration>);

enum class deadline_kind : std::uint8_t { read,
    write };

bool deadline_lifecycle_transitions_are_exclusive() {
    using deadline_type = ruvia::deadline_lifecycle<deadline_kind>;
    deadline_type deadline;
    const auto now = deadline_type::clock_type::time_point{};
    if (deadline.kind() != nullptr || deadline.expired() || deadline.clear()) {
        return false;
    }

    deadline.arm(now + std::chrono::seconds(1), deadline_kind::read);
    if (deadline.kind() == nullptr || *deadline.kind() != deadline_kind::read ||
        deadline.expire(now).has_value() || deadline.expired()) {
        return false;
    }

    const auto expired_kind = deadline.expire(now + std::chrono::seconds(1));
    if (expired_kind != deadline_kind::read || !deadline.expired() || deadline.kind() == nullptr ||
        *deadline.kind() != deadline_kind::read || !deadline.clear()) {
        return false;
    }

    deadline.arm(now, deadline_kind::write);
    deadline.reset();
    if (deadline.kind() != nullptr || deadline.expired() || deadline.clear()) {
        return false;
    }

    ruvia::deadline_lifecycle<> unlabelled;
    if (unlabelled.kind() != nullptr || unlabelled.expired()) {
        return false;
    }
    unlabelled.arm(ruvia::deadline_lifecycle<>::clock_type::now());
    return unlabelled.kind() != nullptr &&
           unlabelled.expire(ruvia::deadline_lifecycle<>::clock_type::now()).has_value() &&
           unlabelled.expired() && unlabelled.clear();
}

bool operation_deadline_uses_one_absolute_deadline() {
    using deadline_type = ruvia::operation_deadline;
    const deadline_type unlimited;
    if (unlimited.deadline().has_value() || unlimited.remaining().has_value() || unlimited.expired() ||
        unlimited.timeout().has_value()) {
        return false;
    }

    const deadline_type expired(deadline_type::clock_type::time_point{});
    if (!expired.deadline().has_value() || !expired.expired() ||
        expired.remaining() != std::chrono::milliseconds::zero()) {
        return false;
    }

    const deadline_type active(std::optional<ruvia::operation_timeout>(std::chrono::seconds(1)));
    const auto deadline_value = active.deadline();
    const auto remaining = active.remaining();
    if (!deadline_value.has_value() || !remaining.has_value() || remaining->count() <= 0 ||
        *remaining > std::chrono::seconds(1)) {
        return false;
    }
    // Nested operations receive the same absolute deadline, not a fresh relative bound.
    const auto forwarded = active.timeout();
    if (!forwarded.has_value() || forwarded->deadline() != deadline_value || forwarded->duration().has_value()) {
        return false;
    }
    return active.constrained_by(std::chrono::seconds(2)).deadline() == deadline_value &&
           unlimited.constrained_by(std::chrono::seconds(2)).deadline().has_value() &&
           !unlimited.constrained_by(std::nullopt).deadline().has_value();
}

}  // namespace

TEST(Ruvia, DeadlineLifecycleTransitionsAreExclusive) {
    EXPECT_TRUE(deadline_lifecycle_transitions_are_exclusive());
}

TEST(Ruvia, OperationDeadlineUsesOneAbsoluteDeadline) {
    EXPECT_TRUE(operation_deadline_uses_one_absolute_deadline());
}

TEST(Ruvia, operation_timeout_is_valid_by_construction) {
    using clock_type = ruvia::operation_timeout::clock_type;
    static_assert(std::is_convertible_v<std::chrono::seconds, ruvia::operation_timeout>);
    static_assert(std::is_convertible_v<ruvia::timeout_duration, ruvia::operation_timeout>);
    static_assert(!std::is_convertible_v<clock_type::time_point, ruvia::operation_timeout>);
    static_assert(!std::is_constructible_v<ruvia::operation_timeout, std::chrono::duration<double>>);

    const ruvia::operation_timeout relative = std::chrono::nanoseconds(1);
    EXPECT_EQ(relative.duration(), std::chrono::milliseconds(1));
    EXPECT_FALSE(relative.deadline().has_value());
    EXPECT_EQ(ruvia::operation_timeout(ruvia::timeout_duration(std::chrono::seconds(2))).duration(),
        std::chrono::milliseconds(2000));
    EXPECT_THROW(ruvia::operation_timeout(std::chrono::milliseconds(0)), std::invalid_argument);
    EXPECT_THROW(ruvia::operation_timeout(std::chrono::nanoseconds(-1)), std::invalid_argument);
    EXPECT_THROW(ruvia::operation_timeout(ruvia::timeout_duration{}), std::invalid_argument);
    EXPECT_THROW(static_cast<void>(ruvia::operation_options{.timeout_ = std::chrono::seconds(-1)}), std::invalid_argument);

    // An absolute deadline is accepted as given, including one that has passed.
    const ruvia::operation_timeout passed(clock_type::time_point{});
    EXPECT_EQ(passed.deadline(), clock_type::time_point{});
    EXPECT_FALSE(passed.duration().has_value());
    const auto start = clock_type::now();
    EXPECT_EQ(passed.deadline_from(start), clock_type::time_point{});
    EXPECT_EQ(relative.deadline_from(start), start + std::chrono::milliseconds(1));
    EXPECT_EQ(ruvia::operation_timeout(std::chrono::hours::max()).deadline_from(start), clock_type::time_point::max());

    EXPECT_FALSE(ruvia::operation_timeout::try_create(std::chrono::milliseconds(0)).has_value());
    EXPECT_FALSE(ruvia::operation_timeout::try_create(std::chrono::seconds(-3)).has_value());
    EXPECT_FALSE(ruvia::operation_timeout::try_create(ruvia::timeout_duration{}).has_value());
    EXPECT_EQ(ruvia::operation_timeout::try_create(std::chrono::microseconds(1500)),
        ruvia::operation_timeout(std::chrono::milliseconds(2)));
    EXPECT_EQ(ruvia::operation_timeout::try_create(ruvia::timeout_duration(std::chrono::seconds(1))),
        ruvia::operation_timeout(std::chrono::seconds(1)));
}

TEST(Ruvia, operation_timeout_overrides_only_tighten) {
    using clock_type = ruvia::operation_timeout::clock_type;
    // Two relative limits keep the shorter.
    auto merged = ruvia::override_operation_options({.timeout_ = std::chrono::seconds(5)}, {.timeout_ = std::chrono::seconds(1)});
    EXPECT_EQ(merged.timeout_, ruvia::operation_timeout(std::chrono::seconds(1)));
    merged = ruvia::override_operation_options({.timeout_ = std::chrono::seconds(1)}, {.timeout_ = std::chrono::seconds(5)});
    EXPECT_EQ(merged.timeout_, ruvia::operation_timeout(std::chrono::seconds(1)));
    // Two deadlines keep the earlier.
    const auto early = clock_type::now() + std::chrono::seconds(1);
    const auto late = early + std::chrono::seconds(10);
    merged = ruvia::override_operation_options(
        {.timeout_ = ruvia::operation_timeout(late)}, {.timeout_ = ruvia::operation_timeout(early)});
    EXPECT_EQ(merged.timeout_->deadline(), early);
    merged = ruvia::override_operation_options(
        {.timeout_ = ruvia::operation_timeout(early)}, {.timeout_ = ruvia::operation_timeout(late)});
    EXPECT_EQ(merged.timeout_->deadline(), early);
    // A mixed pair resolves the relative limit at merge time and keeps the earlier deadline.
    merged = ruvia::override_operation_options({.timeout_ = std::chrono::hours(1)}, {.timeout_ = ruvia::operation_timeout(early)});
    EXPECT_EQ(merged.timeout_->deadline(), early);
    const auto before = clock_type::now();
    merged = ruvia::override_operation_options({.timeout_ = ruvia::operation_timeout(late)}, {.timeout_ = std::chrono::milliseconds(5)});
    ASSERT_TRUE(merged.timeout_.has_value());
    ASSERT_TRUE(merged.timeout_->deadline().has_value());
    EXPECT_GE(*merged.timeout_->deadline(), before + std::chrono::milliseconds(5));
    EXPECT_LT(*merged.timeout_->deadline(), late);
    // An unspecified override inherits; a supplied token replaces.
    ruvia::stop_source source;
    merged = ruvia::override_operation_options({.timeout_ = std::chrono::seconds(1)}, {.stop_token_ = source.token()});
    EXPECT_EQ(merged.timeout_, ruvia::operation_timeout(std::chrono::seconds(1)));
    EXPECT_TRUE(merged.stop_token_.has_state());
    merged = ruvia::override_operation_options({}, {.timeout_ = std::chrono::seconds(1)});
    EXPECT_EQ(merged.timeout_, ruvia::operation_timeout(std::chrono::seconds(1)));
}

TEST(Ruvia, timeout_duration_checks_the_original_integer_domain) {
    using wide_milliseconds = std::chrono::duration<std::uint64_t, std::milli>;
    constexpr wide_milliseconds wide(std::numeric_limits<std::uint64_t>::max());
    constexpr ruvia::timeout_duration saturated(wide);
    static_assert(saturated.value() == std::chrono::milliseconds::max());
    static_assert(!std::is_convertible_v<ruvia::timeout_duration, std::chrono::milliseconds>);
    static_assert(!std::is_constructible_v<ruvia::timeout_duration, std::chrono::duration<double>>);

    ruvia::operation_options options{.timeout_ = wide};
    ASSERT_TRUE(options.timeout_.has_value());
    EXPECT_EQ(options.timeout_->duration(), std::chrono::milliseconds::max());

    options.timeout_ = std::chrono::nanoseconds(1);
    EXPECT_EQ(options.timeout_->duration(), std::chrono::milliseconds(1));

    using thirds = std::chrono::duration<std::int64_t, std::ratio<1, 3>>;
    EXPECT_EQ(ruvia::timeout_duration(thirds(7)).value(), std::chrono::milliseconds(2334));
    EXPECT_EQ(ruvia::timeout_duration(thirds(-7)).value(), std::chrono::milliseconds(-2333));
    using wide_hours = std::chrono::duration<std::int64_t, std::ratio<3600>>;
    EXPECT_EQ(ruvia::timeout_duration(wide_hours::min()).value(),
        std::chrono::milliseconds::min());
}

TEST(Ruvia, wide_positive_timeouts_remain_active_and_can_be_tightened) {
    using wide_milliseconds = std::chrono::duration<std::uint64_t, std::milli>;
    const wide_milliseconds wide(std::numeric_limits<std::uint64_t>::max());
    const ruvia::operation_deadline timeout{std::optional<ruvia::operation_timeout>(wide)};
    EXPECT_FALSE(timeout.expired());
    ASSERT_TRUE(timeout.remaining().has_value());
    EXPECT_GT(*timeout.remaining(), std::chrono::milliseconds::zero());

    const ruvia::operation_deadline unlimited;
    EXPECT_FALSE(unlimited.constrained_by(wide).expired());
    ruvia::deadline_lifecycle<> deadline;
    deadline.arm(*timeout.deadline());
    EXPECT_FALSE(deadline.expire(std::chrono::steady_clock::now()).has_value());
    EXPECT_TRUE(deadline.expire(std::chrono::steady_clock::time_point::max()).has_value());

    const auto tightened = ruvia::override_operation_options(
        {.timeout_ = wide}, {.timeout_ = std::chrono::nanoseconds(1)});
    ASSERT_TRUE(tightened.timeout_.has_value());
    EXPECT_EQ(tightened.timeout_->duration(), std::chrono::milliseconds(1));
    const auto retained = ruvia::override_operation_options(tightened, {.timeout_ = wide});
    EXPECT_EQ(retained.timeout_, tightened.timeout_);
}

TEST(Ruvia, timer_conversions_do_not_narrow_before_saturation) {
    using clock_type = std::chrono::steady_clock;
    using wide_ticks = std::chrono::duration<std::uint64_t, clock_type::duration::period>;
    const wide_ticks ticks(std::numeric_limits<std::uint64_t>::max());
    EXPECT_EQ(ruvia::saturating_deadline(clock_type::time_point{}, ticks),
        clock_type::time_point::max());

    using wide_milliseconds = std::chrono::duration<std::uint64_t, std::milli>;
    const wide_milliseconds milliseconds(std::numeric_limits<std::uint64_t>::max());
    EXPECT_EQ(ruvia::ceil_milliseconds(milliseconds),
        std::chrono::milliseconds::max());
    EXPECT_EQ(ruvia::ceil_milliseconds(std::chrono::nanoseconds(1)),
        std::chrono::milliseconds(1));
}

namespace ruvia::test::task_completion_tests {

struct allocation_counts final {
    std::size_t allocations_{0};
    std::size_t deallocations_{0};
};

template <typename t_type>
class counting_allocator {
public:
    using value_type = t_type;

    explicit counting_allocator(allocation_counts& counts) noexcept
        : counts_(&counts) {}

    template <typename u_type>
    counting_allocator(const counting_allocator<u_type>& other) noexcept
        : counts_(other.counts()) {}

    [[nodiscard]] t_type* allocate(std::size_t count) {
        ++counts_->allocations_;
        return std::allocator<t_type>{}.allocate(count);
    }

    void deallocate(t_type* pointer, std::size_t count) noexcept {
        ++counts_->deallocations_;
        std::allocator<t_type>{}.deallocate(pointer, count);
    }

    [[nodiscard]] allocation_counts* counts() const noexcept {
        return counts_;
    }

    template <typename u_type>
    [[nodiscard]] bool operator==(const counting_allocator<u_type>& other) const noexcept {
        return counts_ == other.counts();
    }

private:
    allocation_counts* counts_;
};

struct frame_allocation final {
    explicit frame_allocation(allocation_counts& counts)
        : values_(256, 0, counting_allocator<int>(counts)) {}

    std::vector<int, counting_allocator<int>> values_;
};

template <typename result_type>
ruvia::task<result_type> cold_frame(frame_allocation frame, bool& ran) {
    ran = true;
    if constexpr (std::is_void_v<result_type>) {
        co_return;
    } else {
        co_return static_cast<int>(frame.values_.size());
    }
}

template <typename result_type>
bool check_cold_frame(bool cross_thread) {
    allocation_counts counts;
    bool ran = false;
    std::optional<ruvia::task<result_type>> task_value;
    auto create = [&] {
        task_value.emplace(cold_frame<result_type>(frame_allocation(counts), ran));
    };
    if (cross_thread) {
        std::thread(create).join();
    } else {
        create();
    }
    if (ran || counts.allocations_ == counts.deallocations_) {
        return false;
    }
    if (cross_thread) {
        std::thread([&] { task_value.reset(); }).join();
    } else {
        task_value.reset();
    }
    return !ran && counts.allocations_ == counts.deallocations_;
}

}  // namespace ruvia::test::task_completion_tests

TEST(Ruvia, ColdTaskFrameSameThread) {
    EXPECT_TRUE(ruvia::test::task_completion_tests::check_cold_frame<void>(false));
    EXPECT_TRUE(ruvia::test::task_completion_tests::check_cold_frame<int>(false));
}

TEST(Ruvia, ColdTaskFrameCrossThread) {
    EXPECT_TRUE(ruvia::test::task_completion_tests::check_cold_frame<void>(true));
    EXPECT_TRUE(ruvia::test::task_completion_tests::check_cold_frame<int>(true));
}

namespace ruvia::test::blocking_pool_tests {

template <typename value_type>
concept separated_blocking_result_access =
    requires(blocking_result<value_type>& result, const blocking_result<value_type>& constant) {
        { result.value() } -> std::same_as<std::add_lvalue_reference_t<value_type>>;
        { constant.value() } -> std::same_as<std::conditional_t<std::is_void_v<value_type>, void, std::add_lvalue_reference_t<const value_type>>>;
        { std::move(result).take_value() } -> std::same_as<value_type>;
    } &&
    (!requires(blocking_result<value_type>& result) { result.take_value(); }) &&
    (!requires(blocking_result<value_type>& result) { std::move(result).value(); }) &&
    (!requires(const blocking_result<value_type>& result) { std::move(result).value(); }) &&
    (!requires(const blocking_result<value_type>& result) { std::move(result).take_value(); });

static_assert(separated_blocking_result_access<int>);
static_assert(separated_blocking_result_access<void>);
static_assert(separated_blocking_result_access<std::unique_ptr<int>>);

struct yielding_result final {
    explicit yielding_result(int value) noexcept
        : value_(value) {}

    yielding_result(const yielding_result&) = delete;
    yielding_result& operator=(const yielding_result&) = delete;
    yielding_result(yielding_result&& other) noexcept
        : value_(std::exchange(other.value_, -1)) {
        // Moving a user result may yield while completion and suspension race.
        std::this_thread::yield();
    }

    int value_;
};

struct receiver_move_failure final {
    explicit receiver_move_failure(std::thread::id receiver) noexcept
        : receiver_(receiver) {}
    receiver_move_failure(const receiver_move_failure&) = delete;
    receiver_move_failure(receiver_move_failure&& other)
        : receiver_(other.receiver_) {
        if (std::this_thread::get_id() == receiver_) {
            throw std::runtime_error("receiver move failed");
        }
    }
    std::thread::id receiver_;
};

task<void> exercise_receiver_move_failure(worker_ref worker, bool& success) {
    const auto receiver = std::this_thread::get_id();
    auto result = co_await run_blocking(worker, {}, [receiver] { return receiver_move_failure(receiver); });
    if (result.status() != blocking_status::completed || !result.failed()) {
        co_return;
    }
    try {
        static_cast<void>(std::move(result).take_value());
    } catch (const std::runtime_error&) {
        success = true;
    }
}

task<void> exercise_completion_during_suspend(worker_ref worker, bool& success) {
    for (int value = 0; value < 1024; ++value) {
        auto result = co_await run_blocking(worker, [value] { return yielding_result(value); });
        const auto& const_result = result;
        if (!result.completed() || const_result.value().value_ != value ||
            result.value().value_ != value || std::move(result).take_value().value_ != value) {
            co_return;
        }
    }
    success = true;
}

}  // namespace ruvia::test::blocking_pool_tests

namespace {

using ruvia::blocking_pool;
using ruvia::blocking_pool_options;

[[nodiscard]] bool blocking_pool_defaults_are_bounded_by_cpu_policy() {
    blocking_pool pool;
    const bool valid = pool.thread_count() >= 2 && pool.thread_count() <= 8 &&
                       pool.queue_capacity() == pool.thread_count() * 64;
    pool.stop();
    pool.join();
    return valid;
}
using ruvia::blocking_status;
using ruvia::blocking_submit_status;
using ruvia::task;
using ruvia::worker_ref;

// A task the pool runs on its own thread: it reports that it started, then
// holds its thread until the test releases it.
struct thread_gate final {
    std::binary_semaphore started_{0};
    std::binary_semaphore release_{0};

    void occupy(blocking_pool& pool) {
        const auto submitted = pool.submit([this] {
            started_.release();
            release_.acquire();
        });
        if (submitted != blocking_submit_status::accepted) {
            std::terminate();
        }
        started_.acquire();
    }
};

struct reentrant_discard_state final {
    blocking_pool* pool_{nullptr};
    std::atomic_int destroyed_{0};
    std::atomic_int ran_{0};
    std::atomic_int rejected_{0};
};

struct reentrant_discarded_task final {
    reentrant_discard_state* state_{nullptr};

    explicit reentrant_discarded_task(reentrant_discard_state& state_value)
        : state_(&state_value) {}
    reentrant_discarded_task(const reentrant_discarded_task&) = delete;
    reentrant_discarded_task& operator=(const reentrant_discarded_task&) = delete;
    reentrant_discarded_task(reentrant_discarded_task&& other) noexcept
        : state_(std::exchange(other.state_, nullptr)) {}

    ~reentrant_discarded_task() {
        if (state_ == nullptr) {
            return;
        }
        state_->destroyed_.fetch_add(1, std::memory_order_relaxed);
        static_cast<void>(state_->pool_->stats());
        state_->pool_->stop();
        if (state_->pool_->submit([] {}) == blocking_submit_status::pool_stopped) {
            state_->rejected_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    void operator()() const noexcept {
        state_->ran_.fetch_add(1, std::memory_order_relaxed);
    }
};

enum class move_action { query,
    submit,
    stop };

struct reentrant_move_state final {
    blocking_pool* pool_{};
    move_action action_{};
    std::atomic<int> moves_{};
    std::atomic<int> queries_{};
    std::atomic<int> destroyed_{};
    std::atomic<int> ran_{};
    std::binary_semaphore ran_signal_{0};
    blocking_submit_status nested_status_{blocking_submit_status::accepted};
};

struct reentrant_moving_task final {
    explicit reentrant_moving_task(reentrant_move_state& state)
        : state_(&state) {}
    reentrant_moving_task(const reentrant_moving_task&) = delete;
    reentrant_moving_task& operator=(const reentrant_moving_task&) = delete;
    reentrant_moving_task(reentrant_moving_task&& other) noexcept
        : state_(other.state_),
          owns_(std::exchange(other.owns_, false)) {
        const auto move = state_->moves_.fetch_add(1) + 1;
        static_cast<void>(state_->pool_->stats());
        state_->queries_.fetch_add(1);
        if (move == 3) {
            if (state_->action_ == move_action::submit) {
                state_->nested_status_ = state_->pool_->submit([] {}).status();
            } else if (state_->action_ == move_action::stop) {
                state_->pool_->stop();
            }
        }
    }
    ~reentrant_moving_task() {
        static_cast<void>(state_->pool_->stats());
        state_->queries_.fetch_add(1);
        if (owns_) {
            state_->destroyed_.fetch_add(1);
        }
    }
    void operator()() const noexcept {
        state_->ran_.fetch_add(1);
        state_->ran_signal_.release();
    }

    reentrant_move_state* state_;
    bool owns_{true};
};

bool test_callable_moves_and_destruction_can_query_pool() {
    for (const auto action : {move_action::query, move_action::submit}) {
        thread_gate gate;
        reentrant_move_state state;
        blocking_pool pool({.thread_count_ = 1, .queue_capacity_ = 1});
        state.pool_ = &pool;
        state.action_ = action;
        gate.occupy(pool);
        ruvia::move_only_function<void()> task_value{reentrant_moving_task(state)};
        const auto status = pool.submit(std::move(task_value));
        const auto queued = pool.stats();
        gate.release_.release();
        if (status != blocking_submit_status::accepted) {
            pool.join();
            return false;
        }
        state.ran_signal_.acquire();
        pool.join();
        const auto finished = pool.stats();
        if (queued.queued_ != 1 || finished.completed_ != 2 || finished.running_ != 0 ||
            state.moves_.load() < 4 || state.queries_.load() < 4 ||
            state.ran_.load() != 1 || state.destroyed_.load() != 1 ||
            (action == move_action::submit &&
                (state.nested_status_ != blocking_submit_status::queue_full || finished.rejected_ != 1))) {
            return false;
        }
    }
    return true;
}

bool test_callable_move_can_stop_pool_before_publication() {
    thread_gate gate;
    reentrant_move_state state;
    blocking_pool pool({.thread_count_ = 1, .queue_capacity_ = 2});
    state.pool_ = &pool;
    state.action_ = move_action::stop;
    gate.occupy(pool);
    std::atomic<bool> queued_ran{};
    const auto queued = pool.submit([&] { queued_ran.store(true); });
    ruvia::move_only_function<void()> task_value{reentrant_moving_task(state)};
    auto status = pool.submit(std::move(task_value));
    gate.release_.release();
    pool.join();
    const auto stopped = pool.stats();
    // The callable that lost the race to stop() comes back to the caller untouched.
    const bool returned = status == blocking_submit_status::pool_stopped && status.rejected() != nullptr &&
                          state.destroyed_.load() == 0;
    static_cast<void>(std::move(status).take_rejected());
    return returned && queued == blocking_submit_status::accepted && stopped.queued_ == 0 &&
           stopped.running_ == 0 && stopped.discarded_ == 2 && stopped.completed_ == 1 && !queued_ran.load() &&
           state.ran_.load() == 0 && state.destroyed_.load() == 1;
}

task<void> exercise_results(worker_ref worker_value, bool& success) {
    auto value = co_await ruvia::run_blocking(worker_value, {}, [] { return 42; });
    if (!value.completed() || std::move(value).take_value() != 42) {
        co_return;
    }

    auto thrown = co_await ruvia::run_blocking(
        worker_value, {}, []() -> int { throw std::system_error(std::make_error_code(std::errc::permission_denied)); });
    const auto failure = thrown.error();
    auto relocated = std::move(thrown);
    for (auto* outcome : {&thrown, &relocated}) {
        if (outcome->status() != blocking_status::completed || !outcome->failed() ||
            outcome->completed() || outcome->error() != failure) {
            co_return;
        }
        bool rethrown = false;
        try {
            static_cast<void>(std::move(*outcome).take_value());
        } catch (const std::system_error& error) {
            rethrown = error.code() == std::errc::permission_denied;
        }
        if (!rethrown) {
            co_return;
        }
    }

    std::atomic_bool ran{false};
    auto empty = co_await ruvia::run_blocking(worker_value, {}, [flag = &ran] { flag->store(true); });
    if (!empty.completed() || !ran.load()) {
        co_return;
    }
    static_cast<void>(std::move(empty).take_value());

    // A moved-only result travels back by move, not by copy.
    auto owned = co_await ruvia::run_blocking(worker_value, {}, [] { return std::make_unique<int>(7); });
    if (!owned.completed()) {
        co_return;
    }
    const auto pointer = std::move(owned).take_value();
    if (pointer == nullptr || *pointer != 7) {
        co_return;
    }

    success = true;
}

task<void> exercise_cancellation(worker_ref worker_value, thread_gate& gate_value,
    ruvia::stop_source& source_value, std::atomic_bool& callable_finished,
    std::weak_ptr<int>& abandoned_resource, bool& success) {
    auto owned_resource = std::make_shared<int>(12);
    abandoned_resource = owned_resource;
    auto result_value = co_await ruvia::run_blocking(worker_value,
        {.timeout_ = std::chrono::seconds(30), .stop_token_ = source_value.token()},
        [&, owned_resource = std::move(owned_resource)]() mutable {
            gate_value.started_.release();
            gate_value.release_.acquire();
            callable_finished.store(true, std::memory_order_release);
            return std::move(owned_resource);
        });
    if (result_value.status() != blocking_status::cancelled || result_value.completed() || result_value.failed()) {
        gate_value.release_.release();
        co_return;
    }
    try {
        static_cast<void>(std::move(result_value).take_value());
    } catch (const ruvia::blocking_operation_rejected& error) {
        success = error.status() == blocking_status::cancelled;
    }
    gate_value.release_.release();
}

// A wedged callable must not pin its caller forever: the wait has a deadline,
// even though the pool thread stays occupied until the callable returns.
task<void> exercise_timeout(
    worker_ref worker_value, thread_gate& gate_value, std::weak_ptr<int>& abandoned_resource, bool& success) {
    auto owned_resource = std::make_shared<int>(1);
    abandoned_resource = owned_resource;
    auto timed_out = co_await ruvia::run_blocking(worker_value, {.timeout_ = std::chrono::milliseconds(20)},
        [&gate_value, owned_resource = std::move(owned_resource)]() mutable {
            gate_value.release_.acquire();
            return std::move(owned_resource);
        });
    if (timed_out.status() != blocking_status::timed_out || timed_out.completed()) {
        co_return;
    }
    bool rejected = false;
    try {
        static_cast<void>(std::move(timed_out).take_value());
    } catch (const ruvia::blocking_operation_rejected& error) {
        rejected = error.status() == blocking_status::timed_out;
    }
    gate_value.release_.release();

    // A deadline that is not reached behaves exactly like the untimed wait.
    auto in_time = co_await ruvia::run_blocking(worker_value, {.timeout_ = std::chrono::seconds(30)}, [] { return 9; });
    success = rejected && in_time.completed() && std::move(in_time).take_value() == 9;
}

task<void> exercise_saturating_timeout(
    blocking_pool& pool, worker_ref worker_value, thread_gate& gate_value, bool& success) {
    // Keep the pool occupied so a wrapped timeout cannot hide behind a task
    // that happens to finish before the receiver arms its deadline.
    std::atomic_bool completed{false};
    std::thread releaser([&pool, &gate_value, &completed] {
        while (pool.stats().queued_ == 0 && !completed.load(std::memory_order_relaxed)) {
            std::this_thread::yield();
        }
        gate_value.release_.release();
    });

    using wide_milliseconds = std::chrono::duration<std::uint64_t, std::milli>;
    auto result_value = co_await ruvia::run_blocking(
        worker_value, {.timeout_ = wide_milliseconds(std::numeric_limits<std::uint64_t>::max())},
        [] { return 17; });
    completed.store(true, std::memory_order_relaxed);
    releaser.join();
    success = result_value.completed() && std::move(result_value).take_value() == 17;
}

task<void> count_on_worker(std::atomic_int& order, int& observed_value) {
    observed_value = order.fetch_add(1);
    co_return;
}

task<void> block_then_count(worker_ref worker_value, std::atomic_int& order, int& observed_value, bool& completed) {
    auto result_value = co_await ruvia::run_blocking(
        worker_value, {}, [] { std::this_thread::sleep_for(std::chrono::milliseconds(50)); });
    completed = result_value.completed();
    observed_value = order.fetch_add(1);
}

// The point of the pool: while one handler waits on blocking work, the worker
// keeps running other coroutines instead of sitting inside the blocking call.
task<void> exercise_worker_stays_free(worker_ref worker_value, bool& success) {
    std::atomic_int order{0};
    int blocking_order = -1;
    int free_order = -1;
    bool blocking_completed = false;

    ruvia::task_scope scope(worker_value);
    scope.spawn(block_then_count(worker_value, order, blocking_order, blocking_completed));
    scope.spawn(count_on_worker(order, free_order));
    co_await scope.join();

    success = blocking_completed && free_order == 0 && blocking_order == 1;
}

// A pool that stops must not leave a suspended handler waiting forever: tasks
// discarded from the queue still answer their waiter.
task<void> exercise_stopped_pool(
    blocking_pool& pool, worker_ref worker_value, thread_gate& gate_value,
    std::weak_ptr<int>& discarded_resource, bool& success) {
    std::thread stopper([&pool] {
        while (pool.stats().queued_ == 0) {
            std::this_thread::yield();
        }
        pool.stop();
    });

    auto owned_resource = std::make_shared<int>(1);
    discarded_resource = owned_resource;
    auto result_value = co_await ruvia::run_blocking(worker_value, {},
        [owned_resource = std::move(owned_resource)]() mutable { return std::move(owned_resource); });
    stopper.join();
    gate_value.release_.release();

    if (result_value.status() != blocking_status::pool_stopped || result_value.completed()) {
        co_return;
    }
    bool rejected = false;
    try {
        static_cast<void>(std::move(result_value).take_value());
    } catch (const ruvia::blocking_operation_rejected& error) {
        rejected = error.status() == blocking_status::pool_stopped;
    }
    // A stopped pool refuses new work rather than queueing it forever.
    auto refused = co_await ruvia::run_blocking(worker_value, {}, [] { return 2; });
    // A stopping pool is shutdown accounting, never the overload signal an
    // operator sizes the pool from.
    const auto stats = pool.stats();
    success = rejected && refused.status() == blocking_status::pool_stopped && stats.rejected_ == 0 &&
              stats.discarded_ >= 2;
}

task<void> exercise_queue_full(
    blocking_pool& pool, worker_ref worker_value, thread_gate& gate_value, bool& success) {
    // One thread is occupied and the single queue slot is taken, so this one
    // has nowhere to go.
    const auto queued = pool.submit([] {});
    auto result_value = co_await ruvia::run_blocking(worker_value, {}, [] { return 1; });
    gate_value.release_.release();
    const auto stats = pool.stats();
    success = queued == blocking_submit_status::accepted &&
              result_value.status() == blocking_status::queue_full && stats.rejected_ == 1 &&
              stats.discarded_ == 0;
}

task<void> exercise_worker_stopping(
    ruvia::event_loop_attachment& attachment, worker_ref worker_value, thread_gate& gate_value, bool& success) {
    asio::post(attachment.loop().executor(), [&attachment] { attachment.stop(); });
    auto result_value = co_await ruvia::run_blocking(worker_value, {}, [&gate_value] {
        gate_value.release_.acquire();
        return 5;
    });
    success = result_value.status() == blocking_status::worker_stopping && !result_value.completed() &&
              !result_value.failed();
}

// Offloading from a worker that has ALREADY stopped is the same shutdown
// outcome, not an exception the caller never asked for.
task<void> exercise_stopped_worker(
    ruvia::event_loop_attachment& attachment, worker_ref worker_value, std::atomic_bool& ran, bool& success) {
    attachment.stop();
    auto result_value = co_await ruvia::run_blocking(worker_value, {}, [flag = &ran] {
        flag->store(true);
        return 1;
    });
    success = result_value.status() == blocking_status::worker_stopping;
}

// Outcomes the wait would decide without the callable are decided before
// submission, so the callable never occupies a pool thread for them.
task<void> exercise_settled_before_submit(
    worker_ref worker_value, worker_ref other_worker, std::atomic_bool& ran, bool& success) {
    ruvia::stop_source stopped;
    stopped.request_stop();
    auto cancelled = co_await ruvia::run_blocking(
        worker_value, {.stop_token_ = stopped.token()}, [flag = &ran] { flag->store(true); });
    auto expired = co_await ruvia::run_blocking(worker_value,
        {.timeout_ = ruvia::operation_timeout(std::chrono::steady_clock::time_point{})},
        [flag = &ran] { flag->store(true); });
    bool nonpositive_rejected = false;
    try {
        static_cast<void>(ruvia::run_blocking(
            worker_value, {.timeout_ = std::chrono::milliseconds(0)}, [flag = &ran] { flag->store(true); }));
    } catch (const std::invalid_argument&) {
        nonpositive_rejected = true;
    }
    bool misplaced = false;
    try {
        static_cast<void>(co_await ruvia::run_blocking(other_worker, {}, [flag = &ran] {
            flag->store(true);
        }));
    } catch (const std::logic_error&) {
        misplaced = true;
    }
    success = cancelled.status() == blocking_status::cancelled && expired.status() == blocking_status::timed_out &&
              nonpositive_rejected && misplaced;
}

// A worker whose event loop was given no pool reports that state; the callable
// runs nowhere else.
task<void> exercise_worker_without_pool(worker_ref worker_value, std::atomic_bool& ran, bool& success) {
    auto result_value = co_await ruvia::run_blocking(worker_value, {}, [flag = &ran] { flag->store(true); });
    if (result_value.status() != blocking_status::unavailable || result_value.completed() || result_value.failed()) {
        co_return;
    }
    try {
        static_cast<void>(std::move(result_value).take_value());
    } catch (const ruvia::blocking_operation_rejected& error) {
        success = error.status() == blocking_status::unavailable;
    }
}

bool test_destruction_does_not_join_running_callable() {
    auto started_promise = std::make_shared<std::promise<void>>();
    auto started = started_promise->get_future();
    auto finished_promise = std::make_shared<std::promise<void>>();
    auto finished = finished_promise->get_future();
    std::promise<void> release_promise;
    auto release = release_promise.get_future().share();
    std::promise<void> destroyed_promise;
    auto destroyed = destroyed_promise.get_future();

    auto pool = std::make_unique<blocking_pool>(blocking_pool_options{.thread_count_ = 1});
    if (pool->submit(
            [started = std::move(started_promise), finished = std::move(finished_promise), release] {
                started->set_value();
                release.wait();
                finished->set_value();
            }) != blocking_submit_status::accepted) {
        return false;
    }
    started.wait();

    std::thread destroyer([pool = std::move(pool), &destroyed_promise]() mutable {
        pool.reset();
        destroyed_promise.set_value();
    });
    const bool returned_before_callable =
        destroyed.wait_for(std::chrono::milliseconds(500)) == std::future_status::ready;

    release_promise.set_value();
    destroyer.join();
    const bool callable_finished =
        finished.wait_for(std::chrono::milliseconds(500)) == std::future_status::ready;
    return returned_before_callable && callable_finished;
}

bool test_join_stops_and_waits_for_running_callable() {
    auto started_promise = std::make_shared<std::promise<void>>();
    auto started = started_promise->get_future();
    std::promise<void> release_promise;
    auto release = release_promise.get_future().share();
    std::promise<void> joined_promise;
    auto joined = joined_promise.get_future();

    blocking_pool pool(blocking_pool_options{.thread_count_ = 1});
    if (pool.submit([started = std::move(started_promise), release] {
            started->set_value();
            release.wait();
        }) != blocking_submit_status::accepted) {
        return false;
    }
    started.wait();

    std::thread joiner([&pool, &joined_promise] {
        pool.join();
        joined_promise.set_value();
    });
    const bool join_waited =
        joined.wait_for(std::chrono::milliseconds(200)) == std::future_status::timeout;

    release_promise.set_value();
    joiner.join();
    return join_waited && joined.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready;
}

bool test_join_rejects_pool_thread_before_stopping() {
    blocking_pool pool(blocking_pool_options{.thread_count_ = 1, .queue_capacity_ = 1});
    std::promise<bool> completed;
    auto result_value = completed.get_future();
    if (pool.submit([&] {
            bool rejected = false;
            try {
                pool.join();
            } catch (const std::logic_error& error) {
                rejected = std::string_view(error.what()) ==
                           "cannot join a blocking pool from one of its threads";
            }
            const auto still_accepting = pool.submit([] {}) == blocking_submit_status::accepted;
            completed.set_value(rejected && still_accepting);
        }) != blocking_submit_status::accepted) {
        return false;
    }
    const bool rejected = result_value.get();
    pool.join();
    return rejected;
}

bool test_stop_drains_without_running_queued_tasks() {
    constexpr int queued_task_count = 3;
    thread_gate gate;
    reentrant_discard_state state;
    blocking_pool pool(blocking_pool_options{.thread_count_ = 1, .queue_capacity_ = queued_task_count});
    state.pool_ = &pool;
    gate.occupy(pool);

    for (int i = 0; i < queued_task_count; ++i) {
        if (pool.submit(reentrant_discarded_task(state)) !=
            blocking_submit_status::accepted) {
            gate.release_.release();
            pool.join();
            return false;
        }
    }

    pool.stop();
    const auto stopped = pool.stats();
    gate.release_.release();
    pool.join();
    const auto finished = pool.stats();
    return stopped.queued_ == 0 && stopped.running_ == 1 &&
           stopped.discarded_ == queued_task_count * 2 &&
           state.destroyed_.load(std::memory_order_relaxed) == queued_task_count &&
           state.rejected_.load(std::memory_order_relaxed) == queued_task_count &&
           state.ran_.load(std::memory_order_relaxed) == 0 && finished.running_ == 0 &&
           finished.completed_ == 1;
}

}  // namespace

TEST(Ruvia, blocking_pool_results_and_worker_progress) {
    bool results = false;
    bool worker_stays_free = false;
    {
        blocking_pool pool(blocking_pool_options{.thread_count_ = 2});
        asio::io_context io_context;
        auto attachment = ruvia::attach_event_loop(io_context, {.queue_capacity_ = 8, .blocking_pool_ = &pool});
        const auto worker_value = attachment.loop().worker();
        ruvia::test::run_worker_tasks(attachment, exercise_results(worker_value, results),
            exercise_worker_stays_free(worker_value, worker_stays_free));
        pool.join();
    }
    EXPECT_TRUE(results);
    EXPECT_TRUE(worker_stays_free);
}

TEST(Ruvia, blocking_pool_cross_thread_completion_preserves_results) {
    bool success = false;
    blocking_pool pool({.thread_count_ = 1});
    asio::io_context io_context;
    auto attachment = ruvia::attach_event_loop(io_context, {.queue_capacity_ = 8, .blocking_pool_ = &pool});
    ruvia::test::run_worker_tasks(attachment,
        ruvia::test::blocking_pool_tests::exercise_completion_during_suspend(attachment.loop().worker(), success));
    pool.join();
    EXPECT_TRUE(success);
}

TEST(Ruvia, blocking_pool_receiver_move_failure_is_reported) {
    bool success = false;
    blocking_pool pool({.thread_count_ = 1});
    asio::io_context io_context;
    auto attachment = ruvia::attach_event_loop(io_context, {.queue_capacity_ = 8, .blocking_pool_ = &pool});
    ruvia::test::run_worker_tasks(attachment,
        ruvia::test::blocking_pool_tests::exercise_receiver_move_failure(attachment.loop().worker(), success));
    pool.join();
    EXPECT_TRUE(success);
}

TEST(Ruvia, blocking_pool_cancellation_releases_the_waiter) {
    bool cancelled = false;
    std::atomic_bool cancelled_callable_finished{false};
    std::weak_ptr<int> abandoned_resource;
    {
        thread_gate gate;
        ruvia::stop_source source;
        blocking_pool pool(blocking_pool_options{.thread_count_ = 1});
        asio::io_context io_context;
        auto attachment = ruvia::attach_event_loop(io_context, {.queue_capacity_ = 8, .blocking_pool_ = &pool});
        const auto worker_value = attachment.loop().worker();
        std::thread canceller([&] {
            gate.started_.acquire();
            source.request_stop();
        });
        ruvia::test::run_worker_tasks(attachment, exercise_cancellation(worker_value, gate, source,
                                                      cancelled_callable_finished, abandoned_resource, cancelled));
        canceller.join();
        pool.join();
    }
    EXPECT_TRUE(cancelled);
    EXPECT_TRUE(cancelled_callable_finished.load(std::memory_order_acquire));
    EXPECT_TRUE(abandoned_resource.expired());
}

TEST(Ruvia, blocking_pool_timeout_releases_the_waiter) {
    bool timeout = false;
    std::weak_ptr<int> abandoned_resource;
    {
        thread_gate gate;
        blocking_pool pool(blocking_pool_options{.thread_count_ = 2});
        asio::io_context io_context;
        auto attachment = ruvia::attach_event_loop(io_context, {.queue_capacity_ = 8, .blocking_pool_ = &pool});
        const auto worker_value = attachment.loop().worker();
        ruvia::test::run_worker_tasks(
            attachment, exercise_timeout(worker_value, gate, abandoned_resource, timeout));
        pool.join();
    }
    EXPECT_TRUE(timeout);
    EXPECT_TRUE(abandoned_resource.expired());
}

TEST(Ruvia, blocking_pool_timeout_saturates) {
    bool saturating_timeout = false;
    {
        thread_gate gate;
        blocking_pool pool(blocking_pool_options{.thread_count_ = 1});
        gate.occupy(pool);
        asio::io_context io_context;
        auto attachment = ruvia::attach_event_loop(io_context, {.queue_capacity_ = 8, .blocking_pool_ = &pool});
        const auto worker_value = attachment.loop().worker();
        ruvia::test::run_worker_tasks(attachment,
            exercise_saturating_timeout(pool, worker_value, gate, saturating_timeout));
        pool.join();
    }
    EXPECT_TRUE(saturating_timeout);
}

TEST(Ruvia, blocking_pool_stop_releases_queued_waiters) {
    bool stopped_pool = false;
    std::weak_ptr<int> discarded_resource;
    {
        thread_gate gate;
        blocking_pool pool(blocking_pool_options{.thread_count_ = 1});
        gate.occupy(pool);
        asio::io_context io_context;
        auto attachment = ruvia::attach_event_loop(io_context, {.queue_capacity_ = 8, .blocking_pool_ = &pool});
        const auto worker_value = attachment.loop().worker();
        ruvia::test::run_worker_tasks(attachment,
            exercise_stopped_pool(pool, worker_value, gate, discarded_resource, stopped_pool));
        pool.join();
    }
    EXPECT_TRUE(stopped_pool);
    EXPECT_TRUE(discarded_resource.expired());
}

TEST(Ruvia, blocking_pool_full_queue_rejects_work) {
    bool queue_full = false;
    {
        thread_gate gate;
        blocking_pool pool(blocking_pool_options{.thread_count_ = 1, .queue_capacity_ = 1});
        gate.occupy(pool);
        asio::io_context io_context;
        auto attachment = ruvia::attach_event_loop(io_context, {.queue_capacity_ = 8, .blocking_pool_ = &pool});
        const auto worker_value = attachment.loop().worker();
        ruvia::test::run_worker_tasks(attachment, exercise_queue_full(pool, worker_value, gate, queue_full));
        pool.join();
    }
    EXPECT_TRUE(queue_full);
}

TEST(Ruvia, blocking_pool_preserves_fifo_and_recovers_queue_capacity) {
    for (const std::size_t capacity : {1, 3, 5, 8}) {
        std::size_t next = 0;
        std::array<std::size_t, 8> observed{};
        thread_gate gate;
        std::binary_semaphore completed{0};
        blocking_pool pool({.thread_count_ = 1, .queue_capacity_ = capacity});
        EXPECT_EQ(pool.queue_capacity(), capacity);
        for (std::size_t round = 0; round != 16; ++round) {
            next = 0;
            gate.occupy(pool);
            for (std::size_t index = 0; index != capacity; ++index) {
                EXPECT_EQ(pool.submit([&, index, round] {
                    observed[next++] = round * capacity + index;
                    if (index + 1 == capacity) {
                        completed.release();
                    }
                }),
                    blocking_submit_status::accepted);
            }
            EXPECT_EQ(pool.stats().queued_, capacity);
            EXPECT_EQ(pool.submit([] {}), blocking_submit_status::queue_full);
            gate.release_.release();
            completed.acquire();
            for (std::size_t index = 0; index != capacity; ++index) {
                EXPECT_EQ(observed[index], round * capacity + index);
            }
        }
        pool.join();
    }
}

TEST(Ruvia, blocking_pool_worker_stop_releases_the_waiter) {
    bool worker_stopping = false;
    {
        thread_gate gate;
        blocking_pool pool(blocking_pool_options{.thread_count_ = 1});
        asio::io_context io_context;
        auto attachment = ruvia::attach_event_loop(io_context, {.queue_capacity_ = 8, .blocking_pool_ = &pool});
        const auto worker_value = attachment.loop().worker();
        ruvia::test::run_worker_tasks(
            attachment, exercise_worker_stopping(attachment, worker_value, gate, worker_stopping));
        gate.release_.release();
        pool.join();
    }
    EXPECT_TRUE(worker_stopping);
}

TEST(Ruvia, blocking_pool_stopped_worker_rejects_work) {
    bool stopped_worker = false;
    std::atomic_bool stopped_worker_task_ran{false};
    {
        blocking_pool pool(blocking_pool_options{.thread_count_ = 1});
        asio::io_context io_context;
        auto attachment = ruvia::attach_event_loop(io_context, {.queue_capacity_ = 8, .blocking_pool_ = &pool});
        const auto worker_value = attachment.loop().worker();
        ruvia::test::run_worker_tasks(
            attachment, exercise_stopped_worker(attachment, worker_value, stopped_worker_task_ran, stopped_worker));
        pool.join();
        stopped_worker =
            stopped_worker && !stopped_worker_task_ran.load() && pool.stats().completed_ == 0;
    }
    EXPECT_TRUE(stopped_worker);
}

TEST(Ruvia, blocking_pool_rejects_settled_operations_before_submission) {
    bool settled_before_submit = false;
    std::atomic_bool settled_task_ran{false};
    {
        blocking_pool pool(blocking_pool_options{.thread_count_ = 1});
        asio::io_context io_context;
        auto attachment = ruvia::attach_event_loop(io_context, {.queue_capacity_ = 8, .blocking_pool_ = &pool});
        asio::io_context other_context;
        auto other_attachment = ruvia::attach_event_loop(other_context, {.queue_capacity_ = 8});
        const auto worker_value = attachment.loop().worker();
        // The worker lends out the pool its loop borrows, and nothing else.
        EXPECT_EQ(attachment.loop().worker().blocking_pool(), &pool);
        EXPECT_EQ(other_attachment.loop().worker().blocking_pool(), nullptr);
        EXPECT_EQ(ruvia::worker_ref{}.blocking_pool(), nullptr);
        ruvia::test::run_worker_tasks(attachment, exercise_settled_before_submit(worker_value,
                                                      other_attachment.loop().worker(), settled_task_ran,
                                                      settled_before_submit));
        pool.join();
        settled_before_submit =
            settled_before_submit && !settled_task_ran.load() && pool.stats().completed_ == 0;
    }
    EXPECT_TRUE(settled_before_submit);
}

TEST(Ruvia, blocking_pool_worker_without_pool_reports_unavailable) {
    bool unavailable = false;
    std::atomic_bool ran{false};
    asio::io_context io_context;
    auto attachment = ruvia::attach_event_loop(io_context, {.queue_capacity_ = 8});
    ruvia::test::run_worker_tasks(
        attachment, exercise_worker_without_pool(attachment.loop().worker(), ran, unavailable));
    EXPECT_TRUE(unavailable);
    EXPECT_TRUE(!ran.load());
}

TEST(Ruvia, blocking_pool_rejects_empty_tasks) {
    bool rejects_empty_task = false;
    {
        blocking_pool pool(blocking_pool_options{.thread_count_ = 1});
        try {
            static_cast<void>(pool.submit({}));
        } catch (const std::invalid_argument&) {
            rejects_empty_task = true;
        }
        pool.stop();
        pool.join();
        rejects_empty_task =
            rejects_empty_task && pool.submit([] {}) == blocking_submit_status::pool_stopped;
    }
    EXPECT_TRUE(rejects_empty_task);
}

TEST(Ruvia, blocking_pool_default_sizing_is_bounded) {
    EXPECT_TRUE(blocking_pool_defaults_are_bounded_by_cpu_policy());
}

TEST(Ruvia, blocking_pool_destruction_does_not_join_running_work) {
    EXPECT_TRUE(test_destruction_does_not_join_running_callable());
}

TEST(Ruvia, blocking_pool_join_waits_for_running_work) {
    EXPECT_TRUE(test_join_stops_and_waits_for_running_callable());
}

TEST(Ruvia, blocking_pool_join_rejects_its_own_thread) {
    EXPECT_TRUE(test_join_rejects_pool_thread_before_stopping());
}

TEST(Ruvia, blocking_pool_stop_discards_queued_work_without_running_it) {
    EXPECT_TRUE(test_stop_drains_without_running_queued_tasks());
}

TEST(Ruvia, blocking_pool_callable_moves_and_destruction_allow_reentry) {
    EXPECT_TRUE(test_callable_moves_and_destruction_can_query_pool());
}

TEST(Ruvia, blocking_pool_callable_move_can_stop_before_publication) {
    EXPECT_TRUE(test_callable_move_can_stop_pool_before_publication());
}

TEST(Ruvia, blocking_pool_join_waits_for_discard_cleanup_and_allows_reentrant_join) {
    thread_gate gate;
    std::binary_semaphore discard_started{0};
    std::binary_semaphore release_discard{0};
    std::atomic_bool await_discard{false};
    std::atomic_bool discarded{false};
    std::atomic_bool ran{false};
    blocking_pool pool({.thread_count_ = 1, .queue_capacity_ = 1});
    gate.occupy(pool);
    auto payload = std::shared_ptr<int>(new int(17), [&](int* value) {
        if (await_discard.load()) {
            discard_started.release();
            release_discard.acquire();
            pool.join();
        }
        delete value;
        discarded = true;
    });
    const auto status = pool.submit([payload = std::move(payload), &ran] { ran = *payload == 17; });
    if (status != blocking_submit_status::accepted) {
        gate.release_.release();
        pool.join();
        FAIL() << "failed to queue the discard probe";
    }
    await_discard = true;
    std::thread stopper([&] { pool.stop(); });
    if (!discard_started.try_acquire_for(std::chrono::seconds(5))) {
        await_discard = false;
        release_discard.release();
        gate.release_.release();
        stopper.join();
        pool.join();
        FAIL() << "queued task destruction did not start";
    }
    gate.release_.release();
    std::promise<void> join_started;
    auto joining = join_started.get_future();
    std::promise<void> join_finished;
    auto joined = join_finished.get_future();
    std::thread joiner([&] {
        join_started.set_value();
        pool.join();
        join_finished.set_value();
    });
    joining.wait();
    const bool waited_for_discard =
        joined.wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout;
    release_discard.release();
    stopper.join();
    joiner.join();
    EXPECT_TRUE(waited_for_discard);
    EXPECT_TRUE(discarded.load());
    EXPECT_FALSE(ran.load());
    const auto finished = pool.stats();
    EXPECT_EQ(finished.queued_, 0U);
    EXPECT_EQ(finished.running_, 0U);
    EXPECT_EQ(finished.completed_, 1U);
    EXPECT_EQ(finished.discarded_, 1U);
}

TEST(Ruvia, blocking_pool_rejects_overflowing_capacity_options) {
    constexpr std::size_t wrapped_threads =
        std::size_t{1} << (std::numeric_limits<std::size_t>::digits - 6);
    for (const blocking_pool_options options : {
             blocking_pool_options{.thread_count_ = wrapped_threads, .queue_capacity_ = 0},
             blocking_pool_options{
                 .thread_count_ = (std::numeric_limits<std::size_t>::max() / 64) + 1,
                 .queue_capacity_ = 0},
             blocking_pool_options{
                 .thread_count_ = std::numeric_limits<std::size_t>::max(),
                 .queue_capacity_ = 1},
         }) {
        bool rejected = false;
        try {
            blocking_pool pool(options);
            static_cast<void>(pool);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        EXPECT_TRUE(rejected);
    }
}

TEST(Ruvia, blocking_pool_submit_returns_rejected_callable) {
    thread_gate gate;
    blocking_pool pool({.thread_count_ = 1, .queue_capacity_ = 1});
    gate.occupy(pool);
    auto owned = std::make_shared<int>(7);
    const std::weak_ptr<int> watch = owned;
    EXPECT_EQ(pool.submit([] {}), blocking_submit_status::accepted);

    // A full queue returns the callable with everything it owns.
    auto full = pool.submit([owned = std::move(owned)] { static_cast<void>(owned); });
    EXPECT_EQ(full.status(), blocking_submit_status::queue_full);
    ASSERT_NE(full.rejected(), nullptr);
    EXPECT_FALSE(watch.expired());
    auto returned = std::move(full).take_rejected();
    EXPECT_EQ(full.rejected(), nullptr);
    EXPECT_FALSE(watch.expired());

    gate.release_.release();
    pool.stop();
    // A stopped pool returns it as well; the caller decides where it dies.
    auto stopped = pool.submit(std::move(returned));
    EXPECT_EQ(stopped.status(), blocking_submit_status::pool_stopped);
    ASSERT_NE(stopped.rejected(), nullptr);
    EXPECT_FALSE(watch.expired());
    static_cast<void>(std::move(stopped).take_rejected());
    EXPECT_TRUE(watch.expired());
    pool.join();
    const auto stats = pool.stats();
    EXPECT_EQ(stats.rejected_, 1U);
}

namespace ruvia::test::channel_tests {

struct rejection_move_state final {
    std::function<void()> close_;
    std::binary_semaphore close_requested_{0};
    std::binary_semaphore close_completed_{0};
    bool observe_move_{true};
    bool close_completed_during_move_{false};
    bool same_thread_close_completed_{false};
    int destroyed_{0};

    void observe_move() {
        if (!std::exchange(observe_move_, false)) {
            return;
        }
        close_requested_.release();
        // If the return-value move holds the channel lock, let it finish after
        // this bounded wait so the closer can retire and the test can report.
        close_completed_during_move_ = close_completed_.try_acquire_for(std::chrono::milliseconds(300));
        if (close_completed_during_move_) {
            close_();
            same_thread_close_completed_ = true;
        }
    }
};

struct rejection_payload final {
    explicit rejection_payload(int value, rejection_move_state* state = nullptr)
        : value_(std::make_unique<int>(value)),
          state_(state) {}
    rejection_payload(const rejection_payload&) = delete;
    rejection_payload& operator=(const rejection_payload&) = delete;
    rejection_payload(rejection_payload&& other)
        : state_(other.state_) {
        if (state_ != nullptr) {
            state_->observe_move();
        }
        value_ = std::move(other.value_);
    }
    ~rejection_payload() {
        if (value_ && state_ != nullptr) {
            ++state_->destroyed_;
            state_->close_();
        }
    }

    std::unique_ptr<int> value_;
    rejection_move_state* state_;
};

bool test_rejected_payload_moves_can_close_the_channel() {
    bool success = true;
    for (const auto expected : {channel_send_status::closed, channel_send_status::full, channel_send_status::worker_stopping}) {
        asio::io_context io_context;
        auto attachment = attach_event_loop(io_context, {.queue_capacity_ = 8});
        {
            auto [sender, receiver] = make_channel<rejection_payload>(attachment.loop().worker(), {.capacity_ = 1});
            if (expected == channel_send_status::closed) {
                sender.close();
            } else if (expected == channel_send_status::full) {
                success = sender.send(rejection_payload(11)).accepted() && success;
            } else {
                attachment.stop();
            }
            rejection_move_state state;
            state.close_ = [&] { sender.close(); };
            std::thread closer([&] {
                state.close_requested_.acquire();
                sender.close();
                state.close_completed_.release();
            });
            {
                auto rejected = sender.send(rejection_payload(42, &state));
                closer.join();
                success = rejected == expected && expected == rejected &&
                          rejected != channel_send_status::sent && !rejected.accepted() &&
                          rejected.rejected() != nullptr && rejected.rejected()->value_ &&
                          *rejected.rejected()->value_ == 42 && state.close_completed_during_move_ &&
                          state.same_thread_close_completed_ && state.destroyed_ == 0 && success;
                auto taken = std::move(rejected).take_rejected();
                success = rejected.rejected() == nullptr && taken.value_ && *taken.value_ == 42 && success;
            }
            success = state.destroyed_ == 1 && success;
            receiver.close();
        }
    }
    return success;
}

class blocking_throwing_move final {
public:
    explicit blocking_throwing_move(int value) noexcept
        : value_(value) {}

    blocking_throwing_move(const blocking_throwing_move&) = delete;
    blocking_throwing_move& operator=(const blocking_throwing_move&) = delete;
    // This fixture intentionally models a move that can throw.
    blocking_throwing_move(blocking_throwing_move&& other) noexcept(false) {
        if (throw_on_move_.load(std::memory_order_relaxed)) {
            throw std::runtime_error("requested move failure");
        }
        value_ = std::exchange(other.value_, 0);
    }

    [[nodiscard]] int value() const noexcept {
        return value_;
    }

    static inline std::atomic_bool throw_on_move_{false};

private:
    int value_{0};
};

ruvia::task<void> receive_last(ruvia::channel_receiver<int>& receiver, bool& success) {
    const auto value = co_await receiver.receive();
    const auto closed = co_await receiver.receive();
    success = value.has_value() && value.value() == 3 &&
              closed.status() == ruvia::worker_wait_status::closed;
}

ruvia::task<void> receive_one(ruvia::channel_receiver<int>& receiver, int& value) {
    const auto result = co_await receiver.receive();
    if (result.has_value()) {
        value = result.value();
    }
}

ruvia::task<void> receive_queued_then_stopping(ruvia::event_loop_attachment& attachment,
    ruvia::channel_receiver<int>& receiver, bool& success) {
    const auto value = co_await receiver.receive();
    asio::post(attachment.loop().executor(), [&attachment] { attachment.stop(); });
    const auto stopping = co_await receiver.receive();
    success = value.has_value() && value.value() == 9 &&
              stopping.status() == ruvia::worker_wait_status::worker_stopping;
}

ruvia::task<void> receive_throwing_move(
    ruvia::channel_receiver<blocking_throwing_move>& receiver, bool& success) {
    const auto result_value = co_await receiver.receive({.timeout_ = std::chrono::seconds(1)});
    success = result_value.has_value() && result_value.value().value() == 6;
}

// A receive whose move out of the dequeued slot fails must still release the
// slot: the failed value is lost, later values and closure are unaffected.
ruvia::task<void> receive_after_move_failure(ruvia::channel_sender<blocking_throwing_move> sender,
    ruvia::channel_receiver<blocking_throwing_move>& receiver, bool& success) {
    bool move_failed = false;
    blocking_throwing_move::throw_on_move_.store(true, std::memory_order_relaxed);
    try {
        static_cast<void>(co_await receiver.receive());
    } catch (const std::runtime_error&) {
        move_failed = true;
    }
    blocking_throwing_move::throw_on_move_.store(false, std::memory_order_relaxed);
    const auto next = co_await receiver.receive();
    sender.close();
    const auto closed = co_await receiver.receive();
    success = move_failed && next.has_value() && next.value().value() == 2 &&
              closed.status() == ruvia::worker_wait_status::closed;
}

ruvia::task<void> receive_after_late_cancellation(
    ruvia::channel_receiver<int>& receiver, ruvia::stop_token stop_token_value,
    ruvia::stop_token next_stop_token, bool& success) {
    const auto first = co_await receiver.receive({.stop_token_ = std::move(stop_token_value)});
    const auto second = co_await receiver.receive({.stop_token_ = std::move(next_stop_token)});
    success = first.has_value() && first.value() == 1 && second.has_value() && second.value() == 2;
}

ruvia::task<void> receive_until_closed(ruvia::channel_receiver<int>& receiver, bool& success) {
    const auto result_value = co_await receiver.receive({.timeout_ = std::chrono::seconds(1)});
    success = result_value.status() == ruvia::worker_wait_status::closed;
}

ruvia::channel_receiver<int>::receive_operation make_cold_receive_after_receiver_close(
    ruvia::worker_ref worker_value, bool timed, bool buffered = false) {
    auto [sender, receiver] = ruvia::make_channel<int>(std::move(worker_value), {.capacity_ = 1});
    if (buffered && !sender.send(42).accepted()) {
        throw std::logic_error("could not queue the cold receive input");
    }
    if (timed) {
        return receiver.receive({.timeout_ = std::chrono::seconds(1)});
    }
    return receiver.receive();
}

ruvia::task<void> verify_cold_receive_operations(ruvia::channel_receiver<int>::receive_operation receive,
    ruvia::channel_receiver<int>::receive_operation timed_receive, bool& success) {
    const auto cold_closed = co_await std::move(receive);
    const auto timed_cold_closed = co_await std::move(timed_receive);
    success = cold_closed.status() == ruvia::worker_wait_status::closed &&
              timed_cold_closed.status() == ruvia::worker_wait_status::closed;
}

ruvia::task<void> verify_channel_side_closure(ruvia::worker_ref worker_value, bool& success) {
    auto [sender, receiver] = ruvia::make_channel<int>(worker_value, {.capacity_ = 1});
    if (!sender.send(42).accepted()) {
        co_return;
    }
    auto cold_receive = receiver.receive();
    sender.close();
    const auto buffered = co_await std::move(cold_receive);
    const auto drained = co_await receiver.receive();
    if (!buffered.has_value() || buffered.value() != 42 ||
        drained.status() != ruvia::worker_wait_status::closed) {
        co_return;
    }

    auto [retired_sender, retired_receiver] = ruvia::make_channel<int>(worker_value, {.capacity_ = 1});
    if (!retired_sender.send(7).accepted()) {
        co_return;
    }
    auto receive = retired_receiver.receive();
    auto timed_receive = retired_receiver.receive({.timeout_ = std::chrono::seconds(1)});
    retired_sender.close();
    retired_receiver.close();
    retired_receiver.close();
    const auto closed = co_await std::move(receive);
    const auto timed_closed = co_await std::move(timed_receive);
    success = closed.status() == ruvia::worker_wait_status::closed &&
              timed_closed.status() == ruvia::worker_wait_status::closed &&
              retired_sender.send(8).status() == ruvia::channel_send_status::closed;
}

ruvia::task<void> verify_receiver_retirement_after_worker_stop(
    ruvia::event_loop_attachment& attachment, bool& success) {
    auto [sender, receiver] = ruvia::make_channel<int>(attachment.loop().worker(), {.capacity_ = 1});
    if (!sender.send(9).accepted()) {
        co_return;
    }
    auto receive = receiver.receive();
    auto timed_receive = receiver.receive({.timeout_ = std::chrono::seconds(1)});
    receiver.close();
    attachment.stop();
    const auto stopping = co_await std::move(receive);
    const auto timed_stopping = co_await std::move(timed_receive);
    success = stopping.status() == ruvia::worker_wait_status::worker_stopping &&
              timed_stopping.status() == ruvia::worker_wait_status::worker_stopping &&
              sender.send(10).status() == ruvia::channel_send_status::worker_stopping;
}

struct buffered_payload final {
    explicit buffered_payload(int& destructions)
        : bytes_(1024, 'x'),
          destructions_(&destructions) {}
    buffered_payload(const buffered_payload&) = delete;
    buffered_payload& operator=(const buffered_payload&) = delete;
    buffered_payload(buffered_payload&& other) noexcept
        : bytes_(std::move(other.bytes_)),
          destructions_(std::exchange(other.destructions_, nullptr)) {}
    ~buffered_payload() {
        if (destructions_ != nullptr) {
            ++*destructions_;
        }
    }

    std::string bytes_;
    int* destructions_;
};

ruvia::task<void> verify_retired_payload_receive(
    ruvia::channel_receiver<buffered_payload>::receive_operation receive, bool& success) {
    const auto result_value = co_await std::move(receive);
    success = result_value.status() == ruvia::worker_wait_status::closed && !result_value.has_value();
}

bool test_retired_receiver_storage_is_released() {
    enum class cold_operation_retirement { absent,
        discarded,
        completed };
    bool success = true;
    for (const auto retirement : {cold_operation_retirement::absent, cold_operation_retirement::discarded, cold_operation_retirement::completed}) {
        asio::io_context io_context;
        auto attachment = ruvia::attach_event_loop(io_context, {.queue_capacity_ = 8});
        int destructions = 0;
        std::optional<ruvia::channel_receiver<buffered_payload>::receive_operation> cold_receive;
        {
            auto [sender, receiver] = ruvia::make_channel<buffered_payload>(
                attachment.loop().worker(), {.capacity_ = 1});
            success = sender.send(buffered_payload(destructions)).accepted() && success;
            if (retirement != cold_operation_retirement::absent) {
                cold_receive.emplace(receiver.receive());
            }
            receiver.close();
            // Retirement destroys the buffered value while the sender still
            // holds the shared state.
            success = destructions == 1 && success;
        }
        if (cold_receive) {
            if (retirement == cold_operation_retirement::completed) {
                bool closed = false;
                ruvia::test::run_worker_tasks(attachment, verify_retired_payload_receive(std::move(*cold_receive), closed));
                success = closed && success;
            }
            cold_receive.reset();
        }
        success = destructions == 1 && success;
    }
    return success;
}

struct teardown_payload;

// Endpoint owners a buffered payload can tear down from its destructor.
struct teardown_endpoints final {
    std::unique_ptr<channel_sender<teardown_payload>> sender_;
    std::unique_ptr<channel_receiver<teardown_payload>> receiver_;
    bool armed_{false};
    bool release_endpoints_{false};
    int armed_destructions_{0};
};

// The defaulted move keeps the endpoint reference in the moved-from source, so
// every payload object a drain retires runs the teardown, moved-from or not.
struct teardown_payload final {
    explicit teardown_payload(teardown_endpoints& endpoints) noexcept
        : endpoints_(&endpoints) {}
    teardown_payload(const teardown_payload&) = delete;
    teardown_payload& operator=(const teardown_payload&) = delete;
    teardown_payload(teardown_payload&&) noexcept = default;
    teardown_payload& operator=(teardown_payload&&) noexcept = default;
    ~teardown_payload() {
        if (!endpoints_->armed_) {
            return;
        }
        ++endpoints_->armed_destructions_;
        if (endpoints_->sender_) {
            endpoints_->sender_->close();
        }
        if (endpoints_->release_endpoints_) {
            endpoints_->receiver_.reset();
            endpoints_->sender_.reset();
        }
    }

    teardown_endpoints* endpoints_;
};

// Receiver retirement destroys buffered payloads whose destructors reenter the
// channel: closing the sender must not deadlock on the queue lock, and dropping
// every endpoint must not free the state while the drain is still running.
bool test_retired_payload_destructors_can_tear_down_the_channel() {
    bool success = true;
    for (const bool release_endpoints : {false, true}) {
        asio::io_context io_context;
        auto attachment = attach_event_loop(io_context, {.queue_capacity_ = 8});
        {
            teardown_endpoints endpoints;
            endpoints.release_endpoints_ = release_endpoints;
            auto [sender, receiver] =
                make_channel<teardown_payload>(attachment.loop().worker(), {.capacity_ = 2});
            endpoints.sender_ = std::make_unique<channel_sender<teardown_payload>>(std::move(sender));
            endpoints.receiver_ = std::make_unique<channel_receiver<teardown_payload>>(std::move(receiver));
            success = endpoints.sender_->send(teardown_payload(endpoints)).accepted() &&
                      endpoints.sender_->send(teardown_payload(endpoints)).accepted() && success;
            endpoints.armed_ = true;
            endpoints.receiver_->close();
            success = endpoints.armed_destructions_ == 2 && success;
            endpoints.armed_ = false;
            if (release_endpoints) {
                success = !endpoints.sender_ && !endpoints.receiver_ && success;
            } else {
                success = endpoints.sender_->send(teardown_payload(endpoints)).status() == channel_send_status::closed &&
                          success;
            }
        }
    }
    return success;
}

ruvia::task<void> exercise_blocking_pool(ruvia::worker_ref worker_value, bool& success) {
    {
        ruvia::stop_source source;
        auto [cancel_sender, cancel_receiver] = ruvia::make_channel<int>(worker_value, {.capacity_ = 1});
        std::binary_semaphore receiver_scheduled{0};
        std::thread stopper([&] {
            receiver_scheduled.acquire();
            source.request_stop();
        });
        static_cast<void>(worker_value.post([&receiver_scheduled] { receiver_scheduled.release(); }));
        const auto cancelled =
            co_await cancel_receiver.receive({.timeout_ = std::chrono::seconds(1), .stop_token_ = source.token()});
        stopper.join();
        if (cancelled.status() != ruvia::worker_wait_status::cancelled ||
            !cancel_sender.send(5).accepted()) {
            co_return;
        }
        const auto recovered = co_await cancel_receiver.receive();
        if (!recovered.has_value() || recovered.value() != 5) {
            co_return;
        }
    }

    {
        ruvia::stop_source source;
        source.request_stop();
        auto [cancel_sender, cancel_receiver] = ruvia::make_channel<int>(worker_value, {.capacity_ = 1});
        const auto cancelled = co_await cancel_receiver.receive({.stop_token_ = source.token()});
        if (cancelled.status() != ruvia::worker_wait_status::cancelled) {
            co_return;
        }
    }

    {
        ruvia::stop_source source;
        ruvia::stop_source next_source;
        auto [late_sender, late_receiver] = ruvia::make_channel<int>(worker_value, {.capacity_ = 1});
        bool generation_safe = false;
        ruvia::task_scope scope(worker_value);
        scope.spawn(receive_after_late_cancellation(
            late_receiver, source.token(), next_source.token(), generation_safe));
        if (!late_sender.send(1).accepted()) {
            late_receiver.close();
            co_await scope.join();
            co_return;
        }
        source.request_stop();
        static_cast<void>(worker_value.post([&late_sender] {
            if (!late_sender.send(2).accepted()) {
                std::terminate();
            }
        }));
        co_await scope.join();
        if (!generation_safe) {
            co_return;
        }
    }

    {
        auto [sender, receiver] = ruvia::make_channel<int>(worker_value, {.capacity_ = 1});
        int delivered = 0;
        ruvia::task_scope scope(worker_value);
        scope.spawn(receive_one(receiver, delivered));
        const bool first_sent = sender.send(1).accepted();
        const bool second_sent = sender.send(2).accepted();
        const auto full = sender.send(3);
        sender.close();
        co_await scope.join();
        const auto buffered = co_await receiver.receive();
        if (!first_sent || !second_sent || full.status() != ruvia::channel_send_status::full ||
            delivered != 1 || !buffered.has_value() || buffered.value() != 2) {
            co_return;
        }
    }

    {
        ruvia::stop_source source;
        auto [same_stack_sender, same_stack_receiver] =
            ruvia::make_channel<int>(worker_value, {.capacity_ = 1});
        bool first_cancelled = false;
        ruvia::task_scope scope(worker_value);
        scope.spawn([](ruvia::channel_receiver<int>& rx, ruvia::stop_token token, bool& cancelled) -> ruvia::task<void> {
            const auto outcome = co_await rx.receive({.stop_token_ = std::move(token)});
            cancelled = outcome.status() == ruvia::worker_wait_status::cancelled;
        }(same_stack_receiver, source.token(), first_cancelled));
        source.request_stop();
        if (!same_stack_sender.send(77).accepted()) {
            same_stack_receiver.close();
            co_await scope.join();
            co_return;
        }
        co_await scope.join();
        const auto queued = co_await same_stack_receiver.receive();
        if (!first_cancelled || !queued.has_value() || queued.value() != 77) {
            co_return;
        }
    }
    {
        auto [close_sender, close_receiver] = ruvia::make_channel<int>(worker_value, {.capacity_ = 1});
        bool pending_closed = false;
        ruvia::task_scope scope(worker_value);
        scope.spawn(receive_until_closed(close_receiver, pending_closed));
        bool duplicate_rejected = false;
        try {
            static_cast<void>(co_await close_receiver.receive());
        } catch (const std::logic_error&) {
            duplicate_rejected = true;
        }
        close_receiver.close();
        co_await scope.join();
        if (!pending_closed || !duplicate_rejected ||
            close_sender.send(7).status() != ruvia::channel_send_status::closed) {
            co_return;
        }
    }

    auto [sender, receiver] = ruvia::make_channel<int>(worker_value, {.capacity_ = 2});
    auto active_receiver = std::move(receiver);
    const auto send1 = sender.send(1);
    const auto send2 = sender.send(2);
    const auto send3 = sender.send(99);
    if (!send1.accepted() || !send2.accepted() ||
        send3.status() != ruvia::channel_send_status::full || send3.rejected() == nullptr ||
        *send3.rejected() != 99) {
        co_return;
    }

    const auto first = co_await active_receiver.receive();
    const auto second = co_await active_receiver.receive();
    if (!first.has_value() || first.value() != 1 || !second.has_value() || second.value() != 2) {
        co_return;
    }
    const auto timeout = co_await active_receiver.receive({.timeout_ = std::chrono::milliseconds(1)});
    if (timeout.status() != ruvia::worker_wait_status::timed_out) {
        co_return;
    }
    const auto passed_deadline =
        co_await active_receiver.receive({.timeout_ = ruvia::operation_timeout(std::chrono::steady_clock::time_point{})});
    const auto deadline = co_await active_receiver.receive(
        {.timeout_ = ruvia::operation_timeout(std::chrono::steady_clock::now() + std::chrono::milliseconds(1))});
    if (passed_deadline.status() != ruvia::worker_wait_status::timed_out ||
        deadline.status() != ruvia::worker_wait_status::timed_out) {
        co_return;
    }
    try {
        static_cast<void>(active_receiver.receive({.timeout_ = std::chrono::milliseconds(-1)}));
        co_return;
    } catch (const std::invalid_argument&) {
    }

    ruvia::task_scope scope(worker_value);
    scope.spawn(receive_last(active_receiver, success));
    const bool sent = sender.send(3).accepted();
    sender.close();
    co_await scope.join();
    success = success && sent;
    const auto closed_send = sender.send(4);
    if (closed_send.status() != ruvia::channel_send_status::closed ||
        closed_send.rejected() == nullptr || *closed_send.rejected() != 4) {
        success = false;
    }
}

class bounded_reentrant_close final {
public:
    explicit bounded_reentrant_close(std::function<void()> close)
        : close_(std::move(close)),
          closer_([this] {
              requested_.acquire();
              close_();
              completed_.release();
          }) {}

    ~bounded_reentrant_close() {
        if (!requested_callback_) {
            requested_.release();
        }
        closer_.join();
    }

    void invoke() {
        requested_callback_ = true;
        requested_.release();
        completed_during_callback_ = completed_.try_acquire_for(std::chrono::milliseconds(300));
        if (completed_during_callback_) {
            close_();
        }
    }

    [[nodiscard]] bool completed_during_callback() const noexcept {
        return completed_during_callback_;
    }

private:
    std::function<void()> close_;
    std::binary_semaphore requested_{0};
    std::binary_semaphore completed_{0};
    bool requested_callback_{false};
    bool completed_during_callback_{false};
    std::thread closer_;
};

struct reentrant_payload final {
    int value_;
    std::function<void()>* on_move_;
    std::function<void()>* on_destroy_;

    reentrant_payload(int value, std::function<void()>& on_move, std::function<void()>& on_destroy)
        : value_(value),
          on_move_(&on_move),
          on_destroy_(&on_destroy) {}
    reentrant_payload(const reentrant_payload&) = delete;
    reentrant_payload(reentrant_payload&& other)
        : value_(std::exchange(other.value_, 0)),
          on_move_(other.on_move_),
          on_destroy_(other.on_destroy_) {
        if (*on_move_) {
            auto callback = std::move(*on_move_);
            *on_move_ = {};
            callback();
        }
    }
    ~reentrant_payload() {
        if (*on_destroy_) {
            auto callback = std::move(*on_destroy_);
            *on_destroy_ = {};
            callback();
        }
    }
};

ruvia::task<void> verify_reentrant_receive(
    ruvia::channel_receiver<reentrant_payload>& receiver, bool& success) {
    const auto first = co_await receiver.receive();
    const auto second = co_await receiver.receive();
    const auto closed = co_await receiver.receive();
    success = first.has_value() && first.value().value_ == 1 &&
              second.has_value() && second.value().value_ == 2 &&
              closed.status() == ruvia::worker_wait_status::closed;
}

bool test_reentrant_payload_moves_and_destruction() {
    asio::io_context io;
    auto attachment = ruvia::attach_event_loop(io, {.queue_capacity_ = 8});
    bool success = true;
    {
        auto [sender, receiver] = ruvia::make_channel<reentrant_payload>(attachment.loop().worker(), {.capacity_ = 1});
        bounded_reentrant_close move_close([&] { sender.close(); });
        std::function<void()> on_move = [&] { move_close.invoke(); };
        std::function<void()> on_destroy;
        const auto result = sender.send(reentrant_payload(42, on_move, on_destroy));
        success = result.status() == ruvia::channel_send_status::closed &&
                  result.rejected() != nullptr && result.rejected()->value_ == 42 &&
                  move_close.completed_during_callback();
        receiver.close();
    }
    {
        auto [sender, receiver] = ruvia::make_channel<reentrant_payload>(attachment.loop().worker(), {.capacity_ = 2});
        bounded_reentrant_close move_close([&] { sender.close(); });
        bounded_reentrant_close destroy_close([&] { sender.close(); });
        std::function<void()> on_move;
        std::function<void()> on_destroy;
        success = sender.send(reentrant_payload(1, on_move, on_destroy)).accepted() && success;
        success = sender.send(reentrant_payload(2, on_move, on_destroy)).accepted() && success;
        on_move = [&] { move_close.invoke(); };
        on_destroy = [&] { destroy_close.invoke(); };
        bool received = false;
        ruvia::test::run_worker_tasks(attachment, verify_reentrant_receive(receiver, received));
        success = received && !on_move && !on_destroy && move_close.completed_during_callback() &&
                  destroy_close.completed_during_callback() && success;
    }
    return success;
}

ruvia::task<void> verify_commit_order(
    ruvia::channel_receiver<reentrant_payload>& receiver, bool& success) {
    const auto first = co_await receiver.receive();
    const auto second = co_await receiver.receive();
    success = first.has_value() && first.value().value_ == 2 &&
              second.has_value() && second.value().value_ == 1;
}

bool test_send_reservation_does_not_block_other_producers() {
    asio::io_context io;
    auto attachment = ruvia::attach_event_loop(io, {.queue_capacity_ = 8});
    auto [sender, receiver] = ruvia::make_channel<reentrant_payload>(attachment.loop().worker(), {.capacity_ = 2});
    std::binary_semaphore entered{0};
    std::binary_semaphore proceed{0};
    std::binary_semaphore second_completed{0};
    std::function<void()> first_move = [&] {
        entered.release();
        proceed.acquire();
    };
    std::function<void()> no_move;
    std::function<void()> no_destroy;
    bool first_sent = false;
    std::thread producer([&] {
        first_sent = sender.send(reentrant_payload(1, first_move, no_destroy)).accepted();
    });
    entered.acquire();
    bool second_sent = false;
    std::thread second_producer([&] {
        second_sent = sender.send(reentrant_payload(2, no_move, no_destroy)).accepted();
        second_completed.release();
    });
    const bool concurrent = second_completed.try_acquire_for(std::chrono::milliseconds(300));
    proceed.release();
    producer.join();
    second_producer.join();
    sender.close();
    bool ordered = false;
    ruvia::test::run_worker_tasks(attachment, verify_commit_order(receiver, ordered));
    receiver.close();
    return first_sent && second_sent && concurrent && ordered;
}

ruvia::task<void> verify_send_wait_admission(ruvia::worker_ref worker_value, bool& success) {
    auto [sender, receiver] = ruvia::make_channel<int>(worker_value, {.capacity_ = 1});
    if (!(co_await sender.send_wait(1)).accepted() || sender.send(9).status() != ruvia::channel_send_status::full) {
        co_return;
    }
    // Waiting senders take released slots in FIFO order.
    auto second = ruvia::channel_send_status::full;
    auto third = ruvia::channel_send_status::full;
    ruvia::task_scope scope(worker_value);
    scope.spawn([](ruvia::channel_sender<int> s, ruvia::channel_send_status& out) -> ruvia::task<void> {
        out = (co_await s.send_wait(2)).status();
    }(sender, second));
    scope.spawn([](ruvia::channel_sender<int> s, ruvia::channel_send_status& out) -> ruvia::task<void> {
        out = (co_await s.send_wait(3)).status();
    }(sender, third));
    int values[3]{};
    for (auto& value : values) {
        auto received = co_await receiver.receive({.timeout_ = std::chrono::seconds(30)});
        if (!received.has_value()) {
            co_return;
        }
        value = received.value();
    }
    co_await scope.join();
    success = values[0] == 1 && values[1] == 2 && values[2] == 3 && second == ruvia::channel_send_status::sent &&
              third == ruvia::channel_send_status::sent;
}

ruvia::task<void> verify_send_wait_returns_undelivered_values(ruvia::worker_ref worker_value, bool& success) {
    using value_type = std::unique_ptr<int>;
    using ruvia::channel_send_status;
    const auto returned = [](ruvia::channel_send_result<value_type>& result, channel_send_status status, int expected) {
        if (result.status() != status || result.rejected() == nullptr) {
            return false;
        }
        const auto value = std::move(result).take_rejected();
        return value != nullptr && *value == expected;
    };
    auto [sender, receiver] = ruvia::make_channel<value_type>(worker_value, {.capacity_ = 1});
    if (!sender.send(std::make_unique<int>(0)).accepted()) {
        co_return;
    }
    auto timed = co_await sender.send_wait(std::make_unique<int>(1), {.timeout_ = std::chrono::milliseconds(5)});
    auto passed = co_await sender.send_wait(std::make_unique<int>(2),
        {.timeout_ = ruvia::operation_timeout(std::chrono::steady_clock::time_point{})});
    ruvia::stop_source stopped;
    stopped.request_stop();
    auto already_cancelled = co_await sender.send_wait(std::make_unique<int>(3), {.stop_token_ = stopped.token()});
    ruvia::stop_source later;
    static_cast<void>(worker_value.post([&later] { later.request_stop(); }));
    auto cancelled = co_await sender.send_wait(std::make_unique<int>(4), {.stop_token_ = later.token()});
    const ruvia::channel_sender<value_type> unbound_sender;
    auto unbound = co_await unbound_sender.send_wait(std::make_unique<int>(5));
    static_cast<void>(worker_value.post([&sender] { sender.close(); }));
    auto closed = co_await sender.send_wait(std::make_unique<int>(6));
    // The buffered value still drains after closure.
    auto drained = co_await receiver.receive();
    success = returned(timed, channel_send_status::timed_out, 1) && returned(passed, channel_send_status::timed_out, 2) &&
              returned(already_cancelled, channel_send_status::cancelled, 3) &&
              returned(cancelled, channel_send_status::cancelled, 4) &&
              returned(unbound, channel_send_status::closed, 5) && returned(closed, channel_send_status::closed, 6) &&
              drained.has_value() && *drained.value() == 0;
}

ruvia::task<void> verify_send_wait_observes_worker_stop(ruvia::event_loop_attachment& attachment,
    ruvia::worker_ref worker_value, bool& success) {
    auto [sender, receiver] = ruvia::make_channel<int>(worker_value, {.capacity_ = 1});
    if (!sender.send(0).accepted()) {
        co_return;
    }
    static_cast<void>(worker_value.post([&attachment] { attachment.stop(); }));
    auto stopping = co_await sender.send_wait(7);
    success = stopping.status() == ruvia::channel_send_status::worker_stopping && stopping.rejected() != nullptr &&
              *stopping.rejected() == 7;
}

}  // namespace ruvia::test::channel_tests

TEST(Ruvia, ChannelLifecycle) {
    using namespace ruvia::test::channel_tests;
    bool success = false;
    bool cold_receive_operations_safe = false;
    bool buffered_cold_receive_operations_safe = false;
    bool channel_side_closure = false;
    {
        asio::io_context io_context;
        auto attachment = ruvia::attach_event_loop(io_context, {.queue_capacity_ = 8});
        const auto worker_value = attachment.loop().worker();
        auto cold_receive = make_cold_receive_after_receiver_close(worker_value, false);
        auto timed_cold_receive = make_cold_receive_after_receiver_close(worker_value, true);
        auto buffered_cold_receive = make_cold_receive_after_receiver_close(worker_value, false, true);
        auto buffered_timed_cold_receive = make_cold_receive_after_receiver_close(worker_value, true, true);
        ruvia::test::run_worker_tasks(attachment, exercise_blocking_pool(worker_value, success),
            verify_cold_receive_operations(std::move(cold_receive), std::move(timed_cold_receive),
                cold_receive_operations_safe),
            verify_cold_receive_operations(std::move(buffered_cold_receive), std::move(buffered_timed_cold_receive),
                buffered_cold_receive_operations_safe),
            verify_channel_side_closure(worker_value, channel_side_closure));
    }

    bool retired_receiver_stopping = false;
    {
        asio::io_context io_context;
        auto attachment = ruvia::attach_event_loop(io_context, {.queue_capacity_ = 8});
        ruvia::test::run_worker_tasks(attachment, verify_receiver_retirement_after_worker_stop(attachment, retired_receiver_stopping));
    }

    bool worker_stopping = false;
    bool stopping_send = false;
    {
        asio::io_context io_context;
        auto attachment = ruvia::attach_event_loop(io_context, {.queue_capacity_ = 8});
        const auto worker_value = attachment.loop().worker();
        auto [sender, receiver] = ruvia::make_channel<int>(worker_value, {.capacity_ = 1});
        ruvia::channel_sender<int> empty_sender;
        ASSERT_FALSE(empty_sender.valid());
        ASSERT_TRUE(sender.valid());
        ASSERT_TRUE(receiver.valid());
        auto [cap_sender, cap_receiver] = ruvia::make_channel<int>(worker_value, {.capacity_ = 2});
        ASSERT_TRUE(cap_sender.valid());
        ASSERT_TRUE(cap_receiver.valid());
        cap_receiver.close();
        ASSERT_TRUE(sender.send(9).accepted());
        ruvia::test::run_worker_tasks(attachment, receive_queued_then_stopping(attachment, receiver, worker_stopping));
        sender.close();
        const auto stopping_result = sender.send(10);
        stopping_send = stopping_result.status() == ruvia::channel_send_status::worker_stopping &&
                        stopping_result.rejected() != nullptr && *stopping_result.rejected() == 10;
    }

    bool move_failed = false;
    bool recovered_value = false;
    bool recovered_send = false;
    {
        asio::io_context io_context;
        auto attachment = ruvia::attach_event_loop(io_context, {.queue_capacity_ = 8});
        const auto worker_value = attachment.loop().worker();
        auto [sender, receiver] = ruvia::make_channel<blocking_throwing_move>(worker_value, {.capacity_ = 1});
        std::binary_semaphore receiver_scheduled{0};
        asio::post(io_context, [&receiver_scheduled] { receiver_scheduled.release(); });
        std::thread sending_thread([&] {
            receiver_scheduled.acquire();
            blocking_throwing_move::throw_on_move_.store(true, std::memory_order_relaxed);
            try {
                static_cast<void>(sender.send(blocking_throwing_move(5)));
            } catch (const std::runtime_error&) {
                move_failed = true;
            }
            blocking_throwing_move::throw_on_move_.store(false, std::memory_order_relaxed);
            recovered_send = sender.send(blocking_throwing_move(6)).accepted();
        });
        ruvia::test::run_worker_tasks(attachment, receive_throwing_move(receiver, recovered_value));
        sending_thread.join();
        sender.close();
    }

    bool failed_receive_move_releases_slot = false;
    {
        asio::io_context io_context;
        auto attachment = ruvia::attach_event_loop(io_context, {.queue_capacity_ = 8});
        auto [sender, receiver] = ruvia::make_channel<blocking_throwing_move>(attachment.loop().worker(), {.capacity_ = 2});
        ASSERT_TRUE(sender.send(blocking_throwing_move(1)).accepted());
        ASSERT_TRUE(sender.send(blocking_throwing_move(2)).accepted());
        ruvia::test::run_worker_tasks(
            attachment, receive_after_move_failure(std::move(sender), receiver, failed_receive_move_releases_slot));
    }

    EXPECT_TRUE(success);
    EXPECT_TRUE(cold_receive_operations_safe);
    EXPECT_TRUE(buffered_cold_receive_operations_safe);
    EXPECT_TRUE(channel_side_closure);
    EXPECT_TRUE(retired_receiver_stopping);
    EXPECT_TRUE(worker_stopping);
    EXPECT_TRUE(stopping_send);
    EXPECT_TRUE(move_failed);
    EXPECT_TRUE(recovered_value);
    EXPECT_TRUE(recovered_send);
    EXPECT_TRUE(failed_receive_move_releases_slot);
}

TEST(Ruvia, ChannelSendPinsStateDuringReentrantRebinding) {
    using namespace ruvia::test::channel_tests;
    for (const bool fail_move : {false, true}) {
        SCOPED_TRACE(fail_move);
        asio::io_context io;
        auto attachment = ruvia::attach_event_loop(io, {.queue_capacity_ = 8});
        auto [sender, receiver] = ruvia::make_channel<reentrant_payload>(
            attachment.loop().worker(), {.capacity_ = 1});
        std::optional<ruvia::channel_receiver<reentrant_payload>> owner(std::move(receiver));
        owner->close();
        bool state_pinned = false;
        std::function<void()> on_move = [&] {
            owner.reset();
            sender = {};
            state_pinned = !owner.has_value() && !sender.valid();
            if (fail_move) {
                throw std::runtime_error("requested move failure");
            }
        };
        std::function<void()> on_destroy;

        if (fail_move) {
            EXPECT_THROW(static_cast<void>(sender.send(reentrant_payload(42, on_move, on_destroy))),
                std::runtime_error);
        } else {
            const auto result = sender.send(reentrant_payload(42, on_move, on_destroy));
            EXPECT_EQ(result.status(), ruvia::channel_send_status::closed);
            ASSERT_NE(result.rejected(), nullptr);
            EXPECT_EQ(result.rejected()->value_, 42);
        }
        EXPECT_TRUE(state_pinned);
        EXPECT_FALSE(sender.valid());
    }
}

TEST(Ruvia, ChannelRejectedPayloadMovesCanCloseChannel) {
    EXPECT_TRUE(ruvia::test::channel_tests::test_rejected_payload_moves_can_close_the_channel());
}

TEST(Ruvia, ChannelRetiredReceiverStorageIsReleased) {
    EXPECT_TRUE(ruvia::test::channel_tests::test_retired_receiver_storage_is_released());
}

TEST(Ruvia, ChannelRetiredPayloadDestructorsCanTearDownChannel) {
    EXPECT_TRUE(ruvia::test::channel_tests::test_retired_payload_destructors_can_tear_down_the_channel());
}

TEST(Ruvia, ChannelReentrantPayloadMovesAndDestruction) {
    EXPECT_TRUE(ruvia::test::channel_tests::test_reentrant_payload_moves_and_destruction());
}

TEST(Ruvia, ChannelSendReservationDoesNotBlockOtherProducers) {
    EXPECT_TRUE(ruvia::test::channel_tests::test_send_reservation_does_not_block_other_producers());
}

TEST(Ruvia, ChannelSendWaitWaitsForSpaceAndReturnsUndeliveredValues) {
    asio::io_context io_context;
    auto attachment = ruvia::attach_event_loop(io_context, {.queue_capacity_ = 16});
    const auto worker_value = attachment.loop().worker();
    {
        // Off a worker thread, awaiting is rejected before anything is queued.
        auto [sender, receiver] = ruvia::make_channel<int>(worker_value, {.capacity_ = 1});
        auto operation = sender.send_wait(1);
        EXPECT_THROW(static_cast<void>(operation.await_ready()), std::logic_error);
    }
    bool admission = false;
    bool undelivered = false;
    ruvia::test::run_worker_tasks(attachment,
        ruvia::test::channel_tests::verify_send_wait_admission(worker_value, admission),
        ruvia::test::channel_tests::verify_send_wait_returns_undelivered_values(worker_value, undelivered));
    EXPECT_TRUE(admission);
    EXPECT_TRUE(undelivered);
}

TEST(Ruvia, ChannelSendWaitObservesWorkerStop) {
    asio::io_context io_context;
    auto attachment = ruvia::attach_event_loop(io_context, {.queue_capacity_ = 16});
    const auto worker_value = attachment.loop().worker();
    bool success = false;
    ruvia::test::run_worker_tasks(
        attachment, ruvia::test::channel_tests::verify_send_wait_observes_worker_stop(attachment, worker_value, success));
    EXPECT_TRUE(success);
}

TEST(Ruvia, ChannelSendWaitResumesOnTheAwaitingWorker) {
    ruvia::event_loop_pool loops({.loop_count_ = 2, .queue_capacity_ = 16});
    const auto receiving = loops.loop(0);
    const auto sending = loops.loop(1);
    auto [sender, receiver] = ruvia::make_channel<int>(receiving.worker(), {.capacity_ = 1});
    loops.start();
    auto produced = sending.start([](ruvia::channel_sender<int> s, ruvia::worker_ref self) -> ruvia::task<bool> {
        bool on_sender = true;
        bool delivered = true;
        for (int value = 1; value <= 3; ++value) {
            delivered = delivered && (co_await s.send_wait(value)).accepted();
            on_sender = on_sender && self.is_current();
        }
        const bool resumed_on_sender = delivered && on_sender;
        co_return resumed_on_sender;
    }(sender, sending.worker()));
    auto consumed = receiving.start([](ruvia::channel_receiver<int>& r) -> ruvia::task<int> {
        int sum = 0;
        for (int i = 0; i < 3; ++i) {
            auto received = co_await r.receive({.timeout_ = std::chrono::seconds(30)});
            sum += received.has_value() ? received.value() : 100;
        }
        co_return sum;
    }(receiver));
    EXPECT_TRUE(std::move(produced).get());
    EXPECT_EQ(std::move(consumed).get(), 6);
    loops.stop();
    loops.join();
}
