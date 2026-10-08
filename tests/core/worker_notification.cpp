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

#include "ruvia/core/EventLoopAttachment.h"
#include "ruvia/core/WorkerNotification.h"
#include "ruvia/core/WorkerRuntimeContext.h"
#include "ruvia/core/detail/io/AsioAwait.h"
#include "ruvia/core/detail/worker/WorkerNotification.h"

#include "test_harness.h"

namespace {

using namespace std::chrono_literals;
constexpr auto kDeadline = 5s;

class WorkerLoop final {
public:
    explicit WorkerLoop(std::size_t queue_capacity = 128)
        : attachment_(ruvia::attachEventLoop(context_, {.queue_capacity = queue_capacity})),
          loop_(attachment_.loop()),
          exitFuture_(exitPromise_.get_future()) {}

    ~WorkerLoop() {
        if (thread_.joinable()) {
            static_cast<void>(stop());
        }
    }

    WorkerLoop(const WorkerLoop&) = delete;
    WorkerLoop& operator=(const WorkerLoop&) = delete;

    void start() {
        thread_ = std::thread([this] {
            try {
                attachment_.run();
            } catch (...) {
                runFailure_ = std::current_exception();
            }
            exitPromise_.set_value();
        });
    }

    [[nodiscard]] bool stop() {
        attachment_.stop();
        if (thread_.joinable()) {
            if (exitFuture_.wait_for(kDeadline) != std::future_status::ready) {
                std::terminate();
            }
            thread_.join();
        }
        return runFailure_ == nullptr;
    }

    [[nodiscard]] asio::io_context& context() noexcept {
        return context_;
    }

    [[nodiscard]] const ruvia::EventLoop& loop() const noexcept {
        return loop_;
    }

private:
    asio::io_context context_;
    ruvia::EventLoopAttachment attachment_;
    ruvia::EventLoop loop_;
    std::promise<void> exitPromise_;
    std::future<void> exitFuture_;
    std::thread thread_;
    std::exception_ptr runFailure_;
};

struct SpawnedTask final {
    std::shared_ptr<std::promise<std::exception_ptr>> completion;
    std::future<std::exception_ptr> result;
};

[[nodiscard]] SpawnedTask spawn(WorkerLoop& worker, ruvia::Task<void> task) {
    auto completion = std::make_shared<std::promise<std::exception_ptr>>();
    auto result = completion->get_future();
    asio::co_spawn(worker.context(), ruvia::detail::taskAsAwaitable(std::move(task)),
        [completion](std::exception_ptr error) { completion->set_value(std::move(error)); });
    return {std::move(completion), std::move(result)};
}

[[nodiscard]] SpawnedTask spawn(
    ruvia::WorkerRuntimeContext& runtime, ruvia::Task<void> task) {
    auto completion = std::make_shared<std::promise<std::exception_ptr>>();
    auto result = completion->get_future();
    asio::co_spawn(runtime.ioContext(), ruvia::detail::taskAsAwaitable(std::move(task)),
        [completion](std::exception_ptr error) { completion->set_value(std::move(error)); });
    return {std::move(completion), std::move(result)};
}

[[nodiscard]] bool finish(SpawnedTask& task) {
    if (task.result.wait_for(kDeadline) != std::future_status::ready) {
        return false;
    }
    try {
        if (const auto failure = task.result.get()) {
            std::rethrow_exception(failure);
        }
    } catch (...) {
        return false;
    }
    return true;
}

ruvia::Task<void> closeNotification(ruvia::WorkerNotification& notification) {
    notification.close();
    co_return;
}

ruvia::Task<void> closeAndStopRuntime(ruvia::WorkerNotification& notification,
    ruvia::WorkerRuntimeContext& runtime, asio::steady_timer& watchdog) {
    asio::error_code ignored;
    watchdog.cancel(ignored);
    notification.close();
    runtime.close();
    co_return;
}

ruvia::Task<void> waitForOne(ruvia::WorkerNotification& notification,
    std::promise<void>* started, std::atomic<int>* result) {
    if (started != nullptr) {
        started->set_value();
    }
    const auto status = co_await notification.wait();
    if (result != nullptr) {
        result->store(status == ruvia::WorkerNotificationWaitStatus::kNotified ? 1 : 2,
            std::memory_order_release);
    }
}

ruvia::Task<void> coldWait(ruvia::WorkerNotification& notification) {
    static_cast<void>(co_await notification.wait());
}

#ifdef __linux__
struct StateWaitSignal final {
    std::mutex mutex;
    std::condition_variable changed;
    std::size_t armed{0};

    void notifyArmed() {
        {
            const std::lock_guard lock(mutex);
            ++armed;
        }
        changed.notify_all();
    }

    [[nodiscard]] bool waitFor(std::size_t target, std::chrono::steady_clock::time_point deadline) {
        std::unique_lock lock(mutex);
        return changed.wait_until(lock, deadline, [&] { return armed >= target; });
    }
};

struct StateWaitAwaiter final {
    ruvia::detail::WorkerNotificationState& state;
    StateWaitSignal* signal;

    [[nodiscard]] bool await_ready() {
        return state.waitReady();
    }

