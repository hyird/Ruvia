#include "ruvia/core/pool_lease_scheduler.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <initializer_list>
#include <memory_resource>
#include <optional>
#include <utility>

#include <asio/io_context.hpp>
#include <asio/post.hpp>

#include "ruvia/core/event_loop.h"
#include "ruvia/core/task_scope.h"

#include "worker_task_fixture.h"

namespace {

class counting_memory_resource final : public std::pmr::memory_resource {
public:
    std::size_t allocations_{};
    std::size_t deallocations_{};
    std::size_t live_blocks_{};

private:
    void* do_allocate(std::size_t bytes_value, std::size_t alignment) override {
        void* const block = std::pmr::new_delete_resource()->allocate(bytes_value, alignment);
        ++allocations_;
        ++live_blocks_;
        return block;
    }

    void do_deallocate(void* block, std::size_t bytes_value, std::size_t alignment) override {
        ++deallocations_;
        --live_blocks_;
        std::pmr::new_delete_resource()->deallocate(block, bytes_value, alignment);
    }

    [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
};

ruvia::task<void> exercise_lease_and_close(
    ruvia::pool_lease_scheduler& scheduler, asio::io_context& io_context, bool& success) {
    {
        auto discarded_cold_acquire = scheduler.acquire(std::nullopt);
        static_cast<void>(discarded_cold_acquire);
    }
    const auto first = co_await scheduler.acquire();
    if (!first.has_value()) {
        co_return;
    }
    const auto index = first.value();
    scheduler.release(index);

    const auto reacquired = co_await scheduler.acquire(std::nullopt);
    if (!reacquired.has_value() || reacquired.value() != index) {
        co_return;
    }

    bool handoff_released = false;
    asio::post(io_context, [&scheduler, &handoff_released, index] {
        scheduler.release(index);
        handoff_released = true;
    });
    const auto handed_off = co_await scheduler.acquire(std::nullopt);
    if (!handed_off.has_value() || handed_off.value() != index) {
        co_return;
    }

    asio::post(io_context, [&scheduler] { (void)scheduler.close(); });
    const auto waiting_at_close = co_await scheduler.acquire(std::nullopt);
    if (!handoff_released ||
        waiting_at_close.status() != ruvia::worker_wait_status::closed ||
        !scheduler.closing() || scheduler.close()) {
        co_return;
    }
    scheduler.release(index);
    const auto after_close = co_await scheduler.acquire(std::nullopt);
    success = after_close.status() == ruvia::worker_wait_status::closed;
    auto moved_acquire = scheduler.acquire(std::nullopt);
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
    const auto zero_timeout = co_await scheduler.acquire(std::chrono::milliseconds(0));
    const auto negative_timeout = co_await scheduler.acquire(std::chrono::milliseconds(-5));
    const auto result_value = co_await scheduler.acquire(std::chrono::milliseconds(1));
    success = zero_timeout.status() == ruvia::worker_wait_status::timed_out &&
              negative_timeout.status() == ruvia::worker_wait_status::timed_out &&
              result_value.status() == ruvia::worker_wait_status::timed_out;
}

ruvia::task<void> exercise_saturated_acquire_timeout(
    ruvia::pool_lease_scheduler& scheduler, asio::io_context& io_context, bool& success) {
    asio::post(io_context, [&scheduler] { (void)scheduler.close(); });
    const auto result_value = co_await scheduler.acquire(std::chrono::milliseconds::max());
    // A maximum positive timeout is effectively unbounded: its worker timer
    // must not wrap into the past and time out before the close notification.
    success = result_value.status() == ruvia::worker_wait_status::closed;
}

ruvia::task<void> exercise_acquire_cancellation(ruvia::pool_lease_scheduler& scheduler,
    asio::io_context& io_context, const ruvia::worker_handle& worker_value, bool& success) {
    ruvia::stop_source source;
    asio::post(io_context, [&scheduler, &source] {
        source.request_stop();
        (void)scheduler.close();
    });
    const auto result_value = co_await scheduler.acquire(std::nullopt, source.token());
    // Cancellation is committed before request_stop() returns. A same-stack
    // close must not replace it with closed while resumption is deferred.
    success = result_value.status() == ruvia::worker_wait_status::cancelled;
}

ruvia::task<void> observe_waiter(ruvia::pool_lease_scheduler& scheduler,
    std::optional<std::chrono::milliseconds> timeout, ruvia::stop_token token,
    std::optional<ruvia::worker_wait_status>& status) {
    const auto result = co_await scheduler.acquire(timeout, std::move(token));
    status = result.status();
    if (result.has_value()) {
        scheduler.release(result.value());
    }
}

ruvia::task<void> expire_then_release(ruvia::pool_lease_scheduler& scheduler, std::size_t slot,
    ruvia::stop_source& cancellation, std::optional<ruvia::worker_wait_status>& status) {
    const auto result = co_await scheduler.acquire(std::chrono::milliseconds(1));
    status = result.status();
    cancellation.request_stop();
    scheduler.release(slot);
}

ruvia::task<void> exercise_timeout_between_cancel_and_release(
    const ruvia::worker_handle& worker, bool& success) {
    ruvia::pool_lease_scheduler scheduler(1, worker);
    const auto held = co_await scheduler.acquire(std::nullopt);
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

ruvia::task<void> exercise_nonpositive_timeout_does_not_steal_same_turn_release(
    const ruvia::worker_handle& worker_value, bool& success) {
    ruvia::pool_lease_scheduler scheduler(1, worker_value);
    const auto first = co_await scheduler.acquire(std::chrono::milliseconds(0));
    if (!first.has_value()) {
        co_return;
    }
    bool zero_timed_out = false;
    bool negative_timed_out = false;
    ruvia::task_scope scope(worker_value);
    scope.spawn([&]() -> ruvia::task<void> {
        const auto outcome = co_await scheduler.acquire(std::chrono::milliseconds(0));
        zero_timed_out = outcome.status() == ruvia::worker_wait_status::timed_out;
    }());
    scope.spawn([&]() -> ruvia::task<void> {
        const auto outcome = co_await scheduler.acquire(std::chrono::milliseconds(-1));
        negative_timed_out = outcome.status() == ruvia::worker_wait_status::timed_out;
    }());
    scheduler.release(first.value());
    co_await scope.join();
    const auto reacquired = co_await scheduler.acquire(std::chrono::milliseconds(0));
    if (!reacquired.has_value() || reacquired.value() != first.value()) {
        co_return;
    }
    scheduler.release(reacquired.value());
    success = zero_timed_out && negative_timed_out;
}
ruvia::task<void> exercise_pmr_lifecycle(ruvia::pool_lease_scheduler& scheduler,
    asio::io_context& io_context, const ruvia::worker_handle& worker_value,
    counting_memory_resource& resource, std::size_t baseline_blocks,
    std::optional<ruvia::worker_wait_result<std::size_t>>& retained_result, bool& success) {
    for (std::size_t i = 0; i < 32; ++i) {
        {
            auto discarded_cold_acquire = scheduler.acquire(std::nullopt);
            static_cast<void>(discarded_cold_acquire);
        }
        if (resource.live_blocks_ != baseline_blocks) {
            co_return;
        }
    }

    const auto held = co_await scheduler.acquire(std::nullopt);
    if (!held.has_value()) {
        co_return;
    }
    for (std::size_t i = 0; i < 32; ++i) {
        scheduler.release(held.value());
        const auto acquired = co_await scheduler.acquire(std::nullopt);
        if (!acquired.has_value() || acquired.value() != held.value() ||
            resource.live_blocks_ != baseline_blocks) {
            co_return;
        }
        retained_result.emplace(acquired);
    }

    for (std::size_t i = 0; i < 32; ++i) {
        const auto timed_out = co_await scheduler.acquire(std::chrono::milliseconds(1));
        if (timed_out.status() != ruvia::worker_wait_status::timed_out || resource.live_blocks_ != baseline_blocks) {
            co_return;
        }

        ruvia::stop_source source;
        asio::post(io_context, [&source] { source.request_stop(); });
        const auto cancelled = co_await scheduler.acquire(std::nullopt, source.token());
        if (cancelled.status() != ruvia::worker_wait_status::cancelled || resource.live_blocks_ != baseline_blocks) {
            co_return;
        }
    }

    scheduler.release(held.value());
    success = resource.live_blocks_ == baseline_blocks && retained_result.has_value() &&
              retained_result->has_value() && retained_result->value() == held.value();
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
            ruvia::pool_lease_scheduler scheduler(0, attachment.loop().handle());
            std::optional<ruvia::pool_lease_acquire> pending;
            {
                std::optional<std::chrono::milliseconds> timeout = std::chrono::milliseconds(1);
                if (with_stop_token) {
                    pending.emplace(scheduler.acquire(timeout, {}));
                } else {
                    pending.emplace(scheduler.acquire(timeout));
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
    const ruvia::worker_handle& worker_value, ruvia::stop_source& source, bool& cancelled) {
    static_cast<void>(worker_value.post([&source] { source.request_stop(); }));
    const auto result_value = co_await std::move(pending);
    cancelled = result_value.status() == ruvia::worker_wait_status::cancelled;
}

bool test_scheduler_retains_worker_binding_for_lazy_acquires() {
    asio::io_context context;
    auto attachment = ruvia::attach_event_loop(context, {.queue_capacity_ = 8});
    const auto worker_value = attachment.loop().handle();
    counting_memory_resource memory;
    bool success = true;
    {
        std::optional<ruvia::pool_lease_scheduler> scheduler;
        std::optional<ruvia::pool_lease_acquire> pending;
        ruvia::stop_source source;
        {
            auto temporary_worker = worker_value;
            scheduler.emplace(0, temporary_worker, &memory);
            pending.emplace(scheduler->acquire(std::nullopt, source.token()));
        }
        const auto baseline = memory.live_blocks_;
        // An unstarted operation releases its reservation before its scheduler.
        // The scheduler needs a live worker, so this runs before the loop retires.
        {
            ruvia::pool_lease_scheduler temporary_scheduler(1, worker_value, &memory);
            auto discarded = temporary_scheduler.acquire(std::nullopt, ruvia::stop_source{}.token());
        }
        success = memory.live_blocks_ == baseline;
        bool cancelled = false;
        auto prepared = std::move(*pending);
        pending.reset();
        ruvia::test::run_worker_tasks(
            attachment, observe_worker_acquire(std::move(prepared), worker_value, source, cancelled));
        success = success && cancelled && memory.live_blocks_ == baseline;
    }
    return success && memory.live_blocks_ == 0 && memory.allocations_ == memory.deallocations_;
}

ruvia::task<void> exercise_pmr_test(asio::io_context& io_context,
    const ruvia::worker_handle& worker_value, bool& success) {
    counting_memory_resource resource;
    std::optional<ruvia::worker_wait_result<std::size_t>> retained_result;
    {
        ruvia::pool_lease_scheduler scheduler(1, worker_value, &resource);
        const auto baseline_blocks = resource.live_blocks_;
        co_await exercise_pmr_lifecycle(scheduler, io_context, worker_value,
            resource, baseline_blocks, retained_result, success);
        success = success && resource.live_blocks_ == baseline_blocks;
    }
    success = success && resource.live_blocks_ == 0 &&
              resource.allocations_ == resource.deallocations_;
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
    const auto result = co_await scheduler.acquire(std::nullopt);
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

task<void> exercise_long_chain(const worker_handle& worker, bool& success) {
    constexpr std::size_t waiter_count = 512;
    pool_lease_scheduler scheduler(1, worker);
    task_scope scope(worker);
    handoff_state state;
    const auto held = co_await scheduler.acquire(std::nullopt);
    for (std::size_t index = 0; index != waiter_count; ++index) {
        scope.spawn(release_immediately(scheduler, state, index));
    }
    scheduler.release(held.value());
    co_await scope.join();
    success = state.peak_ == 1 && state.depth_ == 0 &&
              state.completed_ == waiter_count && state.ordered_;
}

task<void> exercise_committed_handoff(const worker_handle& worker, bool& success) {
    pool_lease_scheduler scheduler(1, worker);
    task_scope scope(worker);
    stop_source cancellation;
    std::optional<worker_wait_status> handed_off;
    std::optional<worker_wait_status> closed;
    const auto held = co_await scheduler.acquire(std::nullopt);
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

}  // namespace ruvia::test::lease_handoff

int main() {
    asio::io_context io_context;
    auto attachment = ruvia::attach_event_loop(io_context, {.queue_capacity_ = 8});
    const auto worker_value = attachment.loop().handle();
    ruvia::pool_lease_scheduler lease_scheduler(1, worker_value);
    ruvia::pool_lease_scheduler saturated_timeout_scheduler(0, worker_value);
    ruvia::pool_lease_scheduler worker_timeout_scheduler(0, worker_value);
    ruvia::pool_lease_scheduler cancellation_scheduler(0, worker_value);
    bool lease_success = false;
    bool worker_timeout_success = false;
    bool nonpositive_timeout_success = false;
    bool saturated_timeout_success = false;
    bool cancellation_success = false;
    bool pmr_lifecycle_success = false;
    bool mixed_waiters_success = false;
    bool handoff_chain_success = false;
    bool committed_handoff_success = false;
    ruvia::test::run_worker_tasks(attachment,
        ruvia::test::lease_handoff::exercise_long_chain(worker_value, handoff_chain_success),
        ruvia::test::lease_handoff::exercise_committed_handoff(worker_value, committed_handoff_success),
        exercise_lease_and_close(lease_scheduler, io_context, lease_success),
        exercise_worker_bound_acquire_timeout(worker_timeout_scheduler, worker_timeout_success),
        exercise_nonpositive_timeout_does_not_steal_same_turn_release(
            worker_value, nonpositive_timeout_success),
        exercise_saturated_acquire_timeout(saturated_timeout_scheduler, io_context, saturated_timeout_success),
        exercise_timeout_between_cancel_and_release(worker_value, mixed_waiters_success),
        exercise_acquire_cancellation(cancellation_scheduler, io_context, worker_value, cancellation_success),
        exercise_pmr_test(io_context, worker_value, pmr_lifecycle_success));
    return lease_success && worker_timeout_success && nonpositive_timeout_success &&
                   saturated_timeout_success &&
                   cancellation_success && pmr_lifecycle_success && mixed_waiters_success &&
                   handoff_chain_success && committed_handoff_success &&
                   test_scheduler_owns_timeout_for_lazy_acquires() &&
                   test_scheduler_retains_worker_binding_for_lazy_acquires()
               ? 0
               : 1;
}
