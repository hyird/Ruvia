#pragma once

#include <mutex>
#include <stdexcept>
#include <utility>

#include "app/app_state.h"

namespace ruvia::detail {

inline void ensure_app_not_running(bool running, const char* message) {
    if (running) {
        throw std::logic_error(message);
    }
}
template <typename configure_type>
application& mutate_stopped_app(
    application& app, app_state& state_value, const char* running_message, configure_type&& configure) {
    std::lock_guard lock(state_value.mutex_);
    ensure_app_not_running(state_value.lifecycle_.active(), running_message);
    std::forward<configure_type>(configure)(state_value);
    return app;
}

}  // namespace ruvia::detail
