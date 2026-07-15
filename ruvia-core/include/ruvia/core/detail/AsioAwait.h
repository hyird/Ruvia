#pragma once

#include <asio/associated_executor.hpp>
#include <asio/async_result.hpp>
#include <asio/awaitable.hpp>
#include <asio/post.hpp>
#include <asio/use_awaitable.hpp>

#include <coroutine>
#include <exception>
#include <system_error>
#include <type_traits>
#include <utility>
#include <variant>

#include "ruvia/core/Task.h"

namespace ruvia::detail {

template <typename T>
class TaskCompletionResult;

template <typename T>
class TaskCompletionSuccess final {
public:
    [[nodiscard]] T takeValue() && {
        return std::move(value_);
    }

private:
    friend class TaskCompletionResult<T>;

    explicit TaskCompletionSuccess(T value)
        : value_(std::move(value)) {}

    T value_;
};

template <>
class TaskCompletionSuccess<void> final {
private:
    friend class TaskCompletionResult<void>;

    constexpr TaskCompletionSuccess() noexcept = default;
};

class TaskCompletionFailure final {
public:
    [[nodiscard]] const std::exception_ptr& exception() const & noexcept {
        return exception_;
    }
    const std::exception_ptr& exception() const && = delete;

private:
    template <typename>
    friend class TaskCompletionResult;

    explicit TaskCompletionFailure(std::exception_ptr exception) noexcept
        : exception_(std::move(exception)) {}

    std::exception_ptr exception_;
};

// Adapting Task completion into an Asio completion token preserves exactly one
// terminal: success owns the value (or a void marker), while failure owns the
// exception. Callers cannot observe or construct an empty or contradictory
// value/exception tuple.
template <typename T>
class TaskCompletionResult final {
public:
    [[nodiscard]] TaskCompletionSuccess<T>* success() & noexcept {
        return std::get_if<TaskCompletionSuccess<T>>(&value_);
    }
    TaskCompletionSuccess<T>* success() && = delete;

    [[nodiscard]] const TaskCompletionFailure* failure() const & noexcept {
        return std::get_if<TaskCompletionFailure>(&value_);
    }
    const TaskCompletionFailure* failure() const && = delete;

private:
    template <typename, typename>
    friend class TaskCompletionState;

    using Value = std::variant<TaskCompletionSuccess<T>, TaskCompletionFailure>;

    template <typename Alternative>
    explicit TaskCompletionResult(Alternative alternative)
        : value_(std::move(alternative)) {}

    [[nodiscard]] static TaskCompletionResult makeSuccess(T value) {
        return TaskCompletionResult(TaskCompletionSuccess<T>(std::move(value)));
    }

    [[nodiscard]] static TaskCompletionResult makeFailure(
        std::exception_ptr exception) noexcept {
        return TaskCompletionResult(TaskCompletionFailure(std::move(exception)));
    }

    Value value_;
};

template <>
class TaskCompletionResult<void> final {
public:
    [[nodiscard]] const TaskCompletionSuccess<void>*
    success() const & noexcept {
        return std::get_if<TaskCompletionSuccess<void>>(&value_);
    }
    const TaskCompletionSuccess<void>* success() const && = delete;

    [[nodiscard]] const TaskCompletionFailure* failure() const & noexcept {
        return std::get_if<TaskCompletionFailure>(&value_);
    }
    const TaskCompletionFailure* failure() const && = delete;

private:
    template <typename, typename>
    friend class TaskCompletionState;

    using Value = std::variant<
        TaskCompletionSuccess<void>,
        TaskCompletionFailure>;

    template <typename Alternative>
    explicit TaskCompletionResult(Alternative alternative) noexcept
        : value_(std::move(alternative)) {}

    [[nodiscard]] static TaskCompletionResult makeSuccess() noexcept {
        return TaskCompletionResult(TaskCompletionSuccess<void>());
    }

    [[nodiscard]] static TaskCompletionResult makeFailure(
        std::exception_ptr exception) noexcept {
        return TaskCompletionResult(TaskCompletionFailure(std::move(exception)));
    }

