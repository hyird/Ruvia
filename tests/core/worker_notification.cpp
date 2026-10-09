#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <coroutine>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <future>
#include <limits>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#ifdef __linux__
#include <dirent.h>
#include <fcntl.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <spawn.h>
#include <sys/eventfd.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/steady_timer.hpp>

#include "ruvia/core/detail/io/asio_await.h"
#include "ruvia/core/event_loop_attachment.h"
#include "ruvia/core/worker_notification.h"
#include "ruvia/core/worker_runtime_context.h"

#include "test_harness.h"
#include "worker_notification.h"

namespace {

using namespace std::chrono_literals;
constexpr auto wait_timeout = 5s;

class worker_loop final {
public:
    explicit worker_loop(std::size_t queue_capacity = 128)
        : attachment_(ruvia::attach_event_loop(context_, {.queue_capacity_ = queue_capacity})),
          loop_(attachment_.loop()),
          exit_future_(exit_promise_.get_future()) {}

    ~worker_loop() {
        if (thread_.joinable()) {
            static_cast<void>(stop());
        }
    }

    worker_loop(const worker_loop&) = delete;
    worker_loop& operator=(const worker_loop&) = delete;

    void start() {
        thread_ = std::thread([this] {
            try {
                attachment_.run();
            } catch (...) {
                run_failure_ = std::current_exception();
            }
            exit_promise_.set_value();
        });
    }

    [[nodiscard]] bool stop() {
        attachment_.stop();
        if (thread_.joinable()) {
            if (exit_future_.wait_for(wait_timeout) != std::future_status::ready) {
                std::terminate();
            }
            thread_.join();
        }
        return run_failure_ == nullptr;
    }

    [[nodiscard]] asio::io_context& context() noexcept {
        return context_;
    }

