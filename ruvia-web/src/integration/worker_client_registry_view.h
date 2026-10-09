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
class http_client_registry;
class redis_registry;

// A copyable view of the complete client registry set owned by one worker.
// The only states are fully attached and explicitly detached: partial registry
// graphs are not representable. Dispatch retirement detaches the view after all
// posted work has joined, before the worker-owned registries are destroyed.
class worker_client_registry_view final {
public:
    [[nodiscard]] static constexpr worker_client_registry_view detached() noexcept {
        return worker_client_registry_view(nullptr);
    }

    constexpr worker_client_registry_view(
        db_registry& databases, redis_registry& redis, http_client_registry& http_clients) noexcept
        : databases_(&databases),
          redis_(&redis),
          http_clients_(&http_clients) {}

    [[nodiscard]] constexpr bool attached() const noexcept {
        return http_clients_ != nullptr;
    }

    friend constexpr bool operator==(
        const worker_client_registry_view&, const worker_client_registry_view&) noexcept = default;

#ifdef RUVIA_ENABLE_DATABASE
    [[nodiscard]] db_handle db(
        ::ruvia::operation_scope& operation_scope, const stop_token& stop_token) const;
    [[nodiscard]] db_handle db(std::string_view alias, ::ruvia::operation_scope& operation_scope,
        const stop_token& stop_token) const;
#endif

#ifdef RUVIA_ENABLE_REDIS
    [[nodiscard]] redis_handle redis(
        ::ruvia::operation_scope& operation_scope, const stop_token& stop_token) const;
    [[nodiscard]] redis_handle redis(std::string_view alias, ::ruvia::operation_scope& operation_scope,
        const stop_token& stop_token) const;
#endif

    [[nodiscard]] http_client_handle get_http_client(
        ::ruvia::operation_scope& operation_scope, const stop_token& stop_token) const;
    [[nodiscard]] http_client_handle get_http_client(std::string_view alias,
        ::ruvia::operation_scope& operation_scope, const stop_token& stop_token) const;

private:
    explicit constexpr worker_client_registry_view(std::nullptr_t) noexcept {}

    db_registry* databases_{nullptr};
    redis_registry* redis_{nullptr};
    http_client_registry* http_clients_{nullptr};
};

}  // namespace detail
}  // namespace ruvia
