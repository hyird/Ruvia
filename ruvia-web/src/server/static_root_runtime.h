#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <vector>

#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/core/task.h"

#include "http/static_root_config_storage.h"
#include "http/static_root_index.h"
#include "server/document_root_binding.h"
#include "server/http_server_worker_state.h"

namespace ruvia {
class blocking_pool;
class worker_handle;
}  // namespace ruvia

namespace ruvia::detail {

struct http_server_options;

// The application's one document-root refresher. A single claimed worker walks
// the root and precompresses changed files once per interval; every worker
// adopts the published immutable snapshot on its own thread. The runtime graph
// owns this object, so it outlives every worker and their joined refresh tasks.
class document_root_publisher final {
public:
    // Builds the initial snapshot; a failure here fails application startup.
    document_root_publisher(const std::filesystem::path& root, const static_root_config_storage& config,
        std::chrono::milliseconds interval, static_root_precompression_options precompression);
    ~document_root_publisher();

    document_root_publisher(const document_root_publisher&) = delete;
    document_root_publisher& operator=(const document_root_publisher&) = delete;

    [[nodiscard]] std::chrono::milliseconds interval() const noexcept {
        return interval_;
    }

    [[nodiscard]] std::size_t failures() const noexcept {
        return failures_.load(std::memory_order_relaxed);
    }

    void record_failure() noexcept {
        failures_.fetch_add(1, std::memory_order_relaxed);
    }

    // The coordinator constructs workers one at a time before any of them
    // starts, so a plain flag suffices: the first worker to ask owns the walk.
    [[nodiscard]] bool claim_refresh() noexcept;

    // Cold path, once per worker per interval: the published snapshot when it
    // is not `adopted`, otherwise null. A worker holds its adopted snapshot,
    // so a matching address always names the same live snapshot.
    [[nodiscard]] document_root_snapshot published_unless(const static_root* adopted) const;

    // One rebuild pass on the claiming worker. A changed tree is published; a
    // failed pass is counted and the last complete snapshot stays published.
    [[nodiscard]] task<void> refresh(
        blocking_pool& pool, const worker_handle& worker, const http_server_worker_state& state);

private:
    void publish(document_root_snapshot snapshot);

    std::chrono::milliseconds interval_;
    static_root_precompression_options precompression_;
    mutable std::mutex mutex_;
    // Guarded by mutex_ for readers. Only refresh() replaces it, on the
    // claiming worker, so that pass reads it without the lock.
    document_root_snapshot current_;
    bool refresh_claimed_{false};
    std::atomic<std::size_t> failures_{0};
};

// One worker's adopted document-root generations. Request bindings lease the
// current generation with a plain counter; a superseded generation stays until
// its leases drain, so a long request pins only the snapshot it borrows. The
// worker joins refresh() before teardown.
class static_root_runtime final {
public:
    static_root_runtime(http_server_options& options, const worker_handle& worker_value,
        const http_server_worker_state& state_value, std::pmr::memory_resource* resource);
    ~static_root_runtime();
    static_root_runtime(const static_root_runtime&) = delete;
    static_root_runtime& operator=(const static_root_runtime&) = delete;

    [[nodiscard]] bool enabled() const noexcept {
        return publisher_ != nullptr;
    }

    // Once per interval while the worker admits work: reclaim drained
    // generations, run the application's rebuild if this worker claimed it,
    // then adopt the published snapshot.
    [[nodiscard]] task<void> refresh();

private:
    using generation_owner =
        std::unique_ptr<document_root_generation, pmr_object_deleter<document_root_generation>>;

    void adopt_published();

    http_server_options& options_;
    document_root_publisher* publisher_;
    const worker_handle& worker_;
    const http_server_worker_state& state_;
    std::pmr::memory_resource* resource_;
    generation_owner current_;
    std::pmr::vector<generation_owner> retired_;
    bool refreshes_;
};

}  // namespace ruvia::detail
