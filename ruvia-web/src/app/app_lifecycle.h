#pragma once

#include <cstdint>

namespace ruvia::detail {

enum class app_lifecycle_state : std::uint8_t {
    stopped,
    preparing,
    starting,
    running,
    stopping,
};

enum class app_stop_request : std::uint8_t {
    ignored,
    requested,
};

// application owns the mutex protecting this state machine. application::run() is the sole
// lifecycle owner; other threads may only request the monotonic transition to
// stopping.
class app_lifecycle final {
public:
    [[nodiscard]] app_lifecycle_state state() const noexcept {
        return state_;
    }

    [[nodiscard]] bool active() const noexcept {
        return state_ != app_lifecycle_state::stopped;
    }

    [[nodiscard]] bool stop_requested() const noexcept {
        return state_ == app_lifecycle_state::stopping;
    }

    [[nodiscard]] bool begin_run() noexcept {
        if (state_ != app_lifecycle_state::stopped) {
            return false;
        }
        state_ = app_lifecycle_state::preparing;
        return true;
    }

    [[nodiscard]] bool publish_runtime() noexcept {
        if (state_ != app_lifecycle_state::preparing) {
            return false;
        }
        state_ = app_lifecycle_state::starting;
        return true;
    }

    [[nodiscard]] bool mark_running() noexcept {
        if (state_ != app_lifecycle_state::starting) {
            return false;
        }
        state_ = app_lifecycle_state::running;
        return true;
    }

    [[nodiscard]] app_stop_request request_stop() noexcept {
        switch (state_) {
            case app_lifecycle_state::preparing:
            case app_lifecycle_state::starting:
            case app_lifecycle_state::running:
                state_ = app_lifecycle_state::stopping;
                return app_stop_request::requested;
            case app_lifecycle_state::stopped:
            case app_lifecycle_state::stopping:
                return app_stop_request::ignored;
        }
        return app_stop_request::ignored;
    }

    void complete_run() noexcept {
        state_ = app_lifecycle_state::stopped;
    }

private:
    app_lifecycle_state state_{app_lifecycle_state::stopped};
};

}  // namespace ruvia::detail
