#pragma once

#include <atomic>
#include <memory>
#include <memory_resource>
#include <mutex>

#include <asio/any_io_executor.hpp>

#include "ruvia/core/worker_post_counters.h"
#include "ruvia/web/web_worker.h"

#include "integration/worker_client_registry_view.h"

namespace ruvia::detail {

class worker_capabilities;
class worker_state_registry;

class web_worker_dispatch final : public std::enable_shared_from_this<web_worker_dispatch> {
public:
    using task = move_only_function<ruvia::task<void>(web_worker_context&)>;

    web_worker_dispatch(asio::any_io_executor executor, worker_handle worker_value,
        std::pmr::memory_resource* resource, worker_capabilities& capabilities,
        move_only_function<void(std::exception_ptr)> failed);
    ~web_worker_dispatch();

    web_worker_dispatch(const web_worker_dispatch&) = delete;
    web_worker_dispatch& operator=(const web_worker_dispatch&) = delete;

    [[nodiscard]] web_worker_handle handle();
    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] worker_id_type id() const noexcept;
    [[nodiscard]] web_worker_post_result_type post(task task);
    void close() noexcept;
    void retire() noexcept;
    [[nodiscard]] bool accepting() const noexcept;
    [[nodiscard]] web_worker_stats stats() const noexcept;

    // Reconcile the outstanding_ reservation post() took for a start-lambda that
    // is destroyed without running (a rejected post, or a shutdown that abandons
    // queued queue work behind a task that threw). Public only so the post()
    // reservation deleter can reach it; not a task-completion signal.
    void abandon() noexcept;

private:
    void start(task task);
    [[nodiscard]] ruvia::task<void> run(task task);
    void complete() noexcept;

    asio::any_io_executor executor_;
    worker_handle worker_;
    std::pmr::memory_resource* resource_;
    worker_client_registry_view client_registries_;
    const worker_state_registry* worker_states_;
    blocking_pool* blocking_pool_;
    move_only_function<void(std::exception_ptr)> failed_;
    mutable std::mutex submit_mutex_;
    stop_source stop_source_;
    stop_token stop_token_{stop_source_.token()};
    std::atomic_size_t outstanding_{0};
    // Tasks which have entered start() and still may touch worker-owned state.
    // Pending producers are intentionally excluded: their factory only uses the
    // stable dispatch endpoint and can be abandoned after the worker detaches.
    std::atomic_size_t active_started_{0};
    worker_post_counters post_counters_;
    std::atomic_uint64_t completed_{0};
    std::atomic_uint64_t failed_count_{0};
    bool accepting_{true};
};

}  // namespace ruvia::detail