    [[nodiscard]] bool await_suspend(std::coroutine_handle<> continuation) {
        const bool suspended = state.beginWait(continuation);
        if (suspended && signal != nullptr) {
            signal->notifyArmed();
        }
        return suspended;
    }

    [[nodiscard]] ruvia::WorkerNotificationWaitStatus await_resume() {
        return state.takeWaitResult();
    }
};

ruvia::Task<void> waitStateRounds(ruvia::detail::WorkerNotificationState& state,
    StateWaitSignal& signal, std::size_t target, std::atomic<std::size_t>& completed, bool& success) {
    for (std::size_t index = 0; index < target; ++index) {
        if (co_await StateWaitAwaiter{state, &signal} !=
            ruvia::WorkerNotificationWaitStatus::kNotified) {
            success = false;
            co_return;
        }
        completed.store(index + 1, std::memory_order_release);
    }
    state.close();
    success = true;
}

ruvia::Task<void> waitStateOnce(ruvia::detail::WorkerNotificationState& state,
    StateWaitSignal& signal, std::atomic<int>& result) {
    const auto status = co_await StateWaitAwaiter{state, &signal};
    result.store(status == ruvia::WorkerNotificationWaitStatus::kNotified ? 1 : 2,
        std::memory_order_release);
}

ruvia::Task<void> closeState(ruvia::detail::WorkerNotificationState& state) {
    state.close();
    co_return;
}
#endif

ruvia::Task<void> notifyOnWorker(
    ruvia::WorkerNotification& notification, std::atomic<int>& result) {
    result.store(static_cast<int>(notification.notify()), std::memory_order_release);
    co_return;
}

ruvia::Task<void> closeAndDestroyOnResume(
    std::unique_ptr<ruvia::WorkerNotification>& notification, std::promise<void>* started,
    std::atomic<int>& result) {
    if (started != nullptr) {
        started->set_value();
    }
    const auto status = co_await notification->wait();
    notification->close();
    notification.reset();
    result.store(status == ruvia::WorkerNotificationWaitStatus::kNotified ? 1 : 2,
        std::memory_order_release);
}

ruvia::Task<void> waitUntilClosed(ruvia::WorkerNotification& notification,
    std::promise<void>* started, std::atomic<int>& result) {
    if (started != nullptr) {
        started->set_value();
    }
    for (;;) {
        const auto status = co_await notification.wait();
        if (status == ruvia::WorkerNotificationWaitStatus::kClosed) {
            result.store(2, std::memory_order_release);
            co_return;
        }
    }
}

struct RoundState final {
    std::mutex mutex;
    std::condition_variable changed;
    std::size_t completed{0};

