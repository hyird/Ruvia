#pragma once

#include <coroutine>
#include <stdexcept>
#include <utility>

#include "ruvia/core/detail/task/task_promise.h"
#include "ruvia/core/task.h"

namespace ruvia::detail {

template <typename t_type>
[[nodiscard]] task<t_type> task_promise<t_type>::get_return_object() noexcept {
    return task<t_type>{std::coroutine_handle<task_promise<t_type>>::from_promise(*this)};
}

inline task<void> task_promise<void>::get_return_object() noexcept {
    return task<void>{std::coroutine_handle<task_promise<void>>::from_promise(*this)};
}

// Serves value-returning tasks and task<void> alike: task_promise<void>::result() returns void,
// so `return promise.result();` below is well-formed for it too. Keeping one
// awaiter keeps the empty-task check and the suspend sequence stated once.
//
// await_resume carries no [[nodiscard]]: the compiler calls it as part of the
// co_await expansion rather than callers, and the attribute is not portable on
// a void return type. task itself is [[nodiscard]], which is the check that
// catches a dropped task.
template <typename t_type>
class task_awaiter final {
public:
    explicit task_awaiter(task<t_type>&& task_value)
        : task_(take_ready_to_start(std::move(task_value))) {}

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

    // Scoped owners must release frame-held parameters before publishing
    // completion. Ordinary task awaiting still retains its frame until this
    // awaiter is destroyed; only a completed frame may be retired explicitly.
    void retire_completed_frame() noexcept {
        if (task_.handle_ != nullptr && !task_.handle_.done()) {
            std::terminate();
        }
        task_.reset();
    }

private:
    [[nodiscard]] static task<t_type> take_ready_to_start(task<t_type>&& task_value) {
        task_value.prepare_direct_start();
        task<t_type> moved(std::move(task_value));
        if (moved.registration_.scoped()) {
            moved.registration_.start();
        }
        return moved;
    }

    task<t_type> task_;
};

}  // namespace ruvia::detail

namespace ruvia {

template <typename t_type>
[[nodiscard]] detail::task_awaiter<t_type> task<t_type>::operator co_await() && {
    return detail::task_awaiter<t_type>{std::move(*this)};
}

}  // namespace ruvia
