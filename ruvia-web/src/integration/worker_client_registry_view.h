#pragma once

#include <cstddef>
#include <string_view>

namespace ruvia {

class http_client_handle;
class stop_token;
class operation_scope;

#ifdef RUVIA_ENABLE_DATABASE
class db_handle;
#endif

#ifdef RUVIA_ENABLE_REDIS
class redis_handle;
#endif

namespace detail {

class db_registry;
struct request_io_metrics;
class http_client_registry;
class redis_registry;

// A copyable view of the complete client registry set owned by one worker.
// The only states are fully attached and detached: partial registry graphs are
// not representable. Detached views are only placeholders for contexts built
// without worker capabilities; an attached view never outlives its worker,
// which joins all posted work before destroying its registries.
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
        ) noexcept
        : http_clients_(&http_clients)
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

    [[nodiscard]] constexpr bool attached() const noexcept {
        return http_clients_ != nullptr;
    }

    // `io_metrics` is the request's counter block, or null outside a request.
#ifdef RUVIA_ENABLE_DATABASE
    [[nodiscard]] db_handle db(::ruvia::operation_scope& operation_scope, request_io_metrics* io_metrics,
        const stop_token& stop_token) const;
    [[nodiscard]] db_handle db(std::string_view alias, ::ruvia::operation_scope& operation_scope,
        request_io_metrics* io_metrics, const stop_token& stop_token) const;
#endif

#ifdef RUVIA_ENABLE_REDIS
    [[nodiscard]] redis_handle redis(::ruvia::operation_scope& operation_scope, request_io_metrics* io_metrics,
        const stop_token& stop_token) const;
    [[nodiscard]] redis_handle redis(std::string_view alias, ::ruvia::operation_scope& operation_scope,
        request_io_metrics* io_metrics, const stop_token& stop_token) const;
#endif

    [[nodiscard]] http_client_handle client(
        ::ruvia::operation_scope& operation_scope, const stop_token& stop_token) const;
    [[nodiscard]] http_client_handle client(std::string_view alias,
        ::ruvia::operation_scope& operation_scope, const stop_token& stop_token) const;

private:
    explicit constexpr worker_client_registry_view(std::nullptr_t) noexcept {}

    http_client_registry* http_clients_{nullptr};
#ifdef RUVIA_ENABLE_DATABASE
    db_registry* databases_{nullptr};
#endif
#ifdef RUVIA_ENABLE_REDIS
    redis_registry* redis_{nullptr};
#endif
};

}  // namespace detail
}  // namespace ruvia
