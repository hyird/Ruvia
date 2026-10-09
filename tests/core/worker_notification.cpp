#include "ruvia/core/worker_notification.h"

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

#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/steady_timer.hpp>

#include "ruvia/core/asio_task.h"
#include "ruvia/core/async.h"
#include "ruvia/core/event_loop_attachment.h"
#include "ruvia/core/worker_runtime_context.h"

#include "test_harness.h"

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
    asio::co_spawn(worker_value.context(), ruvia::as_awaitable(std::move(task_value)),
        [completion](std::exception_ptr error) { completion->set_value(std::move(error)); });
    return {std::move(completion), std::move(result_value)};
}

[[nodiscard]] spawned_task spawn(
    ruvia::worker_runtime_context& runtime, ruvia::task<void> task_value) {
    auto completion = std::make_shared<std::promise<std::exception_ptr>>();
    auto result_value = completion->get_future();
    asio::co_spawn(runtime.io_context(), ruvia::as_awaitable(std::move(task_value)),
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

RUVIA_TEST(worker_notification_startup_validation_rolls_back) {
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
    RUVIA_CHECK(worker.stop());
}

}  // namespace
