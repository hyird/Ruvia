#include <ruvia/core/Task.h>
#include <ruvia/core/detail/io/AsioAwait.h>

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/post.hpp>

#include <functional>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace {

// An asio initiation may complete synchronously, invoking the completion
// handler inside await_suspend. The awaiter must then consume the result
// without resuming the not-yet-suspended coroutine.
ruvia::Task<void> exerciseSynchronousCompletion(bool& success) {
    const auto result = co_await ruvia::detail::asyncAsio<std::size_t>([](auto handler) mutable {
        handler(std::make_error_code(std::errc::connection_reset), std::size_t{42});
    });
    success = result.errorCode() == std::make_error_code(std::errc::connection_reset) && result.result() == 42;
}

ruvia::Task<void> exerciseSynchronousVoid(bool& success) {
    co_await ruvia::detail::asyncAsio([](auto handler) mutable {
        handler(std::error_code{});
    });
    success = true;
}

// The ordinary deferred path: the coroutine suspends first, the completion is
// posted and later resumes it.
ruvia::Task<void> exerciseDeferredCompletion(asio::io_context& ioContext, bool& success) {
    const auto result = co_await ruvia::detail::asyncAsio<std::size_t>([&ioContext](auto handler) mutable {
        asio::post(ioContext, [handler = std::move(handler)]() mutable {
            handler(std::make_error_code(std::errc::timed_out), std::size_t{7});
        });
    });
    success = result.errorCode() == std::make_error_code(std::errc::timed_out) && result.result() == 7;
}

// A throwing initiation propagates from the await-expression, leaving the
// awaiter without a pending completion.
ruvia::Task<void> exerciseInitiateFailure(bool& caught) {
    try {
        co_await ruvia::detail::asyncAsio<void>([](auto) {
            throw std::runtime_error("initiate failure");
        });
    } catch (const std::runtime_error&) {
        caught = true;
    }
}

ruvia::Task<void> exerciseAll(asio::io_context& ioContext, bool& syncOk, bool& voidOk, bool& deferredOk, bool& initiateCaught) {
    co_await exerciseSynchronousCompletion(syncOk);
    co_await exerciseSynchronousVoid(voidOk);
    co_await exerciseDeferredCompletion(ioContext, deferredOk);
    co_await exerciseInitiateFailure(initiateCaught);
}

}  // namespace

int main() {
    asio::io_context ioContext;
    bool syncOk = false;
    bool voidOk = false;
    bool deferredOk = false;
    bool initiateCaught = false;
    asio::co_spawn(ioContext, ruvia::detail::taskAsAwaitable(exerciseAll(ioContext, syncOk, voidOk, deferredOk, initiateCaught)), asio::detached);
    ioContext.run();
    return syncOk && voidOk && deferredOk && initiateCaught ? 0 : 1;
}
