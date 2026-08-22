#include <ruvia/web/detail/app/AppLifecycle.h>

namespace {

using ruvia::detail::AppLifecycle;
using ruvia::detail::AppLifecycleState;
using ruvia::detail::AppStopRequest;

bool normalRunAndStop() {
    AppLifecycle lifecycle;
    return lifecycle.state() == AppLifecycleState::kStopped &&
           !lifecycle.active() &&
           lifecycle.beginRun() &&
           lifecycle.state() == AppLifecycleState::kPreparing &&
           lifecycle.publishRuntime() &&
           lifecycle.state() == AppLifecycleState::kStarting &&
           lifecycle.markRunning() &&
           lifecycle.state() == AppLifecycleState::kRunning &&
           lifecycle.requestStop() == AppStopRequest::kRequested &&
           lifecycle.requestStop() == AppStopRequest::kIgnored;
}

bool stopDuringPreparationIsDurable() {
    AppLifecycle lifecycle;
    return lifecycle.beginRun() &&
           lifecycle.requestStop() == AppStopRequest::kRequested &&
           lifecycle.stopRequested() &&
           !lifecycle.publishRuntime() &&
           !lifecycle.markRunning() &&
           lifecycle.requestStop() == AppStopRequest::kIgnored;
}

bool stopDuringStartupPreventsRunning() {
    AppLifecycle lifecycle;
    return lifecycle.beginRun() &&
           lifecycle.publishRuntime() &&
           lifecycle.requestStop() == AppStopRequest::kRequested &&
           !lifecycle.markRunning() &&
           lifecycle.state() == AppLifecycleState::kStopping;
}

bool completedRunCanRestart() {
    AppLifecycle lifecycle;
    if (!lifecycle.beginRun() || lifecycle.beginRun()) {
        return false;
    }
    lifecycle.completeRun();
    return lifecycle.state() == AppLifecycleState::kStopped && lifecycle.beginRun();
}

}  // namespace

int main() {
    return normalRunAndStop() && stopDuringPreparationIsDurable() && stopDuringStartupPreventsRunning() && completedRunCanRestart() ? 0 : 1;
}
