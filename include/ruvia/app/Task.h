#pragma once

#include "ruvia/app/detail/TaskPromise.h"

#include <coroutine>
#include <utility>

namespace ruvia {

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

}  // namespace ruvia

#include "ruvia/app/detail/TaskAwaiter.h"
