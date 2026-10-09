#pragma once

#include <memory>
#include <memory_resource>
#include <utility>
#include <vector>

#include "ruvia/core/blocking_pool.h"
#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/web/detail/controller/controller_descriptors.h"

#include "router/compiled_route_plan.h"
#include "router/router.h"
#include "server/acceptor.h"
#include "server/web_worker_runtime.h"

namespace ruvia::detail {

struct app_worker_slot final {
    app_worker_slot(controller_store configured_controllers,
        std::unique_ptr<router, pmr_object_deleter<router>> configured_router,
        std::unique_ptr<web_worker_runtime, pmr_object_deleter<web_worker_runtime>> configured_runtime)
        : controllers_(std::move(configured_controllers)),
          router_(std::move(configured_router)),
          runtime_(std::move(configured_runtime)) {}

    app_worker_slot(app_worker_slot&&) noexcept = default;
    app_worker_slot& operator=(app_worker_slot&&) noexcept = default;
    app_worker_slot(const app_worker_slot&) = delete;
    app_worker_slot& operator=(const app_worker_slot&) = delete;

    // Destruction is intentionally reversed: runtime, router, controllers.
    controller_store controllers_;
    std::unique_ptr<router, pmr_object_deleter<router>> router_;
    std::unique_ptr<web_worker_runtime, pmr_object_deleter<web_worker_runtime>> runtime_;
};

struct app_runtime_graph final {
    explicit app_runtime_graph(std::pmr::memory_resource* resource)
        : blocking_pool_(nullptr, pmr_object_deleter<blocking_pool>{resource}),
          route_plan_(nullptr, pmr_object_deleter<compiled_route_plan>{resource}),
          workers_(resource),
          acceptor_targets_(resource) {}

    // Declared before workers so it is destroyed after suspended worker tasks.
    std::unique_ptr<blocking_pool, pmr_object_deleter<blocking_pool>> blocking_pool_;
    // Every worker-local handler table borrows this immutable lookup plan.
    compiled_route_plan_ptr_type route_plan_;
    std::pmr::vector<app_worker_slot> workers_;
    // Targets refer to heap-stable workers; the acceptor retains its own copy.
    std::pmr::vector<acceptor::worker_target> acceptor_targets_;
    std::unique_ptr<acceptor> ingress_;
};

}  // namespace ruvia::detail
