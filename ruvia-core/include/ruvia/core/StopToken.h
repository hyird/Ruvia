#pragma once

#include <algorithm>
#include <atomic>
#include <exception>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include <ruvia/core/MoveOnlyFunction.h>

namespace ruvia {

namespace detail {
class StopCallbackState final {
public:
    explicit StopCallbackState(MoveOnlyFunction<void()> callback)
        : callback_(std::move(callback)) {}

    void run() noexcept {
        MoveOnlyFunction<void()> callback;
        {
            std::lock_guard lock(mutex_);
            if (!active_) {
                return;
            }
            active_ = false;
            callback = std::move(callback_);
        }
        try {
            if (callback) {
                callback();
            }
        } catch (...) {
            std::terminate();
        }
    }

    void cancel() noexcept {
        std::lock_guard lock(mutex_);
        active_ = false;
        callback_ = nullptr;
    }

private:
    std::mutex mutex_;
    MoveOnlyFunction<void()> callback_;
    bool active_{true};
};

struct StopState final {
    std::atomic_bool requested{false};
    mutable std::mutex mutex;
    mutable std::vector<std::shared_ptr<StopCallbackState>> callbacks;
};

class StopSource;
}  // namespace detail

class StopRegistration final {
public:
    StopRegistration() noexcept = default;
    ~StopRegistration() {
        reset();
    }

    StopRegistration(const StopRegistration&) = delete;
    StopRegistration& operator=(const StopRegistration&) = delete;
    StopRegistration(StopRegistration&& other) noexcept
        : state_(std::move(other.state_)),
          callback_(std::move(other.callback_)) {}
    StopRegistration& operator=(StopRegistration&& other) noexcept {
        if (this != &other) {
            reset();
            state_ = std::move(other.state_);
            callback_ = std::move(other.callback_);
        }
        return *this;
    }

    void reset() noexcept {
        auto state = std::move(state_);
        auto callback = std::move(callback_);
        if (callback == nullptr) {
            return;
        }
        if (state != nullptr) {
            std::lock_guard lock(state->mutex);
            std::erase(state->callbacks, callback);
        }
        callback->cancel();
    }

    [[nodiscard]] bool registered() const noexcept {
        return callback_ != nullptr;
    }

private:
    friend class StopToken;

    StopRegistration(std::shared_ptr<detail::StopState> state, std::shared_ptr<detail::StopCallbackState> callback) noexcept
        : state_(std::move(state)),
          callback_(std::move(callback)) {}

    std::shared_ptr<detail::StopState> state_;
    std::shared_ptr<detail::StopCallbackState> callback_;
};

class StopToken final {
public:
    StopToken() noexcept = default;

    [[nodiscard]] bool stopRequested() const noexcept {
        return state_ != nullptr && state_->requested.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool stoppable() const noexcept {
        return state_ != nullptr;
    }

    // Registers one callback while stop remains unrequested. requestStop() may
    // run on any thread and invokes callbacks on that requesting thread, so a
    // callback that touches worker-affine state must only enqueue work onto its
    // owner. Destroying/resetting the registration suppresses a callback that
    // has not begun; an already-running callback is allowed to finish.
    [[nodiscard]] StopRegistration registerCallback(MoveOnlyFunction<void()> callback) const {
        if (state_ == nullptr || !callback) {
            return {};
        }
        auto registered = std::make_shared<detail::StopCallbackState>(std::move(callback));
        {
            std::lock_guard lock(state_->mutex);
            if (!state_->requested.load(std::memory_order_acquire)) {
                state_->callbacks.push_back(registered);
                return StopRegistration(std::const_pointer_cast<detail::StopState>(state_), std::move(registered));
            }
        }
        registered->run();
        return {};
    }

private:
    friend class detail::StopSource;

    explicit StopToken(std::shared_ptr<detail::StopState> state) noexcept
        : state_(std::move(state)) {}

    std::shared_ptr<const detail::StopState> state_;
};

namespace detail {

class StopSource final {
public:
    StopSource()
        : state_(std::make_shared<StopState>()) {}
    StopSource(const StopSource&) = delete;
    StopSource& operator=(const StopSource&) = delete;
    StopSource(StopSource&&) = delete;
    StopSource& operator=(StopSource&&) = delete;

    void requestStop() noexcept {
        std::vector<std::shared_ptr<StopCallbackState>> callbacks;
        {
            std::lock_guard lock(state_->mutex);
            if (state_->requested.exchange(true, std::memory_order_acq_rel)) {
                return;
            }
            callbacks.swap(state_->callbacks);
        }
        for (const auto& callback : callbacks) {
            callback->run();
        }
    }

    [[nodiscard]] bool stopRequested() const noexcept {
        return state_->requested.load(std::memory_order_acquire);
    }

    [[nodiscard]] StopToken token() const noexcept {
        return StopToken(state_);
    }

private:
    std::shared_ptr<StopState> state_;
};

}  // namespace detail

}  // namespace ruvia
