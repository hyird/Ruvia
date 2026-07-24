#pragma once

namespace ruvia::detail {

enum class AppLifecycleState {
    kStopped,
    kStarting,
    kStartHooksRunning,
    kRunning,
    kStopRequestedDuringStartHooks,
    kStopping,
};

enum class AppStopRequest {
    kIgnored,
    kStopWorkers,
    kStopWorkersAndRunHooks,
};

enum class AppStartHooksCompletion {
    kRunning,
    kRunDeferredStopHooks,
};

// App owns the mutex protecting this state machine. Keeping transitions here
// makes the stop-hook claim part of the lifecycle state instead of an
// independently mutable flag.
class AppLifecycle final {
public:
    [[nodiscard]] AppLifecycleState state() const noexcept {
        return state_;
    }

    [[nodiscard]] bool active() const noexcept {
        return state_ != AppLifecycleState::kStopped;
    }

    [[nodiscard]] bool stopRequested() const noexcept {
        return state_ == AppLifecycleState::kStopRequestedDuringStartHooks || state_ == AppLifecycleState::kStopping;
    }

    [[nodiscard]] bool beginRun() noexcept {
        if (state_ != AppLifecycleState::kStopped) {
            return false;
        }
        state_ = AppLifecycleState::kStarting;
        return true;
    }

    [[nodiscard]] bool beginStartHooks() noexcept {
        if (state_ != AppLifecycleState::kStarting) {
            return false;
        }
        state_ = AppLifecycleState::kStartHooksRunning;
        return true;
    }

    [[nodiscard]] AppStartHooksCompletion completeStartHooks() noexcept {
        if (state_ == AppLifecycleState::kStopRequestedDuringStartHooks) {
            state_ = AppLifecycleState::kStopping;
            return AppStartHooksCompletion::kRunDeferredStopHooks;
        }
        state_ = AppLifecycleState::kRunning;
        return AppStartHooksCompletion::kRunning;
    }

    [[nodiscard]] AppStopRequest requestStop() noexcept {
        switch (state_) {
            case AppLifecycleState::kStarting:
            case AppLifecycleState::kRunning:
                state_ = AppLifecycleState::kStopping;
                return AppStopRequest::kStopWorkersAndRunHooks;
            case AppLifecycleState::kStartHooksRunning:
                state_ = AppLifecycleState::kStopRequestedDuringStartHooks;
                return AppStopRequest::kStopWorkers;
            case AppLifecycleState::kStopped:
            case AppLifecycleState::kStopRequestedDuringStartHooks:
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
