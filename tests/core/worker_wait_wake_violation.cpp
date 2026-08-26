#include <ruvia/core/Channel.h>
#include <ruvia/core/OneShot.h>
#include <ruvia/core/detail/worker/WorkerDispatcher.h>

#include <asio/io_context.hpp>
#include <asio/post.hpp>

#include <coroutine>
#include <cstdlib>
#include <exception>
#include <memory>
#include <string_view>
#include <utility>

namespace {

class ManualOwner final {
public:
    struct promise_type {
        [[nodiscard]] ManualOwner get_return_object() noexcept {
            return ManualOwner(std::coroutine_handle<promise_type>::from_promise(*this));
        }
        [[nodiscard]] std::suspend_always initial_suspend() const noexcept {
            return {};
        }
        [[nodiscard]] std::suspend_always final_suspend() const noexcept {
            return {};
        }
        void return_void() const noexcept {}
        void unhandled_exception() const noexcept {
            std::terminate();
        }
    };

    explicit ManualOwner(std::coroutine_handle<promise_type> handle) noexcept
        : handle_(handle) {}

    ~ManualOwner() {
        if (handle_ != nullptr) {
            handle_.destroy();
        }
    }

    ManualOwner(const ManualOwner&) = delete;
    ManualOwner& operator=(const ManualOwner&) = delete;

    void start() const {
        handle_.resume();
    }

private:
    std::coroutine_handle<promise_type> handle_;
};

// The API's by-value parameter is initialized directly from the call-site
// prvalue. Its first move therefore occurs only after send()/complete() has
// accepted the worker, at the exact boundary before the waiter is woken.
class DetachDispatcherOnMove final {
public:
    explicit DetachDispatcherOnMove(
        std::shared_ptr<ruvia::detail::WorkerDispatcher> dispatcher) noexcept
        : dispatcher_(std::move(dispatcher)) {}

    DetachDispatcherOnMove(const DetachDispatcherOnMove&) = delete;
    DetachDispatcherOnMove& operator=(const DetachDispatcherOnMove&) = delete;
    DetachDispatcherOnMove& operator=(DetachDispatcherOnMove&&) = delete;

    DetachDispatcherOnMove(DetachDispatcherOnMove&& other) noexcept
        : dispatcher_(std::move(other.dispatcher_)) {
        if (dispatcher_ != nullptr) {
            dispatcher_->detachContext();
        }
    }

private:
    std::shared_ptr<ruvia::detail::WorkerDispatcher> dispatcher_;
};

ManualOwner waitForOneShot(ruvia::OneShotReceiver<DetachDispatcherOnMove>& receiver) {
    (void)co_await receiver.wait();
}

ManualOwner waitForChannel(ruvia::ChannelReceiver<DetachDispatcherOnMove>& receiver) {
    (void)co_await receiver.receive();
}

template <typename StartWait, typename Complete>
void runDispatchFailureProbe(StartWait startWait, Complete complete) {
    asio::io_context ioContext;
    auto dispatcher = std::make_shared<ruvia::detail::WorkerDispatcher>(ioContext, 8);
    auto owner = startWait(ruvia::detail::WorkerHandleAccess::make(dispatcher));
    asio::post(ioContext, [&owner] { owner.start(); });
    ioContext.run();

    std::set_terminate([] { std::_Exit(86); });
    try {
        complete(dispatcher);
    } catch (...) {
        // Propagating dispatch failure is not the terminal contract being
        // tested. Exit before suspended-owner cleanup can mask that failure.
    }
    std::_Exit(EXIT_SUCCESS);
}

void probeOneShot() {
    std::shared_ptr<ruvia::OneShotCompletion<DetachDispatcherOnMove>> completion;
    std::shared_ptr<ruvia::OneShotReceiver<DetachDispatcherOnMove>> receiver;
    runDispatchFailureProbe(
        [&](ruvia::WorkerHandle worker) {
            auto pair = ruvia::makeOneShot<DetachDispatcherOnMove>(std::move(worker));
            completion = std::make_shared<ruvia::OneShotCompletion<DetachDispatcherOnMove>>(
                std::move(pair.first));
            receiver = std::make_shared<ruvia::OneShotReceiver<DetachDispatcherOnMove>>(
                std::move(pair.second));
            return waitForOneShot(*receiver);
        },
        [&](std::shared_ptr<ruvia::detail::WorkerDispatcher> dispatcher) {
            (void)completion->complete(DetachDispatcherOnMove(std::move(dispatcher)));
        });
}

void probeChannel() {
    std::shared_ptr<ruvia::ChannelSender<DetachDispatcherOnMove>> sender;
    std::shared_ptr<ruvia::ChannelReceiver<DetachDispatcherOnMove>> receiver;
    runDispatchFailureProbe(
        [&](ruvia::WorkerHandle worker) {
            auto pair =
                ruvia::makeChannel<DetachDispatcherOnMove>(std::move(worker), {.capacity = 1});
            sender = std::make_shared<ruvia::ChannelSender<DetachDispatcherOnMove>>(
                std::move(pair.first));
            receiver = std::make_shared<ruvia::ChannelReceiver<DetachDispatcherOnMove>>(
                std::move(pair.second));
            return waitForChannel(*receiver);
        },
        [&](std::shared_ptr<ruvia::detail::WorkerDispatcher> dispatcher) {
            (void)sender->send(DetachDispatcherOnMove(std::move(dispatcher)));
        });
}

}  // namespace

int main(int argc, char** argv) {
    const std::string_view probe(argc > 1 ? argv[1] : "one_shot");
    try {
        if (probe == "channel") {
            probeChannel();
        } else {
            probeOneShot();
        }
    } catch (...) {
        return EXIT_SUCCESS;
    }
    return EXIT_SUCCESS;
}
