#pragma once

#include <coroutine>
#include <exception>
#include <expected>
#include <memory>
#include <stdexcept>
#include <system_error>
#include <type_traits>
#include <utility>

#include <asio/associated_allocator.hpp>
#include <asio/associated_executor.hpp>
#include <asio/async_result.hpp>
#include <asio/awaitable.hpp>
#include <asio/bind_allocator.hpp>
#include <asio/post.hpp>
#include <asio/use_awaitable.hpp>

#include "ruvia/core/Task.h"
#include "ruvia/core/detail/SuspendRaceState.h"

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
    [[nodiscard]] const std::exception_ptr& exception() const& noexcept {
        return exception_;
    }
    const std::exception_ptr& exception() const&& = delete;

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
        return value_ ? &*value_ : nullptr;
    }
    TaskCompletionSuccess<T>* success() && = delete;

    [[nodiscard]] const TaskCompletionFailure* failure() const& noexcept {
        return value_ ? nullptr : &value_.error();
    }
    const TaskCompletionFailure* failure() const&& = delete;

private:
    template <typename, typename>
    friend class TaskCompletionState;

    using Value = std::expected<TaskCompletionSuccess<T>, TaskCompletionFailure>;

    explicit TaskCompletionResult(TaskCompletionSuccess<T> success)
        : value_(std::move(success)) {}

    explicit TaskCompletionResult(TaskCompletionFailure failure) noexcept
        : value_(std::unexpected(std::move(failure))) {}

    [[nodiscard]] static TaskCompletionResult makeSuccess(T value) {
        return TaskCompletionResult(TaskCompletionSuccess<T>(std::move(value)));
    }

    [[nodiscard]] static TaskCompletionResult makeFailure(std::exception_ptr exception) noexcept {
        return TaskCompletionResult(TaskCompletionFailure(std::move(exception)));
    }

    Value value_;
};

template <>
class TaskCompletionResult<void> final {
public:
    [[nodiscard]] const TaskCompletionSuccess<void>* success() const& noexcept {
        return value_ ? &*value_ : nullptr;
    }
    const TaskCompletionSuccess<void>* success() const&& = delete;

    [[nodiscard]] const TaskCompletionFailure* failure() const& noexcept {
        return value_ ? nullptr : &value_.error();
    }
    const TaskCompletionFailure* failure() const&& = delete;

private:
    template <typename, typename>
    friend class TaskCompletionState;

    using Value = std::expected<TaskCompletionSuccess<void>, TaskCompletionFailure>;

    explicit TaskCompletionResult(TaskCompletionSuccess<void> success) noexcept
        : value_(std::move(success)) {}

    explicit TaskCompletionResult(TaskCompletionFailure failure) noexcept
        : value_(std::unexpected(std::move(failure))) {}

    [[nodiscard]] static TaskCompletionResult makeSuccess() noexcept {
        return TaskCompletionResult(TaskCompletionSuccess<void>());
    }

    [[nodiscard]] static TaskCompletionResult makeFailure(std::exception_ptr exception) noexcept {
        return TaskCompletionResult(TaskCompletionFailure(std::move(exception)));
    }

    Value value_;
};

template <typename T, typename Handler>
class TaskCompletionState final {
public:
    using HandlerAllocator = asio::associated_allocator_t<Handler>;
    using StateAllocator = typename std::allocator_traits<HandlerAllocator>::template rebind_alloc<
        TaskCompletionState>;
    using StateAllocatorTraits = std::allocator_traits<StateAllocator>;

    static TaskCompletionState* create(Task<T>&& taskValue, Handler&& handlerValue) {
        HandlerAllocator handlerAllocator(asio::get_associated_allocator(handlerValue));
        StateAllocator stateAllocator(handlerAllocator);
        auto* state = StateAllocatorTraits::allocate(stateAllocator, 1);
        try {
            StateAllocatorTraits::construct(stateAllocator, state, std::move(taskValue),
                std::forward<Handler>(handlerValue), std::move(handlerAllocator));
        } catch (...) {
            StateAllocatorTraits::deallocate(stateAllocator, state, 1);
            throw;
        }
        return state;
    }

    TaskCompletionState(
        Task<T>&& taskValue, Handler&& handlerValue, HandlerAllocator allocatorValue)
        : task_(std::move(taskValue)),
          handler_(std::forward<Handler>(handlerValue)),
          allocator_(std::move(allocatorValue)) {}

    void start() {
        task_.handle_.promise().setCompletion(this, &TaskCompletionState::complete);
        task_.start();
    }

    static void complete(void* raw) noexcept {
        auto* state = static_cast<TaskCompletionState*>(raw);
        auto executor = asio::get_associated_executor(state->handler_);
        auto completionAllocator = state->allocator_;
        StateOwner owner(state, StateDeleter{StateAllocator(state->allocator_)});
        asio::post(executor, asio::bind_allocator(std::move(completionAllocator),
                                 [owner = std::move(owner)]() mutable {
                                     deliver(std::move(owner));
                                 }));
    }

private:
    struct StateDeleter final {
        void operator()(TaskCompletionState* state) noexcept {
            StateAllocatorTraits::destroy(allocator, state);
            StateAllocatorTraits::deallocate(allocator, state, 1);
        }