    [[nodiscard]] bool waitFor(
        std::size_t target, std::chrono::steady_clock::time_point deadline) {
        std::unique_lock lock(mutex);
        return changed.wait_until(lock, deadline, [&] { return completed >= target; });
    }
};

ruvia::Task<void> waitRuntimeRounds(ruvia::WorkerNotification& notification,
    RoundState& rounds, std::size_t target, bool& success) {
    for (std::size_t index = 0; index < target; ++index) {
        if (co_await notification.wait() != ruvia::WorkerNotificationWaitStatus::kNotified) {
            success = false;
            co_return;
        }
        {
            const std::lock_guard lock(rounds.mutex);
            rounds.completed = index + 1;
        }
        rounds.changed.notify_all();
    }
    success = true;
}

ruvia::Task<void> waitRounds(ruvia::WorkerNotification& notification, RoundState& rounds,
    std::size_t target, bool& success) {
    for (std::size_t index = 0; index < target; ++index) {
        if (co_await notification.wait() != ruvia::WorkerNotificationWaitStatus::kNotified) {
            success = false;
            co_return;
        }
        {
            const std::lock_guard lock(rounds.mutex);
            rounds.completed = index + 1;
        }
        rounds.changed.notify_all();
    }
    notification.close();
    success = true;
}

struct ProducerState final {
    std::atomic<std::size_t> published{0};
    std::atomic<std::size_t> finished{0};
    std::atomic<std::size_t> observed{0};
    std::atomic<bool> earlyClosed{false};
};

ruvia::Task<void> waitForProducers(
    ruvia::WorkerNotification& notification, ProducerState& state, std::size_t producerCount) {
    while (state.finished.load(std::memory_order_acquire) != producerCount) {
        if (co_await notification.wait() != ruvia::WorkerNotificationWaitStatus::kNotified) {
            co_return;
        }
    }
    state.observed.store(state.published.load(std::memory_order_acquire), std::memory_order_release);
}

ruvia::Task<void> observeCloseAndTimer(ruvia::EventLoop loop,
    ruvia::WorkerNotification& notification, std::atomic<bool>& timerFired) {
    asio::steady_timer timer(loop.ioContext());
    timer.expires_after(20ms);
    auto first = co_await ruvia::detail::asyncAsio<void>([&timer](auto handler) mutable {
        timer.async_wait(std::move(handler));
    });
    if (first.errorCode()) {
        throw std::system_error(first.errorCode());
    }

    notification.close();
    timer.expires_after(20ms);
    auto second = co_await ruvia::detail::asyncAsio<void>([&timer](auto handler) mutable {
        timer.async_wait(std::move(handler));
    });
    if (second.errorCode()) {
        throw std::system_error(second.errorCode());
    }
    timerFired.store(true, std::memory_order_release);
}

ruvia::Task<void> probeWrongWorkerWait(
    ruvia::WorkerNotification& notification, bool& rejected) {
    try {
        static_cast<void>(co_await notification.wait());
    } catch (const std::logic_error&) {
        rejected = true;
    }
}

ruvia::Task<void> probeConcurrentWait(
    ruvia::WorkerNotification& notification, bool& rejected) {
    try {
        static_cast<void>(co_await notification.wait());
    } catch (const std::logic_error&) {
        rejected = true;
    }
}

RUVIA_TEST(worker_notification_wait_resource_reuses_fixed_slot) {
    ruvia::detail::WorkerNotificationWaitResource resource;
    std::pmr::polymorphic_allocator<std::byte> allocator(&resource);

    auto* active = allocator.allocate(8);
    const bool overlapRejected = ruvia::testing::throwsOn([&] {
        static_cast<void>(allocator.allocate(1));
    });
    allocator.deallocate(active, 8);
    RUVIA_CHECK(overlapRejected);

    for (std::size_t index = 1; index <= 64; ++index) {
        const auto bytes = index * 3;
        auto* allocation = allocator.allocate(bytes);
        RUVIA_CHECK_EQ(resource.outstandingAllocations(), std::size_t{1});
        allocator.deallocate(allocation, bytes);
        RUVIA_CHECK_EQ(resource.outstandingAllocations(), std::size_t{0});
    }

    auto* aligned = resource.allocate(1024, 64);
    RUVIA_CHECK_EQ(reinterpret_cast<std::uintptr_t>(aligned) % 64, std::uintptr_t{0});
    resource.deallocate(aligned, 1024, 64);
    RUVIA_CHECK(ruvia::testing::throwsOn([&] {
        static_cast<void>(resource.allocate(1025, 64));
    }));
    RUVIA_CHECK(ruvia::testing::throwsOn([&] {
        static_cast<void>(resource.allocate(1, 128));
    }));

    RUVIA_CHECK_EQ(resource.allocationCount(), std::size_t{66});
    RUVIA_CHECK_EQ(resource.deallocationCount(), std::size_t{66});
}

RUVIA_TEST(worker_notification_early_latch_cold_wait_and_repeated_reuse) {
    constexpr std::size_t kRounds = 128;
    WorkerLoop worker;
    ruvia::WorkerNotification notification(worker.loop());

    {
        auto unusedColdWait = coldWait(notification);
        static_cast<void>(unusedColdWait);
    }
    RUVIA_CHECK_EQ(notification.notify(), ruvia::WorkerNotificationStatus::kNotified);
    RUVIA_CHECK_EQ(notification.notify(), ruvia::WorkerNotificationStatus::kCoalesced);

    RoundState rounds;
    bool success = false;
    auto task = spawn(worker, waitRounds(notification, rounds, kRounds, success));
    worker.start();

    std::thread producer([&] {
        const auto deadline = std::chrono::steady_clock::now() + kDeadline;
        for (std::size_t index = 1; index < kRounds; ++index) {
            std::unique_lock lock(rounds.mutex);
            const bool ready = rounds.changed.wait_until(lock, deadline,
                [&] { return rounds.completed >= index; });
            lock.unlock();
            if (!ready) {
                return;
            }
            const auto status = notification.notify();
            if (status == ruvia::WorkerNotificationStatus::kClosed) {
                return;
            }
        }
    });

    const bool completed = finish(task);
    producer.join();
    RUVIA_CHECK(completed);
    RUVIA_CHECK(success);
    RUVIA_CHECK_EQ(rounds.completed, kRounds);
    RUVIA_CHECK(worker.stop());
}

RUVIA_TEST(worker_notification_borrows_worker_runtime_context) {
    constexpr std::size_t kRounds = 4;
    asio::io_context ioContext(ASIO_CONCURRENCY_HINT_UNSAFE_IO);
    ruvia::WorkerRuntimeContext runtime(ioContext, 64);
    ruvia::WorkerNotification notification(runtime);

    bool closeRejectedOffWorker = false;
    try {
        notification.close();
    } catch (const std::logic_error&) {
        closeRejectedOffWorker = true;
    }
    RUVIA_CHECK(closeRejectedOffWorker);

    {
        auto unusedColdWait = coldWait(notification);
        static_cast<void>(unusedColdWait);
    }
    RUVIA_CHECK_EQ(notification.notify(), ruvia::WorkerNotificationStatus::kNotified);
    RUVIA_CHECK_EQ(notification.notify(), ruvia::WorkerNotificationStatus::kCoalesced);

    RoundState rounds;
    bool roundsSucceeded = false;
    auto roundsTask = spawn(runtime, waitRuntimeRounds(notification, rounds, kRounds, roundsSucceeded));

    asio::steady_timer watchdog(ioContext);
    watchdog.expires_after(20s);
    watchdog.async_wait([&](const asio::error_code& error) {
        if (!error) {
            notification.close();
            runtime.close();
        }
    });

    std::promise<std::exception_ptr> runtimeExitPromise;
    auto runtimeExit = runtimeExitPromise.get_future();
    std::thread worker([&] {
        std::exception_ptr failure;
        try {
            runtime.run();
        } catch (...) {
            failure = std::current_exception();
        }
        runtimeExitPromise.set_value(std::move(failure));
    });

    bool roundsAdvanced = true;
    for (std::size_t next = 1; next < kRounds; ++next) {
        if (!rounds.waitFor(next, std::chrono::steady_clock::now() + kDeadline)) {
            roundsAdvanced = false;
            break;
        }
        RUVIA_CHECK_EQ(notification.notify(), ruvia::WorkerNotificationStatus::kNotified);
    }
    const bool roundsFinished = finish(roundsTask);
    RUVIA_CHECK(roundsAdvanced);
    RUVIA_CHECK(roundsFinished);
    if (roundsFinished) {
        RUVIA_CHECK(roundsSucceeded);
        RUVIA_CHECK_EQ(rounds.completed, kRounds);
    }

    std::promise<void> waitStarted;
    auto waitStartedFuture = waitStarted.get_future();
    std::atomic<int> waitResult{0};
    auto pendingWait = spawn(runtime, waitForOne(notification, &waitStarted, &waitResult));
    const bool entered = waitStartedFuture.wait_for(kDeadline) == std::future_status::ready;

    auto shutdown = spawn(runtime, closeAndStopRuntime(notification, runtime, watchdog));
    const bool shutdownCompleted = finish(shutdown);
    const bool waitCompleted = finish(pendingWait);
    RUVIA_CHECK(entered);
    RUVIA_CHECK(shutdownCompleted);
    RUVIA_CHECK(waitCompleted);
    RUVIA_CHECK_EQ(waitResult.load(std::memory_order_acquire), 2);

    bool runtimeExited = runtimeExit.wait_for(kDeadline) == std::future_status::ready;
    if (!runtimeExited) {
        runtime.close();
        runtimeExited = runtimeExit.wait_for(25s) == std::future_status::ready;
    }
    RUVIA_CHECK(runtimeExited);
    if (runtimeExited) {
        const auto failure = runtimeExit.get();
        RUVIA_CHECK(failure == nullptr);
    } else {
        std::terminate();
    }
    worker.join();
    RUVIA_CHECK(!runtime.handle().accepting());
}

RUVIA_TEST(worker_notification_many_producers_coalesce_without_lost_final_state) {
    constexpr std::size_t kProducerCount = 4;
    constexpr std::size_t kNotificationsPerProducer = 10000;
    constexpr std::size_t kExpected = kProducerCount * kNotificationsPerProducer;

    WorkerLoop worker;
    ruvia::WorkerNotification notification(worker.loop());
    ProducerState state;
    auto task = spawn(worker, waitForProducers(notification, state, kProducerCount));
    worker.start();

    std::vector<std::thread> producers;
    producers.reserve(kProducerCount);
    for (std::size_t producerIndex = 0; producerIndex < kProducerCount; ++producerIndex) {
        producers.emplace_back([&] {
            for (std::size_t index = 0; index < kNotificationsPerProducer; ++index) {
                state.published.fetch_add(1, std::memory_order_release);
                if (notification.notify() == ruvia::WorkerNotificationStatus::kClosed) {
                    state.earlyClosed.store(true, std::memory_order_release);
                    return;
                }
            }
            state.finished.fetch_add(1, std::memory_order_release);
            static_cast<void>(notification.notify());
        });
    }

    const bool completed = finish(task);
    for (auto& producer : producers) {
        producer.join();
    }
    const bool observed = state.observed.load(std::memory_order_acquire) == kExpected;
    auto closer = spawn(worker, closeNotification(notification));
    const bool closed = finish(closer);

    RUVIA_CHECK(completed);
    RUVIA_CHECK(closed);
    RUVIA_CHECK(observed);
    RUVIA_CHECK(!state.earlyClosed.load(std::memory_order_acquire));
    RUVIA_CHECK_EQ(state.published.load(std::memory_order_acquire), kExpected);
    RUVIA_CHECK_EQ(notification.notify(), ruvia::WorkerNotificationStatus::kClosed);
    RUVIA_CHECK(worker.stop());
}

RUVIA_TEST(worker_notification_wait_checks_worker_and_rejects_concurrency) {
    WorkerLoop owner;
    WorkerLoop other;
    ruvia::WorkerNotification notification(owner.loop());
    bool wrongWorkerRejected = false;
    bool concurrentWaitRejected = false;
    std::promise<void> firstWaitStarted;
    std::future<void> firstWaitReady = firstWaitStarted.get_future();
    std::atomic<int> firstWaitResult{0};

    owner.start();
    other.start();

    auto wrong = spawn(other, probeWrongWorkerWait(notification, wrongWorkerRejected));
    const bool wrongCompleted = finish(wrong);
    auto first = spawn(owner, waitForOne(notification, &firstWaitStarted, &firstWaitResult));
    const bool firstStarted = firstWaitReady.wait_for(kDeadline) == std::future_status::ready;
    auto concurrent = spawn(owner, probeConcurrentWait(notification, concurrentWaitRejected));
    const bool concurrentCompleted = finish(concurrent);
    auto closer = spawn(owner, closeNotification(notification));
    const bool closeCompleted = finish(closer);
    const bool firstCompleted = finish(first);

    RUVIA_CHECK(wrongCompleted);
    RUVIA_CHECK(wrongWorkerRejected);
    RUVIA_CHECK(firstStarted);
    RUVIA_CHECK(concurrentCompleted);
    RUVIA_CHECK(concurrentWaitRejected);
    RUVIA_CHECK(closeCompleted);
    RUVIA_CHECK(firstCompleted);
    RUVIA_CHECK_EQ(firstWaitResult.load(std::memory_order_acquire), 2);
    RUVIA_CHECK(owner.stop());
    RUVIA_CHECK(other.stop());
}

RUVIA_TEST(worker_notification_normal_resume_can_destroy_owner_immediately) {
    WorkerLoop worker;
    auto notification = std::make_unique<ruvia::WorkerNotification>(worker.loop());
    std::promise<void> started;
    auto startedFuture = started.get_future();
    std::atomic<int> waitResult{0};
    std::atomic<int> notifyResult{-1};
    auto waiting = spawn(worker, closeAndDestroyOnResume(notification, &started, waitResult));
    worker.start();
    const bool entered = startedFuture.wait_for(kDeadline) == std::future_status::ready;

    auto notifier = spawn(worker, notifyOnWorker(*notification, notifyResult));
    const bool notifyCompleted = finish(notifier);
    const bool waitCompleted = finish(waiting);

    RUVIA_CHECK(entered);
    RUVIA_CHECK(notifyCompleted);
    RUVIA_CHECK(waitCompleted);
    RUVIA_CHECK_EQ(notifyResult.load(std::memory_order_acquire),
        static_cast<int>(ruvia::WorkerNotificationStatus::kNotified));
    RUVIA_CHECK_EQ(waitResult.load(std::memory_order_acquire), 1);
    RUVIA_CHECK(notification == nullptr);
    RUVIA_CHECK(worker.stop());
}

RUVIA_TEST(worker_notification_cancel_resume_can_destroy_owner_immediately) {
    WorkerLoop worker;
    auto notification = std::make_unique<ruvia::WorkerNotification>(worker.loop());
    std::promise<void> started;
    auto startedFuture = started.get_future();
    std::atomic<int> waitResult{0};
    auto waiting = spawn(worker, closeAndDestroyOnResume(notification, &started, waitResult));
    worker.start();
    const bool entered = startedFuture.wait_for(kDeadline) == std::future_status::ready;

    auto closer = spawn(worker, closeNotification(*notification));
    const bool closeCompleted = finish(closer);
    const bool waitCompleted = finish(waiting);

    RUVIA_CHECK(entered);
    RUVIA_CHECK(closeCompleted);
    RUVIA_CHECK(waitCompleted);
    RUVIA_CHECK_EQ(waitResult.load(std::memory_order_acquire), 2);
    RUVIA_CHECK(notification == nullptr);
    RUVIA_CHECK(worker.stop());
}

RUVIA_TEST(worker_notification_pending_close_drains_and_worker_timer_survives) {
    WorkerLoop worker;
    ruvia::WorkerNotification notification(worker.loop());
    std::promise<void> started;
    auto startedFuture = started.get_future();
    std::atomic<int> waitResult{0};
    auto waiting = spawn(worker, waitForOne(notification, &started, &waitResult));
    worker.start();
    const bool entered = startedFuture.wait_for(kDeadline) == std::future_status::ready;
    std::atomic<bool> timerFired{false};
    auto closer = spawn(worker, observeCloseAndTimer(worker.loop(), notification, timerFired));

    const bool waitCompleted = finish(waiting);
    const bool closeTaskCompleted = finish(closer);
    RUVIA_CHECK(entered);
    RUVIA_CHECK(waitCompleted);
    RUVIA_CHECK(closeTaskCompleted);
    RUVIA_CHECK(timerFired.load(std::memory_order_acquire));
    RUVIA_CHECK_EQ(waitResult.load(std::memory_order_acquire), 2);
    RUVIA_CHECK(worker.stop());
}

RUVIA_TEST(worker_notification_close_races_producers_without_waiting_for_them) {
    constexpr std::size_t kProducerCount = 4;
    WorkerLoop worker;
    ruvia::WorkerNotification notification(worker.loop());
    std::promise<void> waitStarted;
    auto waitStartedFuture = waitStarted.get_future();
    std::atomic<int> waitResult{0};
    auto waiting = spawn(worker, waitUntilClosed(notification, &waitStarted, waitResult));
    worker.start();
    const bool entered = waitStartedFuture.wait_for(kDeadline) == std::future_status::ready;

    std::atomic<std::size_t> ready{0};
    std::atomic<bool> begin{false};
    std::atomic<std::size_t> openCalls{0};
    std::atomic<std::size_t> closedReturns{0};
    std::vector<std::thread> producers;
    producers.reserve(kProducerCount);
    for (std::size_t index = 0; index < kProducerCount; ++index) {
        producers.emplace_back([&] {
            ready.fetch_add(1, std::memory_order_release);
            while (!begin.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            if (notification.notify() == ruvia::WorkerNotificationStatus::kClosed) {
                closedReturns.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            openCalls.fetch_add(1, std::memory_order_release);
            const auto deadline = std::chrono::steady_clock::now() + 4s;
            while (std::chrono::steady_clock::now() < deadline) {
                if (notification.notify() == ruvia::WorkerNotificationStatus::kClosed) {
                    closedReturns.fetch_add(1, std::memory_order_relaxed);
                    return;
                }
            }
        });
    }
    const auto producerDeadline = std::chrono::steady_clock::now() + kDeadline;
    while (ready.load(std::memory_order_acquire) != kProducerCount &&
           std::chrono::steady_clock::now() < producerDeadline) {
        std::this_thread::yield();
    }
    const bool producersReady = ready.load(std::memory_order_acquire) == kProducerCount;
    begin.store(true, std::memory_order_release);
    const auto callDeadline = std::chrono::steady_clock::now() + kDeadline;
    while (openCalls.load(std::memory_order_acquire) != kProducerCount &&
           std::chrono::steady_clock::now() < callDeadline) {
        std::this_thread::yield();
    }
    const bool producersNotifiedOpen = openCalls.load(std::memory_order_acquire) == kProducerCount;
    std::atomic<bool> timerFired{false};
    auto closer = spawn(worker, observeCloseAndTimer(worker.loop(), notification, timerFired));

    const bool waitCompleted = finish(waiting);
    const bool closeTaskCompleted = finish(closer);
    for (auto& producer : producers) {
        producer.join();
    }
    RUVIA_CHECK(entered);
    RUVIA_CHECK(producersReady);
    RUVIA_CHECK(producersNotifiedOpen);
    RUVIA_CHECK(waitCompleted);
    RUVIA_CHECK(closeTaskCompleted);
    RUVIA_CHECK(timerFired.load(std::memory_order_acquire));
    RUVIA_CHECK_EQ(waitResult.load(std::memory_order_acquire), 2);
    RUVIA_CHECK(closedReturns.load(std::memory_order_relaxed) > 0);
    RUVIA_CHECK_EQ(notification.notify(), ruvia::WorkerNotificationStatus::kClosed);
    RUVIA_CHECK(worker.stop());
}

#ifdef __linux__
RUVIA_TEST(worker_notification_eventfd_saturation_and_asio_wait_rearm) {
    constexpr std::size_t kRounds = 32;
    WorkerLoop worker;
    ruvia::detail::WorkerNotificationState state(worker.loop(), {});
    StateWaitSignal signal;
    std::atomic<std::size_t> completedRounds{0};
    bool success = false;
    std::vector<ruvia::WorkerNotificationStatus> statuses(
        kRounds, ruvia::WorkerNotificationStatus::kClosed);
    auto waiting = spawn(worker, waitStateRounds(state, signal, kRounds, completedRounds, success));
    worker.start();

    const auto deadline = std::chrono::steady_clock::now() + kDeadline;
    const bool firstArmed = signal.waitFor(1, deadline);
    RUVIA_CHECK(firstArmed);

    std::mutex gateMutex;
    std::condition_variable gateChanged;
    bool releaseWorker = false;
    std::promise<void> blocked;
    auto blockedFuture = blocked.get_future();
    const auto blocker = worker.loop().handle().post([&] {
        blocked.set_value();
        std::unique_lock lock(gateMutex);
        static_cast<void>(gateChanged.wait_until(lock, deadline, [&] { return releaseWorker; }));
    });
    const bool blockerEntered = blockedFuture.wait_until(deadline) == std::future_status::ready;
    RUVIA_CHECK(blocker.accepted());
    RUVIA_CHECK(blockerEntered);

    bool saturationWriteSucceeded = false;
    if (firstArmed) {
        if (blockerEntered) {
            const std::uint64_t saturatedCounter = std::numeric_limits<std::uint64_t>::max() - 1;
            const auto bytesWritten = ::write(
                state.senderDescriptor(), &saturatedCounter, sizeof(saturatedCounter));
            saturationWriteSucceeded = bytesWritten == static_cast<ssize_t>(sizeof(saturatedCounter));
            statuses[0] = state.notify();
        } else {
            statuses[0] = state.notify();
        }
    }
    {
        const std::lock_guard lock(gateMutex);
        releaseWorker = true;
    }
    gateChanged.notify_all();

    if (firstArmed) {
        for (std::size_t index = 1; index < kRounds; ++index) {
            if (!signal.waitFor(index + 1, deadline)) {
                break;
            }
            statuses[index] = state.notify();
        }
    }

    bool completed = finish(waiting);
    auto closer = spawn(worker, closeState(state));
    const bool closeCompleted = finish(closer);
    if (!completed) {
        completed = finish(waiting);
    }

    RUVIA_CHECK(saturationWriteSucceeded);
    RUVIA_CHECK_EQ(statuses[0], ruvia::WorkerNotificationStatus::kCoalesced);
    for (std::size_t index = 1; index < kRounds; ++index) {
        RUVIA_CHECK_EQ(statuses[index], ruvia::WorkerNotificationStatus::kNotified);
    }
    RUVIA_CHECK(completed);
    RUVIA_CHECK(closeCompleted);
    RUVIA_CHECK(success);
    RUVIA_CHECK_EQ(completedRounds.load(std::memory_order_acquire), kRounds);
    RUVIA_CHECK_EQ(state.waitResource().allocationCount(), kRounds);
    RUVIA_CHECK_EQ(state.waitResource().deallocationCount(), kRounds);
    RUVIA_CHECK_EQ(state.waitResource().outstandingAllocations(), std::size_t{0});
    RUVIA_CHECK(worker.stop());
}

RUVIA_TEST(worker_notification_eventfd_cancel_releases_asio_wait_slot) {
    WorkerLoop worker;
    ruvia::detail::WorkerNotificationState state(worker.loop(), {});
    StateWaitSignal signal;
    std::atomic<int> waitResult{0};
    auto waiting = spawn(worker, waitStateOnce(state, signal, waitResult));
    worker.start();
    const bool armed = signal.waitFor(1, std::chrono::steady_clock::now() + kDeadline);

    auto closer = spawn(worker, closeState(state));
    const bool closeCompleted = finish(closer);
    const bool waitCompleted = finish(waiting);

    RUVIA_CHECK(armed);
    RUVIA_CHECK(closeCompleted);
    RUVIA_CHECK(waitCompleted);
    RUVIA_CHECK_EQ(waitResult.load(std::memory_order_acquire), 2);
    RUVIA_CHECK_EQ(state.waitResource().allocationCount(), std::size_t{1});
    RUVIA_CHECK_EQ(state.waitResource().deallocationCount(), std::size_t{1});
    RUVIA_CHECK_EQ(state.waitResource().outstandingAllocations(), std::size_t{0});
    RUVIA_CHECK(worker.stop());
}
#endif

RUVIA_TEST(worker_notification_wakes_while_public_post_queue_is_full) {
    WorkerLoop worker(1);
    ruvia::WorkerNotification notification(worker.loop());
    const auto workerHandle = worker.loop().handle();
    std::promise<void> waitStarted;
    auto waitStartedFuture = waitStarted.get_future();
    std::atomic<int> waitResult{0};
    auto waiting = spawn(worker, waitForOne(notification, &waitStarted, &waitResult));
    worker.start();
    const bool entered = waitStartedFuture.wait_for(kDeadline) == std::future_status::ready;

    std::mutex gateMutex;
    std::condition_variable gateChanged;
    bool releaseWorker = false;
    std::promise<void> blocked;
    auto blockedFuture = blocked.get_future();
    const auto blocker = workerHandle.post([&] {
        blocked.set_value();
        std::unique_lock lock(gateMutex);
        static_cast<void>(gateChanged.wait_until(lock,
            std::chrono::steady_clock::now() + kDeadline, [&] { return releaseWorker; }));
    });
    const bool blockerEntered = blockedFuture.wait_for(kDeadline) == std::future_status::ready;

    std::atomic<bool> queuedRan{false};
    const auto queued = workerHandle.post([&] { queuedRan.store(true, std::memory_order_release); });
    const auto full = workerHandle.post([] {});
    const auto notificationResult = notification.notify();
    {
        const std::lock_guard lock(gateMutex);
        releaseWorker = true;
    }
    gateChanged.notify_all();

    const bool waitCompleted = finish(waiting);
    auto closer = spawn(worker, closeNotification(notification));
    const bool closeCompleted = finish(closer);

    RUVIA_CHECK(entered);
    RUVIA_CHECK(blocker.accepted());
    RUVIA_CHECK(blockerEntered);
    RUVIA_CHECK(queued.accepted());
    RUVIA_CHECK_EQ(full.status(), ruvia::PostStatus::kQueueFull);
    RUVIA_CHECK(notificationResult == ruvia::WorkerNotificationStatus::kNotified ||
                notificationResult == ruvia::WorkerNotificationStatus::kCoalesced);
    RUVIA_CHECK(waitCompleted);
    RUVIA_CHECK(closeCompleted);
    RUVIA_CHECK(queuedRan.load(std::memory_order_acquire));
    RUVIA_CHECK_EQ(waitResult.load(std::memory_order_acquire), 1);
    RUVIA_CHECK(worker.stop());
}

#ifdef __linux__
[[nodiscard]] std::size_t openDescriptorCount() {
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

[[nodiscard]] bool installEventFdDupFailureFilter() {
    // Fail only F_DUPFD_CLOEXEC after eventfd succeeds; no process-wide FD limit is changed.
    constexpr std::size_t kCommandOffset = offsetof(struct seccomp_data, args) +
                                           sizeof(std::uint64_t)
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
                                           + sizeof(std::uint32_t)
#endif
        ;
    const sock_filter instructions[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_fcntl, 0, 3),
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, kCommandOffset),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, F_DUPFD_CLOEXEC, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | (EMFILE & SECCOMP_RET_DATA)),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
    sock_fprog program{static_cast<unsigned short>(sizeof(instructions) / sizeof(instructions[0])),
        const_cast<sock_filter*>(instructions)};
    return prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) == 0 &&
           prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program) == 0;
}

