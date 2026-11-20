#pragma once

#include <cstddef>
#include <string_view>
#include <type_traits>
#include <utility>

#include "blocking_pool.h"
#include "operation_options.h"
#include "operation_scope.h"
#include "stop_token.h"
#include "task.h"
#include "worker_ref.h"

namespace ruvia {

class context;
class env;
class http_client;
class http_client_ref;
class web_worker_context;
#ifdef RUVIA_ENABLE_DATABASE
class db_executor;
#endif
#ifdef RUVIA_ENABLE_REDIS
class redis_executor;
#endif

namespace detail {

class borrowed_clients;
class db_registry;
class http_client_registry;
class redis_registry;
class worker_state_registry;
struct request_io_metrics;

template <typename t_type>
struct type_key_tag final {
    inline static constexpr std::byte value{};
};

// One stable address per type, used as the binding's identity. Cheaper and more
// robust than typeid: no RTTI requirement and no cross-boundary name equality.
template <typename t_type>
[[nodiscard]] const void* type_key() noexcept {
    return &type_key_tag<std::remove_cvref_t<t_type>>::value;
}

// A copyable view of the complete client registry set owned by one worker, or
// of the clients a router borrows for its dispatches. The only other state is
// detached: partial registry graphs are not representable. Detached views are
// only placeholders for contexts built without worker capabilities; an
// attached view never outlives its worker, which joins all posted work before
// destroying its registries, nor a router's dispatch.
class worker_client_registry_view final {
public:
    [[nodiscard]] static constexpr worker_client_registry_view detached() noexcept {
        return worker_client_registry_view(nullptr);
    }

    constexpr worker_client_registry_view(http_client_registry& http_clients
#ifdef RUVIA_ENABLE_DATABASE
        ,
        db_registry& databases
#endif
#ifdef RUVIA_ENABLE_REDIS
        ,
        redis_registry& redis
#endif
        ) noexcept :
        http_clients_(&http_clients)
#ifdef RUVIA_ENABLE_DATABASE
        ,
        databases_(&databases)
#endif
#ifdef RUVIA_ENABLE_REDIS
        ,
        redis_(&redis)
#endif
    {
    }

    explicit constexpr worker_client_registry_view(const borrowed_clients& borrowed) noexcept :
        borrowed_(&borrowed) {}

    [[nodiscard]] constexpr bool attached() const noexcept {
        return http_clients_ != nullptr || borrowed_ != nullptr;
    }

    // `io_metrics` is the request's counter block, or null outside a request.
#ifdef RUVIA_ENABLE_DATABASE
    [[nodiscard]] db_executor db(operation_scope& operation_scope, request_io_metrics* io_metrics, const stop_token& stop_token) const;
    [[nodiscard]] db_executor db(std::string_view alias, operation_scope& operation_scope, request_io_metrics* io_metrics, const stop_token& stop_token) const;
#endif

#ifdef RUVIA_ENABLE_REDIS
    [[nodiscard]] redis_executor redis(operation_scope& operation_scope, request_io_metrics* io_metrics, const stop_token& stop_token) const;
    [[nodiscard]] redis_executor redis(std::string_view alias, operation_scope& operation_scope, request_io_metrics* io_metrics, const stop_token& stop_token) const;
#endif

    [[nodiscard]] http_client_ref http_client(operation_scope& operation_scope, const stop_token& stop_token) const;
    [[nodiscard]] http_client_ref http_client(std::string_view alias, operation_scope& operation_scope, const stop_token& stop_token) const;

private:
    explicit constexpr worker_client_registry_view(std::nullptr_t) noexcept {}

    [[nodiscard]] const ::ruvia::http_client& borrowed_http_clients(std::string_view alias) const;

    const borrowed_clients* borrowed_{nullptr};
    http_client_registry* http_clients_{nullptr};
#ifdef RUVIA_ENABLE_DATABASE
    db_registry* databases_{nullptr};
#endif
#ifdef RUVIA_ENABLE_REDIS
    redis_registry* redis_{nullptr};
#endif
};

// The worker-level resources a request or a posted job reaches, built on demand
// by its host (see worker_services_access). It borrows that host, so it must
// not outlive it.
class worker_services final {
public:
    template <typename state_type>
    [[nodiscard]] state_type& worker_state() const {
        return *static_cast<std::remove_const_t<state_type>*>(worker_state_instance(detail::type_key<std::remove_const_t<state_type>>()));
    }

    template <blocking_callable callable_type>
    [[nodiscard]] task<blocking_result<std::invoke_result_t<callable_type&>>> run_blocking(callable_type callable, operation_options options = {}) const {
        return dispatch_blocking(std::move(callable), std::move(options), false);
    }

    template <blocking_callable callable_type>
    [[nodiscard]] task<blocking_result<std::invoke_result_t<callable_type&>>> run_blocking_joined(callable_type callable, operation_options options = {}) const {
        return dispatch_blocking(std::move(callable), std::move(options), true);
    }

    [[nodiscard]] const ruvia::env& env() const noexcept;

    [[nodiscard]] http_client_ref http_client() const;
    [[nodiscard]] http_client_ref http_client(std::string_view alias) const;
#ifdef RUVIA_ENABLE_DATABASE
    [[nodiscard]] db_executor db() const;
    [[nodiscard]] db_executor db(std::string_view alias) const;
#endif
#ifdef RUVIA_ENABLE_REDIS
    [[nodiscard]] redis_executor redis() const;
    [[nodiscard]] redis_executor redis(std::string_view alias) const;
#endif

private:
    friend class ::ruvia::context;
    friend class ::ruvia::web_worker_context;

