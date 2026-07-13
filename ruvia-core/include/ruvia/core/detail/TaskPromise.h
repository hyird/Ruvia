#pragma once

#include <cassert>
#include <concepts>
#include <coroutine>
#include <cstddef>
#include <exception>
#include <new>
#include <optional>
#include <type_traits>
#include <utility>

namespace ruvia {

template <typename T>
class Task;

namespace detail {

[[nodiscard]] void* taskFrameAllocate(std::size_t bytes);
void taskFrameDeallocate(void* pointer) noexcept;

template <typename T>
class TaskPromise;

template <typename T>
class TaskAwaiter;

template <typename T, typename Handler>
class TaskCompletionState;

template <typename T, typename CompletionToken>
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

union TaskExceptionStorage {
    std::exception_ptr exception;

    TaskExceptionStorage() noexcept {}
    ~TaskExceptionStorage() noexcept {}
};

template <typename T>
class TaskPromise final {
public:
    using value_type = T;

    static_assert(!std::is_reference_v<T>, "ruvia::Task<T> does not support reference result types");

    TaskPromise() noexcept {}

    ~TaskPromise() {
        if (hasException_) {
            exceptionPointer()->~exception_ptr();
        }
    }

    static void* operator new(std::size_t size) {
        return taskFrameAllocate(size);
    }

    static void operator delete(void* pointer) noexcept {
        taskFrameDeallocate(pointer);
    }

    static void operator delete(void* pointer, std::size_t) noexcept {
        taskFrameDeallocate(pointer);
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
        value_.emplace(std::forward<U>(value));
    }

    void unhandled_exception() noexcept {
        ::new (static_cast<void*>(std::addressof(exceptionStorage_.exception))) std::exception_ptr(std::current_exception());
        hasException_ = true;
    }

    [[nodiscard]] T result() & {
        if (hasException_) [[unlikely]] {
            std::rethrow_exception(*exceptionPointer());
        }
        assert(value_.has_value());
        return std::move(*value_);
    }

private:
    template <typename>
    friend class ruvia::Task;
    template <typename>
    friend class TaskAwaiter;
    template <typename, typename>
    friend class TaskCompletionState;
    friend struct TaskFinalAwaiter;

    void setContinuation(std::coroutine_handle<> continuation) noexcept {
        continuation_ = continuation;
    }

    void setCompletion(void* state, void (*completion)(void*) noexcept) noexcept {
        completionState_ = state;
        completion_ = completion;
    }

    [[nodiscard]] std::exception_ptr* exceptionPointer() noexcept {
        return std::launder(std::addressof(exceptionStorage_.exception));
    }

    std::optional<T> value_;
    TaskExceptionStorage exceptionStorage_;
    bool hasException_{false};
    std::coroutine_handle<> continuation_{std::noop_coroutine()};
    void* completionState_{nullptr};
    void (*completion_)(void*) noexcept{nullptr};
};

template <>
class TaskPromise<void> final {
public:
    TaskPromise() noexcept {}

    ~TaskPromise() {
        if (hasException_) {
            exceptionPointer()->~exception_ptr();
        }
    }

    static void* operator new(std::size_t size) {
        return taskFrameAllocate(size);
    }

    static void operator delete(void* pointer) noexcept {
        taskFrameDeallocate(pointer);
    }

    static void operator delete(void* pointer, std::size_t) noexcept {
        taskFrameDeallocate(pointer);
    }

    [[nodiscard]] Task<void> get_return_object() noexcept;

    [[nodiscard]] std::suspend_always initial_suspend() const noexcept {
        return {};
    }

    [[nodiscard]] TaskFinalAwaiter final_suspend() noexcept {
        return {};
    }

    void return_void() const noexcept {}

    void unhandled_exception() noexcept {
        ::new (static_cast<void*>(std::addressof(exceptionStorage_.exception))) std::exception_ptr(std::current_exception());
        hasException_ = true;
    }

    void result() {
        if (hasException_) [[unlikely]] {
            std::rethrow_exception(*exceptionPointer());
        }
    }

private:
    template <typename>
    friend class ruvia::Task;
    template <typename>
    friend class TaskAwaiter;
    template <typename, typename>
    friend class TaskCompletionState;
    friend struct TaskFinalAwaiter;

    void setContinuation(std::coroutine_handle<> continuation) noexcept {
        continuation_ = continuation;
    }

    void setCompletion(void* state, void (*completion)(void*) noexcept) noexcept {
        completionState_ = state;
        completion_ = completion;
    }

    [[nodiscard]] std::exception_ptr* exceptionPointer() noexcept {
        return std::launder(std::addressof(exceptionStorage_.exception));
    }

    TaskExceptionStorage exceptionStorage_;
    bool hasException_{false};
    std::coroutine_handle<> continuation_{std::noop_coroutine()};
    void* completionState_{nullptr};
    void (*completion_)(void*) noexcept{nullptr};
};

}  // namespace detail

}  // namespace ruvia
