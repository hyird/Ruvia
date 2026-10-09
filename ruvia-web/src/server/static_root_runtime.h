#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <memory_resource>
#include <vector>

#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/core/task.h"

#include "server/http_server_worker_state.h"

namespace ruvia {
class static_root;
class worker_handle;
}  // namespace ruvia

namespace ruvia::detail {

struct http_server_options;

// Owns indexed generations; in-flight request bindings delay reclamation of
// only the generation they borrow. The worker joins refresh() before teardown.
class static_root_runtime final {
public:
    static_root_runtime(http_server_options& options, const worker_handle& worker_value,
        const http_server_worker_state& state_value, std::pmr::memory_resource* resource);
    ~static_root_runtime();
    static_root_runtime(const static_root_runtime&) = delete;
    static_root_runtime& operator=(const static_root_runtime&) = delete;

    [[nodiscard]] task<void> refresh();
    [[nodiscard]] std::size_t failures() const noexcept {
        return failures_.load(std::memory_order_relaxed);
    }

private:
    using snapshot_owner = std::unique_ptr<static_root, pmr_object_deleter<static_root>>;
    http_server_options& options_;
    const worker_handle& worker_;
    const http_server_worker_state& state_;
    snapshot_owner current_;
    std::pmr::vector<snapshot_owner> retired_;
    std::atomic<std::size_t> failures_{};
};

}  // namespace ruvia::detail
