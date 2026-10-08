#pragma once

#include <condition_variable>
#include <exception>
#include <mutex>
#include <utility>
#include <variant>

namespace ruvia::detail {

// Owns the cross-thread worker handshake. startup_result is a monotonic
// result, while the first terminal worker failure remains available to join().
// Keeping both behind the same cold-path lock prevents callers from assembling
// readiness and failure from independently synchronized fields.
class runtime_completion final {
public:
    [[nodiscard]] bool mark_startup_ready() noexcept {
        {
            std::lock_guard lock(mutex_);
            if (!std::holds_alternative<startup_pending>(startup_)) {
                return false;
            }
            startup_.emplace<startup_ready>();
        }
        startup_cv_.notify_all();
        return true;
    }

    [[nodiscard]] bool mark_startup_failed(std::exception_ptr failure) noexcept {
        if (failure == nullptr) {
            std::terminate();
        }
        {
            std::lock_guard lock(mutex_);
            if (!std::holds_alternative<startup_pending>(startup_)) {
                return false;
            }
            startup_.emplace<startup_failed>(std::move(failure));
        }
        startup_cv_.notify_all();
        return true;
    }

    void mark_startup_aborted() noexcept {
        {
            std::lock_guard lock(mutex_);
            if (!std::holds_alternative<startup_pending>(startup_)) {
                return;
            }
            startup_.emplace<startup_aborted>();
        }
        startup_cv_.notify_all();
    }

    void wait_for_startup() {
        std::exception_ptr failure;
        {
            std::unique_lock lock(mutex_);
            startup_cv_.wait(
                lock, [this] { return !std::holds_alternative<startup_pending>(startup_); });
            if (const auto* failed = std::get_if<startup_failed>(&startup_)) {
                failure = failed->failure();
            }
        }
        if (failure != nullptr) {
            std::rethrow_exception(failure);
        }
    }

    [[nodiscard]] bool mark_serving() noexcept {
        {
            std::lock_guard lock(mutex_);
            if (serving_complete_) {
                return false;
            }
            serving_ = true;
            serving_complete_ = true;
        }
        serving_cv_.notify_all();
        return true;
    }

    void mark_serving_aborted() noexcept {
        {
            std::lock_guard lock(mutex_);
            if (serving_complete_) {
                return;
            }
            serving_complete_ = true;
        }
        serving_cv_.notify_all();
    }

    [[nodiscard]] bool wait_for_serving() {
        std::exception_ptr failure;
        bool serving = false;
        {
            std::unique_lock lock(mutex_);
            serving_cv_.wait(lock, [this] { return serving_complete_ || failure_ != nullptr; });
            failure = failure_;
            serving = serving_;
        }
        if (failure != nullptr) {
            std::rethrow_exception(failure);
        }
        return serving;
    }

    [[nodiscard]] bool record_failure(std::exception_ptr failure) noexcept {
        if (failure == nullptr) {
            return false;
        }
        {
            std::lock_guard lock(mutex_);
            if (failure_ != nullptr) {
                return false;
            }
            failure_ = failure;
            if (std::holds_alternative<startup_pending>(startup_)) {
                startup_.emplace<startup_failed>(std::move(failure));
            }
            serving_complete_ = true;
        }
        startup_cv_.notify_all();
        serving_cv_.notify_all();
        return true;
    }

    [[nodiscard]] std::exception_ptr failure() const noexcept {
        std::lock_guard lock(mutex_);
        return failure_;
    }

private:
    class startup_pending final {};
    class startup_ready final {};
    class startup_aborted final {};

    class startup_failed final {
    public:
        explicit startup_failed(std::exception_ptr failure) noexcept
            : failure_(std::move(failure)) {
            if (failure_ == nullptr) {
                std::terminate();
            }
        }

        [[nodiscard]] std::exception_ptr failure() const noexcept {
            return failure_;
        }

    private:
        std::exception_ptr failure_;
    };

    using startup_result = std::variant<startup_pending, startup_ready, startup_aborted, startup_failed>;

    mutable std::mutex mutex_;
    std::condition_variable startup_cv_;
    std::condition_variable serving_cv_;
    startup_result startup_;
    std::exception_ptr failure_;
    bool serving_{false};
    bool serving_complete_{false};
};

}  // namespace ruvia::detail
