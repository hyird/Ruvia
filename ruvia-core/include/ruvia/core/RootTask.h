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

#include "ruvia/core/WorkerHandle.h"
#include "ruvia/core/detail/util/FailureReport.h"

namespace ruvia {

class EventLoop;

namespace detail {

using EventLoopFailureSink = std::function<void(std::exception_ptr)>;

class root_task_completion final {
public:
    root_task_completion(WorkerHandle worker, EventLoopFailureSink failure_sink)
        : worker_(std::move(worker)),
          failureSink_(std::move(failure_sink)) {}

    root_task_completion(const root_task_completion&) = delete;
    root_task_completion& operator=(const root_task_completion&) = delete;

    [[nodiscard]] bool valid() const noexcept {
        const std::lock_guard lock(mutex_);
        return handleAlive_;
    }

    void wait() const {
        if (worker_.isCurrent()) {
            throw std::logic_error("cannot wait for a root task on its event loop");
        }
        std::unique_lock lock(mutex_);
        completed_.wait(lock, [this] { return complete_; });
    }

    void abandon() noexcept {
        std::exception_ptr unobserved;
        {
            const std::lock_guard lock(mutex_);
            if (!handleAlive_) {
                return;
            }
            handleAlive_ = false;
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
            if (!handleAlive_) {
                unobserved = failure_;
            }
        }
        completed_.notify_all();
        report(std::move(unobserved));
    }

    void completeFailure(std::exception_ptr failure) noexcept {
        stage_failure(std::move(failure));
        publish_completion();
    }

    [[nodiscard]] std::exception_ptr consume_failure() {
        wait();
        std::lock_guard lock(mutex_);
        if (!handleAlive_) {
            throw std::logic_error("root task result was already consumed");
        }
        handleAlive_ = false;
        return failure_;
    }

private:
    void report(std::exception_ptr failure) const noexcept {
        if (!failure) {
            return;
        }
        if (failureSink_) {
            failureSink_(std::move(failure));
            return;
        }
        reportUnhandledFailure("unobserved event-loop root task", std::move(failure));
    }

    WorkerHandle worker_;
    EventLoopFailureSink failureSink_;
    mutable std::mutex mutex_;
    mutable std::condition_variable completed_;
    std::exception_ptr failure_;
    bool complete_{false};
    bool handleAlive_{true};
};

struct root_task_void_result final {};

template <typename T>
class RootTaskState final {
public:
    RootTaskState(WorkerHandle worker, EventLoopFailureSink failure_sink)
        : completion_(std::move(worker), std::move(failure_sink)) {}

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

    void completeFailure(std::exception_ptr failure) noexcept {
        completion_.completeFailure(std::move(failure));
    }

    template <typename value_type>
        requires std::same_as<value_type, std::remove_cv_t<T>>
    void stage_value(value_type value) {
        value_.emplace(std::move(value));
    }

    void stage_value() noexcept
        requires std::is_void_v<T>
    {}

    T get() {
        auto failure = completion_.consume_failure();
        if (failure) {
            std::rethrow_exception(failure);
        }
        if constexpr (!std::is_void_v<T>) {
            struct retire_value final {
                std::optional<T>& value;
                ~retire_value() {
                    value.reset();
                }
            } retire{value_};
            return std::move(*value_);
        }
    }

private:
    root_task_completion completion_;
    [[no_unique_address]] std::conditional_t<std::is_void_v<T>, root_task_void_result, std::optional<T>> value_;
};

}  // namespace detail

template <typename T>
class [[nodiscard]] RootTask final {
public:
    RootTask(const RootTask&) = delete;
    RootTask& operator=(const RootTask&) = delete;

    RootTask(RootTask&& other) noexcept
        : state_(std::move(other.state_)) {}

    RootTask& operator=(RootTask&& other) noexcept {
        if (this != &other) {
            reset();
            state_ = std::move(other.state_);
        }
        return *this;
    }

    ~RootTask() {
        reset();
    }

    [[nodiscard]] bool valid() const noexcept {
        return state_ && state_->valid();
    }

    void wait() const {
        requireState().wait();
    }

    // User result moves run without the state mutex and may re-enter this
    // RootTask. The object must remain alive for the entire synchronous call;
    // replacing its owned state during a move is supported.
    decltype(auto) get() {
        if (!state_) {
            throw std::logic_error("root task has no result");
        }
        auto state = state_;
        if constexpr (std::is_void_v<T>) {
            state->get();
            if (state_ == state) {
                state_.reset();
            }
            return;
        } else {
            auto value = state->get();
            if (state_ == state) {
                state_.reset();
            }
            return value;
        }
    }

private:
    explicit RootTask(std::shared_ptr<detail::RootTaskState<T>> state) noexcept
        : state_(std::move(state)) {}

    [[nodiscard]] const detail::RootTaskState<T>& requireState() const {
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

    std::shared_ptr<detail::RootTaskState<T>> state_;
    friend class EventLoop;
};

}  // namespace ruvia
