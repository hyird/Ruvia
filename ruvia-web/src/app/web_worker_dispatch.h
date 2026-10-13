#pragma once

#include <memory>
#include <memory_resource>

#include "ruvia/core/task_scope.h"
#include "ruvia/core/worker_runtime.h"
#include "ruvia/web/web_worker.h"

#include "app/worker_post_counters.h"
#include "integration/worker_client_registry_view.h"

namespace ruvia::detail {

class worker_capabilities;
class worker_state_registry;

// The endpoint behind every web_worker_handle. Core owns admission: posts go
// through worker_handle::post_factory and accepted jobs start only while that
// worker accepts and its stop token is clear. Started jobs run in the worker's
// task_scope, which the server worker joins before retiring capabilities.
//
// Escaped handles share this object so their terminal queries (valid,
// accepting, id, stats) remain safe after the server is gone. The borrowed
// server state below is touched only by jobs started on the worker; core
// abandons every other accepted job without running it.
class web_worker_dispatch final {
public:
    using job_type = move_only_function<ruvia::task<void>(web_worker_context&)>;

    web_worker_dispatch(ruvia::worker_runtime& runtime, std::pmr::memory_resource* resource,
        worker_capabilities& capabilities, task_scope& jobs, const stop_token& stop_token_value);

    web_worker_dispatch(const web_worker_dispatch&) = delete;
    web_worker_dispatch& operator=(const web_worker_dispatch&) = delete;

    [[nodiscard]] static web_worker_handle handle(std::shared_ptr<web_worker_dispatch> dispatch) noexcept;
    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] bool accepting() const noexcept;
    [[nodiscard]] worker_id_type id() const noexcept;
    [[nodiscard]] web_worker_post_result_type post(job_type job);
    [[nodiscard]] web_worker_stats stats() const noexcept;

    // Reconciles the outstanding reservation of an accepted job that core
    // destroys without running. Public only for the reservation deleter.
    void abandon() noexcept;

private:
    void start(job_type job);
    [[nodiscard]] ruvia::task<void> run(job_type job);

    // A stable copy: queries stay valid after the server's runtime is gone.
    worker_handle worker_;
    ruvia::worker_runtime& runtime_;
    std::pmr::memory_resource* resource_;
    worker_client_registry_view client_registries_;
    const worker_state_registry& worker_states_;
    blocking_pool* blocking_pool_;
    task_scope& jobs_;
    const stop_token& stop_token_;
    worker_post_counters post_counters_;
    std::atomic_uint64_t completed_{0};
    std::atomic_uint64_t failed_count_{0};
    std::atomic_size_t outstanding_{0};
};

}  // namespace ruvia::detail
