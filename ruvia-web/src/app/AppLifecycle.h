#pragma once

#include <cstdint>

namespace ruvia::detail {

enum class AppLifecycleState : std::uint8_t {
    kStopped,
    kPreparing,
    kStarting,
    kRunning,
    kStopping,
};

enum class AppStopRequest : std::uint8_t {
    kIgnored,
    kRequested,
};

// App owns the mutex protecting this state machine. App::run() is the sole
// lifecycle owner; other threads may only request the monotonic transition to
// kStopping.
class AppLifecycle final {
public:
    [[nodiscard]] AppLifecycleState state() const noexcept {
        return state_;
    }

    [[nodiscard]] bool active() const noexcept {
        return state_ != AppLifecycleState::kStopped;
    }

    [[nodiscard]] bool stopRequested() const noexcept {
        return state_ == AppLifecycleState::kStopping;
    }

    [[nodiscard]] bool beginRun() noexcept {
        if (state_ != AppLifecycleState::kStopped) {
            return false;
        }
        state_ = AppLifecycleState::kPreparing;
        return true;
    }

    [[nodiscard]] bool publishRuntime() noexcept {
        if (state_ != AppLifecycleState::kPreparing) {
            return false;
        }
        state_ = AppLifecycleState::kStarting;
        return true;
    }

    [[nodiscard]] bool markRunning() noexcept {
        if (state_ != AppLifecycleState::kStarting) {
            return false;
        }
        state_ = AppLifecycleState::kRunning;
        return true;
    }

    [[nodiscard]] AppStopRequest requestStop() noexcept {
        switch (state_) {
            case AppLifecycleState::kPreparing:
            case AppLifecycleState::kStarting:
            case AppLifecycleState::kRunning:
                state_ = AppLifecycleState::kStopping;
                return AppStopRequest::kRequested;
            case AppLifecycleState::kStopped:
            case AppLifecycleState::kStopping:
                return AppStopRequest::kIgnored;
        }
        return AppStopRequest::kIgnored;
    }

    void completeRun() noexcept {
        state_ = AppLifecycleState::kStopped;
    }

private:
    AppLifecycleState state_{AppLifecycleState::kStopped};
};

}  // namespace ruvia::detail
