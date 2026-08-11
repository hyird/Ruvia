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
#include <semaphore>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>

template <typename T>
concept HasAnyRvalueWorkerWaitAccessor = requires(T&& result) { std::move(result).value(); } || requires(T&& result) { std::move(result).closed(); } || requires(T&& result) { std::move(result).workerStopping(); } || requires(T&& result) { std::move(result).timedOut(); } || requires(T&& result) { std::move(result).cancelled(); };

template <typename T>
concept HasRvalueWorkerBorrow = requires(T&& receiver) { std::move(receiver).worker(); };

static_assert(!std::is_default_constructible_v<ruvia::OneShotReceiver<int>>);
static_assert(std::is_move_constructible_v<ruvia::OneShotReceiver<int>>);
static_assert(!std::is_move_assignable_v<ruvia::OneShotReceiver<int>>);
static_assert(!HasAnyRvalueWorkerWaitAccessor<ruvia::WorkerWaitResult<int>>);
static_assert(!HasRvalueWorkerBorrow<ruvia::OneShotReceiver<int>>);

namespace {

class ThrowingMove final {
public:
    explicit ThrowingMove(int value) noexcept
        : value_(value) {}

    ThrowingMove(const ThrowingMove&) = delete;
    ThrowingMove& operator=(const ThrowingMove&) = delete;
    ThrowingMove(ThrowingMove&& other) {
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
    success = result.value() != nullptr && *result.value() == expected;
}

ruvia::Task<void> waitForWorkerStopping(ruvia::OneShotReceiver<int>& receiver, bool& success) {
    const auto result = co_await receiver.wait();
    success = result.workerStopping() != nullptr;
}

ruvia::Task<ruvia::WorkerWaitResult<int>> makeColdWaitAfterReceiverClose(ruvia::WorkerHandle worker, bool timed) {
    auto [completion, receiver] = ruvia::makeOneShot<int>(std::move(worker));
    if (timed) {
        return receiver.waitFor(std::chrono::seconds(1));
    }
    return receiver.wait();
}

ruvia::Task<void> verifyColdReceiverTasks(ruvia::Task<ruvia::WorkerWaitResult<int>> wait, ruvia::Task<ruvia::WorkerWaitResult<int>> timedWait, bool& success) {
    const auto coldClosed = co_await std::move(wait);
    const auto timedColdClosed = co_await std::move(timedWait);
    success = coldClosed.closed() != nullptr && timedColdClosed.closed() != nullptr;
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
        if (result.value() == nullptr || result.value()->value() != 6) {
            co_return;
        }
    }

    {
        auto [completion, receiver] = ruvia::makeOneShot<int>(worker);
        const auto completed = completion.complete(7);
        const auto duplicate = completion.complete(8);
        if (!completed.accepted() || duplicate.status() != ruvia::OneShotCompleteStatus::kAlreadyCompleted || duplicate.rejected() == nullptr || *duplicate.rejected() != 8) {
            co_return;
        }
        const auto result = co_await receiver.wait();
        if (result.value() == nullptr || *result.value() != 7) {
            co_return;
        }
    }

    {
        auto [completion, receiver] = ruvia::makeOneShot<int>(worker);
        const auto timeout = co_await receiver.waitFor(std::chrono::milliseconds(1));
        if (timeout.timedOut() == nullptr || !completion.complete(9).accepted()) {
            co_return;
        }
        const auto late = co_await receiver.wait();
        if (late.value() == nullptr || *late.value() != 9) {
            co_return;
        }
    }

    {
        ruvia::detail::StopSource source;
        auto [completion, receiver] = ruvia::makeOneShot<int>(worker);
        ruvia::detail::WorkerHandleAccess::defer(worker, [&source] { source.requestStop(); });
        const auto cancelled = co_await receiver.wait(source.token());
        if (cancelled.cancelled() == nullptr || !completion.complete(11).accepted()) {
            co_return;
        }
        const auto late = co_await receiver.wait();
        if (late.value() == nullptr || *late.value() != 11) {
            co_return;
        }
    }

    {
        ruvia::detail::StopSource source;
        source.requestStop();
        auto [completion, receiver] = ruvia::makeOneShot<int>(worker);
        const auto cancelled = co_await receiver.waitFor(std::chrono::seconds(1), source.token());
        if (cancelled.cancelled() == nullptr || !completion.complete(12).accepted()) {
            co_return;
        }
        const auto late = co_await receiver.wait();
        if (late.value() == nullptr || *late.value() != 12) {
            co_return;
        }
    }

    {
        auto [completion, receiver] = ruvia::makeOneShot<int>(worker);
        receiver.close();
        const auto closed = co_await receiver.wait();
        const auto rejected = completion.complete(10);
        if (closed.closed() == nullptr || rejected.status() != ruvia::OneShotCompleteStatus::kReceiverClosed || rejected.rejected() == nullptr || *rejected.rejected() != 10) {
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
        asio::co_spawn(ioContext, ruvia::detail::taskAsAwaitable(exercise(worker, success)), asio::detached);
        asio::co_spawn(ioContext, ruvia::detail::taskAsAwaitable(verifyColdReceiverTasks(std::move(coldWait), std::move(timedColdWait), coldReceiverTasksSafe)), asio::detached);
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
        asio::co_spawn(ioContext, ruvia::detail::taskAsAwaitable(waitForWorkerStopping(receiver, workerStopping)), asio::detached);
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
        asio::co_spawn(ioContext, ruvia::detail::taskAsAwaitable(waitForValue(receiver, 77, crossThreadValue)), asio::detached);
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
    const bool allPassed = success && coldReceiverTasksSafe && workerStopping && crossThreadValue && crossThreadCompleted;
    return allPassed ? 0 : 1;
}
