#include "ruvia/core/pool_lease_scheduler.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <initializer_list>
#include <optional>
#include <stdexcept>
#include <utility>

#include <asio/io_context.hpp>
#include <asio/post.hpp>

#include "ruvia/core/event_loop.h"
#include "ruvia/core/task_scope.h"

#include "worker_task_fixture.h"

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
