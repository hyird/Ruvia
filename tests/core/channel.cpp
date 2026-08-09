#include <ruvia/core/Channel.h>
#include <ruvia/core/TaskScope.h>
#include <ruvia/core/detail/io/AsioAwait.h>
#include <ruvia/core/detail/worker/WorkerDispatcher.h>

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/post.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <semaphore>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>

static_assert(!std::is_default_constructible_v<ruvia::ChannelReceiver<int>>);
static_assert(std::is_move_constructible_v<ruvia::ChannelReceiver<int>>);
static_assert(!std::is_move_assignable_v<ruvia::ChannelReceiver<int>>);

namespace {

class ThrowingMove final {
public:
    explicit ThrowingMove(int value) noexcept
        : value_(value) {}

    ThrowingMove(const ThrowingMove&) = delete;
    ThrowingMove& operator=(const ThrowingMove&) = delete;
    ThrowingMove(ThrowingMove&& other) {
        if (throwOnMove.load(std::memory_order_relaxed)) {
            throw std::runtime_error("requested move failure");
        }
        value_ = std::exchange(other.value_, 0);
    }

    [[nodiscard]] int value() const noexcept {
        return value_;
    }

    static inline std::atomic_bool throwOnMove{false};

private:
    int value_{0};
};

ruvia::Task<void> receiveLast(ruvia::ChannelReceiver<int>& receiver, bool& success) {
    const auto value = co_await receiver.receive();
    const auto closed = co_await receiver.receive();
    success = value.value() != nullptr && *value.value() == 3 && closed.closed() != nullptr;
}

ruvia::Task<void> receiveQueuedThenStopping(ruvia::ChannelReceiver<int>& receiver, bool& success) {
    const auto value = co_await receiver.receive();
    const auto stopping = co_await receiver.receive();
    success = value.value() != nullptr && *value.value() == 9 && stopping.workerStopping() != nullptr;
}

ruvia::Task<void> receiveThrowingMove(ruvia::ChannelReceiver<ThrowingMove>& receiver, bool& success) {
    const auto result = co_await receiver.receive();
    success = result.value() != nullptr && result.value()->value() == 6;
}

ruvia::Task<ruvia::WorkerWaitResult<int>> makeColdReceiveAfterReceiverClose(ruvia::WorkerHandle worker, bool timed) {
    auto [sender, receiver] = ruvia::makeChannel<int>(std::move(worker), 1);
    if (timed) {
        return receiver.receiveFor(std::chrono::seconds(1));
    }
    return receiver.receive();
}

ruvia::Task<void> verifyColdReceiverTasks(ruvia::Task<ruvia::WorkerWaitResult<int>> receive, ruvia::Task<ruvia::WorkerWaitResult<int>> timedReceive, bool& success) {
    const auto coldClosed = co_await std::move(receive);
    const auto timedColdClosed = co_await std::move(timedReceive);
    success = coldClosed.closed() != nullptr && timedColdClosed.closed() != nullptr;
}

ruvia::Task<void> exercise(ruvia::WorkerHandle worker, bool& success) {
    auto [sender, receiver] = ruvia::makeChannel<int>(worker, 2);
    auto activeReceiver = std::move(receiver);
    const auto send1 = sender.send(1);
    const auto send2 = sender.send(2);
    const auto send3 = sender.send(99);
    if (!send1.accepted() || !send2.accepted() || send3.status() != ruvia::ChannelSendStatus::kFull || send3.rejected() == nullptr || *send3.rejected() != 99) {
        co_return;
    }

    const auto first = co_await activeReceiver.receive();
    const auto second = co_await activeReceiver.receive();
    if (first.value() == nullptr || *first.value() != 1 || second.value() == nullptr || *second.value() != 2) {
        co_return;
    }
    const auto timeout = co_await activeReceiver.receiveFor(std::chrono::milliseconds(1));
    if (timeout.timedOut() == nullptr) {
        co_return;
    }

    ruvia::TaskScope scope(worker);
    scope.spawn(receiveLast(activeReceiver, success));
    if (!sender.send(3).accepted()) {
        co_return;
    }
    sender.close();
    co_await scope.join();
    const auto closedSend = sender.send(4);
    if (closedSend.status() != ruvia::ChannelSendStatus::kClosed || closedSend.rejected() == nullptr || *closedSend.rejected() != 4) {
        success = false;
    }
}

}  // namespace

