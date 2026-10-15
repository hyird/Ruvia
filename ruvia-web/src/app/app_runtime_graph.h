#pragma once

#include <memory>
#include <memory_resource>
#include <utility>
#include <vector>

#include "ruvia/core/blocking_pool.h"
#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/core/worker_group.h"
#include "ruvia/web/detail/controller/controller_descriptors.h"

#include "router/compiled_route_plan.h"
#include "router/route_table.h"
#include "server/static_root_runtime.h"
#include "server/web_worker_runtime.h"

namespace ruvia::detail {

struct app_worker_slot final {
    app_worker_slot(controller_store configured_controllers,
        std::unique_ptr<route_table, pmr_object_deleter<route_table>> configured_routes,
        std::unique_ptr<web_worker_runtime, pmr_object_deleter<web_worker_runtime>> configured_runtime)
        : controllers_(std::move(configured_controllers)),
          routes_(std::move(configured_routes)),
          runtime_(std::move(configured_runtime)) {}

    app_worker_slot(app_worker_slot&&) noexcept = default;
    app_worker_slot& operator=(app_worker_slot&&) noexcept = default;
    app_worker_slot(const app_worker_slot&) = delete;
    app_worker_slot& operator=(const app_worker_slot&) = delete;

    // Destruction is intentionally reversed: runtime, routes, controllers.
    controller_store controllers_;
    std::unique_ptr<route_table, pmr_object_deleter<route_table>> routes_;
    std::unique_ptr<web_worker_runtime, pmr_object_deleter<web_worker_runtime>> runtime_;
};

struct app_runtime_graph final {
    explicit app_runtime_graph(std::pmr::memory_resource* resource)
        : blocking_pool_(nullptr, pmr_object_deleter<blocking_pool>{resource}),
          document_root_(nullptr, pmr_object_deleter<document_root_publisher>{resource}),
          route_plan_(nullptr, pmr_object_deleter<compiled_route_plan>{resource}),
          workers_(resource),
          group_(resource) {}

    // Declared before workers so it is destroyed after suspended worker tasks.
    std::unique_ptr<blocking_pool, pmr_object_deleter<blocking_pool>> blocking_pool_;
    // The one document-root refresher; null without a document root. Workers
    // borrow it and adopt its snapshots, so it is declared before them.
    std::unique_ptr<document_root_publisher, pmr_object_deleter<document_root_publisher>> document_root_;
    // Every worker's route_table binds its instances to this one immutable plan.
    compiled_route_plan_ptr_type route_plan_;
    std::pmr::vector<app_worker_slot> workers_;
    // The one lifecycle over every independently listening worker. It borrows
    // their core runtimes, so it is declared after them.
    worker_group group_;
    // Guarded by the application mutex. Worker handles are published only
    // once every worker is ready, so a posted job never meets a starting worker.
    bool workers_ready_{false};
};

}  // namespace ruvia::detail
