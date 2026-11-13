#pragma once

#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <type_traits>
#include <utility>

#include "ruvia/core/blocking_pool.h"
#include "ruvia/core/scoped_operation.h"
#include "ruvia/core/stop_token.h"
#include "ruvia/core/task.h"
#include "ruvia/core/worker_ref.h"
#include "ruvia/http/borrowed_text.h"
#include "ruvia/web/detail/integration/worker_context_capabilities.h"

namespace ruvia {

class http_client_ref;
#ifdef RUVIA_ENABLE_DATABASE
class db_session;
#endif
#ifdef RUVIA_ENABLE_REDIS
class redis_session;
#endif

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

    [[nodiscard]] const worker_ref& worker() const& noexcept RUVIA_LIFETIMEBOUND;
    const worker_ref& worker() const&& = delete;
    [[nodiscard]] ::ruvia::stop_token stop_token() const noexcept;

    template <typename state_type>
    [[nodiscard]] state_type& worker_state() const {
        return capabilities_.worker_state<state_type>();
    }

    // The callable owns its inputs and must not borrow worker state. Completion
    // resumes here; worker shutdown and options.timeout_ end the wait, not the callable.
    // Rejection and callable exceptions are retained in blocking_result; inspect
    // status() or use std::move(result).take_value() to throw. The worker stop token
    // is bound automatically and combined with options.stop_token_. With application::blocking_pool(nullptr) the
    // result reports blocking_status::unavailable.
    template <typename callable_type>
    [[nodiscard]] task<blocking_result<std::invoke_result_t<callable_type&>>> run_blocking(
        callable_type callable, operation_options options = {}) const {
        return capabilities_.run_blocking(std::move(callable), std::move(options));
    }

#ifdef RUVIA_ENABLE_DATABASE
    [[nodiscard]] db_session db() const;
    [[nodiscard]] db_session db(std::string_view alias) const;
#endif
#ifdef RUVIA_ENABLE_REDIS
    [[nodiscard]] redis_session redis() const;
    [[nodiscard]] redis_session redis(std::string_view alias) const;
#endif
    [[nodiscard]] http_client_ref client() const;
    [[nodiscard]] http_client_ref client(std::string_view alias) const;

private:
    friend class detail::web_worker_dispatch;

    web_worker_context(const worker_ref& worker_value,
        const detail::worker_client_registry_view& client_registries,
        const detail::worker_state_registry* worker_states, const ::ruvia::stop_token& stop_token_value) noexcept;
    web_worker_context(worker_ref&&, const detail::worker_client_registry_view&,
        const detail::worker_state_registry*, const ::ruvia::stop_token&) = delete;
    web_worker_context(const worker_ref&, const detail::worker_client_registry_view&,
        const detail::worker_state_registry*, ::ruvia::stop_token&&) = delete;
    web_worker_context(worker_ref&&, const detail::worker_client_registry_view&,
        const detail::worker_state_registry*, ::ruvia::stop_token&&) = delete;

    // The server worker owns these values and joins every posted job before
    // retiring them. Contexts borrow them so starting a job does not copy
    // endpoint or cancellation-state ownership on the worker thread.
    detail::worker_context_capabilities capabilities_;
    const detail::worker_client_registry_view& client_registries_;
    // Each posted callback gets an independent operation lifetime. Declared
    // last so cold frames are destroyed before the callback context disappears.
    mutable ::ruvia::operation_scope operation_scope_;
};

using web_worker_post_result = post_outcome<task<void>(web_worker_context&)>;

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
        requires std::invocable<std::decay_t<fn_type>&, web_worker_context&> &&
                 std::same_as<std::invoke_result_t<std::decay_t<fn_type>&, web_worker_context&>,
                     task<void>>
    [[nodiscard]] web_worker_post_result post(fn_type&& fn) const {
        return post_task(move_only_function<task<void>(web_worker_context&)>(std::forward<fn_type>(fn)));
    }

private:
    friend class detail::web_worker_dispatch;

    web_worker_ref(std::shared_ptr<detail::web_worker_dispatch> dispatch) noexcept;

    [[nodiscard]] web_worker_post_result post_task(
        move_only_function<task<void>(web_worker_context&)> task) const;

    // The handle owns a stable terminal endpoint. Server shutdown closes it;
    // retaining a handle cannot retain the server or its io_context.
    std::shared_ptr<detail::web_worker_dispatch> dispatch_;
};

}  // namespace ruvia
