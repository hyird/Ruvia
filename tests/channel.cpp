#include <ruvia/core/Channel.h>
#include <ruvia/core/TaskScope.h>
#include <ruvia/core/detail/AsioAwait.h>
#include <ruvia/core/detail/WorkerDispatcher.h>

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>

#include <memory>
#include <chrono>

namespace {

ruvia::Task<void> receiveLast(ruvia::ChannelReceiver<int>& receiver, bool& success) {
    const auto value = co_await receiver.receive();
    const auto closed = co_await receiver.receive();
    success = value.status == ruvia::ChannelReceiveStatus::kValue && value.value == 3 &&
              closed.status == ruvia::ChannelReceiveStatus::kClosed;
}

ruvia::Task<void> exercise(ruvia::WorkerHandle worker, bool& success) {
    auto [sender, receiver] = ruvia::makeChannel<int>(worker, 2);
    const auto send1 = sender.send(1);
    const auto send2 = sender.send(2);
    const auto send3 = sender.send(99);
    if (send1 != ruvia::ChannelSendResult::kSent ||
        send2 != ruvia::ChannelSendResult::kSent ||
        send3 != ruvia::ChannelSendResult::kFull) {
        co_return;
    }

    const auto first = co_await receiver.receive();
    const auto second = co_await receiver.receive();
    if (first.status != ruvia::ChannelReceiveStatus::kValue || first.value != 1 ||
        second.status != ruvia::ChannelReceiveStatus::kValue || second.value != 2) {
        co_return;
    }
    const auto timeout = co_await receiver.receiveFor(std::chrono::milliseconds(1));
    if (timeout.status != ruvia::ChannelReceiveStatus::kTimeout) {
        co_return;
    }

    ruvia::TaskScope scope(worker);
    scope.spawn(receiveLast(receiver, success));
    if (sender.send(3) != ruvia::ChannelSendResult::kSent) {
        co_return;
    }
    sender.close();
    co_await scope.join();
}

}

int main() {
    asio::io_context ioContext;
    const auto dispatcher = std::make_shared<ruvia::detail::WorkerDispatcher>(ioContext, 8);
    const auto worker = ruvia::detail::WorkerHandleAccess::make(dispatcher);
    bool success = false;
    asio::co_spawn(ioContext,
                   ruvia::detail::taskAsAwaitable(exercise(worker, success)),
                   asio::detached);
    ioContext.run();
    dispatcher->close();
    return success ? 0 : 1;
}
