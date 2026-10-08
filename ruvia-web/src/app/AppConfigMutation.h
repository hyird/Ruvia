#pragma once

#include <mutex>
#include <utility>

#include "app/AppConfigGuards.h"
#include "app/AppState.h"

namespace ruvia::detail {

template <typename Configure>
App& mutateStoppedApp(
    App& app, AppState& state, const char* runningMessage, Configure&& configure) {
    std::lock_guard lock(state.mutex);
    ensureAppNotRunning(state.lifecycle.active(), runningMessage);
    std::forward<Configure>(configure)(state);
    return app;
}

}  // namespace ruvia::detail
