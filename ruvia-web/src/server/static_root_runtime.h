#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <memory_resource>
#include <vector>

#include "ruvia/core/Task.h"
#include "ruvia/core/memory/PmrObject.h"

#include "server/HttpServerWorkerState.h"

namespace ruvia {
class StaticRoot;
class WorkerHandle;
}  // namespace ruvia

namespace ruvia::detail {

struct HttpServerOptions;

// Owns indexed generations; in-flight request bindings delay reclamation of
// only the generation they borrow. The worker joins refresh() before teardown.
class static_root_runtime final {
public:
    static_root_runtime(HttpServerOptions& options, const WorkerHandle& worker,
        const HttpServerWorkerState& state, std::pmr::memory_resource* resource);
    ~static_root_runtime();
    static_root_runtime(const static_root_runtime&) = delete;
    static_root_runtime& operator=(const static_root_runtime&) = delete;

    [[nodiscard]] Task<void> refresh();
    [[nodiscard]] std::size_t failures() const noexcept {
        return failures_.load(std::memory_order_relaxed);
    }

private:
    using snapshot_owner = std::unique_ptr<StaticRoot, PmrObjectDeleter<StaticRoot>>;
    HttpServerOptions& options_;
    const WorkerHandle& worker_;
    const HttpServerWorkerState& state_;
    snapshot_owner current_;
    std::pmr::vector<snapshot_owner> retired_;
    std::atomic<std::size_t> failures_{};
};

}  // namespace ruvia::detail