        StateAllocator allocator;
    };

    using StateOwner = std::unique_ptr<TaskCompletionState, StateDeleter>;

    static void deliver(StateOwner owner) {
        auto result = [&owner]() {
            if constexpr (std::is_void_v<T>) {
                try {
                    owner->task_.handle_.promise().result();
                    return TaskCompletionResult<void>::makeSuccess();
                } catch (...) {
                    return TaskCompletionResult<void>::makeFailure(std::current_exception());
                }
            } else {
                try {
                    return TaskCompletionResult<T>::makeSuccess(
                        owner->task_.handle_.promise().result());
                } catch (...) {
                    return TaskCompletionResult<T>::makeFailure(std::current_exception());
                }
            }
        }();
        auto handler = std::move(owner->handler_);
        owner.reset();
        std::move(handler)(std::move(result));
    }

    Task<T> task_;
    Handler handler_;
    HandlerAllocator allocator_;
};

template <typename T, typename CompletionToken>
    requires AsioTaskResult<T>
inline auto asyncStartTask(Task<T>&& task, CompletionToken&& token) {
    if (task.handle_ == nullptr) {
        throw std::logic_error("cannot adapt an empty ruvia::Task to asio::awaitable");
    }
    if constexpr (std::is_void_v<T>) {
        return asio::async_initiate<CompletionToken, void(TaskCompletionResult<void>)>(
            [](auto&& handler, Task<T> taskValue) {
                using Handler = std::decay_t<decltype(handler)>;
                auto* state = TaskCompletionState<T, Handler>::create(
                    std::move(taskValue), std::forward<decltype(handler)>(handler));
                state->start();
            },
            token, std::move(task));
    } else {
        return asio::async_initiate<CompletionToken, void(TaskCompletionResult<T>)>(
            [](auto&& handler, Task<T> taskValue) {
                using Handler = std::decay_t<decltype(handler)>;
                auto* state = TaskCompletionState<T, Handler>::create(
                    std::move(taskValue), std::forward<decltype(handler)>(handler));
                state->start();
            },
            token, std::move(task));
    }
}

template <typename T>
    requires AsioTaskResult<T>
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

template <typename Result, typename Initiate>
class AsioCompletionAwaiter;
template <typename Result>
class AsioCompletion final {
public:
    [[nodiscard]] static AsioCompletion completed(std::error_code errorCode, Result result) {
        return AsioCompletion(errorCode, std::move(result));
    }

    [[nodiscard]] std::error_code errorCode() const noexcept {
        return errorCode_;
    }

    [[nodiscard]] const Result& result() const& noexcept {
        return result_;
    }
    const Result& result() const&& = delete;

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

    AsioCompletion(std::error_code errorCode, Result&& result) noexcept(
        std::is_nothrow_move_constructible_v<Result>)
        : errorCode_(errorCode),
          result_(std::move(result)) {}

    std::error_code errorCode_;
    Result result_;
};

template <>
class AsioCompletion<void> final {
public:
    [[nodiscard]] static AsioCompletion completed(std::error_code errorCode) noexcept {
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
    //
    // An asio initiation may complete synchronously (for example a closed
    // socket or a resolver cache hit), invoking the completion handler before
    // await_suspend returns. Resuming the continuation from inside that window
    // would run the coroutine while it is still suspended-by-await_suspend and
    // potentially destroy its frame before this function returns. The
    // SuspendRaceState records the completion instead; the return value of
    // await_suspend then reports the actual race order, so a synchronous
    // completion is consumed by await_resume without resuming at all.
    [[nodiscard]] bool await_suspend(std::coroutine_handle<> handle) {
        if constexpr (std::is_void_v<Result>) {
            initiate_([this, handle](std::error_code ec, auto&&...) mutable {
                if (state_.complete(AsioCompletion<void>::completed(ec))) {
                    handle.resume();
                }
            });
        } else {
            initiate_([this, handle](std::error_code ec, Result result) mutable {
                if (state_.complete(AsioCompletion<Result>::completed(ec, std::move(result)))) {
                    handle.resume();
                }
            });
        }
        return state_.suspend(handle);
    }

    [[nodiscard]] AsioCompletion<Result> await_resume() {
        return state_.takeValue();
    }

private:
    Initiate initiate_;
    SuspendRaceState<AsioCompletion<Result>> state_;
};

template <typename Result = void, typename Initiate>
[[nodiscard]] auto asyncAsio(Initiate&& initiate) {
    return AsioCompletionAwaiter<Result, std::decay_t<Initiate>>(std::forward<Initiate>(initiate));
}

}  // namespace ruvia::detail
