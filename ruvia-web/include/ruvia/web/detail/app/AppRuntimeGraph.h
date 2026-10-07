#pragma once

#include <memory>
#include <memory_resource>
#include <utility>
#include <vector>

#include "ruvia/core/BlockingPool.h"
#include "ruvia/core/memory/PmrObject.h"
#include "ruvia/web/detail/controller/ControllerDescriptors.h"
#include "ruvia/web/detail/router/CompiledRoutePlan.h"
#include "ruvia/web/detail/router/Router.h"
#include "ruvia/web/detail/server/WebWorkerRuntime.h"
#include "ruvia/web/detail/server/acceptor.h"

namespace ruvia::detail {

struct AppWorkerSlot final {
    AppWorkerSlot(ControllerStore configuredControllers,
        std::unique_ptr<Router, PmrObjectDeleter<Router>> configuredRouter,
        std::unique_ptr<WebWorkerRuntime, PmrObjectDeleter<WebWorkerRuntime>> configuredRuntime)
        : controllers(std::move(configuredControllers)),
          router(std::move(configuredRouter)),
          runtime(std::move(configuredRuntime)) {}

    AppWorkerSlot(AppWorkerSlot&&) noexcept = default;
    AppWorkerSlot& operator=(AppWorkerSlot&&) noexcept = default;
    AppWorkerSlot(const AppWorkerSlot&) = delete;
    AppWorkerSlot& operator=(const AppWorkerSlot&) = delete;

    // Destruction is intentionally reversed: runtime, router, controllers.
    ControllerStore controllers;
    std::unique_ptr<Router, PmrObjectDeleter<Router>> router;
    std::unique_ptr<WebWorkerRuntime, PmrObjectDeleter<WebWorkerRuntime>> runtime;
};

struct AppRuntimeGraph final {
    explicit AppRuntimeGraph(std::pmr::memory_resource* resource)
        : blockingPool(nullptr, PmrObjectDeleter<BlockingPool>{resource}),
          routePlan(nullptr, PmrObjectDeleter<CompiledRoutePlan>{resource}),
          workers(resource),
          acceptor_targets(resource) {}

    // Declared before workers so it is destroyed after suspended worker tasks.
    std::unique_ptr<BlockingPool, PmrObjectDeleter<BlockingPool>> blockingPool;
    // Every worker-local handler table borrows this immutable lookup plan.
    CompiledRoutePlanPtr routePlan;
    std::pmr::vector<AppWorkerSlot> workers;
    // Targets refer to heap-stable workers; the acceptor retains its own copy.
    std::pmr::vector<acceptor::worker_target> acceptor_targets;
    std::unique_ptr<acceptor> ingress;
};

}  // namespace ruvia::detail
