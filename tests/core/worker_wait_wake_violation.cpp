#include <ruvia/core/Channel.h>
#include <ruvia/core/OneShot.h>
#include <ruvia/core/detail/worker/WorkerDispatcher.h>

#include <asio/io_context.hpp>
#include <asio/post.hpp>

#include <atomic>
#include <coroutine>
#include <cstdlib>
#include <exception>
#include <memory>
#include <semaphore>
#include <string_view>
#include <thread>
#include <utility>

namespace {

class ManualOwner final {
public:
    struct promise_type {
        [[nodiscard]] ManualOwner get_return_object() noexcept {
            return ManualOwner(
                std::coroutine_handle<promise_type>::from_promise(*this));
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

    explicit ManualOwner(
        std::coroutine_handle<promise_type> handle) noexcept
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

class GatedMove final {
public:
    explicit GatedMove(int value) noexcept
        : value_(value) {}

    GatedMove(const GatedMove&) = delete;
    GatedMove& operator=(const GatedMove&) = delete;
    GatedMove& operator=(GatedMove&&) = delete;

    GatedMove(GatedMove&& other) noexcept
        : value_(std::exchange(other.value_, 0)) {
        if (blockNext_.exchange(false, std::memory_order_acq_rel)) {
            moveEntered_.release();
            moveReleased_.acquire();
        }
    }

    static void blockNextMove() noexcept {
        blockNext_.store(true, std::memory_order_release);
    }

    static void waitUntilMoveEntered() {
        moveEntered_.acquire();
    }

    static void releaseMove() noexcept {
        moveReleased_.release();
    }

private:
    int value_;
    static inline std::atomic_bool blockNext_{false};
    static inline std::binary_semaphore moveEntered_{0};
    static inline std::binary_semaphore moveReleased_{0};
};

ManualOwner waitForOneShot(ruvia::OneShotReceiver<GatedMove>& receiver) {
    (void)co_await receiver.wait();
}

ManualOwner waitForChannel(ruvia::ChannelReceiver<GatedMove>& receiver) {
    (void)co_await receiver.receive();
}

template <typename StartWait, typename Complete>
void runDispatchFailureProbe(StartWait startWait, Complete complete) {
    asio::io_context ioContext;
    auto dispatcher =
        std::make_shared<ruvia::detail::WorkerDispatcher>(ioContext, 8);
    auto owner = startWait(
        ruvia::detail::WorkerHandleAccess::make(dispatcher));
    asio::post(ioContext, [&owner] { owner.start(); });
    ioContext.run();

    GatedMove::blockNextMove();
    std::thread completing([complete = std::move(complete)]() mutable {
        try {
            complete();
        } catch (...) {
            // The old implementation propagated defer() after detaching the
            // waiter. That is not recoverable, so returning an exception is the
            // contract failure this death probe must distinguish.
            std::_Exit(EXIT_SUCCESS);
        }
        std::_Exit(EXIT_SUCCESS);
    });

    GatedMove::waitUntilMoveEntered();
    dispatcher->detachContext();
    GatedMove::releaseMove();
    completing.join();
}

void probeOneShot() {
    std::shared_ptr<ruvia::OneShotCompletion<GatedMove>> completion;
    std::shared_ptr<ruvia::OneShotReceiver<GatedMove>> receiver;
    runDispatchFailureProbe(
        [&](ruvia::WorkerHandle worker) {
            auto pair = ruvia::makeOneShot<GatedMove>(std::move(worker));
            completion = std::make_shared<ruvia::OneShotCompletion<GatedMove>>(
                std::move(pair.first));
            receiver =
                std::make_shared<ruvia::OneShotReceiver<GatedMove>>(
                    std::move(pair.second));
            return waitForOneShot(*receiver);
        },
        [&] { (void)completion->complete(GatedMove(1)); });
}

void probeChannel() {
    std::shared_ptr<ruvia::ChannelSender<GatedMove>> sender;
    std::shared_ptr<ruvia::ChannelReceiver<GatedMove>> receiver;
    runDispatchFailureProbe(
        [&](ruvia::WorkerHandle worker) {
            auto pair =
                ruvia::makeChannel<GatedMove>(std::move(worker), 1);
            sender = std::make_shared<ruvia::ChannelSender<GatedMove>>(
                std::move(pair.first));
            receiver =
                std::make_shared<ruvia::ChannelReceiver<GatedMove>>(
                    std::move(pair.second));
            return waitForChannel(*receiver);
        },
        [&] { (void)sender->send(GatedMove(1)); });
}

}  // namespace

int main(int argc, char** argv) {
    std::set_terminate([] { std::_Exit(86); });
    const std::string_view probe(argc > 1 ? argv[1] : "one_shot");
    if (probe == "channel") {
        probeChannel();
    } else {
        probeOneShot();
    }
    return EXIT_SUCCESS;
}
