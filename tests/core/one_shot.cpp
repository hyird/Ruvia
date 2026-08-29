#include <ruvia/core/OneShot.h>
#include <ruvia/core/TaskScope.h>
#include <ruvia/core/detail/io/AsioAwait.h>
#include <ruvia/core/detail/worker/WorkerDispatcher.h>

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/post.hpp>

#include <chrono>
#include <concepts>
#include <memory>
#include <memory_resource>
#include <semaphore>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>

template <typename T>
concept HasRvalueWorkerWaitValue = requires(T&& result) { std::move(result).value(); };

template <typename T>
concept HasRvalueWorkerBorrow = requires(T&& receiver) { std::move(receiver).worker(); };

template <typename T>
concept HasRvalueOneShotWait = requires(T&& receiver) { std::move(receiver).wait(); };

template <typename T>
concept HasRvalueOneShotCancellableWait = requires(
    T&& receiver, ruvia::StopToken stopToken) { std::move(receiver).wait(std::move(stopToken)); };

template <typename T>
concept HasRvalueOneShotTimedWait =
    requires(T&& receiver) { std::move(receiver).waitFor(std::chrono::seconds(1)); };

template <typename T>
concept HasRvalueOneShotTimedCancellableWait = requires(T&& receiver, ruvia::StopToken stopToken) {
    std::move(receiver).waitFor(std::chrono::seconds(1), std::move(stopToken));
};

template <typename T>
concept HasPositionalOneShotResourceFactory = requires(ruvia::WorkerHandle worker,
    std::pmr::memory_resource* resource) { ruvia::makeOneShot<T>(worker, resource); };

template <typename T>
concept HasOneShotOptionsFactory =
    requires(ruvia::WorkerHandle worker, std::pmr::memory_resource* resource) {
        ruvia::makeOneShot<T>(worker, ruvia::OneShotOptions{.resource = resource});
    };

static_assert(!std::is_default_constructible_v<ruvia::OneShotReceiver<int>>);
static_assert(std::is_move_constructible_v<ruvia::OneShotReceiver<int>>);
static_assert(!std::is_move_assignable_v<ruvia::OneShotReceiver<int>>);
static_assert(!HasRvalueWorkerWaitValue<ruvia::WorkerWaitResult<int>>);
static_assert(!HasRvalueWorkerBorrow<ruvia::OneShotReceiver<int>>);
static_assert(!HasRvalueOneShotWait<ruvia::OneShotReceiver<int>>);
static_assert(!HasRvalueOneShotCancellableWait<ruvia::OneShotReceiver<int>>);
static_assert(!HasRvalueOneShotTimedWait<ruvia::OneShotReceiver<int>>);
static_assert(!HasRvalueOneShotTimedCancellableWait<ruvia::OneShotReceiver<int>>);
static_assert(std::is_aggregate_v<ruvia::OneShotOptions>);
static_assert(std::same_as<decltype(ruvia::OneShotOptions{}.resource), std::pmr::memory_resource*>);
static_assert(HasOneShotOptionsFactory<int>);
static_assert(!HasPositionalOneShotResourceFactory<int>);
static_assert(std::is_same_v<decltype(std::declval<const ruvia::WorkerWaitResult<int>&>().status()),
    ruvia::WorkerWaitStatus>);
static_assert(std::is_same_v<decltype(std::declval<const ruvia::WorkerWaitResult<int>&>().value()),
    const int&>);
static_assert(!std::is_constructible_v<ruvia::WorkerWaitResult<int>, ruvia::WorkerWaitStatus>);

namespace {

class ThrowingMove final {
public:
    explicit ThrowingMove(int value) noexcept
        : value_(value) {}

    ThrowingMove(const ThrowingMove&) = delete;
    ThrowingMove& operator=(const ThrowingMove&) = delete;
    // This fixture intentionally models a move that can throw.
    ThrowingMove(ThrowingMove&& other) noexcept(false) {
        if (throwOnMove) {
            throw std::runtime_error("requested move failure");
        }
        value_ = std::exchange(other.value_, 0);
    }

