#include <ruvia/core/Channel.h>
#include <ruvia/core/TaskScope.h>
#include <ruvia/core/detail/AsioAwait.h>
#include <ruvia/core/detail/WorkerDispatcher.h>

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>

#include <memory>
#include <chrono>
#include <type_traits>

static_assert(!std::is_default_constructible_v<ruvia::ChannelReceiver<int>>);
static_assert(std::is_move_constructible_v<ruvia::ChannelReceiver<int>>);
static_assert(!std::is_move_assignable_v<ruvia::ChannelReceiver<int>>);

namespace {

ruvia::Task<void> receiveLast(ruvia::ChannelReceiver<int>& receiver, bool& success) {
    const auto value = co_await receiver.receive();
    const auto closed = co_await receiver.receive();
    success = value.value() != nullptr && *value.value() == 3 &&
              closed.closed() != nullptr;
}

ruvia::Task<void> exercise(ruvia::WorkerHandle worker, bool& success) {
    auto [sender, receiver] = ruvia::makeChannel<int>(worker, 2);
    auto activeReceiver = std::move(receiver);
    const auto send1 = sender.send(1);
    const auto send2 = sender.send(2);
    const auto send3 = sender.send(99);
    if (send1 != ruvia::ChannelSendResult::kSent ||
        send2 != ruvia::ChannelSendResult::kSent ||
        send3 != ruvia::ChannelSendResult::kFull) {
        co_return;
    }

    const auto first = co_await activeReceiver.receive();
    const auto second = co_await activeReceiver.receive();
    if (first.value() == nullptr || *first.value() != 1 ||
        second.value() == nullptr || *second.value() != 2) {
        co_return;
    }
    const auto timeout = co_await activeReceiver.receiveFor(std::chrono::milliseconds(1));
    if (timeout.timedOut() == nullptr) {
        co_return;
    }

    ruvia::TaskScope scope(worker);
    scope.spawn(receiveLast(activeReceiver, success));
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
