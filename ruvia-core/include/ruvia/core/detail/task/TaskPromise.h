#pragma once

#include <cassert>
#include <concepts>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <type_traits>
#include <utility>
#include <variant>

namespace ruvia {

template <typename T>
class Task;
class TaskScope;

namespace detail {

enum class TaskFrameOwnership : std::uint8_t {
    kCold,
    kStarted,
};

[[nodiscard]] void* taskFrameAllocate(std::size_t bytes);
void taskFrameDeallocate(void* pointer) noexcept;
void taskFrameDeallocateSized(void* pointer, std::size_t bytes) noexcept;

template <typename T>
class TaskPromise;

template <typename T>
class TaskAwaiter;

template <typename T, typename Handler>
class TaskCompletionState;

template <typename T>
concept AsioTaskResult = std::is_void_v<T> || std::is_nothrow_move_constructible_v<T>;

template <typename T, typename CompletionToken>
    requires AsioTaskResult<T>
auto asyncStartTask(Task<T>&& task, CompletionToken&& token);

struct TaskFinalAwaiter final {
    [[nodiscard]] bool await_ready() const noexcept {
        return false;
    }

    template <typename Promise>
    [[nodiscard]] std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise> handle) const noexcept {
        auto& promise = handle.promise();
        if (promise.completion_ != nullptr) {
            promise.completion_(promise.completionState_);
            return std::noop_coroutine();
        }
        return promise.continuation_;
    }

    void await_resume() const noexcept {}
};

struct TaskPromisePending final {};
struct TaskPromiseCompleted final {};

template <typename T>
class TaskPromiseValue final {
public:
    template <typename U>
        requires std::constructible_from<T, U>
    explicit TaskPromiseValue(U&& value) noexcept(std::is_nothrow_constructible_v<T, U>)
        : value_(std::forward<U>(value)) {}

    [[nodiscard]] T takeValue() && {
        return std::move(value_);
    }

private:
    T value_;
};

class TaskPromiseFailure final {
public:
    explicit TaskPromiseFailure(std::exception_ptr exception) noexcept
        : exception_(std::move(exception)) {}

    [[nodiscard]] const std::exception_ptr& exception() const& noexcept {
        return exception_;
    }
    const std::exception_ptr& exception() const&& = delete;

private:
    std::exception_ptr exception_;
};

template <typename T>
class TaskPromise final {
public:
    using value_type = T;

    static_assert(!std::is_reference_v<T>, "ruvia::Task<T> does not support reference result types");

    TaskPromise() noexcept = default;

    static void* operator new(std::size_t size) {
        return taskFrameAllocate(size);
    }

    static void operator delete(void* pointer) noexcept {
        taskFrameDeallocate(pointer);
    }

    static void operator delete(void* pointer, std::size_t size) noexcept {
        taskFrameDeallocateSized(pointer, size);
    }

    [[nodiscard]] Task<T> get_return_object() noexcept;

    [[nodiscard]] std::suspend_always initial_suspend() const noexcept {
        return {};
    }

    [[nodiscard]] TaskFinalAwaiter final_suspend() noexcept {
        return {};
    }

    template <typename U>
        requires std::constructible_from<T, U>
    void return_value(U&& value) noexcept(std::is_nothrow_constructible_v<T, U>) {
        state_.template emplace<TaskPromiseValue<T>>(std::forward<U>(value));
    }

    void unhandled_exception() noexcept {
        state_.template emplace<TaskPromiseFailure>(std::current_exception());
    }

    [[nodiscard]] T result() & {
        if (const auto* failure = std::get_if<TaskPromiseFailure>(&state_)) [[unlikely]] {
            std::rethrow_exception(failure->exception());
        }
        auto* value = std::get_if<TaskPromiseValue<T>>(&state_);
        assert(value != nullptr);
        return std::move(*value).takeValue();
    }

private:
    template <typename>
    friend class ruvia::Task;
    template <typename>
    friend class TaskAwaiter;
    template <typename, typename>
    friend class TaskCompletionState;
    friend class ruvia::TaskScope;
    friend struct TaskFinalAwaiter;

    void setContinuation(std::coroutine_handle<> continuation) noexcept {
        continuation_ = continuation;
    }

    void markStarted() noexcept {
        assert(ownership_ == TaskFrameOwnership::kCold);
        ownership_ = TaskFrameOwnership::kStarted;
    }

    [[nodiscard]] bool started() const noexcept {
        return ownership_ == TaskFrameOwnership::kStarted;
    }

    void setCompletion(void* state, void (*completion)(void*) noexcept) noexcept {
        completionState_ = state;
        completion_ = completion;
    }

    using State = std::variant<TaskPromisePending, TaskPromiseValue<T>, TaskPromiseFailure>;

    State state_;
    TaskFrameOwnership ownership_{TaskFrameOwnership::kCold};
    std::coroutine_handle<> continuation_{std::noop_coroutine()};
    void* completionState_{nullptr};
    void (*completion_)(void*) noexcept {nullptr};
};

template <>
class TaskPromise<void> final {
public:
    TaskPromise() noexcept = default;

    static void* operator new(std::size_t size) {
        return taskFrameAllocate(size);
    }

    static void operator delete(void* pointer) noexcept {
        taskFrameDeallocate(pointer);
    }

    static void operator delete(void* pointer, std::size_t size) noexcept {
        taskFrameDeallocateSized(pointer, size);
    }

    [[nodiscard]] Task<void> get_return_object() noexcept;

    [[nodiscard]] std::suspend_always initial_suspend() const noexcept {
        return {};
    }

    [[nodiscard]] TaskFinalAwaiter final_suspend() noexcept {
        return {};
    }

    void return_void() noexcept {
        state_.template emplace<TaskPromiseCompleted>();
    }

    void unhandled_exception() noexcept {
        state_.template emplace<TaskPromiseFailure>(std::current_exception());
    }

    void result() {
        if (const auto* failure = std::get_if<TaskPromiseFailure>(&state_)) [[unlikely]] {
            std::rethrow_exception(failure->exception());
        }
        assert(std::holds_alternative<TaskPromiseCompleted>(state_));
    }

private:
    template <typename>
    friend class ruvia::Task;
    template <typename>
    friend class TaskAwaiter;
    template <typename, typename>
    friend class TaskCompletionState;
    friend class ruvia::TaskScope;
    friend struct TaskFinalAwaiter;

    void setContinuation(std::coroutine_handle<> continuation) noexcept {
        continuation_ = continuation;
    }

    void markStarted() noexcept {
        assert(ownership_ == TaskFrameOwnership::kCold);
        ownership_ = TaskFrameOwnership::kStarted;
    }

    [[nodiscard]] bool started() const noexcept {
        return ownership_ == TaskFrameOwnership::kStarted;
    }

    void setCompletion(void* state, void (*completion)(void*) noexcept) noexcept {
        completionState_ = state;
        completion_ = completion;
    }

    using State = std::variant<TaskPromisePending, TaskPromiseCompleted, TaskPromiseFailure>;

    State state_;
    TaskFrameOwnership ownership_{TaskFrameOwnership::kCold};
    std::coroutine_handle<> continuation_{std::noop_coroutine()};
    void* completionState_{nullptr};
    void (*completion_)(void*) noexcept {nullptr};
};

}  // namespace detail

}  // namespace ruvia