int main() {
    bool success = false;
    bool coldReceiverTasksSafe = false;
    {
        asio::io_context ioContext;
        const auto dispatcher = std::make_shared<ruvia::detail::WorkerDispatcher>(ioContext, 8);
        const auto worker = ruvia::detail::WorkerHandleAccess::make(dispatcher);
        auto coldReceive = makeColdReceiveAfterReceiverClose(worker, false);
        auto timedColdReceive = makeColdReceiveAfterReceiverClose(worker, true);
        asio::co_spawn(ioContext, ruvia::detail::taskAsAwaitable(exercise(worker, success)), asio::detached);
        asio::co_spawn(ioContext, ruvia::detail::taskAsAwaitable(verifyColdReceiverTasks(std::move(coldReceive), std::move(timedColdReceive), coldReceiverTasksSafe)), asio::detached);
        ioContext.run();
        dispatcher->close();
        dispatcher->stopTimers();
    }

    bool workerStopping = false;
    bool stoppingSend = false;
    {
        asio::io_context ioContext;
        const auto dispatcher = std::make_shared<ruvia::detail::WorkerDispatcher>(ioContext, 8);
        const auto worker = ruvia::detail::WorkerHandleAccess::make(dispatcher);
        auto [sender, receiver] = ruvia::makeChannel<int>(worker, 1);
        if (!sender.send(9).accepted()) {
            return 1;
        }
        asio::co_spawn(ioContext, ruvia::detail::taskAsAwaitable(receiveQueuedThenStopping(receiver, workerStopping)), asio::detached);
        asio::post(ioContext, [dispatcher] { dispatcher->close(); });
        ioContext.run();
        sender.close();
        const auto stoppingResult = sender.send(10);
        stoppingSend = stoppingResult.status() == ruvia::ChannelSendStatus::kWorkerStopping && stoppingResult.rejected() != nullptr && *stoppingResult.rejected() == 10;
        dispatcher->stopTimers();
    }

    bool moveFailed = false;
    bool recoveredValue = false;
    bool recoveredSend = false;
    {
        asio::io_context ioContext;
        const auto dispatcher = std::make_shared<ruvia::detail::WorkerDispatcher>(ioContext, 8);
        const auto worker = ruvia::detail::WorkerHandleAccess::make(dispatcher);
        auto [sender, receiver] = ruvia::makeChannel<ThrowingMove>(worker, 1);
        std::binary_semaphore receiverScheduled{0};
        asio::co_spawn(ioContext, ruvia::detail::taskAsAwaitable(receiveThrowingMove(receiver, recoveredValue)), asio::detached);
        asio::post(ioContext, [&receiverScheduled] { receiverScheduled.release(); });
        std::thread sendingThread([&] {
            receiverScheduled.acquire();
            ThrowingMove::throwOnMove.store(true, std::memory_order_relaxed);
            try {
                static_cast<void>(sender.send(ThrowingMove(5)));
            } catch (const std::runtime_error&) {
                moveFailed = true;
            }
            ThrowingMove::throwOnMove.store(false, std::memory_order_relaxed);
            recoveredSend = sender.send(ThrowingMove(6)).accepted();
        });
        ioContext.run();
        sendingThread.join();
        sender.close();
        dispatcher->close();
        dispatcher->stopTimers();
    }

    const bool allPassed = success && coldReceiverTasksSafe && workerStopping && stoppingSend && moveFailed && recoveredValue && recoveredSend;
    return allPassed ? 0 : 1;
}