[[nodiscard]] int runStartupNativeFailureChild() {
    WorkerLoop worker;
    const auto descriptorsBefore = openDescriptorCount();
    if (descriptorsBefore == 0 || !installEventFdDupFailureFilter()) {
        return 1;
    }

    bool failedAfterEventFd = false;
    try {
        ruvia::WorkerNotification notification(
            worker.loop(), {.startupTimeout = std::chrono::milliseconds(250)});
    } catch (const std::system_error& error) {
        failedAfterEventFd = error.code().value() == EMFILE &&
                             &error.code().category() == &std::system_category();
    }
    const bool eventFdClosed = openDescriptorCount() == descriptorsBefore;
    return failedAfterEventFd && eventFdClosed ? 0 : 1;
}

[[nodiscard]] bool startupNativeFailureChildMode() noexcept {
    const char* mode = std::getenv("RUVIA_WORKER_NOTIFICATION_STARTUP_CHILD");
    return mode != nullptr && std::string_view(mode) == "eventfd-dup-failure";
}

[[nodiscard]] std::vector<std::string> startupNativeFailureChildEnvironment() {
    constexpr std::string_view kFilterPrefix = "RUVIA_TEST_FILTER=";
    constexpr std::string_view kFirstPrefix = "RUVIA_TEST_FIRST=";
    constexpr std::string_view kLastPrefix = "RUVIA_TEST_LAST=";
    constexpr std::string_view kModePrefix = "RUVIA_WORKER_NOTIFICATION_STARTUP_CHILD=";
    std::vector<std::string> environment;
    for (char** entry = ::environ; entry != nullptr && *entry != nullptr; ++entry) {
        const std::string_view value(*entry);
        if (value.starts_with(kFilterPrefix) || value.starts_with(kFirstPrefix) ||
            value.starts_with(kLastPrefix) || value.starts_with(kModePrefix)) {
            continue;
        }
        environment.emplace_back(*entry);
    }
    environment.emplace_back("RUVIA_TEST_FILTER=worker_notification_startup_validation_rolls_back");
    environment.emplace_back("RUVIA_WORKER_NOTIFICATION_STARTUP_CHILD=eventfd-dup-failure");
    return environment;
}

