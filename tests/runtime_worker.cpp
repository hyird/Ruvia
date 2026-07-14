#include <ruvia/core/Runtime.h>
#include <ruvia/core/detail/WorkerDispatcher.h>
#include <ruvia/core/detail/WorkerSelection.h>

#include <atomic>
#include <future>
#include <memory>
#include <stdexcept>
#include <string_view>

namespace {

bool testDispatchAndAffinity() {
    ruvia::Runtime runtime({.workerCount = 2, .mailboxCapacity = 4});
    const auto first = runtime.worker(0);
    const auto second = runtime.worker(1);
    if (!first.valid() || first.id() == 0 || first.id() == second.id() || first.isCurrent()) {
        return false;
    }
    constexpr std::string_view key = "device-42";
    if (runtime.workerFor(key).id() !=
        runtime.workerFor(ruvia::detail::workerSelectionHash(key)).id()) {
        return false;
    }

    std::promise<bool> completed;
    auto result = completed.get_future();
    auto moveOnly = std::make_unique<int>(42);
    if (first.post([worker = first,
                    value = std::move(moveOnly),
                    completed = std::move(completed)]() mutable {
            completed.set_value(worker.isCurrent() && *value == 42);
        }) != ruvia::PostResult::kAccepted) {
        return false;
    }

    runtime.start();
    const bool success = result.get();
    runtime.stop();
    runtime.join();
    return success && first.post([] {}) == ruvia::PostResult::kWorkerStopping;
}

bool testBoundedMailbox() {
    ruvia::Runtime runtime({.workerCount = 1, .mailboxCapacity = 2});
    const auto worker = runtime.worker(0);
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

    runtime.start();
    result.get();
    runtime.stop();
    runtime.join();
    return calls.load() == 2;
}

bool testFailurePropagation() {
    ruvia::Runtime runtime({.workerCount = 1, .mailboxCapacity = 1});
    struct Listener final : ruvia::detail::WorkerShutdownListener {
        void workerStopping() noexcept override { notified = true; }
        bool notified{false};
    };
    const auto listener = std::make_shared<Listener>();
    ruvia::detail::WorkerHandleAccess::registerShutdownListener(
        runtime.worker(0), listener);
    if (runtime.worker(0).post([] { throw std::runtime_error("posted task failed"); }) !=
        ruvia::PostResult::kAccepted) {
        return false;
    }
    runtime.start();
    try {
        runtime.join();
    } catch (const std::runtime_error& error) {
        return listener->notified &&
               std::string_view(error.what()) == "posted task failed";
    }
    return false;
}

bool testExpiredHandle() {
    ruvia::WorkerHandle handle;
    {
        ruvia::Runtime runtime({.workerCount = 1, .mailboxCapacity = 1});
        handle = runtime.worker(0);
    }
    return !handle.valid() && handle.post([] {}) == ruvia::PostResult::kWorkerStopping;
}

}

int main() {
    return testDispatchAndAffinity() && testBoundedMailbox() &&
                   testFailurePropagation() && testExpiredHandle()
               ? 0
               : 1;
}
