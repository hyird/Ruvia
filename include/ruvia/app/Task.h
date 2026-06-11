#pragma once

#include <cassert>
#include <concepts>
#include <coroutine>
#include <cstddef>
#include <exception>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace ruvia {

template <typename T>
class Task;

namespace detail {

// Coroutine frame allocation for ruvia::Task<T>. Routes through the process
// mimalloc heap instead of the system allocator; defined in MemoryPool.cpp.
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
    // Manually managed exception slot: a plain std::exception_ptr member would
    // pay a non-inlined destructor call per frame even when no exception fired.
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

    // Manually managed exception slot: a plain std::exception_ptr member would
    // pay a non-inlined destructor call per frame even when no exception fired.
    TaskExceptionStorage exceptionStorage_;
    bool hasException_{false};
    std::coroutine_handle<> continuation_{std::noop_coroutine()};
    void* completionState_{nullptr};
    void (*completion_)(void*) noexcept{nullptr};
};

}  // namespace detail

template <typename T = void>
class [[nodiscard]] Task {
public:
    using value_type = T;
    using promise_type = detail::TaskPromise<T>;
    using handle_type = std::coroutine_handle<promise_type>;

    Task() = delete;

    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;

    Task(Task&& other) noexcept : handle_(std::exchange(other.handle_, {})) {}

    Task& operator=(Task&& other) noexcept {
        if (this == &other) {
            return *this;
        }
        reset();
        handle_ = std::exchange(other.handle_, {});
        return *this;
    }

    ~Task() {
        reset();
    }

    [[nodiscard]] detail::TaskAwaiter<T> operator co_await() &&;
    [[nodiscard]] detail::TaskAwaiter<T> operator co_await() & = delete;
    [[nodiscard]] detail::TaskAwaiter<T> operator co_await() const& = delete;
    [[nodiscard]] detail::TaskAwaiter<T> operator co_await() const&& = delete;

private:
    template <typename>
    friend class detail::TaskPromise;
    template <typename>
    friend class detail::TaskAwaiter;
    template <typename, typename>
    friend class detail::TaskCompletionState;
    template <typename U, typename CompletionToken>
    friend auto detail::asyncStartTask(Task<U>&&, CompletionToken&&);

    explicit Task(handle_type handle) noexcept : handle_(handle) {}

    void start() noexcept {
        if (handle_ != nullptr) {
            handle_.resume();
        }
    }

    void reset() noexcept {
        if (handle_ != nullptr) {
            handle_.destroy();
            handle_ = nullptr;
        }
    }

    handle_type handle_;
};

template <>
class [[nodiscard]] Task<void> {
public:
    using value_type = void;
    using promise_type = detail::TaskPromise<void>;
    using handle_type = std::coroutine_handle<promise_type>;

    Task() = delete;

    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;

    Task(Task&& other) noexcept : handle_(std::exchange(other.handle_, {})) {}

    Task& operator=(Task&& other) noexcept {
        if (this == &other) {
            return *this;
        }
        reset();
        handle_ = std::exchange(other.handle_, {});
        return *this;
    }

    ~Task() {
        reset();
    }

    [[nodiscard]] detail::TaskAwaiter<void> operator co_await() &&;
    [[nodiscard]] detail::TaskAwaiter<void> operator co_await() & = delete;
    [[nodiscard]] detail::TaskAwaiter<void> operator co_await() const& = delete;
    [[nodiscard]] detail::TaskAwaiter<void> operator co_await() const&& = delete;

private:
    template <typename>
    friend class detail::TaskPromise;
    template <typename>
    friend class detail::TaskAwaiter;
    template <typename, typename>
    friend class detail::TaskCompletionState;
    template <typename U, typename CompletionToken>
    friend auto detail::asyncStartTask(Task<U>&&, CompletionToken&&);

    explicit Task(handle_type handle) noexcept : handle_(handle) {}

    void start() noexcept {
        if (handle_ != nullptr) {
            handle_.resume();
        }
    }

    void reset() noexcept {
        if (handle_ != nullptr) {
            handle_.destroy();
            handle_ = nullptr;
        }
    }

    handle_type handle_;
};

namespace detail {

template <typename T>
[[nodiscard]] Task<T> TaskPromise<T>::get_return_object() noexcept {
    return Task<T>{std::coroutine_handle<TaskPromise<T>>::from_promise(*this)};
}

inline Task<void> TaskPromise<void>::get_return_object() noexcept {
    return Task<void>{std::coroutine_handle<TaskPromise<void>>::from_promise(*this)};
}

template <typename T>
class TaskAwaiter final {
public:
    explicit TaskAwaiter(Task<T>&& task) : task_(std::move(task)) {
        if (task_.handle_ == nullptr) {
            throw std::logic_error("cannot await an empty ruvia::Task");
        }
    }

    [[nodiscard]] bool await_ready() const noexcept {
        return task_.handle_.done();
    }

    [[nodiscard]] std::coroutine_handle<> await_suspend(std::coroutine_handle<> continuation) noexcept {
        task_.handle_.promise().setContinuation(continuation);
        return task_.handle_;
    }

    [[nodiscard]] T await_resume() {
        return task_.handle_.promise().result();
    }

private:
    Task<T> task_;
};

template <>
class TaskAwaiter<void> final {
public:
    explicit TaskAwaiter(Task<void>&& task) : task_(std::move(task)) {
        if (task_.handle_ == nullptr) {
            throw std::logic_error("cannot await an empty ruvia::Task");
        }
    }

    [[nodiscard]] bool await_ready() const noexcept {
        return task_.handle_.done();
    }

    [[nodiscard]] std::coroutine_handle<> await_suspend(std::coroutine_handle<> continuation) noexcept {
        task_.handle_.promise().setContinuation(continuation);
        return task_.handle_;
    }

    void await_resume() {
        task_.handle_.promise().result();
    }

private:
    Task<void> task_;
};

}  // namespace detail

template <typename T>
[[nodiscard]] detail::TaskAwaiter<T> Task<T>::operator co_await() && {
    return detail::TaskAwaiter<T>{std::move(*this)};
}

inline detail::TaskAwaiter<void> Task<void>::operator co_await() && {
    return detail::TaskAwaiter<void>{std::move(*this)};
}

}  // namespace ruvia
