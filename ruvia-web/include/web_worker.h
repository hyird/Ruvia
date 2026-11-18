#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>
#include <utility>

#include "borrowed_text.h"
#include "delivery_result.h"
#include "event_loop.h"
#include "move_only_function.h"
#include "operation_scope.h"
#include "stop_token.h"
#include "task.h"
#include "worker_ref.h"
#include "worker_services.h"

// Cross-thread job posting to a server's business workers: web_worker_ref is
// the handle application::workers() / worker_for() return, and every posted
// job runs on its worker with a web_worker_context.

namespace ruvia {

namespace detail {
class web_worker_dispatch;
}  // namespace detail

// A job posted with web_worker_ref::post() or try_start(): its worker, the worker's stop
// token and, through worker_services_access, the worker's resources.
// Executors and clients bind to this job's own operation lifetime and carry
// no request I/O metrics.
class web_worker_context final : public worker_services_access<web_worker_context> {
public:
    web_worker_context(const web_worker_context&) = delete;
    web_worker_context& operator=(const web_worker_context&) = delete;
    web_worker_context(web_worker_context&&) = delete;
    web_worker_context& operator=(web_worker_context&&) = delete;

    [[nodiscard]] const worker_ref& worker() const& noexcept RUVIA_LIFETIMEBOUND;
    const worker_ref& worker() const&& = delete;
    [[nodiscard]] ::ruvia::stop_token stop_token() const noexcept;

private:
    friend class detail::web_worker_dispatch;
    friend class worker_services_access<web_worker_context>;

    [[nodiscard]] detail::worker_services services() const noexcept;

    web_worker_context(const worker_ref& worker_value, const detail::worker_client_registry_view& client_registries, const detail::worker_state_registry* worker_states, const ::ruvia::stop_token& stop_token_value, const ruvia::env* env) noexcept;
    web_worker_context(worker_ref&&, const detail::worker_client_registry_view&, const detail::worker_state_registry*, const ::ruvia::stop_token&, const ruvia::env*) = delete;
    web_worker_context(const worker_ref&, const detail::worker_client_registry_view&, const detail::worker_state_registry*, ::ruvia::stop_token&&, const ruvia::env*) = delete;
    web_worker_context(worker_ref&&, const detail::worker_client_registry_view&, const detail::worker_state_registry*, ::ruvia::stop_token&&, const ruvia::env*) = delete;

    // The server worker owns these values and joins every posted job before
    // retiring them. Contexts borrow them so starting a job does not copy
    // endpoint or cancellation-state ownership on the worker thread.
    const worker_ref& worker_;
    const ::ruvia::stop_token& stop_token_;
    const detail::worker_state_registry* worker_states_;
    const detail::worker_client_registry_view& client_registries_;
    const ruvia::env* env_;
    // Each posted callback gets an independent operation lifetime. Declared
    // last so cold frames are destroyed before the callback context disappears.
    mutable detail::operation_scope operation_scope_;
};

using web_worker_post_result = post_outcome<task<void>(web_worker_context&)>;
using web_worker_start_result = delivery_result<move_only_function<task<void>(web_worker_context&)>, post_status, post_status::accepted, root_task<void>>;

struct web_worker_stats final {
    std::uint64_t accepted_{0};
    std::uint64_t queue_full_{0};
    std::uint64_t worker_stopping_{0};
    std::uint64_t completed_{0};
    std::uint64_t failed_{0};
    std::size_t outstanding_{0};
};

class web_worker_ref final {
public:
    web_worker_ref() noexcept = default;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] bool accepting() const noexcept;
    [[nodiscard]] bool is_current() const noexcept;
    [[nodiscard]] worker_id_type id() const noexcept;
    [[nodiscard]] web_worker_stats stats() const noexcept;

    template <typename fn_type>
        requires std::invocable<std::decay_t<fn_type>&, web_worker_context&> && std::same_as<std::invoke_result_t<std::decay_t<fn_type>&, web_worker_context&>, task<void>>
    [[nodiscard]] web_worker_post_result post(fn_type&& fn) const {
        return post_task(move_only_function<task<void>(web_worker_context&)>(std::forward<fn_type>(fn)));
    }

    // Same admission as post(), with a root that completes after the callable
    // and its context retire. A queued job discarded on stop completes with
    // operation_canceled; a running job observes the worker's stop token.
    template <typename fn_type>
        requires std::invocable<std::decay_t<fn_type>&, web_worker_context&> && std::same_as<std::invoke_result_t<std::decay_t<fn_type>&, web_worker_context&>, task<void>>
    [[nodiscard]] web_worker_start_result try_start(fn_type&& fn) const {
        const auto worker = *this;
        return worker.start_task(move_only_function<task<void>(web_worker_context&)>(std::forward<fn_type>(fn)));
    }

private:
    friend class detail::web_worker_dispatch;

    web_worker_ref(std::shared_ptr<detail::web_worker_dispatch> dispatch) noexcept;

    [[nodiscard]] web_worker_post_result post_task(move_only_function<task<void>(web_worker_context&)> task) const;
    [[nodiscard]] web_worker_start_result start_task(move_only_function<task<void>(web_worker_context&)> task) const;

    // The handle owns a stable terminal endpoint. Server shutdown closes it;
    // retaining a handle cannot retain the server or its io_context.
    std::shared_ptr<detail::web_worker_dispatch> dispatch_;
};

}  // namespace ruvia
