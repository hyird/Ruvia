#pragma once

#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <string_view>
#include <type_traits>
#include <utility>

#include "ruvia/core/blocking_pool.h"
#include "ruvia/core/scoped_operation.h"
#include "ruvia/core/stop_token.h"
#include "ruvia/core/task.h"
#include "ruvia/core/worker_handle.h"
#include "ruvia/web/detail/integration/worker_context_capabilities.h"
#include "ruvia/web/http_client_handle.h"

#ifdef RUVIA_ENABLE_DATABASE
#include "ruvia/web/db/db_handle.h"
#endif

#ifdef RUVIA_ENABLE_REDIS
#include "ruvia/web/redis/redis_handle.h"
#endif

namespace ruvia {

namespace detail {
class web_worker_dispatch;
class worker_client_registry_view;
class worker_state_registry;
}  // namespace detail

class web_worker_context final {
public:
    web_worker_context(const web_worker_context&) = delete;
    web_worker_context& operator=(const web_worker_context&) = delete;
    web_worker_context(web_worker_context&&) = delete;
    web_worker_context& operator=(web_worker_context&&) = delete;

    [[nodiscard]] const worker_handle& worker() const& noexcept;
    const worker_handle& worker() const&& = delete;
    // Same worker-reclaimable pool as context::pool(). Posted jobs have no
    // request arena; allocate temporary and result storage here. Destroy that
    // storage on this worker within the job's scope. Pool caching does not
    // extend the lifetime of objects or borrowed views.
    [[nodiscard]] std::pmr::memory_resource* pool() const noexcept;
    [[nodiscard]] stop_token get_stop_token() const noexcept;

    template <typename state_type>
    [[nodiscard]] state_type& worker_state() const {
        return capabilities_.worker_state<state_type>();
    }

    template <typename callable_type>
    [[nodiscard]] task<std::invoke_result_t<callable_type&>> run_blocking(callable_type callable) const {
        return capabilities_.run_blocking(std::move(callable));
    }

    template <typename rep_type, typename period_type, typename callable_type>
    [[nodiscard]] task<std::invoke_result_t<callable_type&>> run_blocking(
        std::chrono::duration<rep_type, period_type> timeout, callable_type callable) const {
        return capabilities_.run_blocking(timeout, std::move(callable));
    }

    template <typename callable_type>
    [[nodiscard]] task<blocking_result<std::invoke_result_t<callable_type&>>> try_run_blocking(callable_type callable) const {
        return capabilities_.try_run_blocking(std::move(callable));
    }

    template <typename rep_type, typename period_type, typename callable_type>
    [[nodiscard]] task<blocking_result<std::invoke_result_t<callable_type&>>> try_run_blocking(
        std::chrono::duration<rep_type, period_type> timeout, callable_type callable) const {
        return capabilities_.try_run_blocking(timeout, std::move(callable));
    }

#ifdef RUVIA_ENABLE_DATABASE
    [[nodiscard]] db_handle db() const;
    [[nodiscard]] db_handle db(std::string_view alias) const;
#endif
#ifdef RUVIA_ENABLE_REDIS
    [[nodiscard]] redis_handle redis() const;
    [[nodiscard]] redis_handle redis(std::string_view alias) const;
#endif
    [[nodiscard]] http_client_handle get_http_client() const;
    [[nodiscard]] http_client_handle get_http_client(std::string_view alias) const;

private:
    friend class detail::web_worker_dispatch;

    web_worker_context(const worker_handle& worker_value, std::pmr::memory_resource* resource,
        const detail::worker_client_registry_view& client_registries,
        const detail::worker_state_registry* worker_states, blocking_pool* blocking_pool_value,
        const stop_token& stop_token_value) noexcept;
    web_worker_context(worker_handle&&, std::pmr::memory_resource*,
        const detail::worker_client_registry_view&, const detail::worker_state_registry*, blocking_pool*,
        const stop_token&) = delete;
    web_worker_context(const worker_handle&, std::pmr::memory_resource*,
        const detail::worker_client_registry_view&, const detail::worker_state_registry*, blocking_pool*,
        stop_token&&) = delete;
    web_worker_context(worker_handle&&, std::pmr::memory_resource*,
        const detail::worker_client_registry_view&, const detail::worker_state_registry*, blocking_pool*,
        stop_token&&) = delete;

    // web_worker_dispatch owns these stable values until every posted task has
    // completed. In particular, client_registries_ borrows its view; retire()
    // detaches that view only after active_started_ reaches zero. Contexts borrow
    // them so starting a task does not copy endpoint or cancellation-state
    // ownership on the worker thread.
    detail::worker_context_capabilities capabilities_;
    std::pmr::memory_resource* resource_;
    const detail::worker_client_registry_view& client_registries_;
    // Each posted callback gets an independent operation lifetime. Declared
    // last so cold frames are destroyed before the callback context disappears.
    mutable ::ruvia::operation_scope operation_scope_;
};

using web_worker_post_result_type = post_outcome<task<void>(web_worker_context&)>;

struct web_worker_stats final {
    std::uint64_t accepted_{0};
    std::uint64_t queue_full_{0};
    std::uint64_t worker_stopping_{0};
    std::uint64_t completed_{0};
    std::uint64_t failed_{0};
    std::size_t outstanding_{0};
};

class web_worker_handle final {
public:
    web_worker_handle() noexcept = default;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] bool accepting() const noexcept;
    [[nodiscard]] worker_id_type id() const noexcept;
    [[nodiscard]] web_worker_stats stats() const noexcept;

    template <typename fn_type>
        requires std::invocable<std::decay_t<fn_type>&, web_worker_context&> &&
                 std::same_as<std::invoke_result_t<std::decay_t<fn_type>&, web_worker_context&>,
                     task<void>>
    [[nodiscard]] web_worker_post_result_type post(fn_type&& fn) const {
        return post_task(move_only_function<task<void>(web_worker_context&)>(std::forward<fn_type>(fn)));
    }

private:
    friend class detail::web_worker_dispatch;

    web_worker_handle(std::shared_ptr<detail::web_worker_dispatch> dispatch) noexcept;

    [[nodiscard]] web_worker_post_result_type post_task(
        move_only_function<task<void>(web_worker_context&)> task) const;

    // The handle owns a stable terminal endpoint. Server shutdown closes it;
    // retaining a handle cannot retain the server or its io_context.
    std::shared_ptr<detail::web_worker_dispatch> dispatch_;
};

}  // namespace ruvia