    Value value_;
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
            auto result = [this]() {
                try {
                    task_.handle_.promise().result();
                    return TaskCompletionResult<void>::makeSuccess();
                } catch (...) {
                    return TaskCompletionResult<void>::makeFailure(
                        std::current_exception());
                }
            }();
            std::move(handler_)(std::move(result));
        } else {
            auto result = [this]() {
                try {
                    return TaskCompletionResult<T>::makeSuccess(
                        task_.handle_.promise().result());
                } catch (...) {
                    return TaskCompletionResult<T>::makeFailure(
                        std::current_exception());
                }
            }();
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
    if (const auto* failure = result.failure()) {
        std::rethrow_exception(failure->exception());
    }
    co_return std::move(*result.success()).takeValue();
}

inline asio::awaitable<void> taskAsAwaitable(Task<void> task) {
    auto result = co_await asyncStartTask(std::move(task), asio::use_awaitable);
    if (const auto* failure = result.failure()) {
        std::rethrow_exception(failure->exception());
    }
    co_return;
}

struct AsioCompletionPending final {};

template <typename Result, typename Initiate>
class AsioCompletionAwaiter;

template <typename Result>
class AsioCompletion final {
public:
    [[nodiscard]] static AsioCompletion completed(
        std::error_code errorCode,
        Result result) {
        return AsioCompletion(errorCode, std::move(result));
    }

    [[nodiscard]] std::error_code errorCode() const noexcept {
        return errorCode_;
    }

    [[nodiscard]] const Result& result() const & noexcept {
        return result_;
    }
    const Result& result() const && = delete;

    [[nodiscard]] Result& result() & noexcept {
        return result_;
    }
    Result& result() && = delete;

    [[nodiscard]] Result takeResult() && {
        return std::move(result_);
    }

private:
    template <typename, typename>
    friend class AsioCompletionAwaiter;

    AsioCompletion(std::error_code errorCode, Result&& result)
        noexcept(std::is_nothrow_move_constructible_v<Result>)
        : errorCode_(errorCode), result_(std::move(result)) {}

    std::error_code errorCode_;
    Result result_;
};

template <>
class AsioCompletion<void> final {
public:
    [[nodiscard]] static AsioCompletion completed(
        std::error_code errorCode) noexcept {
        return AsioCompletion(errorCode);
    }

    [[nodiscard]] std::error_code errorCode() const noexcept {
        return errorCode_;
    }

private:
    template <typename, typename>
    friend class AsioCompletionAwaiter;

    explicit AsioCompletion(std::error_code errorCode) noexcept
        : errorCode_(errorCode) {}

    std::error_code errorCode_;
};

// Asio completion signatures always provide an error code and may also provide
// a result that remains meaningful on partial failure (for example transferred
// bytes). The completion therefore owns both fields; only pending versus
// completed is exclusive.
template <typename Result, typename Initiate>
class AsioCompletionAwaiter final {
public:
    explicit AsioCompletionAwaiter(Initiate initiate)
        : initiate_(std::move(initiate)) {}

    [[nodiscard]] bool await_ready() const noexcept {
        return false;
    }

    // If initiate_ throws, the exception propagates from the await-expression
    // directly ([expr.await]/5), without an exception_ptr side channel.
    [[nodiscard]] bool await_suspend(std::coroutine_handle<> handle) {
        if constexpr (std::is_void_v<Result>) {
            initiate_([this, handle](std::error_code ec, auto&&...) mutable {
                state_.template emplace<AsioCompletion<void>>(
                    AsioCompletion<void>::completed(ec));
                handle.resume();
            });
        } else {
            initiate_([this, handle](std::error_code ec, Result result) mutable {
                state_.template emplace<AsioCompletion<Result>>(
                    AsioCompletion<Result>::completed(
                        ec, std::move(result)));
                handle.resume();
            });
        }
        return true;
    }

    [[nodiscard]] AsioCompletion<Result> await_resume() {
        auto* completion = std::get_if<AsioCompletion<Result>>(&state_);
        if (completion == nullptr) {
            std::terminate();
        }
        return std::move(*completion);
    }

private:
    using State = std::variant<
        AsioCompletionPending,
        AsioCompletion<Result>>;

    Initiate initiate_;
    State state_;
};

template <typename Result = void, typename Initiate>
[[nodiscard]] auto asyncAsio(Initiate&& initiate) {
    return AsioCompletionAwaiter<Result, std::decay_t<Initiate>>(
        std::forward<Initiate>(initiate));
}

}  // namespace ruvia::detail
