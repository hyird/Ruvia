#include <ruvia/core/EventLoopPool.h>
#include <ruvia/core/detail/io/AsioAwait.h>
#include <ruvia/core/detail/RuntimeLifecycle.h>
#include <ruvia/core/detail/worker/WorkerDispatcher.h>
#include <ruvia/core/detail/worker/WorkerSelection.h>
#include <ruvia/core/detail/worker/WorkerSignal.h>

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/ip/udp.hpp>

#include <atomic>
#include <array>
#include <condition_variable>
#include <cstdio>
#include <future>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

namespace {

static_assert(std::move_constructible<ruvia::EventLoopAttachment>);
static_assert(!std::assignable_from<
    ruvia::EventLoopAttachment&,
    ruvia::EventLoopAttachment&&>);

ruvia::Task<void> waitForSignal(
    ruvia::detail::WorkerSignal& signal,
    bool& resumed,
    std::size_t& remaining,
    ruvia::EventLoopAttachment& attachment) {
    {
        auto discardedColdWait = signal.wait();
        static_cast<void>(discardedColdWait);
    }
    co_await signal.wait();
    resumed = true;
    if (--remaining == 0) {
        attachment.stop();
    }
}

bool testWorkerSignalIsWorkerAffine() {
    bool invalidWorkerRejected = false;
    ruvia::WorkerHandle invalidWorker;
    try {
        ruvia::detail::WorkerSignal invalid(invalidWorker);
    } catch (const std::invalid_argument&) {
        invalidWorkerRejected = true;
    }

    asio::io_context ioContext;
    auto attachment = ruvia::attachEventLoop(ioContext);
    const auto workerHandle = attachment.loop().handle();
    ruvia::detail::WorkerSignal firstSignal(workerHandle);
    ruvia::detail::WorkerSignal secondSignal(workerHandle);
    bool firstResumed = false;
    bool secondResumed = false;
    std::size_t remaining = 2;
    asio::co_spawn(
        ioContext,
        ruvia::detail::taskAsAwaitable(
            waitForSignal(firstSignal, firstResumed, remaining, attachment)),
        asio::detached);
    asio::co_spawn(
        ioContext,
        ruvia::detail::taskAsAwaitable(
            waitForSignal(secondSignal, secondResumed, remaining, attachment)),
        asio::detached);
    asio::post(ioContext, [&] {
        firstSignal.notify();
        secondSignal.notify();
    });
    ioContext.run();
    return invalidWorkerRejected && firstResumed && secondResumed;
}

bool testWorkerSignalHasNoArbitraryWaiterLimit() {
    constexpr std::size_t kWaiterCount = 16;
    asio::io_context ioContext;
    auto attachment = ruvia::attachEventLoop(ioContext);
    const auto workerHandle = attachment.loop().handle();
    ruvia::detail::WorkerSignal signal(workerHandle);
    std::array<bool, kWaiterCount> resumed{};
    std::size_t remaining = kWaiterCount;
    for (std::size_t index = 0; index < resumed.size(); ++index) {
        asio::co_spawn(
            ioContext,
            ruvia::detail::taskAsAwaitable(
                waitForSignal(signal, resumed[index], remaining, attachment)),
            asio::detached);
    }

    asio::post(ioContext, [&] {
        signal.notify();
    });
    ioContext.run();
    for (const bool value : resumed) {
        if (!value) {
            return false;
        }
    }
    return true;
}

ruvia::Task<void> startColdSignalWait(
    ruvia::Task<void> coldWait,
    bool& rejected) {
    try {
        co_await std::move(coldWait);
    } catch (const std::logic_error&) {
        rejected = true;
    }
}

bool testWorkerSignalRechecksAffinityWhenColdWaitStarts() {
    asio::io_context ownerContext;
    asio::io_context otherContext;
    auto ownerAttachment = ruvia::attachEventLoop(ownerContext);
    auto otherAttachment = ruvia::attachEventLoop(otherContext);
    const auto ownerHandle = ownerAttachment.loop().handle();
    ruvia::detail::WorkerSignal signal(ownerHandle);
    std::optional<ruvia::Task<void>> coldWait;

    asio::post(ownerContext, [&] {
        coldWait.emplace(signal.wait());
        ownerAttachment.stop();
    });
    ownerContext.run();
    if (!coldWait.has_value()) {
        return false;
    }

    bool rejected = false;
    asio::co_spawn(
        otherContext,
        ruvia::detail::taskAsAwaitable(startColdSignalWait(
            std::move(*coldWait), rejected)),
        asio::detached);
    asio::post(otherContext, [&] { otherAttachment.stop(); });
    otherContext.run();
    coldWait.reset();
    return rejected;
}

bool testDispatchAndAffinity() {
    ruvia::EventLoopPool loops({.loopCount = 2, .mailboxCapacity = 4});
    const auto first = loops.loop(0);
    const auto second = loops.loop(1);
    if (!first.valid() || first.id() == 0 || first.id() == second.id() || first.isCurrent()) {
        return false;
    }
    constexpr std::string_view key = "device-42";
    if (loops.loopFor(key).id() !=
        loops.loopFor(ruvia::detail::workerSelectionHash(key)).id()) {
        return false;
    }
    if (&first.ioContext() != &first.executor().context() ||
        first.handle().id() != first.id()) {
        return false;
    }
    asio::ip::tcp::socket tcp(first.ioContext());
    asio::ip::udp::socket udp(first.ioContext());
    if (&tcp.get_executor().context() != &first.ioContext() ||
        &udp.get_executor().context() != &first.ioContext()) {
        return false;
    }

    std::promise<bool> completed;
    auto result = completed.get_future();
    std::atomic_bool stopCallbackRan{false};
    std::atomic_bool stopCallbackOnLoop{false};
    auto stopRegistration = first.onStop([&] {
        stopCallbackOnLoop = first.isCurrent();
        stopCallbackRan = true;
    });
    auto moveOnly = std::make_unique<int>(42);
    if (first.post([worker = first,
                    value = std::move(moveOnly),
                    completed = std::move(completed)]() mutable {
            completed.set_value(worker.isCurrent() && *value == 42);
        }) != ruvia::PostStatus::kAccepted) {
        return false;
    }

    loops.start();
    const bool success = result.get();
    loops.stop();
    loops.join();
    return success && stopRegistration.valid() && stopCallbackRan &&
           stopCallbackOnLoop &&
           first.post([] {}) == ruvia::PostStatus::kWorkerStopping;
}

bool testBoundedMailbox() {
    ruvia::EventLoopPool loops({.loopCount = 1, .mailboxCapacity = 2});
    const auto worker = loops.loop(0);
    std::atomic<int> calls{0};
    std::promise<void> completed;
    auto result = completed.get_future();
    std::promise<void> retried;
    auto retriedResult = retried.get_future();

    if (worker.post([&] { ++calls; }) != ruvia::PostStatus::kAccepted ||
        worker.post([&] {
            ++calls;
            completed.set_value();
        }) != ruvia::PostStatus::kAccepted) {
        return false;
    }
    auto rejected = worker.post(
        [value = std::make_unique<int>(9), &calls, &retried] {
            calls.fetch_add(*value);
            retried.set_value();
        });
    if (rejected != ruvia::PostStatus::kQueueFull ||
        rejected.rejected() == nullptr) {
        return false;
    }

    loops.start();
    result.get();
    if (worker.post(std::move(rejected).takeRejected()) !=
        ruvia::PostStatus::kAccepted) {
        return false;
    }
    retriedResult.get();
    loops.stop();
    loops.join();
    bool recoveredAfterStop = false;
    auto stopped = worker.post([&recoveredAfterStop] {
        recoveredAfterStop = true;
    });
    if (stopped != ruvia::PostStatus::kWorkerStopping ||
        stopped.rejected() == nullptr) {
        return false;
    }
    auto stoppedTask = std::move(stopped).takeRejected();
    stoppedTask();
    return calls.load() == 11 && recoveredAfterStop;
}

bool testExternalEventLoopAttachment() {
    asio::io_context ioContext;
    {
        auto attachment = ruvia::attachEventLoop(
            ioContext,
            {.mailboxCapacity = 4});
        const auto loop = attachment.loop();
        if (!attachment.valid() || !loop.valid() ||
            &loop.ioContext() != &ioContext) {
            return false;
        }

        asio::ip::tcp::socket tcp(loop.executor());
        asio::ip::udp::socket udp(loop.executor());
        if (&tcp.get_executor().context() != &ioContext ||
            &udp.get_executor().context() != &ioContext) {
            return false;
        }

        bool duplicateRejected = false;
        try {
            auto duplicate = ruvia::attachEventLoop(ioContext);
        } catch (const std::invalid_argument&) {
            duplicateRejected = true;
        }
        if (!duplicateRejected) {
            return false;
        }

        std::promise<bool> completed;
        auto result = completed.get_future();
        std::atomic_bool stopCallbackRan{false};
        std::atomic_bool stopCallbackOnLoop{false};
        auto stopRegistration = loop.onStop([&] {
            stopCallbackOnLoop = loop.isCurrent();
            stopCallbackRan = true;
        });
        if (loop.post([loop, completed = std::move(completed)]() mutable {
                completed.set_value(loop.isCurrent());
            }) != ruvia::PostStatus::kAccepted) {
            return false;
        }

        std::thread externalThread([&] { ioContext.run(); });
        bool dispatchedOnExternalThread = false;
        try {
            dispatchedOnExternalThread = result.get();
            attachment.stop();
            externalThread.join();
        } catch (...) {
            attachment.stop();
            if (externalThread.joinable()) {
                externalThread.join();
            }
            throw;
        }
        if (!dispatchedOnExternalThread || !stopRegistration.valid() ||
            !stopCallbackRan || !stopCallbackOnLoop ||
            loop.post([] {}) != ruvia::PostStatus::kWorkerStopping) {
            return false;
        }
    }

    ioContext.restart();
    {
        auto attachment = ruvia::attachEventLoop(ioContext);
        attachment.stop();
        ioContext.run();
    }

    ioContext.restart();
    try {
        auto invalid = ruvia::attachEventLoop(
            ioContext,
            {.mailboxCapacity = 0});
    } catch (const std::invalid_argument&) {
        return true;
    }
    return false;
}

bool testFailurePropagation() {
    ruvia::EventLoopPool loops({.loopCount = 1, .mailboxCapacity = 1});
    const auto loop = loops.loop(0);
    std::atomic_bool stopCallbackRan{false};
    std::atomic_bool stopCallbackOnLoop{false};
    auto stopRegistration = loop.onStop([&] {
        stopCallbackOnLoop = loop.isCurrent();
        stopCallbackRan = true;
    });
    struct Listener final : ruvia::detail::WorkerShutdownListener {
        void workerStopping() noexcept override { notified = true; }
        bool notified{false};
    };
    const auto listener = std::make_shared<Listener>();
    ruvia::detail::WorkerHandleAccess::registerShutdownListener(
        loop.handle(), listener);
    if (loop.post([] { throw std::runtime_error("posted task failed"); }) !=
        ruvia::PostStatus::kAccepted) {
        return false;
    }
    loops.start();
    try {
        loops.join();
    } catch (const std::runtime_error& error) {
        return stopRegistration.valid() && stopCallbackRan &&
               stopCallbackOnLoop && listener->notified &&
               std::string_view(error.what()) == "posted task failed";
    }
    return false;
}

bool testJoinBeforeStartDrainsOnOwners() {
    ruvia::EventLoopPool loops({.loopCount = 2, .mailboxCapacity = 1});
    const auto first = loops.loop(0);
    const auto second = loops.loop(1);
    std::atomic<unsigned> taskCalls{0};
    std::atomic<unsigned> stopCalls{0};
    std::atomic_bool tasksOnOwners{true};
    std::atomic_bool stopsOnOwners{true};

    auto firstStop = first.onStop([&] {
        if (!first.isCurrent()) {
            stopsOnOwners.store(false, std::memory_order_relaxed);
        }
        stopCalls.fetch_add(1, std::memory_order_relaxed);
    });
    auto secondStop = second.onStop([&] {
        if (!second.isCurrent()) {
            stopsOnOwners.store(false, std::memory_order_relaxed);
        }
        stopCalls.fetch_add(1, std::memory_order_relaxed);
    });
    if (first.post([&] {
            if (!first.isCurrent()) {
                tasksOnOwners.store(false, std::memory_order_relaxed);
            }
            taskCalls.fetch_add(1, std::memory_order_relaxed);
        }) != ruvia::PostStatus::kAccepted ||
        second.post([&] {
            if (!second.isCurrent()) {
                tasksOnOwners.store(false, std::memory_order_relaxed);
            }
            taskCalls.fetch_add(1, std::memory_order_relaxed);
        }) != ruvia::PostStatus::kAccepted) {
        return false;
    }

    loops.join();
    const bool rejectedAfterJoin =
        first.post([] {}) == ruvia::PostStatus::kWorkerStopping &&
        second.post([] {}) == ruvia::PostStatus::kWorkerStopping;
    return firstStop.valid() && secondStop.valid() && rejectedAfterJoin &&
           taskCalls.load(std::memory_order_relaxed) == 2 &&
           stopCalls.load(std::memory_order_relaxed) == 2 &&
           tasksOnOwners.load(std::memory_order_relaxed) &&
           stopsOnOwners.load(std::memory_order_relaxed);
}

bool testStopBeforeStartPropagatesFailure() {
    ruvia::EventLoopPool loops({.loopCount = 1, .mailboxCapacity = 1});
    const auto loop = loops.loop(0);
    std::atomic_bool stopOnOwner{false};
    auto stopRegistration = loop.onStop([&] {
        stopOnOwner.store(loop.isCurrent(), std::memory_order_release);
    });
    if (loop.post([] {
            throw std::runtime_error("pre-start task failed");
        }) != ruvia::PostStatus::kAccepted) {
        return false;
    }

    loops.stop();
    try {
        loops.join();
    } catch (const std::runtime_error& error) {
        return stopRegistration.valid() &&
               stopOnOwner.load(std::memory_order_acquire) &&
               std::string_view(error.what()) == "pre-start task failed";
    }
    return false;
}

bool testJoinRejectsPoolWorker() {
    ruvia::EventLoopPool loops({.loopCount = 1, .mailboxCapacity = 1});
    const auto loop = loops.loop(0);
    std::promise<bool> completed;
    auto result = completed.get_future();
    if (loop.post([&] {
            bool rejected = false;
            try {
                loops.join();
            } catch (const std::logic_error& error) {
                rejected = std::string_view(error.what()) ==
                    "cannot join an event loop pool from one of its workers";
            }
            completed.set_value(rejected && loop.isCurrent());
        }) != ruvia::PostStatus::kAccepted) {
        return false;
    }

    loops.start();
    const bool rejected = result.get();
    loops.stop();
    loops.join();
    return rejected;
}

bool testExecutorFailureDrainsShutdownOnOwners() {
    struct AbandonProbe final {
        explicit AbandonProbe(std::atomic_bool& destroyed) noexcept
            : destroyed_(&destroyed) {}
        ~AbandonProbe() {
            destroyed_->store(true, std::memory_order_release);
        }
        std::atomic_bool* destroyed_;
    };

    ruvia::EventLoopPool loops({.loopCount = 2, .mailboxCapacity = 2});
    const auto failedLoop = loops.loop(0);
    const auto peerLoop = loops.loop(1);
    std::atomic<unsigned> failedStopCalls{0};
    std::atomic<unsigned> peerStopCalls{0};
    std::atomic_bool failedStopOnOwner{false};
    std::atomic_bool peerStopOnOwner{false};
    std::atomic_bool shutdownContinuationDrained{false};
    std::atomic_bool abandonedMailboxRan{false};
    std::atomic_bool abandonedMailboxDestroyed{false};

    auto failedStop = failedLoop.onStop([&] {
        failedStopOnOwner.store(
            failedLoop.isCurrent(), std::memory_order_release);
        failedStopCalls.fetch_add(1, std::memory_order_relaxed);
        asio::post(failedLoop.ioContext(), [] {
            throw std::runtime_error("secondary shutdown handler failed");
        });
        asio::post(failedLoop.ioContext(), [&] {
            shutdownContinuationDrained.store(
                failedLoop.isCurrent(), std::memory_order_release);
        });
    });
    auto peerStop = peerLoop.onStop([&] {
        peerStopOnOwner.store(
            peerLoop.isCurrent(), std::memory_order_release);
        peerStopCalls.fetch_add(1, std::memory_order_relaxed);
    });

    asio::post(failedLoop.ioContext(), [] {
        throw std::runtime_error("executor handler failed");
    });
    if (failedLoop.post(
            [probe = std::make_unique<AbandonProbe>(
                 abandonedMailboxDestroyed),
             &abandonedMailboxRan] {
                abandonedMailboxRan.store(true, std::memory_order_release);
            }) != ruvia::PostStatus::kAccepted) {
        return false;
    }

    loops.start();
    try {
        loops.join();
    } catch (const std::runtime_error& error) {
        return failedStop.valid() && peerStop.valid() &&
               std::string_view(error.what()) == "executor handler failed" &&
               failedStopCalls.load(std::memory_order_relaxed) == 1 &&
               peerStopCalls.load(std::memory_order_relaxed) == 1 &&
               failedStopOnOwner.load(std::memory_order_acquire) &&
               peerStopOnOwner.load(std::memory_order_acquire) &&
               shutdownContinuationDrained.load(std::memory_order_acquire) &&
               !abandonedMailboxRan.load(std::memory_order_acquire) &&
               abandonedMailboxDestroyed.load(std::memory_order_acquire);
    }
    return false;
}

bool testExpiredHandle() {
    ruvia::EventLoop loop;
    {
        ruvia::EventLoopPool loops({.loopCount = 1, .mailboxCapacity = 1});
        loop = loops.loop(0);
    }
    return loop.valid() && !loop.accepting() &&
           loop.post([] {}) == ruvia::PostStatus::kWorkerStopping;
}

bool testEscapedWorkerHandleBecomesDetachedEndpoint() {
    ruvia::WorkerHandle worker;
    ruvia::WorkerId liveId = 0;
    {
        ruvia::EventLoopPool loops({.loopCount = 1, .mailboxCapacity = 1});
        worker = loops.loop(0).handle();
        liveId = worker.id();
        if (!worker.valid() || liveId == 0) {
            return false;
        }
    }

    bool internalDeferRejected = false;
    try {
        ruvia::detail::WorkerHandleAccess::defer(worker, [] {});
    } catch (const std::runtime_error&) {
        internalDeferRejected = true;
    }
    return !worker.valid() && !worker.accepting() && !worker.isCurrent() &&
           worker.id() == 0 &&
           worker.post([] {}) == ruvia::PostStatus::kWorkerStopping &&
           internalDeferRejected;
}

bool testFailureDestroysAbandonedMailboxTasks() {
    struct DestructionProbe final {
        explicit DestructionProbe(bool& value) noexcept
            : destroyed(&value) {}
        bool* destroyed;
        ~DestructionProbe() { *destroyed = true; }
    };

    asio::io_context ioContext;
    const auto dispatcher =
        std::make_shared<ruvia::detail::WorkerDispatcher>(ioContext, 2);
    const auto worker = ruvia::detail::WorkerHandleAccess::make(dispatcher);
    bool queuedTaskDestroyed = false;
    if (worker.post([] { throw std::runtime_error("stop mailbox drain"); }) !=
            ruvia::PostStatus::kAccepted ||
        worker.post(
            [probe = std::make_unique<DestructionProbe>(queuedTaskDestroyed)] {}) !=
            ruvia::PostStatus::kAccepted) {
        return false;
    }
    try {
        ioContext.run();
    } catch (const std::runtime_error&) {
    }
    if (!queuedTaskDestroyed) {
        return false;
    }
    dispatcher->detachContext();
    return queuedTaskDestroyed && !worker.valid();
}

bool testDispatcherLifecycleHooksAreWorkerAffine() {
    asio::io_context ioContext;
    const auto dispatcher =
        std::make_shared<ruvia::detail::WorkerDispatcher>(ioContext, 1);
    const auto worker = ruvia::detail::WorkerHandleAccess::make(dispatcher);
    bool startupOnWorker = false;
    bool failureOnWorker = false;
    bool shutdownOnWorker = false;
    bool receivedStartupFailure = false;

    dispatcher->runContext(
        [&] {
            startupOnWorker = worker.isCurrent();
            throw std::runtime_error("worker startup failed");
        },
        [&](std::exception_ptr failure) noexcept {
            failureOnWorker = worker.isCurrent();
            try {
                std::rethrow_exception(failure);
            } catch (const std::runtime_error& error) {
                receivedStartupFailure =
                    std::string_view(error.what()) == "worker startup failed";
            } catch (...) {
            }
        },
        [&]() noexcept { shutdownOnWorker = worker.isCurrent(); });
    dispatcher->detachContext();
    return startupOnWorker && failureOnWorker && shutdownOnWorker &&
           receivedStartupFailure;
}

// A stop callback runs after every caller that could have received its
// exception is gone. Dropping it would make a failed cleanup invisible, so the
// pool records it as its first failure and join() rethrows it.
bool testStopCallbackFailureReachesJoin() {
    ruvia::EventLoopPool loops({.loopCount = 1, .mailboxCapacity = 2});
    const auto loop = loops.loop(0);
    std::atomic<unsigned> stopCalls{0};
    auto stopRegistration = loop.onStop([&] {
        stopCalls.fetch_add(1, std::memory_order_relaxed);
        throw std::runtime_error("stop callback failed");
    });

    loops.start();
    loops.stop();

    bool rethrown = false;
    try {
        loops.join();
    } catch (const std::runtime_error& error) {
        rethrown = std::string_view(error.what()) == "stop callback failed";
    } catch (...) {
    }
    return rethrown && stopCalls.load(std::memory_order_relaxed) == 1;
}

bool testLifecycleTransitionsAreMonotonic() {
    using Lifecycle = ruvia::detail::RuntimeLifecycle;
    Lifecycle lifecycle;
    lifecycle.completeStop();
    if (lifecycle.state() != Lifecycle::State::kReady ||
        !lifecycle.start()) {
        return false;
    }
    lifecycle.completeStop();
    if (lifecycle.state() != Lifecycle::State::kRunning ||
        lifecycle.start() ||
        !lifecycle.requestStop() ||
        lifecycle.state() != Lifecycle::State::kStopping ||
        lifecycle.requestStop()) {
        return false;
    }

    lifecycle.completeStop();
    return lifecycle.state() == Lifecycle::State::kStopped &&
           !lifecycle.requestStop() &&
           lifecycle.state() == Lifecycle::State::kStopped &&
           !lifecycle.start();
}

bool testConcurrentStopHasOneInitiator() {
    using Lifecycle = ruvia::detail::RuntimeLifecycle;
    constexpr std::size_t kThreadCount = 16;
    Lifecycle lifecycle;
    if (!lifecycle.start()) {
        return false;
    }

    std::atomic<std::size_t> initiators{0};
    std::mutex gateMutex;
    std::condition_variable gateChanged;
    bool start = false;
    std::vector<std::thread> threads;
    threads.reserve(kThreadCount);
    try {
        for (std::size_t i = 0; i < kThreadCount; ++i) {
            threads.emplace_back([&] {
                {
                    std::unique_lock lock(gateMutex);
                    gateChanged.wait(lock, [&] { return start; });
                }
                if (lifecycle.requestStop()) {
                    initiators.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }
    } catch (...) {
        {
            std::lock_guard lock(gateMutex);
            start = true;
        }
        gateChanged.notify_all();
        for (auto& thread : threads) {
            if (thread.joinable()) {
                thread.join();
            }
        }
        throw;
    }
    {
        std::lock_guard lock(gateMutex);
        start = true;
    }
    gateChanged.notify_all();
    for (auto& thread : threads) {
        thread.join();
    }

    lifecycle.completeStop();
    return initiators.load(std::memory_order_relaxed) == 1 &&
           lifecycle.state() == Lifecycle::State::kStopped &&
           !lifecycle.requestStop();
}

}

int main() {
    const auto run = [](const char* name, bool (*test)()) {
        std::printf("[ RUN ] %s\n", name);
        std::fflush(stdout);
        const bool passed = test();
        std::printf("[%s] %s\n", passed ? " ok " : "FAIL", name);
        std::fflush(stdout);
        return passed;
    };
    return run("worker_signal_is_worker_affine", testWorkerSignalIsWorkerAffine) &&
               run(
                   "worker_signal_has_no_waiter_limit",
                   testWorkerSignalHasNoArbitraryWaiterLimit) &&
               run(
                   "worker_signal_rechecks_cold_wait_affinity",
                   testWorkerSignalRechecksAffinityWhenColdWaitStarts) &&
               run("dispatch_and_affinity", testDispatchAndAffinity) &&
               run("bounded_mailbox", testBoundedMailbox) &&
               run("external_event_loop_attachment", testExternalEventLoopAttachment) &&
               run("failure_propagation", testFailurePropagation) &&
               run(
                   "join_before_start_drains_on_owners",
                   testJoinBeforeStartDrainsOnOwners) &&
               run(
                   "stop_before_start_propagates_failure",
                   testStopBeforeStartPropagatesFailure) &&
               run("join_rejects_pool_worker", testJoinRejectsPoolWorker) &&
               run(
                   "executor_failure_drains_shutdown_on_owners",
                   testExecutorFailureDrainsShutdownOnOwners) &&
               run("expired_handle", testExpiredHandle) &&
               run(
                   "escaped_worker_handle_detaches",
                   testEscapedWorkerHandleBecomesDetachedEndpoint) &&
               run(
                   "failure_destroys_abandoned_mailbox_tasks",
                   testFailureDestroysAbandonedMailboxTasks) &&
               run(
                   "dispatcher_lifecycle_hooks_are_worker_affine",
                   testDispatcherLifecycleHooksAreWorkerAffine) &&
               run(
                   "stop_callback_failure_reaches_join",
                   testStopCallbackFailureReachesJoin) &&
               run(
                   "lifecycle_transitions_are_monotonic",
                   testLifecycleTransitionsAreMonotonic) &&
               run(
                   "concurrent_stop_has_one_initiator",
                   testConcurrentStopHasOneInitiator)
        ? 0
        : 1;
}
