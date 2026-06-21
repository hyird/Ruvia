#pragma once

#include "ruvia/app/detail/TaskPromise.h"

#include <coroutine>
#include <stdexcept>
#include <utility>

namespace ruvia::detail {

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

}  // namespace ruvia::detail

namespace ruvia {

template <typename T>
[[nodiscard]] detail::TaskAwaiter<T> Task<T>::operator co_await() && {
    return detail::TaskAwaiter<T>{std::move(*this)};
}

inline detail::TaskAwaiter<void> Task<void>::operator co_await() && {
    return detail::TaskAwaiter<void>{std::move(*this)};
}

}  // namespace ruvia