    [[nodiscard]] const ruvia::event_loop& loop() const noexcept {
        return loop_;
    }

private:
    asio::io_context context_;
    ruvia::event_loop_attachment attachment_;
    ruvia::event_loop loop_;
    std::promise<void> exit_promise_;
    std::future<void> exit_future_;
    std::thread thread_;
    std::exception_ptr run_failure_;
};

struct spawned_task final {
    std::shared_ptr<std::promise<std::exception_ptr>> completion_;
    std::future<std::exception_ptr> result_;
};

[[nodiscard]] spawned_task spawn(worker_loop& worker_value, ruvia::task<void> task_value) {
    auto completion = std::make_shared<std::promise<std::exception_ptr>>();
    auto result_value = completion->get_future();
    asio::co_spawn(worker_value.context(), ruvia::detail::task_as_awaitable(std::move(task_value)),
        [completion](std::exception_ptr error) { completion->set_value(std::move(error)); });
    return {std::move(completion), std::move(result_value)};
}

[[nodiscard]] spawned_task spawn(
    ruvia::worker_runtime_context& runtime, ruvia::task<void> task_value) {
    auto completion = std::make_shared<std::promise<std::exception_ptr>>();
    auto result_value = completion->get_future();
    asio::co_spawn(runtime.io_context(), ruvia::detail::task_as_awaitable(std::move(task_value)),
        [completion](std::exception_ptr error) { completion->set_value(std::move(error)); });
    return {std::move(completion), std::move(result_value)};
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

ruvia::task<void> close_and_stop_runtime(ruvia::worker_notification& notification,
    ruvia::worker_runtime_context& runtime, asio::steady_timer& watchdog_value) {
    watchdog_value.cancel();
    notification.close();
    runtime.close();
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

#ifdef __linux__
struct state_wait_signal final {
    std::mutex mutex_;
    std::condition_variable changed_;
    std::size_t armed_{0};

    void notify_armed() {
        {
            const std::lock_guard lock(mutex_);
            ++armed_;
        }
        changed_.notify_all();
    }

    [[nodiscard]] bool wait_for(std::size_t target, std::chrono::steady_clock::time_point deadline_value) {
        std::unique_lock lock(mutex_);
        return changed_.wait_until(lock, deadline_value, [&] { return armed_ >= target; });
    }
};

struct state_wait_awaiter final {
    ruvia::detail::worker_notification_state& state_;
    state_wait_signal* signal_;

    [[nodiscard]] bool await_ready() {
        return state_.wait_ready();
    }

    [[nodiscard]] bool await_suspend(std::coroutine_handle<> continuation) {
        const bool suspended = state_.begin_wait(continuation);
        if (suspended && signal_ != nullptr) {
            signal_->notify_armed();
        }
        return suspended;
    }

    [[nodiscard]] ruvia::worker_notification_wait_status await_resume() {
        return state_.take_wait_result();
    }
};

ruvia::task<void> wait_state_rounds(ruvia::detail::worker_notification_state& state_value,
    state_wait_signal& signal, std::size_t target, std::atomic<std::size_t>& completed, bool& success) {
    for (std::size_t index = 0; index < target; ++index) {
        if (co_await state_wait_awaiter{state_value, &signal} !=
            ruvia::worker_notification_wait_status::notified) {
            success = false;
            co_return;
        }
        completed.store(index + 1, std::memory_order_release);
    }
    state_value.close();
    success = true;
}

ruvia::task<void> wait_state_once(ruvia::detail::worker_notification_state& state_value,
    state_wait_signal& signal, std::atomic<int>& result_value) {
    const auto status = co_await state_wait_awaiter{state_value, &signal};
    result_value.store(status == ruvia::worker_notification_wait_status::notified ? 1 : 2,
        std::memory_order_release);
}

ruvia::task<void> close_state(ruvia::detail::worker_notification_state& state_value) {
    state_value.close();
    co_return;
}
#endif

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

ruvia::task<void> observe_close_and_timer(ruvia::event_loop loop,
    ruvia::worker_notification& notification, std::atomic<bool>& timer_fired) {
    asio::steady_timer timer(loop.io_context());
    timer.expires_after(20ms);
    auto first = co_await ruvia::detail::async_asio<void>([&timer](auto handler) mutable {
        timer.async_wait(std::move(handler));
    });
    if (first.error_code()) {
        throw std::system_error(first.error_code());
    }

    notification.close();
    timer.expires_after(20ms);
    auto second = co_await ruvia::detail::async_asio<void>([&timer](auto handler) mutable {
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

RUVIA_TEST(worker_notification_wait_resource_reuses_fixed_slot) {
    ruvia::detail::worker_notification_wait_resource resource;
    std::pmr::polymorphic_allocator<std::byte> allocator(&resource);

    auto* active = allocator.allocate(8);
    const bool overlap_rejected = ruvia::testing::throws_on([&] {
        static_cast<void>(allocator.allocate(1));
    });
    allocator.deallocate(active, 8);
    RUVIA_CHECK(overlap_rejected);

    for (std::size_t index = 1; index <= 64; ++index) {
        const auto bytes_value = index * 3;
        auto* allocation = allocator.allocate(bytes_value);
        RUVIA_CHECK_EQ(resource.outstanding_allocations(), std::size_t{1});
        allocator.deallocate(allocation, bytes_value);
        RUVIA_CHECK_EQ(resource.outstanding_allocations(), std::size_t{0});
    }

    auto* aligned = resource.allocate(1024, 64);
    RUVIA_CHECK_EQ(reinterpret_cast<std::uintptr_t>(aligned) % 64, std::uintptr_t{0});
    resource.deallocate(aligned, 1024, 64);
    RUVIA_CHECK(ruvia::testing::throws_on([&] {
        static_cast<void>(resource.allocate(1025, 64));
    }));
    RUVIA_CHECK(ruvia::testing::throws_on([&] {
        static_cast<void>(resource.allocate(1, 128));
    }));

    RUVIA_CHECK_EQ(resource.allocation_count(), std::size_t{66});
    RUVIA_CHECK_EQ(resource.deallocation_count(), std::size_t{66});
}

RUVIA_TEST(worker_notification_early_latch_cold_wait_and_repeated_reuse) {
    constexpr std::size_t round_count = 128;
    worker_loop worker;
    ruvia::worker_notification notification(worker.loop());

    {
        auto unused_cold_wait = cold_wait(notification);
        static_cast<void>(unused_cold_wait);
    }
    RUVIA_CHECK_EQ(notification.notify(), ruvia::worker_notification_status::notified);
    RUVIA_CHECK_EQ(notification.notify(), ruvia::worker_notification_status::coalesced);

    round_state rounds;
    bool success = false;
    auto task_value = spawn(worker, wait_rounds(notification, rounds, round_count, success));
    worker.start();

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
    RUVIA_CHECK(completed);
    RUVIA_CHECK(success);
    RUVIA_CHECK_EQ(rounds.completed_, round_count);
    RUVIA_CHECK(worker.stop());
}

RUVIA_TEST(worker_notification_borrows_worker_runtime_context) {
    constexpr std::size_t round_count = 4;
    asio::io_context io_context(ASIO_CONCURRENCY_HINT_UNSAFE_IO);
    ruvia::worker_runtime_context runtime(io_context, 64);
    ruvia::worker_notification notification(runtime);

    bool close_rejected_off_worker = false;
    try {
        notification.close();
    } catch (const std::logic_error&) {
        close_rejected_off_worker = true;
    }
    RUVIA_CHECK(close_rejected_off_worker);

    {
        auto unused_cold_wait = cold_wait(notification);
        static_cast<void>(unused_cold_wait);
    }
    RUVIA_CHECK_EQ(notification.notify(), ruvia::worker_notification_status::notified);
    RUVIA_CHECK_EQ(notification.notify(), ruvia::worker_notification_status::coalesced);

    round_state rounds;
    bool rounds_succeeded = false;
    auto rounds_task = spawn(runtime, wait_runtime_rounds(notification, rounds, round_count, rounds_succeeded));

    asio::steady_timer watchdog(io_context);
    watchdog.expires_after(20s);
    watchdog.async_wait([&](const asio::error_code& error) {
        if (!error) {
            notification.close();
            runtime.close();
        }
    });

    std::promise<std::exception_ptr> runtime_exit_promise;
    auto runtime_exit = runtime_exit_promise.get_future();
    std::thread worker_value([&] {
        std::exception_ptr failure;
        try {
            runtime.run();
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
        RUVIA_CHECK_EQ(notification.notify(), ruvia::worker_notification_status::notified);
    }
    const bool rounds_finished = finish(rounds_task);
    RUVIA_CHECK(rounds_advanced);
    RUVIA_CHECK(rounds_finished);
    if (rounds_finished) {
        RUVIA_CHECK(rounds_succeeded);
        RUVIA_CHECK_EQ(rounds.completed_, round_count);
    }

    std::promise<void> wait_started;
    auto wait_started_future = wait_started.get_future();
    std::atomic<int> wait_result{0};
    auto pending_wait = spawn(runtime, wait_for_one(notification, &wait_started, &wait_result));
    const bool entered = wait_started_future.wait_for(wait_timeout) == std::future_status::ready;

    auto shutdown = spawn(runtime, close_and_stop_runtime(notification, runtime, watchdog));
    const bool shutdown_completed = finish(shutdown);
    const bool wait_completed = finish(pending_wait);
    RUVIA_CHECK(entered);
    RUVIA_CHECK(shutdown_completed);
    RUVIA_CHECK(wait_completed);
    RUVIA_CHECK_EQ(wait_result.load(std::memory_order_acquire), 2);

    bool runtime_exited = runtime_exit.wait_for(wait_timeout) == std::future_status::ready;
    if (!runtime_exited) {
        runtime.close();
        runtime_exited = runtime_exit.wait_for(25s) == std::future_status::ready;
    }
    RUVIA_CHECK(runtime_exited);
    if (runtime_exited) {
        const auto failure = runtime_exit.get();
        RUVIA_CHECK(failure == nullptr);
    } else {
        std::terminate();
    }
    worker_value.join();
    RUVIA_CHECK(!runtime.handle().accepting());
}

RUVIA_TEST(worker_notification_many_producers_coalesce_without_lost_final_state) {
    constexpr std::size_t producer_count = 4;
    constexpr std::size_t notifications_per_producer = 10000;
    constexpr std::size_t expected = producer_count * notifications_per_producer;

    worker_loop worker;
    ruvia::worker_notification notification(worker.loop());
    producer_state state;
    auto task_value = spawn(worker, wait_for_producers(notification, state, producer_count));
    worker.start();

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

    RUVIA_CHECK(completed);
    RUVIA_CHECK(closed);
    RUVIA_CHECK(observed_value);
    RUVIA_CHECK(!state.early_closed_.load(std::memory_order_acquire));
    RUVIA_CHECK_EQ(state.published_.load(std::memory_order_acquire), expected);
    RUVIA_CHECK_EQ(notification.notify(), ruvia::worker_notification_status::closed);
    RUVIA_CHECK(worker.stop());
}

RUVIA_TEST(worker_notification_wait_checks_worker_and_rejects_concurrency) {
    worker_loop owner;
    worker_loop other;
    ruvia::worker_notification notification(owner.loop());
    bool wrong_worker_rejected = false;
    bool concurrent_wait_rejected = false;
    std::promise<void> first_wait_started;
    std::future<void> first_wait_ready = first_wait_started.get_future();
    std::atomic<int> first_wait_result{0};

    owner.start();
    other.start();

    auto wrong = spawn(other, probe_wrong_worker_wait(notification, wrong_worker_rejected));
    const bool wrong_completed = finish(wrong);
    auto first = spawn(owner, wait_for_one(notification, &first_wait_started, &first_wait_result));
    const bool first_started = first_wait_ready.wait_for(wait_timeout) == std::future_status::ready;
    auto concurrent = spawn(owner, probe_concurrent_wait(notification, concurrent_wait_rejected));
    const bool concurrent_completed = finish(concurrent);
    auto closer = spawn(owner, close_notification(notification));
    const bool close_completed = finish(closer);
    const bool first_completed = finish(first);

    RUVIA_CHECK(wrong_completed);
    RUVIA_CHECK(wrong_worker_rejected);
    RUVIA_CHECK(first_started);
    RUVIA_CHECK(concurrent_completed);
    RUVIA_CHECK(concurrent_wait_rejected);
    RUVIA_CHECK(close_completed);
    RUVIA_CHECK(first_completed);
    RUVIA_CHECK_EQ(first_wait_result.load(std::memory_order_acquire), 2);
    RUVIA_CHECK(owner.stop());
    RUVIA_CHECK(other.stop());
}

RUVIA_TEST(worker_notification_normal_resume_can_destroy_owner_immediately) {
    worker_loop worker;
    auto notification = std::make_unique<ruvia::worker_notification>(worker.loop());
    std::promise<void> started;
    auto started_future = started.get_future();
    std::atomic<int> wait_result{0};
    std::atomic<int> notify_result{-1};
    auto waiting = spawn(worker, close_and_destroy_on_resume(notification, &started, wait_result));
    worker.start();
    const bool entered = started_future.wait_for(wait_timeout) == std::future_status::ready;

    auto notifier = spawn(worker, notify_on_worker(*notification, notify_result));
    const bool notify_completed = finish(notifier);
    const bool wait_completed = finish(waiting);

    RUVIA_CHECK(entered);
    RUVIA_CHECK(notify_completed);
    RUVIA_CHECK(wait_completed);
    RUVIA_CHECK_EQ(notify_result.load(std::memory_order_acquire),
        static_cast<int>(ruvia::worker_notification_status::notified));
    RUVIA_CHECK_EQ(wait_result.load(std::memory_order_acquire), 1);
    RUVIA_CHECK(notification == nullptr);
    RUVIA_CHECK(worker.stop());
}

RUVIA_TEST(worker_notification_cancel_resume_can_destroy_owner_immediately) {
    worker_loop worker;
    auto notification = std::make_unique<ruvia::worker_notification>(worker.loop());
    std::promise<void> started;
    auto started_future = started.get_future();
    std::atomic<int> wait_result{0};
    auto waiting = spawn(worker, close_and_destroy_on_resume(notification, &started, wait_result));
    worker.start();
    const bool entered = started_future.wait_for(wait_timeout) == std::future_status::ready;

    auto closer = spawn(worker, close_notification(*notification));
    const bool close_completed = finish(closer);
    const bool wait_completed = finish(waiting);

    RUVIA_CHECK(entered);
    RUVIA_CHECK(close_completed);
    RUVIA_CHECK(wait_completed);
    RUVIA_CHECK_EQ(wait_result.load(std::memory_order_acquire), 2);
    RUVIA_CHECK(notification == nullptr);
    RUVIA_CHECK(worker.stop());
}

RUVIA_TEST(worker_notification_pending_close_drains_and_worker_timer_survives) {
    worker_loop worker;
    ruvia::worker_notification notification(worker.loop());
    std::promise<void> started;
    auto started_future = started.get_future();
    std::atomic<int> wait_result{0};
    auto waiting = spawn(worker, wait_for_one(notification, &started, &wait_result));
    worker.start();
    const bool entered = started_future.wait_for(wait_timeout) == std::future_status::ready;
    std::atomic<bool> timer_fired{false};
    auto closer = spawn(worker, observe_close_and_timer(worker.loop(), notification, timer_fired));

    const bool wait_completed = finish(waiting);
    const bool close_task_completed = finish(closer);
    RUVIA_CHECK(entered);
    RUVIA_CHECK(wait_completed);
    RUVIA_CHECK(close_task_completed);
    RUVIA_CHECK(timer_fired.load(std::memory_order_acquire));
    RUVIA_CHECK_EQ(wait_result.load(std::memory_order_acquire), 2);
    RUVIA_CHECK(worker.stop());
}

RUVIA_TEST(worker_notification_close_races_producers_without_waiting_for_them) {
    constexpr std::size_t producer_count = 4;
    worker_loop worker;
    ruvia::worker_notification notification(worker.loop());
    std::promise<void> wait_started;
    auto wait_started_future = wait_started.get_future();
    std::atomic<int> wait_result{0};
    auto waiting = spawn(worker, wait_until_closed(notification, &wait_started, wait_result));
    worker.start();
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
    auto closer = spawn(worker, observe_close_and_timer(worker.loop(), notification, timer_fired));

    const bool wait_completed = finish(waiting);
    const bool close_task_completed = finish(closer);
    for (auto& producer : producers) {
        producer.join();
    }
    RUVIA_CHECK(entered);
    RUVIA_CHECK(producers_ready);
    RUVIA_CHECK(producers_notified_open);
    RUVIA_CHECK(wait_completed);
    RUVIA_CHECK(close_task_completed);
    RUVIA_CHECK(timer_fired.load(std::memory_order_acquire));
    RUVIA_CHECK_EQ(wait_result.load(std::memory_order_acquire), 2);
    RUVIA_CHECK(closed_returns.load(std::memory_order_relaxed) > 0);
    RUVIA_CHECK_EQ(notification.notify(), ruvia::worker_notification_status::closed);
    RUVIA_CHECK(worker.stop());
}

#ifdef __linux__
RUVIA_TEST(worker_notification_eventfd_saturation_and_asio_wait_rearm) {
    constexpr std::size_t round_count = 32;
    worker_loop worker;
    ruvia::detail::worker_notification_state state_value(worker.loop(), {});
    state_wait_signal signal;
    std::atomic<std::size_t> completed_rounds{0};
    bool success = false;
    std::vector<ruvia::worker_notification_status> statuses(
        round_count, ruvia::worker_notification_status::closed);
    auto waiting = spawn(worker, wait_state_rounds(state_value, signal, round_count, completed_rounds, success));
    worker.start();

    const auto deadline_value = std::chrono::steady_clock::now() + wait_timeout;
    const bool first_armed = signal.wait_for(1, deadline_value);
    RUVIA_CHECK(first_armed);

    std::mutex gate_mutex;
    std::condition_variable gate_changed;
    bool release_worker = false;
    std::promise<void> blocked;
    auto blocked_future = blocked.get_future();
    const auto blocker = worker.loop().handle().post([&] {
        blocked.set_value();
        std::unique_lock lock(gate_mutex);
        static_cast<void>(gate_changed.wait_until(lock, deadline_value, [&] { return release_worker; }));
    });
    const bool blocker_entered = blocked_future.wait_until(deadline_value) == std::future_status::ready;
    RUVIA_CHECK(blocker.accepted());
    RUVIA_CHECK(blocker_entered);

    bool saturation_write_succeeded = false;
    if (first_armed) {
        if (blocker_entered) {
            const std::uint64_t saturated_counter = std::numeric_limits<std::uint64_t>::max() - 1;
            const auto bytes_written = ::write(
                state_value.sender_descriptor(), &saturated_counter, sizeof(saturated_counter));
            saturation_write_succeeded = bytes_written == static_cast<ssize_t>(sizeof(saturated_counter));
            statuses[0] = state_value.notify();
        } else {
            statuses[0] = state_value.notify();
        }
    }
    {
        const std::lock_guard lock(gate_mutex);
        release_worker = true;
    }
    gate_changed.notify_all();

    if (first_armed) {
        for (std::size_t index = 1; index < round_count; ++index) {
            if (!signal.wait_for(index + 1, deadline_value)) {
                break;
            }
            statuses[index] = state_value.notify();
        }
    }

    bool completed = finish(waiting);
    auto closer = spawn(worker, close_state(state_value));
    const bool close_completed = finish(closer);
    if (!completed) {
        completed = finish(waiting);
    }

    RUVIA_CHECK(saturation_write_succeeded);
    RUVIA_CHECK_EQ(statuses[0], ruvia::worker_notification_status::coalesced);
    for (std::size_t index = 1; index < round_count; ++index) {
        RUVIA_CHECK_EQ(statuses[index], ruvia::worker_notification_status::notified);
    }
    RUVIA_CHECK(completed);
    RUVIA_CHECK(close_completed);
    RUVIA_CHECK(success);
    RUVIA_CHECK_EQ(completed_rounds.load(std::memory_order_acquire), round_count);
    RUVIA_CHECK_EQ(state_value.wait_resource().allocation_count(), round_count);
    RUVIA_CHECK_EQ(state_value.wait_resource().deallocation_count(), round_count);
    RUVIA_CHECK_EQ(state_value.wait_resource().outstanding_allocations(), std::size_t{0});
    RUVIA_CHECK(worker.stop());
}

RUVIA_TEST(worker_notification_eventfd_cancel_releases_asio_wait_slot) {
    worker_loop worker;
    ruvia::detail::worker_notification_state state_value(worker.loop(), {});
    state_wait_signal signal;
    std::atomic<int> wait_result{0};
    auto waiting = spawn(worker, wait_state_once(state_value, signal, wait_result));
    worker.start();
    const bool armed = signal.wait_for(1, std::chrono::steady_clock::now() + wait_timeout);

    auto closer = spawn(worker, close_state(state_value));
    const bool close_completed = finish(closer);
    const bool wait_completed = finish(waiting);

    RUVIA_CHECK(armed);
    RUVIA_CHECK(close_completed);
    RUVIA_CHECK(wait_completed);
    RUVIA_CHECK_EQ(wait_result.load(std::memory_order_acquire), 2);
    RUVIA_CHECK_EQ(state_value.wait_resource().allocation_count(), std::size_t{1});
    RUVIA_CHECK_EQ(state_value.wait_resource().deallocation_count(), std::size_t{1});
    RUVIA_CHECK_EQ(state_value.wait_resource().outstanding_allocations(), std::size_t{0});
    RUVIA_CHECK(worker.stop());
}
#endif

RUVIA_TEST(worker_notification_wakes_while_public_post_queue_is_full) {
    worker_loop worker_value(1);
    ruvia::worker_notification notification(worker_value.loop());
    const auto worker_handle_value = worker_value.loop().handle();
    std::promise<void> wait_started;
    auto wait_started_future = wait_started.get_future();
    std::atomic<int> wait_result{0};
    auto waiting = spawn(worker_value, wait_for_one(notification, &wait_started, &wait_result));
    worker_value.start();
    const bool entered = wait_started_future.wait_for(wait_timeout) == std::future_status::ready;

    std::mutex gate_mutex;
    std::condition_variable gate_changed;
    bool release_worker = false;
    std::promise<void> blocked;
    auto blocked_future = blocked.get_future();
    const auto blocker = worker_handle_value.post([&] {
        blocked.set_value();
        std::unique_lock lock(gate_mutex);
        static_cast<void>(gate_changed.wait_until(lock,
            std::chrono::steady_clock::now() + wait_timeout, [&] { return release_worker; }));
    });
    const bool blocker_entered = blocked_future.wait_for(wait_timeout) == std::future_status::ready;

    std::atomic<bool> queued_ran{false};
    const auto queued = worker_handle_value.post([&] { queued_ran.store(true, std::memory_order_release); });
    const auto full = worker_handle_value.post([] {});
    const auto notification_result = notification.notify();
    {
        const std::lock_guard lock(gate_mutex);
        release_worker = true;
    }
    gate_changed.notify_all();

    const bool wait_completed = finish(waiting);
    auto closer = spawn(worker_value, close_notification(notification));
    const bool close_completed = finish(closer);

    RUVIA_CHECK(entered);
    RUVIA_CHECK(blocker.accepted());
    RUVIA_CHECK(blocker_entered);
    RUVIA_CHECK(queued.accepted());
    RUVIA_CHECK_EQ(full.status(), ruvia::post_status::queue_full);
    RUVIA_CHECK(notification_result == ruvia::worker_notification_status::notified ||
                notification_result == ruvia::worker_notification_status::coalesced);
    RUVIA_CHECK(wait_completed);
    RUVIA_CHECK(close_completed);
    RUVIA_CHECK(queued_ran.load(std::memory_order_acquire));
    RUVIA_CHECK_EQ(wait_result.load(std::memory_order_acquire), 1);
    RUVIA_CHECK(worker_value.stop());
}

#ifdef __linux__
[[nodiscard]] std::size_t open_descriptor_count() {
    DIR* directory = opendir("/proc/self/fd");
    if (directory == nullptr) {
        return 0;
    }
    std::size_t count = 0;
    while (const auto* entry = readdir(directory)) {
        char* end = nullptr;
        static_cast<void>(std::strtoul(entry->d_name, &end, 10));
        if (end != entry->d_name && *end == '\0') {
            ++count;
        }
    }
    closedir(directory);
    return count;
}

[[nodiscard]] bool install_event_fd_dup_failure_filter() {
    // Fail only F_DUPFD_CLOEXEC after eventfd succeeds; no process-wide FD limit is changed.
    constexpr std::size_t command_offset = offsetof(struct seccomp_data, args) +
                                           sizeof(std::uint64_t)
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
                                           + sizeof(std::uint32_t)
#endif
        ;
    const sock_filter instructions[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_fcntl, 0, 3),
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, command_offset),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, F_DUPFD_CLOEXEC, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | (EMFILE & SECCOMP_RET_DATA)),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
    sock_fprog program{static_cast<unsigned short>(sizeof(instructions) / sizeof(instructions[0])),
        const_cast<sock_filter*>(instructions)};
    return prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) == 0 &&
           prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program) == 0;
}

[[nodiscard]] int run_startup_native_failure_child() {
    worker_loop worker;
    const auto descriptors_before = open_descriptor_count();
    if (descriptors_before == 0 || !install_event_fd_dup_failure_filter()) {
        return 1;
    }

    bool failed_after_event_fd = false;
    try {
        ruvia::worker_notification notification(
            worker.loop(), {.startup_timeout_ = std::chrono::milliseconds(250)});
    } catch (const std::system_error& error) {
        failed_after_event_fd = error.code().value() == EMFILE &&
                                &error.code().category() == &std::system_category();
    }
    const bool event_fd_closed = open_descriptor_count() == descriptors_before;
    return failed_after_event_fd && event_fd_closed ? 0 : 1;
}

[[nodiscard]] bool startup_native_failure_child_mode() noexcept {
    const char* mode = std::getenv("RUVIA_WORKER_NOTIFICATION_STARTUP_CHILD");
    return mode != nullptr && std::string_view(mode) == "eventfd-dup-failure";
}

[[nodiscard]] std::vector<std::string> startup_native_failure_child_environment() {
    constexpr std::string_view filter_prefix = "RUVIA_TEST_FILTER=";
    constexpr std::string_view first_prefix = "RUVIA_TEST_FIRST=";
    constexpr std::string_view last_prefix = "RUVIA_TEST_LAST=";
    constexpr std::string_view mode_prefix = "RUVIA_WORKER_NOTIFICATION_STARTUP_CHILD=";
    std::vector<std::string> environment;
    for (char** entry_value = ::environ; entry_value != nullptr && *entry_value != nullptr; ++entry_value) {
        const std::string_view value(*entry_value);
        if (value.starts_with(filter_prefix) || value.starts_with(first_prefix) ||
            value.starts_with(last_prefix) || value.starts_with(mode_prefix)) {
            continue;
        }
        environment.emplace_back(*entry_value);
    }
    environment.emplace_back("RUVIA_TEST_FILTER=worker_notification_startup_validation_rolls_back");
    environment.emplace_back("RUVIA_WORKER_NOTIFICATION_STARTUP_CHILD=eventfd-dup-failure");
    return environment;
}

[[nodiscard]] bool startup_event_fd_dup_failure_closes_partial_state() {
    auto environment = startup_native_failure_child_environment();
    std::vector<char*> environment_pointers;
    environment_pointers.reserve(environment.size() + 1);
    for (auto& entry : environment) {
        environment_pointers.push_back(entry.data());
    }
    environment_pointers.push_back(nullptr);

    char executable_path[] = "/proc/self/exe";
    char* arguments[] = {executable_path, nullptr};
    posix_spawn_file_actions_t file_actions{};
    int spawn_error = posix_spawn_file_actions_init(&file_actions);
    if (spawn_error != 0) {
        return false;
    }
#if defined(__GLIBC__)
#if __GLIBC_PREREQ(2, 34) && defined(__USE_MISC)
    spawn_error = posix_spawn_file_actions_addclosefrom_np(&file_actions, STDERR_FILENO + 1);
#endif
#endif
    pid_t child_value = -1;
    if (spawn_error == 0) {
        spawn_error = posix_spawn(&child_value, executable_path, &file_actions, nullptr, arguments,
            environment_pointers.data());
    }
    const int destroy_error = posix_spawn_file_actions_destroy(&file_actions);
    if (spawn_error != 0) {
        return false;
    }

    int status = 0;
    const auto deadline_value = std::chrono::steady_clock::now() + wait_timeout;
    while (std::chrono::steady_clock::now() < deadline_value) {
        const pid_t result_value = waitpid(child_value, &status, WNOHANG);
        if (result_value == child_value) {
            return destroy_error == 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0;
        }
        if (result_value < 0 && errno != EINTR) {
            break;
        }
        std::this_thread::sleep_for(1ms);
    }

    static_cast<void>(kill(child_value, SIGKILL));
    while (waitpid(child_value, &status, 0) < 0 && errno == EINTR) {
    }
    return false;
}
#endif

RUVIA_TEST(worker_notification_startup_validation_rolls_back) {
#ifdef __linux__
    if (startup_native_failure_child_mode()) {
        RUVIA_CHECK_EQ(run_startup_native_failure_child(), 0);
        return;
    }
#endif
    worker_loop worker;
    {
        ruvia::worker_notification cold(worker.loop());
        auto abandoned_wait = cold_wait(cold);
        static_cast<void>(abandoned_wait);
    }

    bool invalid_loop_rejected = false;
    try {
        ruvia::worker_notification invalid(ruvia::event_loop{});
    } catch (const std::invalid_argument&) {
        invalid_loop_rejected = true;
    }

    bool invalid_timeout_rejected = false;
    try {
        ruvia::worker_notification invalid(worker.loop(), {.startup_timeout_ = 0ms});
    } catch (const std::invalid_argument&) {
        invalid_timeout_rejected = true;
    }
    bool unbounded_timeout_rejected = false;
    try {
        ruvia::worker_notification invalid(worker.loop(), {.startup_timeout_ = 61s});
    } catch (const std::invalid_argument&) {
        unbounded_timeout_rejected = true;
    }

    RUVIA_CHECK(invalid_loop_rejected);
    RUVIA_CHECK(invalid_timeout_rejected);
    RUVIA_CHECK(unbounded_timeout_rejected);
#ifdef __linux__
    RUVIA_CHECK(startup_event_fd_dup_failure_closes_partial_state());
#endif
    RUVIA_CHECK(worker.stop());
}

}  // namespace
