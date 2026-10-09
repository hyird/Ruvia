#include "ruvia/core/detail/io/asio_await.h"

#include <functional>
#include <stdexcept>
#include <system_error>
#include <utility>

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/post.hpp>

#include "ruvia/core/task.h"

namespace {

// An asio initiation may complete synchronously, invoking the completion
// handler inside await_suspend. The awaiter must then consume the result
// without resuming the not-yet-suspended coroutine.
ruvia::task<void> exercise_synchronous_completion(bool& success) {
    const auto result_value = co_await ruvia::detail::async_asio<std::size_t>([](auto handler) mutable {
        handler(std::make_error_code(std::errc::connection_reset), std::size_t{42});
    });
    success = result_value.error_code() == std::make_error_code(std::errc::connection_reset) &&
              result_value.result() == 42;
}

ruvia::task<void> exercise_synchronous_void(bool& success) {
    (void)(co_await ruvia::detail::async_asio(
        [](auto handler) mutable { handler(std::error_code{}); }));
    success = true;
}

// The ordinary deferred path: the coroutine suspends first, the completion is
// posted and later resumes it.
ruvia::task<void> exercise_deferred_completion(asio::io_context& io_context, bool& success) {
    const auto result_value =
        co_await ruvia::detail::async_asio<std::size_t>([&io_context](auto handler) mutable {
            asio::post(io_context, [handler = std::move(handler)]() mutable {
                handler(std::make_error_code(std::errc::timed_out), std::size_t{7});
            });
        });
    success =
        result_value.error_code() == std::make_error_code(std::errc::timed_out) && result_value.result() == 7;
}

// A throwing initiation propagates from the await-expression, leaving the
// awaiter without a pending completion.
ruvia::task<void> exercise_initiate_failure(bool& caught) {
    try {
        (void)(co_await ruvia::detail::async_asio<void>(
            [](auto) { throw std::runtime_error("initiate failure"); }));
    } catch (const std::runtime_error&) {
        caught = true;
    }
}

ruvia::task<void> exercise_all(asio::io_context& io_context, bool& sync_ok, bool& void_ok,
    bool& deferred_ok, bool& initiate_caught) {
    co_await exercise_synchronous_completion(sync_ok);
    co_await exercise_synchronous_void(void_ok);
    co_await exercise_deferred_completion(io_context, deferred_ok);
    co_await exercise_initiate_failure(initiate_caught);
}

}  // namespace

int main() {
    asio::io_context io_context;
    bool sync_ok = false;
    bool void_ok = false;
    bool deferred_ok = false;
    bool initiate_caught = false;
    asio::co_spawn(io_context,
        ruvia::detail::task_as_awaitable(
            exercise_all(io_context, sync_ok, void_ok, deferred_ok, initiate_caught)),
        asio::detached);
    io_context.run();
    return sync_ok && void_ok && deferred_ok && initiate_caught ? 0 : 1;
}
