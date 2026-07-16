#include <ruvia/web/detail/app/AppLifecycle.h>

namespace {

using ruvia::detail::AppLifecycle;
using ruvia::detail::AppLifecycleState;
using ruvia::detail::AppStartHooksCompletion;
using ruvia::detail::AppStopRequest;

bool normalRunAndStopClaimsHooksOnce() {
    AppLifecycle lifecycle;
    return lifecycle.state() == AppLifecycleState::kStopped &&
           !lifecycle.active() && lifecycle.beginRun() &&
           lifecycle.state() == AppLifecycleState::kStarting &&
           lifecycle.beginStartHooks() &&
           lifecycle.completeStartHooks() == AppStartHooksCompletion::kRunning &&
           lifecycle.state() == AppLifecycleState::kRunning &&
           lifecycle.requestStop() == AppStopRequest::kStopWorkersAndRunHooks &&
           lifecycle.requestStop() == AppStopRequest::kIgnored;
}

bool stopDuringStartupClaimsHooksOnce() {
    AppLifecycle lifecycle;
    return lifecycle.beginRun() &&
           lifecycle.requestStop() == AppStopRequest::kStopWorkersAndRunHooks &&
           lifecycle.stopRequested() && !lifecycle.beginStartHooks() &&
           lifecycle.requestStop() == AppStopRequest::kIgnored;
}

bool stopDuringStartHooksDefersHookClaim() {
    AppLifecycle lifecycle;
    return lifecycle.beginRun() && lifecycle.beginStartHooks() &&
           lifecycle.requestStop() == AppStopRequest::kStopWorkers &&
           lifecycle.state() == AppLifecycleState::kStopRequestedDuringStartHooks &&
           lifecycle.requestStop() == AppStopRequest::kIgnored &&
           lifecycle.completeStartHooks() ==
               AppStartHooksCompletion::kRunDeferredStopHooks &&
           lifecycle.state() == AppLifecycleState::kStopping &&
           lifecycle.requestStop() == AppStopRequest::kIgnored;
}

bool completedRunCanRestart() {
    AppLifecycle lifecycle;
    if (!lifecycle.beginRun() || lifecycle.beginRun()) {
        return false;
    }
    lifecycle.completeRun();
    return lifecycle.state() == AppLifecycleState::kStopped &&
           lifecycle.beginRun();
}

}  // namespace

int main() {
    return normalRunAndStopClaimsHooksOnce() &&
                   stopDuringStartupClaimsHooksOnce() &&
                   stopDuringStartHooksDefersHookClaim() &&
                   completedRunCanRestart()
        ? 0
        : 1;
}
