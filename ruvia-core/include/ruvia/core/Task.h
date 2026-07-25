#pragma once

#include "ruvia/core/detail/task/TaskPromise.h"

#include <coroutine>
#include <exception>
#include <utility>

namespace ruvia {

class TaskScope;

// Task is a structured, lazy coroutine owner. A cold Task may be discarded and
// a completed Task may be destroyed, but a started Task must run to completion.
// Cancellation is cooperative: request it through the owning operation/scope
// and then await or join the Task. Destroying a suspended frame would invalidate
// every external await registration that borrows it, so that contract violation
// terminates instead of manufacturing use-after-free cancellation semantics.
//
// T = void is served by this same template rather than by a specialization:
// TaskPromise<void> offers an identical promise interface, so nothing below
// depends on T. Stating the contract once is the point -- a duplicated copy of
// the terminate below and of the co_await deletions can drift out of step with
// this one, and the compiler would not say a word.
template <typename T = void>
class [[nodiscard]] Task {
public:
    using value_type = T;
    using promise_type = detail::TaskPromise<T>;
    using handle_type = std::coroutine_handle<promise_type>;

    Task() = delete;

    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;

    Task(Task&& other) noexcept
        : handle_(std::exchange(other.handle_, {})) {}
    Task& operator=(Task&&) = delete;

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
    friend class TaskScope;

    explicit Task(handle_type handle) noexcept
        : handle_(handle) {}

    void start() noexcept {
        if (handle_ != nullptr) {
            handle_.promise().markStarted();
            handle_.resume();
        }
    }

    void reset() noexcept {
        if (handle_ != nullptr) {
            auto handle = std::exchange(handle_, {});
            if (!handle.done() && handle.promise().started()) {
                std::terminate();
            }
            handle.destroy();
        }
    }

    handle_type handle_;
};

}  // namespace ruvia

#include "ruvia/core/detail/task/TaskAwaiter.h"
