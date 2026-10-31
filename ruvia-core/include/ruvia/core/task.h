#pragma once

#include <coroutine>
#include <exception>
#include <stdexcept>
#include <utility>

#include "ruvia/core/detail/task/task_promise.h"

namespace ruvia {

class task_scope;

// task is a structured, lazy coroutine owner. A cold task may be discarded and
// a completed task may be destroyed, but a started task must run to completion.
// Frame allocation happens when the coroutine is called; allocation failure
// throws std::bad_alloc before a task is returned.
// Over-aligned result types are supported. GCC 13/14 can misalign over-aligned
// coroutine locals independently of the promise allocator; keep such locals in
// separately allocated, owned storage rather than directly in the frame.
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
        : handle_(std::exchange(other.handle_, {})),
          registration_(std::move(other.registration_), std::addressof(handle_)) {}
    task& operator=(task&&) = delete;

    ~task() {
        if (registration_.scoped()) {
            registration_.retire_frame();
        } else {
            reset();
        }
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
    template <typename u_type>
    friend task<u_type> make_scoped_operation(operation_scope&, task<u_type>);

    explicit task(handle_type handle) noexcept
        : handle_(handle) {}

    void prepare_direct_start() const {
        if (registration_.scoped()) {
            registration_.prepare_start();
        }
        if (handle_ == nullptr) {
            throw std::logic_error("cannot start an empty ruvia::task");
        }
    }

    void start() noexcept {
        if (registration_.scoped()) {
            registration_.start();
        }
        handle_.promise().control_.mark_started();
        handle_.resume();
    }

    void prepare_direct_completion() const noexcept {
        if (registration_.scoped()) {
            registration_.prepare_completion();
        }
    }

    void finish_direct_completion() noexcept {
        if (registration_.scoped()) {
            reset();
            registration_.complete();
        }
    }

    static void retire_cold_handle(void* target) noexcept {
        auto& handle_ref = *static_cast<handle_type*>(target);
        if (handle_ref != nullptr) {
            auto handle = std::exchange(handle_ref, {});
            if (!handle.done() && handle.promise().control_.started()) {
                std::terminate();
            }
            handle.destroy();
        }
    }

    void reset() noexcept {
        retire_cold_handle(std::addressof(handle_));
    }

    void bind_scope(operation_scope& scope) noexcept {
        registration_.bind(scope, std::addressof(handle_), &task::retire_cold_handle);
    }

    handle_type handle_{};
    detail::scoped_operation_registration registration_{};
};

}  // namespace ruvia

#include "ruvia/core/detail/task/task_awaiter.h"