    [[nodiscard]] int value() const noexcept {
        return value_;
    }

    static inline bool throwOnMove{false};

private:
    int value_{0};
};

ruvia::Task<void> waitForValue(ruvia::OneShotReceiver<int>& receiver, int expected, bool& success) {
    const auto result = co_await receiver.waitFor(std::chrono::seconds(1));
    success = result.status() == ruvia::WorkerWaitStatus::kValue && result.hasValue() &&
              result.value() == expected;
}

ruvia::Task<void> waitForWorkerStopping(ruvia::OneShotReceiver<int>& receiver, bool& success) {
    const auto result = co_await receiver.wait();
    success = result.status() == ruvia::WorkerWaitStatus::kWorkerStopping && !result.hasValue();
}

ruvia::Task<ruvia::WorkerWaitResult<int>> makeColdWaitAfterReceiverClose(
    ruvia::WorkerHandle worker, bool timed) {
    auto [completion, receiver] = ruvia::makeOneShot<int>(std::move(worker));
    if (timed) {
        return receiver.waitFor(std::chrono::seconds(1));
    }
    return receiver.wait();
}

ruvia::Task<void> verifyColdReceiverTasks(ruvia::Task<ruvia::WorkerWaitResult<int>> wait,
    ruvia::Task<ruvia::WorkerWaitResult<int>> timedWait, bool& success) {
    const auto coldClosed = co_await std::move(wait);
    const auto timedColdClosed = co_await std::move(timedWait);
    success = coldClosed.status() == ruvia::WorkerWaitStatus::kClosed &&
              timedColdClosed.status() == ruvia::WorkerWaitStatus::kClosed;
}

ruvia::Task<void> exercise(ruvia::WorkerHandle worker, bool& success) {
    {
        auto [completion, receiver] = ruvia::makeOneShot<ThrowingMove>(worker);
        bool moveFailed = false;
        ThrowingMove::throwOnMove = true;
        try {
            static_cast<void>(completion.complete(ThrowingMove(5)));
        } catch (const std::runtime_error&) {
            moveFailed = true;
        }
        ThrowingMove::throwOnMove = false;
        if (!moveFailed || !completion.complete(ThrowingMove(6)).accepted()) {
            co_return;
        }
        const auto result = co_await receiver.wait();
        if (!result.hasValue() || result.value().value() != 6) {
            co_return;
        }
    }

    {
        auto [completion, receiver] = ruvia::makeOneShot<int>(worker);
        const auto completed = completion.complete(7);
        const auto duplicate = completion.complete(8);
        if (!completed.accepted() ||
            duplicate.status() != ruvia::OneShotCompleteStatus::kAlreadyCompleted ||
            duplicate.rejected() == nullptr || *duplicate.rejected() != 8) {
            co_return;
        }
        const auto result = co_await receiver.wait();
        if (!result.hasValue() || result.value() != 7) {
            co_return;
        }
    }

    {
        auto [completion, receiver] = ruvia::makeOneShot<int>(worker);
        const auto timeout = co_await receiver.waitFor(std::chrono::milliseconds(1));
        if (timeout.status() != ruvia::WorkerWaitStatus::kTimedOut ||
            !completion.complete(9).accepted()) {
            co_return;
        }
        const auto late = co_await receiver.wait();
        if (!late.hasValue() || late.value() != 9) {
            co_return;
        }
    }

    {
        ruvia::StopSource source;
        auto [completion, receiver] = ruvia::makeOneShot<int>(worker);
        ruvia::detail::WorkerHandleAccess::defer(worker, [&source] { source.requestStop(); });
        const auto cancelled = co_await receiver.wait(source.token());
        if (cancelled.status() != ruvia::WorkerWaitStatus::kCancelled ||
            !completion.complete(11).accepted()) {
            co_return;
        }
        const auto late = co_await receiver.wait();
        if (!late.hasValue() || late.value() != 11) {
            co_return;
        }
    }

    {
        ruvia::StopSource source;
        source.requestStop();
        auto [completion, receiver] = ruvia::makeOneShot<int>(worker);
        const auto cancelled = co_await receiver.waitFor(std::chrono::seconds(1), source.token());
        if (cancelled.status() != ruvia::WorkerWaitStatus::kCancelled ||
            !completion.complete(12).accepted()) {
            co_return;
        }
        const auto late = co_await receiver.wait();
        if (!late.hasValue() || late.value() != 12) {
            co_return;
        }
    }

    {
        auto [completion, receiver] = ruvia::makeOneShot<int>(worker);
        receiver.close();
        const auto closed = co_await receiver.wait();
        const auto rejected = completion.complete(10);
        if (closed.status() != ruvia::WorkerWaitStatus::kClosed ||
            rejected.status() != ruvia::OneShotCompleteStatus::kReceiverClosed ||
            rejected.rejected() == nullptr || *rejected.rejected() != 10) {
            co_return;
        }
    }

    auto [completion, receiver] = ruvia::makeOneShot<int>(worker);
    auto activeReceiver = std::move(receiver);
    ruvia::TaskScope scope(worker);
    scope.spawn(waitForValue(activeReceiver, 42, success));
    if (!completion.complete(42).accepted()) {
        co_return;
    }
    co_await scope.join();
}

}  // namespace

