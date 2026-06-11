#pragma once

#include <asio/associated_executor.hpp>
#include <asio/async_result.hpp>
#include <asio/awaitable.hpp>
#include <asio/post.hpp>
#include <asio/use_awaitable.hpp>

#include <coroutine>
#include <exception>
#include <optional>
#include <system_error>
#include <type_traits>
#include <utility>

#include "ruvia/app/Task.h"

namespace ruvia::detail {

template <typename T>
struct TaskCompletionResult final {
    std::exception_ptr exception;
    std::optional<T> value;
};

template <>
struct TaskCompletionResult<void> final {
    std::exception_ptr exception;
};

template <typename T, typename Handler>
class TaskCompletionState final {
public:
    TaskCompletionState(Task<T>&& taskValue, Handler&& handlerValue)
        : task_(std::move(taskValue)),
          handler_(std::forward<Handler>(handlerValue)) {}

    void start() {
        task_.handle_.promise().setCompletion(this, &TaskCompletionState::complete);
        task_.start();
    }

    static void complete(void* raw) noexcept {
        auto* state = static_cast<TaskCompletionState*>(raw);
        auto executor = asio::get_associated_executor(state->handler_);
        asio::post(executor, [state]() mutable {
            state->deliver();
            delete state;
        });
    }

private:
    void deliver() {
        if constexpr (std::is_void_v<T>) {
            TaskCompletionResult<void> result;
            try {
                task_.handle_.promise().result();
            } catch (...) {
                result.exception = std::current_exception();
            }
            std::move(handler_)(std::move(result));
        } else {
            TaskCompletionResult<T> result;
            try {
                result.value.emplace(task_.handle_.promise().result());
            } catch (...) {
                result.exception = std::current_exception();
            }
            std::move(handler_)(std::move(result));
        }
    }

    Task<T> task_;
    Handler handler_;
};

template <typename T, typename CompletionToken>
auto asyncStartTask(Task<T>&& task, CompletionToken&& token) {
    if (task.handle_ == nullptr) {
        throw std::logic_error("cannot adapt an empty ruvia::Task to asio::awaitable");
    }
    if constexpr (std::is_void_v<T>) {
        return asio::async_initiate<CompletionToken, void(TaskCompletionResult<void>)>(
            [](auto&& handler, Task<T> taskValue) {
                using Handler = std::decay_t<decltype(handler)>;
                auto* state = new TaskCompletionState<T, Handler>(
                    std::move(taskValue),
                    std::forward<decltype(handler)>(handler));
                state->start();
            },
            token,
            std::move(task));
    } else {
        return asio::async_initiate<CompletionToken, void(TaskCompletionResult<T>)>(
            [](auto&& handler, Task<T> taskValue) {
                using Handler = std::decay_t<decltype(handler)>;
                auto* state = new TaskCompletionState<T, Handler>(
                    std::move(taskValue),
                    std::forward<decltype(handler)>(handler));
                state->start();
            },
            token,
            std::move(task));
    }
}

template <typename T>
[[nodiscard]] asio::awaitable<T> taskAsAwaitable(Task<T> task) {
    auto result = co_await asyncStartTask(std::move(task), asio::use_awaitable);
    if (result.exception != nullptr) {
        std::rethrow_exception(result.exception);
    }
    co_return std::move(*result.value);
}

inline asio::awaitable<void> taskAsAwaitable(Task<void> task) {
    auto result = co_await asyncStartTask(std::move(task), asio::use_awaitable);
    if (result.exception != nullptr) {
        std::rethrow_exception(result.exception);
    }
    co_return;
}

template <typename Initiate>
class ErrorAwaiter final {
public:
    explicit ErrorAwaiter(Initiate initiate) : initiate_(std::move(initiate)) {}

    [[nodiscard]] bool await_ready() const noexcept {
        return false;
    }

    // If initiate_ throws, the coroutine is resumed and the exception is
    // rethrown from the await-expression ([expr.await]/5) — same semantics as
    // catching and rethrowing in await_resume, without an exception_ptr slot.
    [[nodiscard]] bool await_suspend(std::coroutine_handle<> handle) {
        initiate_([this, handle](std::error_code ec, auto&&...) mutable {
            ec_ = ec;
            handle.resume();
        });
        return true;
    }

    [[nodiscard]] std::error_code await_resume() const noexcept {
        return ec_;
    }

private:
    Initiate initiate_;
    std::error_code ec_;
};

template <typename Result, typename Initiate>
class ErrorResultAwaiter final {
public:
    explicit ErrorResultAwaiter(Initiate initiate) : initiate_(std::move(initiate)) {}

    [[nodiscard]] bool await_ready() const noexcept {
        return false;
    }

    // See ErrorAwaiter::await_suspend: initiation exceptions propagate from
    // the await-expression directly.
    [[nodiscard]] bool await_suspend(std::coroutine_handle<> handle) {
        initiate_([this, handle](std::error_code ec, Result result) mutable {
            ec_ = ec;
            result_.emplace(std::move(result));
            handle.resume();
        });
        return true;
    }

    [[nodiscard]] std::pair<std::error_code, Result> await_resume() {
        return {ec_, std::move(*result_)};
    }

private:
    Initiate initiate_;
    std::error_code ec_;
    std::optional<Result> result_;
};

template <typename Initiate>
[[nodiscard]] auto asyncError(Initiate&& initiate) {
    return ErrorAwaiter<std::decay_t<Initiate>>(std::forward<Initiate>(initiate));
}

template <typename Result, typename Initiate>
[[nodiscard]] auto asyncResult(Initiate&& initiate) {
    return ErrorResultAwaiter<Result, std::decay_t<Initiate>>(std::forward<Initiate>(initiate));
}

}  // namespace ruvia::detail
