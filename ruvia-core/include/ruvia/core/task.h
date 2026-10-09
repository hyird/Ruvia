#pragma once

#include <coroutine>
#include <exception>
#include <utility>

#include "ruvia/core/detail/task/task_promise.h"

namespace ruvia {

class task_scope;

// task is a structured, lazy coroutine owner. A cold task may be discarded and
// a completed task may be destroyed, but a started task must run to completion.
// Cancellation is cooperative: request it through the owning operation/scope
// and then await or join the task. Destroying a suspended frame would invalidate
// every external await registration that borrows it, so that contract violation
// terminates instead of manufacturing use-after-free cancellation semantics.
//
// T = void is served by this same template rather than by a specialization:
// task_promise<void> offers an identical promise interface, so nothing below
// depends on T. Stating the contract once is the point -- a duplicated copy of
// the terminate below and of the co_await deletions can drift out of step with
// this one, and the compiler would not say a word.
template <typename t_type>
class [[nodiscard]] task {
public:
    using value_type = t_type;
    using promise_type = detail::task_promise<t_type>;
    using handle_type = std::coroutine_handle<promise_type>;

    task() = delete;

    task(const task&) = delete;
    task& operator=(const task&) = delete;

    task(task&& other) noexcept
        : handle_(std::exchange(other.handle_, {})) {}
    task& operator=(task&&) = delete;

    ~task() {
        reset();
    }

    [[nodiscard]] detail::task_awaiter<t_type> operator co_await() &&;
    [[nodiscard]] detail::task_awaiter<t_type> operator co_await() & = delete;
    [[nodiscard]] detail::task_awaiter<t_type> operator co_await() const& = delete;
    [[nodiscard]] detail::task_awaiter<t_type> operator co_await() const&& = delete;

private:
    template <typename>
    friend class detail::task_promise;
    template <typename>
    friend class detail::task_awaiter;
    template <typename, typename>
    friend class detail::task_completion_state;
    template <typename u_type, typename completion_token_type>
        requires detail::asio_task_result<u_type>
    friend auto detail::async_start_task(task<u_type>&&, completion_token_type&&);
    friend class task_scope;

    explicit task(handle_type handle) noexcept
        : handle_(handle) {}

    void start() noexcept {
        if (handle_ != nullptr) {
            handle_.promise().control_.mark_started();
            handle_.resume();
        }
    }

    void reset() noexcept {
        if (handle_ != nullptr) {
            auto handle = std::exchange(handle_, {});
            if (!handle.done() && handle.promise().control_.started()) {
                std::terminate();
            }
            handle.destroy();
        }
    }

    handle_type handle_;
};

}  // namespace ruvia

#include "ruvia/core/detail/task/task_awaiter.h"
