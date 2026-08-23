#pragma once

#include <condition_variable>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include <ruvia/core/WorkerHandle.h>
#include <ruvia/core/detail/util/FailureReport.h>

namespace ruvia {

class EventLoop;

namespace detail {

using EventLoopFailureSink = std::function<void(std::exception_ptr)>;

class RootTaskStateBase {
public:
    RootTaskStateBase(WorkerHandle worker, EventLoopFailureSink failureSink)
        : worker_(std::move(worker)), failureSink_(std::move(failureSink)) {}

    RootTaskStateBase(const RootTaskStateBase&) = delete;
    RootTaskStateBase& operator=(const RootTaskStateBase&) = delete;
    virtual ~RootTaskStateBase() = default;

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

    void completeFailure(std::exception_ptr failure) noexcept {
        std::exception_ptr unobserved;
        {
            const std::lock_guard lock(mutex_);
            if (complete_) {
                std::terminate();
            }
            failure_ = std::move(failure);
            complete_ = true;
            if (!handleAlive_) {
                unobserved = failure_;
            }
        }
        completed_.notify_all();
        report(std::move(unobserved));
    }

protected:
    void beforeGet() const {
        wait();
    }

    [[nodiscard]] std::exception_ptr consumeFailure() {
        std::lock_guard lock(mutex_);
        if (!handleAlive_) {
            throw std::logic_error("root task result was already consumed");
        }
        handleAlive_ = false;
        return failure_;
    }

    void completeSuccess() noexcept {
        {
            const std::lock_guard lock(mutex_);
            if (complete_) {
                std::terminate();
            }
            complete_ = true;
        }
        completed_.notify_all();
    }

    mutable std::mutex mutex_;

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
    mutable std::condition_variable completed_;
    std::exception_ptr failure_;
    bool complete_{false};
    bool handleAlive_{true};
};

template <typename T>
class RootTaskState final : public RootTaskStateBase {
public:
    using RootTaskStateBase::RootTaskStateBase;

    void completeValue(T value) noexcept {
        {
            const std::lock_guard lock(mutex_);
            value_.emplace(std::move(value));
        }
        completeSuccess();
    }

    [[nodiscard]] T get() {
        beforeGet();
        auto failure = consumeFailure();
        if (failure) {
            std::rethrow_exception(failure);
        }
        std::lock_guard lock(mutex_);
        return std::move(*value_);
    }

private:
    std::optional<T> value_;
};

template <>
class RootTaskState<void> final : public RootTaskStateBase {
public:
    using RootTaskStateBase::RootTaskStateBase;

    void completeValue() noexcept {
        completeSuccess();
    }

    void get() {
        beforeGet();
        auto failure = consumeFailure();
        if (failure) {
            std::rethrow_exception(failure);
        }
    }
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

    decltype(auto) get() {
        if (!state_) {
            throw std::logic_error("root task has no result");
        }
        if constexpr (std::is_void_v<T>) {
            state_->get();
            state_.reset();
            return;
        } else {
            auto value = state_->get();
            state_.reset();
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
