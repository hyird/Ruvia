#include "ruvia/web/detail/app/AppState.h"

#include <algorithm>
#include <exception>
#include <mutex>
#include <string_view>
#include <thread>
#include <vector>

#include "ruvia/core/detail/util/FailureReport.h"
#include "ruvia/core/detail/worker/WorkerSelection.h"
#include "ruvia/web/detail/app/AppRunCoordinator.h"
#include "ruvia/web/detail/app/AppRuntimeGraph.h"

namespace ruvia::detail {

AppState::AppState()
    : workerCount(std::max(1U, std::thread::hardware_concurrency())),
      runtime(nullptr, PmrObjectDeleter<AppRuntimeGraph>{appResource()}) {
    listeners.emplace_back(ListenerId{1}, appResource(), "0.0.0.0", 8080, HttpServerListenerDefinition::PlainHttp{});
}

AppState::~AppState() = default;

}  // namespace ruvia::detail

namespace ruvia {

App& app() {
    static App instance;
    return instance;
}

App::App()
    : state_(detail::constructPmrObject<detail::AppState>(detail::appResource())) {}

App::~App() = default;

void App::StateDeleter::operator()(detail::AppState* state) const noexcept {
    detail::destroyPmrObject(state, detail::appResource());
}

const Env& App::env() const noexcept {
    return state_->env;
}

HttpServerStats App::httpStats() const {
    auto& state = *state_;
    std::lock_guard lock(state.mutex);
    HttpServerStats total;
    if (!state.runtime) {
        return total;
    }
    for (const auto& worker : state.runtime->workers) {
        const auto stats = worker.runtime->stats();
        total.activeConnections += stats.activeConnections;
        total.connectionsRefused += stats.connectionsRefused;
        total.connectionFailures += stats.connectionFailures;
        total.acceptFailures += stats.acceptFailures;
        total.workerFailures += stats.workerFailures;
        total.documentRootRefreshFailures += stats.documentRootRefreshFailures;
    }
    return total;
}

BlockingPoolStats App::blockingPoolStats() const {
    auto& state = *state_;
    std::lock_guard lock(state.mutex);
    if (!state.runtime || !state.runtime->blockingPool) {
        return {};
    }
    return state.runtime->blockingPool->stats();
}

std::vector<WebWorkerHandle> App::workers() const {
    auto& state = *state_;
    std::lock_guard lock(state.mutex);
    std::vector<WebWorkerHandle> result;
    if (!state.runtime) {
        return result;
    }
    result.reserve(state.runtime->workers.size());
    for (const auto& worker : state.runtime->workers) {
        result.push_back(worker.runtime->webWorker());
    }
    return result;
}

WebWorkerHandle App::workerFor(std::uint64_t key) const {
    auto& state = *state_;
    std::lock_guard lock(state.mutex);
    if (!state.runtime || state.runtime->workers.empty()) {
        return {};
    }
    return state.runtime->workers[key % state.runtime->workers.size()].runtime->webWorker();
}

WebWorkerHandle App::workerFor(std::string_view key) const {
    return workerFor(detail::workerSelectionHash(key));
}

void App::run() {
    detail::runApp(*this, *state_);
}

void App::stop() {
    auto& state = *state_;
    {
        std::lock_guard lock(state.mutex);
        if (state.lifecycle.requestStop() == detail::AppStopRequest::kIgnored) {
            return;
        }
        if (state.runtime != nullptr) {
            for (auto& worker : state.runtime->workers) {
                try {
                    worker.runtime->stop();
                } catch (...) {
                    detail::reportUnhandledFailure("web worker stop request", std::current_exception());
                }
            }
        }
    }
    state.lifecycleChanged.notify_all();
}

}  // namespace ruvia