[[nodiscard]] bool startupEventFdDupFailureClosesPartialState() {
    auto environment = startupNativeFailureChildEnvironment();
    std::vector<char*> environmentPointers;
    environmentPointers.reserve(environment.size() + 1);
    for (auto& entry : environment) {
        environmentPointers.push_back(entry.data());
    }
    environmentPointers.push_back(nullptr);

    char executablePath[] = "/proc/self/exe";
    char* arguments[] = {executablePath, nullptr};
    posix_spawn_file_actions_t fileActions{};
    int spawnError = posix_spawn_file_actions_init(&fileActions);
    if (spawnError != 0) {
        return false;
    }
#if defined(__GLIBC__)
#if __GLIBC_PREREQ(2, 34) && defined(__USE_MISC)
    spawnError = posix_spawn_file_actions_addclosefrom_np(&fileActions, STDERR_FILENO + 1);
#endif
#endif
    pid_t child = -1;
    if (spawnError == 0) {
        spawnError = posix_spawn(&child, executablePath, &fileActions, nullptr, arguments,
            environmentPointers.data());
    }
    const int destroyError = posix_spawn_file_actions_destroy(&fileActions);
    if (spawnError != 0) {
        return false;
    }

    int status = 0;
    const auto deadline = std::chrono::steady_clock::now() + kDeadline;
    while (std::chrono::steady_clock::now() < deadline) {
        const pid_t result = waitpid(child, &status, WNOHANG);
        if (result == child) {
            return destroyError == 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0;
        }
        if (result < 0 && errno != EINTR) {
            break;
        }
        std::this_thread::sleep_for(1ms);
    }

    static_cast<void>(kill(child, SIGKILL));
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
    }
    return false;
}
#endif

