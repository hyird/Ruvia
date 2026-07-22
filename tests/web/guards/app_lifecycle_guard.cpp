#include <atomic>
#include <thread>

#include <ruvia/web/detail/app/AppLifecycle.h>
#include <ruvia/web/detail/app/AppInternal.h>

namespace {

using ruvia::detail::AppLifecycle;
using ruvia::detail::AppLifecycleState;
using ruvia::detail::AppRuntimeBorrowGate;
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

bool runtimeGraphWaitsForEveryStopBorrow() {
    AppRuntimeBorrowGate gate;
    gate.acquire();
    gate.acquire();

    std::atomic_bool waiterStarted{false};
    std::atomic_bool waiterReturned{false};
    std::thread waiter([&] {
        waiterStarted.store(true, std::memory_order_release);
        gate.wait();
        waiterReturned.store(true, std::memory_order_release);
    });
    while (!waiterStarted.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    gate.release();
    const bool retainedAfterFirstRelease =
        gate.count() == 1 &&
        !waiterReturned.load(std::memory_order_acquire);
    gate.release();
    waiter.join();
    return retainedAfterFirstRelease && gate.count() == 0 &&
           waiterReturned.load(std::memory_order_acquire);
}

}  // namespace

int main() {
    return normalRunAndStopClaimsHooksOnce() &&
                   stopDuringStartupClaimsHooksOnce() &&
                   stopDuringStartHooksDefersHookClaim() &&
                   completedRunCanRestart() &&
                   runtimeGraphWaitsForEveryStopBorrow()
        ? 0
        : 1;
}
