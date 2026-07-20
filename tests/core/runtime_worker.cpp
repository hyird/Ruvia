#include <ruvia/core/EventLoopPool.h>
#include <ruvia/core/detail/AsioAwait.h>
#include <ruvia/core/detail/RuntimeLifecycle.h>
#include <ruvia/core/detail/WorkerDispatcher.h>
#include <ruvia/core/detail/WorkerSelection.h>
#include <ruvia/core/detail/WorkerSignal.h>

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/ip/udp.hpp>

#include <atomic>
#include <array>
#include <barrier>
#include <future>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

namespace {

ruvia::Task<void> waitForSignal(
    ruvia::detail::WorkerSignal& signal,
    bool& resumed) {
    {
        auto discardedColdWait = signal.wait();
        static_cast<void>(discardedColdWait);
    }
    co_await signal.wait();
    resumed = true;
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
    asio::co_spawn(
        ioContext,
        ruvia::detail::taskAsAwaitable(
            waitForSignal(firstSignal, firstResumed)),
        asio::detached);
    asio::co_spawn(
        ioContext,
        ruvia::detail::taskAsAwaitable(
            waitForSignal(secondSignal, secondResumed)),
        asio::detached);
    asio::post(ioContext, [&] {
        firstSignal.notify();
        secondSignal.notify();
        attachment.stop();
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
    for (std::size_t index = 0; index < resumed.size(); ++index) {
        asio::co_spawn(
            ioContext,
            ruvia::detail::taskAsAwaitable(
                waitForSignal(signal, resumed[index])),
            asio::detached);
    }

    asio::post(ioContext, [&] {
        signal.notify();
        attachment.stop();
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
        }) != ruvia::PostResult::kAccepted) {
        return false;
    }

    loops.start();
    const bool success = result.get();
    loops.stop();
    loops.join();
    return success && stopRegistration.valid() && stopCallbackRan &&
           stopCallbackOnLoop &&
           first.post([] {}) == ruvia::PostResult::kWorkerStopping;
}

bool testBoundedMailbox() {
    ruvia::EventLoopPool loops({.loopCount = 1, .mailboxCapacity = 2});
    const auto worker = loops.loop(0);
    std::atomic<int> calls{0};
    std::promise<void> completed;
    auto result = completed.get_future();

    if (worker.post([&] { ++calls; }) != ruvia::PostResult::kAccepted ||
        worker.post([&] {
            ++calls;
            completed.set_value();
        }) != ruvia::PostResult::kAccepted ||
        worker.post([] {}) != ruvia::PostResult::kQueueFull) {
        return false;
    }

    loops.start();
    result.get();
    loops.stop();
    loops.join();
    return calls.load() == 2;
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
            }) != ruvia::PostResult::kAccepted) {
            return false;
        }

        std::thread externalThread([&] { ioContext.run(); });
        const bool dispatchedOnExternalThread = result.get();
        attachment.stop();
        externalThread.join();
        if (!dispatchedOnExternalThread || !stopRegistration.valid() ||
            !stopCallbackRan || !stopCallbackOnLoop ||
            loop.post([] {}) != ruvia::PostResult::kWorkerStopping) {
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
        ruvia::PostResult::kAccepted) {
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

bool testExpiredHandle() {
    ruvia::EventLoop loop;
    {
        ruvia::EventLoopPool loops({.loopCount = 1, .mailboxCapacity = 1});
        loop = loops.loop(0);
    }
    return loop.valid() && !loop.accepting() &&
           loop.post([] {}) == ruvia::PostResult::kWorkerStopping;
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
           worker.post([] {}) == ruvia::PostResult::kWorkerStopping &&
           internalDeferRejected;
}

bool testDetachDestroysAbandonedMailboxTasks() {
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
            ruvia::PostResult::kAccepted ||
        worker.post(
            [probe = std::make_unique<DestructionProbe>(queuedTaskDestroyed)] {}) !=
            ruvia::PostResult::kAccepted) {
        return false;
    }
    try {
        ioContext.run();
    } catch (const std::runtime_error&) {
    }
    if (queuedTaskDestroyed) {
        return false;
    }
    dispatcher->detachContext();
    return queuedTaskDestroyed && !worker.valid();
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
    std::barrier gate(kThreadCount + 1);
    {
        std::vector<std::thread> threads;
        threads.reserve(kThreadCount);
        for (std::size_t i = 0; i < kThreadCount; ++i) {
            threads.emplace_back([&] {
                gate.arrive_and_wait();
                if (lifecycle.requestStop()) {
                    initiators.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }
        gate.arrive_and_wait();
        for (auto& thread : threads) {
            thread.join();
        }
    }

    lifecycle.completeStop();
    return initiators.load(std::memory_order_relaxed) == 1 &&
           lifecycle.state() == Lifecycle::State::kStopped &&
           !lifecycle.requestStop();
}

}

int main() {
    return testWorkerSignalIsWorkerAffine() &&
               testWorkerSignalHasNoArbitraryWaiterLimit() &&
               testWorkerSignalRechecksAffinityWhenColdWaitStarts() &&
               testDispatchAndAffinity() && testBoundedMailbox() &&
               testExternalEventLoopAttachment() &&
               testFailurePropagation() && testExpiredHandle() &&
               testEscapedWorkerHandleBecomesDetachedEndpoint() &&
               testDetachDestroysAbandonedMailboxTasks() &&
               testLifecycleTransitionsAreMonotonic() &&
               testConcurrentStopHasOneInitiator()
               ? 0
               : 1;
}