int main() {
    bool success = false;
    bool coldReceiverTasksSafe = false;
    {
        asio::io_context ioContext;
        const auto dispatcher = std::make_shared<ruvia::detail::WorkerDispatcher>(ioContext, 8);
        const auto worker = ruvia::detail::WorkerHandleAccess::make(dispatcher);
        auto coldWait = makeColdWaitAfterReceiverClose(worker, false);
        auto timedColdWait = makeColdWaitAfterReceiverClose(worker, true);
        asio::co_spawn(
            ioContext, ruvia::detail::taskAsAwaitable(exercise(worker, success)), asio::detached);
        asio::co_spawn(ioContext,
            ruvia::detail::taskAsAwaitable(verifyColdReceiverTasks(
                std::move(coldWait), std::move(timedColdWait), coldReceiverTasksSafe)),
            asio::detached);
        ioContext.run();
        dispatcher->close();
        dispatcher->stopTimers();
    }

    bool workerStopping = false;
    {
        asio::io_context ioContext;
        const auto dispatcher = std::make_shared<ruvia::detail::WorkerDispatcher>(ioContext, 8);
        const auto worker = ruvia::detail::WorkerHandleAccess::make(dispatcher);
        auto [completion, receiver] = ruvia::makeOneShot<int>(worker);
        asio::co_spawn(ioContext,
            ruvia::detail::taskAsAwaitable(waitForWorkerStopping(receiver, workerStopping)),
            asio::detached);
        asio::post(ioContext, [dispatcher] { dispatcher->close(); });
        ioContext.run();
        dispatcher->stopTimers();
    }

    bool crossThreadValue = false;
    bool crossThreadCompleted = false;
    {
        asio::io_context ioContext;
        const auto dispatcher = std::make_shared<ruvia::detail::WorkerDispatcher>(ioContext, 8);
        const auto worker = ruvia::detail::WorkerHandleAccess::make(dispatcher);
        auto [completion, receiver] = ruvia::makeOneShot<int>(worker);
        std::binary_semaphore receiverScheduled{0};
        asio::co_spawn(ioContext,
            ruvia::detail::taskAsAwaitable(waitForValue(receiver, 77, crossThreadValue)),
            asio::detached);
        asio::post(ioContext, [&receiverScheduled] { receiverScheduled.release(); });
        std::thread completer([&] {
            receiverScheduled.acquire();
            crossThreadCompleted = completion.complete(77).accepted();
        });
        ioContext.run();
        completer.join();
        dispatcher->close();
        dispatcher->stopTimers();
    }
    const bool allPassed = success && coldReceiverTasksSafe && workerStopping && crossThreadValue &&
                           crossThreadCompleted;
    return allPassed ? 0 : 1;
}
