#include "ruvia/core/pool_lease_scheduler.h"

#include <chrono>
#include <cstddef>
#include <initializer_list>
#include <memory_resource>
#include <optional>
#include <utility>

#include <asio/io_context.hpp>
#include <asio/post.hpp>

#include "ruvia/core/task_scope.h"
#include "ruvia/core/worker_runtime_context.h"

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
    const auto first = co_await scheduler.acquire(std::nullopt);
    if (!first.has_value()) {
        co_return;
    }
    const auto index = first.value();
    if (scheduler.release(index) != ruvia::pool_lease_release_status::released ||
        scheduler.release(index) != ruvia::pool_lease_release_status::already_released ||
        scheduler.release(index + 1) != ruvia::pool_lease_release_status::invalid_slot) {
        co_return;
    }

    const auto reacquired = co_await scheduler.acquire(std::nullopt);
    if (!reacquired.has_value() || reacquired.value() != index) {
        co_return;
    }

    auto handoff_status = ruvia::pool_lease_release_status::invalid_slot;
    asio::post(io_context,
        [&scheduler, &handoff_status, index] { handoff_status = scheduler.release(index); });
    const auto handed_off = co_await scheduler.acquire(std::nullopt);
    if (!handed_off.has_value() || handed_off.value() != index) {
        co_return;
    }

    asio::post(io_context, [&scheduler] { (void)scheduler.close(); });
    const auto waiting_at_close = co_await scheduler.acquire(std::nullopt);
    if (handoff_status != ruvia::pool_lease_release_status::transferred_to_waiter ||
        waiting_at_close.status() != ruvia::worker_wait_status::closed ||
        !scheduler.closing() || scheduler.close()) {
        co_return;
    }
    if (scheduler.release(index) != ruvia::pool_lease_release_status::released) {
        co_return;
    }
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
    const auto result_value = co_await scheduler.acquire(std::chrono::milliseconds(1));
    success = result_value.status() == ruvia::worker_wait_status::timed_out;
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
        if (scheduler.release(held.value()) != ruvia::pool_lease_release_status::released) {
            co_return;
        }
        const auto acquired = co_await scheduler.acquire(std::nullopt);
        if (!acquired.has_value() || acquired.value() != held.value() ||
            resource.live_blocks_ != baseline_blocks) {
            co_return;
        }
        retained_result = acquired;
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

    success = scheduler.release(held.value()) == ruvia::pool_lease_release_status::released &&
              resource.live_blocks_ == baseline_blocks && retained_result.has_value() &&
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
        ruvia::worker_runtime_context runtime(context, 8);
        {
            ruvia::pool_lease_scheduler scheduler(0, runtime.handle());
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
            ruvia::test::run_worker_tasks(runtime, observe_timed_out_acquire(std::move(prepared), timed_out));
            success = success && timed_out;
            (void)scheduler.close();
        }
        runtime.detach();
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
    ruvia::worker_runtime_context runtime(context, 8);
    counting_memory_resource memory;
    bool success = true;
    {
        std::optional<ruvia::pool_lease_scheduler> scheduler;
        std::optional<ruvia::pool_lease_acquire> pending;
        ruvia::stop_source source;
        {
            auto temporary_worker = runtime.handle();
            scheduler.emplace(0, temporary_worker, &memory);
            pending.emplace(scheduler->acquire(std::nullopt, source.token()));
        }
        const auto baseline = memory.live_blocks_;
        bool cancelled = false;
        auto prepared = std::move(*pending);
        pending.reset();
        ruvia::test::run_worker_tasks(runtime,
            observe_worker_acquire(std::move(prepared), runtime.handle(), source, cancelled));
        success = cancelled && memory.live_blocks_ == baseline;
        // An unstarted operation releases its reservation before its scheduler.
        {
            ruvia::pool_lease_scheduler temporary_scheduler(1, runtime.handle(), &memory);
            auto discarded = temporary_scheduler.acquire(std::nullopt, ruvia::stop_source{}.token());
        }
        success = success && memory.live_blocks_ == baseline;
    }
    runtime.detach();
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

int main() {
    asio::io_context io_context;
    ruvia::worker_runtime_context runtime(io_context, 8);
    const auto worker_value = runtime.handle();
    ruvia::pool_lease_scheduler lease_scheduler(1, worker_value);
    ruvia::pool_lease_scheduler saturated_timeout_scheduler(0, worker_value);
    ruvia::pool_lease_scheduler worker_timeout_scheduler(0, worker_value);
    ruvia::pool_lease_scheduler cancellation_scheduler(0, worker_value);
    bool lease_success = false;
    bool worker_timeout_success = false;
    bool saturated_timeout_success = false;
    bool cancellation_success = false;
    bool pmr_lifecycle_success = false;
    ruvia::test::run_worker_tasks(runtime,
        exercise_lease_and_close(lease_scheduler, io_context, lease_success),
        exercise_worker_bound_acquire_timeout(worker_timeout_scheduler, worker_timeout_success),
        exercise_saturated_acquire_timeout(saturated_timeout_scheduler, io_context, saturated_timeout_success),
        exercise_acquire_cancellation(cancellation_scheduler, io_context, worker_value, cancellation_success),
        exercise_pmr_test(io_context, worker_value, pmr_lifecycle_success));
    runtime.detach();
    return lease_success && worker_timeout_success && saturated_timeout_success &&
                   cancellation_success && pmr_lifecycle_success &&
                   test_scheduler_owns_timeout_for_lazy_acquires() &&
                   test_scheduler_retains_worker_binding_for_lazy_acquires()
               ? 0
               : 1;
}
