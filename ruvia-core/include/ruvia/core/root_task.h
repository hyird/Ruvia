#pragma once

#include <concepts>
#include <condition_variable>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>

#include <asio/io_context.hpp>

#include "ruvia/core/detail/io/asio_await.h"
#include "ruvia/core/task.h"
#include "ruvia/core/worker_handle.h"

namespace ruvia {

class event_loop;

namespace detail {

struct event_loop_state;

// Routes an unobserved root-task failure through the loop's own failure sink,
// borrowed from the live loop state instead of copied into every task. A
// retired loop falls back to the unhandled-failure diagnostic.
void report_root_task_failure(const std::weak_ptr<event_loop_state>& loop, std::exception_ptr failure) noexcept;

class root_task_completion final {
public:
    root_task_completion(worker_handle worker_value, std::weak_ptr<event_loop_state> loop_value) noexcept
        : worker_(std::move(worker_value)),
          loop_(std::move(loop_value)) {}

    root_task_completion(const root_task_completion&) = delete;
    root_task_completion& operator=(const root_task_completion&) = delete;

    [[nodiscard]] bool valid() const noexcept {
        const std::lock_guard lock(mutex_);
        return handle_alive_;
    }

    void wait() const {
        if (worker_.is_current()) {
            throw std::logic_error("cannot wait for a root task on its event loop");
        }
        std::unique_lock lock(mutex_);
        completed_.wait(lock, [this] { return complete_; });
    }

    void abandon() noexcept {
        std::exception_ptr unobserved;
        {
            const std::lock_guard lock(mutex_);
            if (!handle_alive_) {
                return;
            }
            handle_alive_ = false;
            if (complete_) {
                unobserved = failure_;
            }
        }
        report(std::move(unobserved));
    }

    void stage_failure(std::exception_ptr failure) noexcept {
        const std::lock_guard lock(mutex_);
        if (complete_) {
            std::terminate();
        }
        failure_ = std::move(failure);
    }

    // Publish only after the delivery owner has destroyed all intermediate
    // result objects. Their moved-from storage may still borrow a caller PMR.
    void publish_completion() noexcept {
        std::exception_ptr unobserved;
        {
            const std::lock_guard lock(mutex_);
            if (complete_) {
                std::terminate();
            }
            complete_ = true;
            if (!handle_alive_) {
                unobserved = failure_;
            }
        }
        completed_.notify_all();
        report(std::move(unobserved));
    }

    void complete_failure(std::exception_ptr failure) noexcept {
        stage_failure(std::move(failure));
        publish_completion();
    }

    [[nodiscard]] std::exception_ptr consume_failure() {
        wait();
        std::lock_guard lock(mutex_);
        if (!handle_alive_) {
            throw std::logic_error("root task result was already consumed");
        }
        handle_alive_ = false;
        return failure_;
    }

private:
    void report(std::exception_ptr failure) const noexcept {
        if (!failure) {
            return;
        }
        report_root_task_failure(loop_, std::move(failure));
    }

    worker_handle worker_;
    std::weak_ptr<event_loop_state> loop_;
    mutable std::mutex mutex_;
    mutable std::condition_variable completed_;
    std::exception_ptr failure_;
    bool complete_{false};
    bool handle_alive_{true};
};

struct root_task_void_result final {};

template <typename t_type>
class root_task_state final {
public:
    root_task_state(worker_handle worker_value, std::weak_ptr<event_loop_state> loop_value) noexcept
        : completion_(std::move(worker_value), std::move(loop_value)) {}

    // The type-independent completion protocol; only the result is typed here.
    [[nodiscard]] root_task_completion& completion() noexcept {
        return completion_;
    }
    [[nodiscard]] const root_task_completion& completion() const noexcept {
        return completion_;
    }

    template <typename value_type>
        requires std::same_as<std::remove_cvref_t<value_type>, std::remove_cv_t<t_type>>
    void stage_value(value_type&& value) {
        value_.emplace(std::forward<value_type>(value));
    }

    void stage_value() noexcept
        requires std::is_void_v<t_type>
    {}

    t_type get() {
        auto failure = completion_.consume_failure();
        if (failure) {
            std::rethrow_exception(failure);
        }
        if constexpr (!std::is_void_v<t_type>) {
            struct retire_value final {
                std::optional<t_type>& value_;
                ~retire_value() {
                    value_.reset();
                }
            } retire{value_};
            return std::move(*value_);
        }
    }

private:
    root_task_completion completion_;
    [[no_unique_address]] std::conditional_t<std::is_void_v<t_type>, root_task_void_result, std::optional<t_type>> value_;
};

// drain() destroys remaining queue closures without invoking them when
// a posted task throws. Own that destructor path so wait()/get() cannot
// hang and an unobserved failure still reaches the loop sink.
template <typename t_type>
struct root_task_launch_guard final {
    std::optional<task<t_type>> task_;
    std::shared_ptr<detail::root_task_state<t_type>> completion_;

