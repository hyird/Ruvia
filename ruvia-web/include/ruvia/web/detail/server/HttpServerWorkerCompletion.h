#pragma once

#include <condition_variable>
#include <exception>
#include <mutex>
#include <variant>

namespace ruvia::detail {

// Owns the cross-thread worker handshake. Startup is a monotonic three-state
// result, while the first terminal worker failure remains available to join().
// Keeping both behind the same cold-path lock prevents callers from assembling
// readiness and failure from independently synchronized fields.
class HttpServerWorkerCompletion final {
public:
    [[nodiscard]] bool markStartupReady() noexcept {
        {
            std::lock_guard lock(mutex_);
            if (!std::holds_alternative<StartupPending>(startup_)) {
                return false;
            }
            startup_.emplace<StartupReady>();
        }
        startupCv_.notify_all();
        return true;
    }

    [[nodiscard]] bool markStartupFailed(std::exception_ptr failure) noexcept {
        if (failure == nullptr) {
            std::terminate();
        }
        {
            std::lock_guard lock(mutex_);
            if (!std::holds_alternative<StartupPending>(startup_)) {
                return false;
            }
            startup_.emplace<StartupFailed>(failure);
        }
        startupCv_.notify_all();
        return true;
    }

    void waitForStartup() {
        std::exception_ptr failure;
        {
            std::unique_lock lock(mutex_);
            startupCv_.wait(lock, [this] { return !std::holds_alternative<StartupPending>(startup_); });
            if (const auto* failed = std::get_if<StartupFailed>(&startup_)) {
                failure = failed->failure();
            }
        }
        if (failure != nullptr) {
            std::rethrow_exception(failure);
        }
    }

    [[nodiscard]] bool recordWorkerFailure(std::exception_ptr failure) noexcept {
        if (failure == nullptr) {
            return false;
        }
        std::lock_guard lock(mutex_);
        if (workerFailure_ != nullptr) {
            return false;
        }
        workerFailure_ = failure;
        return true;
    }

    [[nodiscard]] std::exception_ptr workerFailure() const noexcept {
        std::lock_guard lock(mutex_);
        return workerFailure_;
    }

private:
    class StartupPending final {};
    class StartupReady final {};

    class StartupFailed final {
    public:
        explicit StartupFailed(std::exception_ptr failure) noexcept
            : failure_(failure) {
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

    using Startup = std::variant<StartupPending, StartupReady, StartupFailed>;

    mutable std::mutex mutex_;
    std::condition_variable startupCv_;
    Startup startup_;
    std::exception_ptr workerFailure_;
};

}  // namespace ruvia::detail
