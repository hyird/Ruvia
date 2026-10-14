#include "ruvia/core/task_scope.h"

#include <chrono>
#include <concepts>
#include <cstddef>
#include <functional>
#include <memory>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include <asio/io_context.hpp>

#include "ruvia/core/timer.h"
#include "ruvia/core/worker_runtime_context.h"

#include "failing_memory_resource.h"
#include "worker_task_fixture.h"

namespace ruvia::test::task_scope_tests {

class reentrant_memory_resource final : public std::pmr::memory_resource {
public:
    void before_next_allocation(std::function<void()> callback) {
        callback_ = std::move(callback);
    }

    [[nodiscard]] std::size_t live_allocations() const noexcept {
        return upstream_.live_allocations();
    }

private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        auto callback = std::exchange(callback_, {});
        if (callback) {
            callback();
        }
        return upstream_.allocate(bytes, alignment);
    }

    void do_deallocate(void* allocation, std::size_t bytes, std::size_t alignment) override {
        upstream_.deallocate(allocation, bytes, alignment);
    }

    [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }

    failing_memory_resource upstream_;
    std::function<void()> callback_;
};

ruvia::task<void> increment(ruvia::worker_handle worker_value, int& value) {
    static_cast<void>(co_await ruvia::sleep_for(worker_value, std::chrono::milliseconds(1)));
    ++value;
    co_return;
}

ruvia::task<void> fail() {
    throw std::runtime_error("child failed");
    co_return;
}

ruvia::task<void> no_op() {
    co_return;
}

ruvia::task<void> observe_join(ruvia::task<void> join, bool& completed) {
    co_await std::move(join);
    completed = true;
}

ruvia::task<void> record_child_start(std::pmr::string payload, int& starts) {
    ++starts;
    static_cast<void>(payload);
    co_return;
}

ruvia::task<void> verify_spawn_rechecks_join_after_allocation(
    ruvia::worker_handle worker_value, bool& success) {
    reentrant_memory_resource resource;
    ruvia::task_scope primary(worker_value, {.resource_ = &resource});
    ruvia::task_scope observer(worker_value);
    bool join_completed = false;
    int starts = 0;
    auto child = record_child_start(std::pmr::string(1024, 'x', &resource), starts);
    resource.before_next_allocation([&] {
        observer.spawn(observe_join(primary.join(), join_completed));
    });

    bool rejected = false;
    try {
        primary.spawn(std::move(child));
    } catch (const std::logic_error&) {
        rejected = true;
    }
    co_await observer.join();
    if (!rejected) {
        // A spawn that incorrectly reopened the joined scope still owns its
        // child. Retire it before reporting the failed admission check.
        co_await primary.join();
    }
    success = join_completed && rejected && starts == 0 &&
              primary.size() == 0 && resource.live_allocations() == 0;
}

ruvia::task<void> wait_for_stop(ruvia::worker_handle worker_value, ruvia::stop_token token, bool& stopped) {
    stopped = co_await ruvia::sleep_for(worker_value, std::chrono::hours(1), std::move(token)) == ruvia::timer_sleep_result::stop_requested;
}

ruvia::task<void> exercise(ruvia::worker_handle worker_value, bool& success) {
    {
        ruvia::task_scope empty_scope(worker_value);
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
        failing_memory_resource resource;
        int completed = 0;
        ruvia::task_scope cold_join_scope(worker_value, {.resource_ = &resource});
        cold_join_scope.spawn(increment(worker_value, completed));
        auto first_join = cold_join_scope.join();
        std::optional<ruvia::task<void>> second_join;
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
        if (!spawn_before_join_started || !repeated_start_rejected || completed != 2 || resource.live_allocations() != 0) {
            co_return;
        }
    }

    {
        failing_memory_resource resource;
        int completed = 0;
        ruvia::task_scope running_scope(worker_value, {.resource_ = &resource});
        auto first_join = running_scope.join();
        auto competing_join = running_scope.join();
        resource.fail_after(0);
        bool allocation_failed = false;
        try {
            running_scope.spawn(no_op());
        } catch (const std::bad_alloc&) {
            allocation_failed = true;
        }
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
        if (!allocation_failed || !join_completed || !competing_join_rejected || !spawn_during_join_rejected || completed != 1 || resource.live_allocations() != 0) {
            co_return;
        }
    }

    {
        failing_memory_resource resource;
        ruvia::task_scope cancelled_scope(worker_value, {.resource_ = &resource});
        {
            auto discarded_join = cancelled_scope.join();
            static_cast<void>(discarded_join);
        }
        bool stopped = false;
        cancelled_scope.spawn(wait_for_stop(worker_value, cancelled_scope.get_stop_token(), stopped));
        auto join = cancelled_scope.join();
        cancelled_scope.request_stop();
        co_await std::move(join);
        if (!stopped || !cancelled_scope.stop_requested() || cancelled_scope.size() != 0 || resource.live_allocations() != 0) {
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
            unjoined_scope.spawn(fail());
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
        struct unjoined_scope_holder final {
            explicit unjoined_scope_holder(const ruvia::worker_handle& w)
                : scope_(w) {}
            ruvia::task_scope scope_;
        };
        auto holder = std::make_shared<unjoined_scope_holder>(worker_value);
        auto child_with_holder = [](std::shared_ptr<unjoined_scope_holder> owned) -> ruvia::task<void> {
            static_cast<void>(owned);
            co_return;
        };
        holder->scope_.spawn(child_with_holder(holder));
        std::weak_ptr<unjoined_scope_holder> weak_holder = holder;
        holder.reset();
        while (!weak_holder.expired()) {
            static_cast<void>(co_await ruvia::sleep_for(worker_value, std::chrono::milliseconds(1)));
        }
    }

    {
        ruvia::task_scope completed_failure_scope(worker_value);
        completed_failure_scope.spawn(fail());
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
    failing_memory_resource resource;
    ruvia::task_scope scope(worker_value, {.resource_ = &resource});
    scope.spawn(increment(worker_value, calls));
    scope.spawn(fail());
    try {
        co_await scope.join();
    } catch (const std::runtime_error& error) {
        success = calls == 1 && scope.size() == 0 && scope.stop_requested() && resource.live_allocations() == 0 &&
                  std::string_view(error.what()) == "child failed";
    }
}

}  // namespace ruvia::test::task_scope_tests

int main() {
    ruvia::stop_token retained_token;
    {
        ruvia::stop_source source;
        retained_token = source.token();
        source.request_stop();
    }
    if (!retained_token.stop_requested()) {
        return 1;
    }

    asio::io_context io_context;
    ruvia::worker_runtime_context runtime(io_context, 8);
    const auto worker_value = runtime.handle();
    bool success = false;
    bool reentrant_spawn_safe = false;
    ruvia::test::run_worker_tasks(runtime,
        ruvia::test::task_scope_tests::exercise(worker_value, success),
        ruvia::test::task_scope_tests::verify_spawn_rechecks_join_after_allocation(worker_value, reentrant_spawn_safe));
    runtime.detach();
    return success && reentrant_spawn_safe ? 0 : 1;
}
