#pragma once

#include "AppConfigGuards.h"
#include "AppInternal.h"

#include <mutex>
#include <utility>

namespace ruvia::detail {

template <typename Configure>
App& mutateStoppedApp(App& app, AppState& state, const char* runningMessage, Configure&& configure) {
    std::lock_guard lock(state.mutex);
    ensureAppNotRunning(state.running, runningMessage);
    std::forward<Configure>(configure)(state);
    return app;
}

}  // namespace ruvia::detail