RUVIA_TEST(worker_notification_startup_validation_rolls_back) {
#ifdef __linux__
    if (startupNativeFailureChildMode()) {
        RUVIA_CHECK_EQ(runStartupNativeFailureChild(), 0);
        return;
    }
#endif
    WorkerLoop worker;
    {
        ruvia::WorkerNotification cold(worker.loop());
        auto abandonedWait = coldWait(cold);
        static_cast<void>(abandonedWait);
    }

    bool invalidLoopRejected = false;
    try {
        ruvia::WorkerNotification invalid(ruvia::EventLoop{});
    } catch (const std::invalid_argument&) {
        invalidLoopRejected = true;
    }

    bool invalidTimeoutRejected = false;
    try {
        ruvia::WorkerNotification invalid(worker.loop(), {.startupTimeout = 0ms});
    } catch (const std::invalid_argument&) {
        invalidTimeoutRejected = true;
    }
    bool unboundedTimeoutRejected = false;
    try {
        ruvia::WorkerNotification invalid(worker.loop(), {.startupTimeout = 61s});
    } catch (const std::invalid_argument&) {
        unboundedTimeoutRejected = true;
    }

    RUVIA_CHECK(invalidLoopRejected);
    RUVIA_CHECK(invalidTimeoutRejected);
    RUVIA_CHECK(unboundedTimeoutRejected);
#ifdef __linux__
    RUVIA_CHECK(startupEventFdDupFailureClosesPartialState());
#endif
    RUVIA_CHECK(worker.stop());
}

}  // namespace
