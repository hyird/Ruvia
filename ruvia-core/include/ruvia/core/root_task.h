#pragma once

#include <concepts>
#include <condition_variable>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include "ruvia/core/detail/util/failure_report.h"
#include "ruvia/core/worker_handle.h"

namespace ruvia {

class event_loop;

namespace detail {

using event_loop_failure_sink_type = std::function<void(std::exception_ptr)>;

class root_task_completion final {
public:
    root_task_completion(worker_handle worker_value, event_loop_failure_sink_type failure_sink)
        : worker_(std::move(worker_value)),
          failure_sink_(std::move(failure_sink)) {}

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
        if (failure_sink_) {
            failure_sink_(std::move(failure));
            return;
        }
        report_unhandled_failure("unobserved event-loop root task", std::move(failure));
    }

    worker_handle worker_;
    event_loop_failure_sink_type failure_sink_;
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
    root_task_state(worker_handle worker_value, event_loop_failure_sink_type failure_sink)
        : completion_(std::move(worker_value), std::move(failure_sink)) {}

    [[nodiscard]] bool valid() const noexcept {
        return completion_.valid();
    }

    void wait() const {
        completion_.wait();
    }

    void abandon() noexcept {
        completion_.abandon();
    }

    void stage_failure(std::exception_ptr failure) noexcept {
        completion_.stage_failure(std::move(failure));
    }

    void publish_completion() noexcept {
        completion_.publish_completion();
    }

    void complete_failure(std::exception_ptr failure) noexcept {
        completion_.complete_failure(std::move(failure));
    }

    template <typename value_type>
        requires std::same_as<value_type, std::remove_cv_t<t_type>>
    void stage_value(value_type value) {
        value_.emplace(std::move(value));
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
            reset();
            state_ = std::move(other.state_);
        }
        return *this;
    }

    ~root_task() {
        reset();
    }

    [[nodiscard]] bool valid() const noexcept {
        return state_ && state_->valid();
    }

    void wait() const {
        require_state().wait();
    }

    // User result moves run without the state mutex and may re-enter this
    // root_task. The object must remain alive for the entire synchronous call;
    // replacing its owned state during a move is supported.
    decltype(auto) get() {
        if (!state_) {
            throw std::logic_error("root task has no result");
        }
        auto state_value = state_;
        if constexpr (std::is_void_v<t_type>) {
            state_value->get();
            if (state_ == state_value) {
                state_.reset();
            }
            return;
        } else {
            auto value = state_value->get();
            if (state_ == state_value) {
                state_.reset();
            }
            return value;
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
        if (state_) {
            state_->abandon();
            state_.reset();
        }
    }

    std::shared_ptr<detail::root_task_state<t_type>> state_;
    friend class event_loop;
};

}  // namespace ruvia