    root_task_launch_guard(task<t_type> task_value, std::shared_ptr<detail::root_task_state<t_type>> state_value) noexcept
        : task_(std::move(task_value)),
          completion_(std::move(state_value)) {}
    root_task_launch_guard(const root_task_launch_guard&) = delete;
    root_task_launch_guard& operator=(const root_task_launch_guard&) = delete;
    root_task_launch_guard(root_task_launch_guard&& other) noexcept
        : task_(std::move(other.task_)),
          completion_(std::move(other.completion_)) {}
    root_task_launch_guard& operator=(root_task_launch_guard&&) = delete;
    ~root_task_launch_guard() {
        // A rejected or discarded launch still owns a cold frame.
        // Retire it before waking a caller that may reclaim its inputs.
        task_.reset();
        if (completion_ != nullptr) {
            completion_->completion().complete_failure(
                std::make_exception_ptr(std::runtime_error("event loop is stopping")));
        }
    }
    void release() noexcept {
        task_.reset();
        completion_.reset();
    }
};
template <typename t_type>
struct root_task_completion_handler final {
    using executor_type = asio::io_context::executor_type;

    executor_type executor_;
    std::shared_ptr<detail::root_task_state<t_type>> completion_;
    std::shared_ptr<void> retirement_lease_;
    bool delivered_{false};

    root_task_completion_handler(executor_type executor_value, std::shared_ptr<detail::root_task_state<t_type>> state_value,
        std::shared_ptr<void> lease_value) noexcept
        : executor_(std::move(executor_value)),
          completion_(std::move(state_value)),
          retirement_lease_(std::move(lease_value)) {}
    root_task_completion_handler(const root_task_completion_handler&) = delete;
    root_task_completion_handler& operator=(const root_task_completion_handler&) = delete;
    root_task_completion_handler(root_task_completion_handler&& other) noexcept
        : executor_(std::move(other.executor_)),
          completion_(std::move(other.completion_)),
          retirement_lease_(std::move(other.retirement_lease_)),
          delivered_(std::exchange(other.delivered_, false)) {}
    root_task_completion_handler& operator=(root_task_completion_handler&&) = delete;
    ~root_task_completion_handler() {
        if (delivered_) {
            completion_->completion().publish_completion();
        }
    }
    [[nodiscard]] executor_type get_executor() const noexcept {
        return executor_;
    }
    void operator()(detail::task_completion_result<t_type>&& result_value) noexcept {
        try {
            if constexpr (std::is_void_v<t_type>) {
                if (result_value != nullptr) {
                    completion_->completion().stage_failure(std::move(result_value));
                } else {
                    completion_->stage_value();
                }
            } else if (result_value.index() == 1) {
                completion_->completion().stage_failure(std::get<1>(result_value));
            } else {
                completion_->stage_value(std::move(std::get<0>(result_value)));
            }
        } catch (...) {
            completion_->completion().stage_failure(std::current_exception());
        }
        delivered_ = true;
    }
};

}  // namespace detail

template <typename t_type>
class [[nodiscard]] root_task final {
public:
    root_task(const root_task&) = delete;
    root_task& operator=(const root_task&) = delete;

    root_task(root_task&& other) noexcept
        : state_(std::move(other.state_)) {}

    root_task& operator=(root_task&& other) noexcept {
        if (this != &other) {
            // The old result may own or replace the source during retirement.
            root_task incoming(std::move(other));
            state_.swap(incoming.state_);
        }
        return *this;
    }

    ~root_task() {
        reset();
    }

    [[nodiscard]] bool valid() const noexcept {
        return state_ && state_->completion().valid();
    }

    void wait() const {
        require_state().completion().wait();
    }

    // User result moves run without the state mutex and may re-enter this
    // root_task. The object must remain alive for the entire synchronous call;
    // replacing its owned state during a move is supported.
    decltype(auto) get() {
        if (!state_) {
            throw std::logic_error("root task has no result");
        }
        auto state_value = state_;
        struct retire_state final {
            std::shared_ptr<detail::root_task_state<t_type>>& state_;
            const std::shared_ptr<detail::root_task_state<t_type>>& expected_;
            ~retire_state() {
                if (state_ == expected_ && !expected_->completion().valid()) {
                    state_.reset();
                }
            }
        } retire{state_, state_value};
        if constexpr (std::is_void_v<t_type>) {
            state_value->get();
            return;
        } else {
            return state_value->get();
        }
    }

private:
    explicit root_task(std::shared_ptr<detail::root_task_state<t_type>> state_value) noexcept
        : state_(std::move(state_value)) {}

    [[nodiscard]] const detail::root_task_state<t_type>& require_state() const {
        if (!state_) {
            throw std::logic_error("root task has no state");
        }
        return *state_;
    }

    void reset() noexcept {
        if (auto state_value = std::exchange(state_, nullptr)) {
            state_value->completion().abandon();
        }
    }

    std::shared_ptr<detail::root_task_state<t_type>> state_;
    friend class event_loop;
};

}  // namespace ruvia
