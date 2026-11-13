#pragma once

#include <coroutine>
#include <exception>
#include <stdexcept>
#include <utility>

#include "ruvia/core/detail/task/task_promise.h"

namespace ruvia {

class task_scope;

template <typename t_type, typename completion_token_type>
    requires detail::asio_task_result<t_type>
auto async_start(task<t_type>&& task_value, completion_token_type&& token);

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
class [[nodiscard]] task final {
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

    [[nodiscard]] task_awaiter<t_type> operator co_await() &&;
    [[nodiscard]] task_awaiter<t_type> operator co_await() & = delete;
    [[nodiscard]] task_awaiter<t_type> operator co_await() const& = delete;
    [[nodiscard]] task_awaiter<t_type> operator co_await() const&& = delete;

private:
    template <typename>
    friend class detail::task_promise;
    template <typename>
    friend class task_awaiter;
    template <typename, typename>
    friend class detail::task_completion_state;
    template <typename u_type, typename completion_token_type>
        requires detail::asio_task_result<u_type>
    friend auto async_start(task<u_type>&&, completion_token_type&&);
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

    void prepare_direct_completion() noexcept {
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

namespace ruvia::detail {

template <typename t_type>
[[nodiscard]] task<t_type> task_promise<t_type>::get_return_object() noexcept {
    return task<t_type>{std::coroutine_handle<task_promise<t_type>>::from_promise(*this)};
}

}  // namespace ruvia::detail

namespace ruvia {

// The awaiter of `co_await std::move(task_value)`: it owns the started task
// for the duration of one await-expression and is neither movable nor
// addressable. Serves value-returning tasks and task<void> alike:
// task_promise<void>::result() returns void, so `return promise.result();`
// below is well-formed for it too. Keeping one
// awaiter keeps the empty-task check and the suspend sequence stated once.
//
// await_resume carries no [[nodiscard]]: the compiler calls it as part of the
// co_await expansion rather than callers, and the attribute is not portable on
// a void return type. task itself is [[nodiscard]], which is the check that
// catches a dropped task.
template <typename t_type>
class task_awaiter final {
public:
    task_awaiter(const task_awaiter&) = delete;
    task_awaiter& operator=(const task_awaiter&) = delete;
    task_awaiter(task_awaiter&&) = delete;
    task_awaiter& operator=(task_awaiter&&) = delete;
    void operator&() const = delete;

    [[nodiscard]] bool await_ready() const noexcept {
        return task_.handle_.done();
    }

    [[nodiscard]] std::coroutine_handle<> await_suspend(
        std::coroutine_handle<> continuation) noexcept {
        task_.handle_.promise().control_.set_continuation(continuation);
        task_.handle_.promise().control_.mark_started();
        return task_.handle_;
    }

    t_type await_resume() {
        task_.prepare_direct_completion();
        struct complete_guard final {
            task_awaiter& awaiter_;
            ~complete_guard() {
                awaiter_.task_.finish_direct_completion();
            }
        } complete_value{*this};
        return task_.handle_.promise().result();
    }

private:
    friend class task<t_type>;

    explicit task_awaiter(task<t_type>&& task_value)
        : task_(take_ready_to_start(std::move(task_value))) {
        // Enter running only after the final owner has received the cold task;
        // optional return-value elision must not decide whether a move is legal.
        if (task_.registration_.scoped()) {
            task_.registration_.start();
        }
    }

    [[nodiscard]] static task<t_type> take_ready_to_start(task<t_type>&& task_value) {
        task_value.prepare_direct_start();
        return std::move(task_value);
    }

    task<t_type> task_;
};

template <typename t_type>
[[nodiscard]] task_awaiter<t_type> task<t_type>::operator co_await() && {
    return task_awaiter<t_type>{std::move(*this)};
}

}  // namespace ruvia