    // `io_metrics` is the request's counter block, null for a posted job.
    constexpr worker_services(const worker_ref& worker, const ::ruvia::stop_token& stop_token, const detail::worker_state_registry* worker_states, detail::worker_client_registry_view clients, detail::operation_scope& operation_scope, detail::request_io_metrics* io_metrics, const ruvia::env* env) noexcept :
        worker_(&worker),
        stop_token_(&stop_token),
        worker_states_(worker_states),
        clients_(clients),
        operation_scope_(&operation_scope),
        io_metrics_(io_metrics),
        env_(env) {}

    template <blocking_callable callable_type>
    [[nodiscard]] task<blocking_result<std::invoke_result_t<callable_type&>>> dispatch_blocking(callable_type callable, operation_options options, bool join_started) const {
        if (!options.stop_token_.stoppable()) {
            options.stop_token_ = *stop_token_;
        }
        if (!stop_token_->stoppable() || options.stop_token_ == *stop_token_) {
            return join_started ? ruvia::run_blocking_joined(*worker_, std::move(callable), std::move(options)) : ruvia::run_blocking(*worker_, std::move(callable), std::move(options));
        }
        return run_blocking_combined(*worker_, *stop_token_, std::move(callable), std::move(options), join_started);
    }

    template <blocking_callable callable_type>
    [[nodiscard]] static task<blocking_result<std::invoke_result_t<callable_type&>>> run_blocking_combined(worker_ref worker_value, ::ruvia::stop_token worker_token, callable_type callable, operation_options options, bool join_started) {
        const linked_operation_options linked(options, worker_token);
        co_return co_await (join_started ? ruvia::run_blocking_joined(std::move(worker_value), std::move(callable), linked.options()) : ruvia::run_blocking(std::move(worker_value), std::move(callable), linked.options()));
    }

    [[nodiscard]] void* worker_state_instance(const void* type_key) const;

    const worker_ref* worker_;
    const ::ruvia::stop_token* stop_token_;
    const detail::worker_state_registry* worker_states_;
    detail::worker_client_registry_view clients_;
    detail::operation_scope* operation_scope_;
    detail::request_io_metrics* io_metrics_;
    const ruvia::env* env_;
};

}  // namespace detail

// The worker-level resources shared by context, request_stream and
// web_worker_context: worker state, the blocking pool, env and the client
// registries. Executors and client handles bind to the host's operation
// lifetime and stop_token(), so only lvalue hosts hand them out.
template <typename host_type>
class worker_services_access {
public:
    // Borrows this worker's registered instance; never hand it to another
    // worker. An unregistered type throws std::logic_error.
    template <typename state_type>
    [[nodiscard]] state_type& worker_state() & RUVIA_LIFETIMEBOUND {
        return services().template worker_state<state_type>();
    }
    template <typename state_type>
    [[nodiscard]] const state_type& worker_state() const& RUVIA_LIFETIMEBOUND {
        return services().template worker_state<std::remove_const_t<state_type>>();
    }
    template <typename state_type>
    state_type& worker_state() && = delete;
    template <typename state_type>
    const state_type& worker_state() const&& = delete;

    // The callable executes on the blocking pool and must own everything it
    // touches. Completion resumes on this worker; options.timeout_ or shutdown
    // stops the wait without joining a still-running foreign callable, and
    // options.stop_token_ is combined with the host's stop_token().
    // Rejection and callable exceptions are retained in blocking_result. Inspect
    // status() for load shedding or use std::move(result).take_value() to throw.
    // With application::blocking_pool(nullptr) the result reports
    // blocking_status::unavailable without submitting the callable.
    template <blocking_callable callable_type>
    [[nodiscard]] task<blocking_result<std::invoke_result_t<callable_type&>>> run_blocking(callable_type callable, operation_options options = {}) const& {
        return services().run_blocking(std::move(callable), std::move(options));
    }
    template <blocking_callable callable_type>
    task<blocking_result<std::invoke_result_t<callable_type&>>> run_blocking(callable_type, operation_options = {}) const&& = delete;

    // Cancels or times out only before a pool thread starts `callable`; once
    // execution has started, waits for `callable` to finish unless this worker
    // itself stops.
    template <blocking_callable callable_type>
    [[nodiscard]] task<blocking_result<std::invoke_result_t<callable_type&>>> run_blocking_joined(callable_type callable, operation_options options = {}) const& {
        return services().run_blocking_joined(std::move(callable), std::move(options));
    }
    template <blocking_callable callable_type>
    task<blocking_result<std::invoke_result_t<callable_type&>>> run_blocking_joined(callable_type, operation_options = {}) const&& = delete;

    // The application's env; an empty one when the host has none.
    [[nodiscard]] const ruvia::env& env() const& noexcept RUVIA_LIFETIMEBOUND {
        return services().env();
    }
    const ruvia::env& env() const&& = delete;

    // Defined in the library and instantiated there for every host.
    [[nodiscard]] http_client_ref http_client() const&;
    [[nodiscard]] http_client_ref http_client(std::string_view alias) const&;
    http_client_ref http_client() const&& = delete;
    http_client_ref http_client(std::string_view) const&& = delete;
#ifdef RUVIA_ENABLE_DATABASE
    [[nodiscard]] db_executor db() const&;
    [[nodiscard]] db_executor db(std::string_view alias) const&;
    db_executor db() const&& = delete;
    db_executor db(std::string_view) const&& = delete;
#endif
#ifdef RUVIA_ENABLE_REDIS
    [[nodiscard]] redis_executor redis() const&;
    [[nodiscard]] redis_executor redis(std::string_view alias) const&;
    redis_executor redis() const&& = delete;
    redis_executor redis(std::string_view) const&& = delete;
#endif

private:
    friend host_type;

    worker_services_access() = default;

    [[nodiscard]] detail::worker_services services() const noexcept {
        return static_cast<const host_type&>(*this).services();
    }
};

}  // namespace ruvia
