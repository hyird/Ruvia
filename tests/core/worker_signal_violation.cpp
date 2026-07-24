#include <ruvia/core/Task.h>
#include <ruvia/core/detail/worker/WorkerDispatcher.h>
#include <ruvia/core/detail/worker/WorkerSignal.h>

#include <asio/io_context.hpp>
#include <asio/post.hpp>

#include <coroutine>
#include <cstdlib>
#include <exception>
#include <memory>
#include <string_view>

namespace {

class ManualOwner final {
public:
    struct promise_type {
        ManualOwner get_return_object() noexcept {
            return ManualOwner(std::coroutine_handle<promise_type>::from_promise(*this));
        }
        std::suspend_always initial_suspend() const noexcept {
            return {};
        }
        std::suspend_always final_suspend() const noexcept {
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

ManualOwner waitForever(ruvia::detail::WorkerSignal& signal) {
    co_await signal.wait();
}

}  // namespace

int main(int argc, char** argv) {
    std::set_terminate([] { std::_Exit(86); });
    asio::io_context io;
    auto dispatcher = std::make_shared<ruvia::detail::WorkerDispatcher>(io, 8);
    const auto workerHandle = ruvia::detail::WorkerHandleAccess::make(dispatcher);
    ruvia::detail::WorkerSignal signal(workerHandle);
    const std::string_view probe(argc > 1 ? argv[1] : "waiter");

    if (probe == "notify") {
        signal.notify();
        return EXIT_FAILURE;
    }

    auto owner = waitForever(signal);
    asio::post(io, [&owner] { owner.start(); });
    io.run();
    if (probe == "dispatch") {
        dispatcher->detachContext();
        io.restart();
        asio::post(io, [&signal] { signal.notify(); });
        io.run();
        return EXIT_FAILURE;
    }

    // owner destruction must reject destroying its linked intrusive Awaiter.
    return EXIT_SUCCESS;
}
