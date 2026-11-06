#pragma once

#include <cstddef>
#include <string_view>

namespace ruvia {

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

    constexpr worker_client_registry_view(db_registry& databases, redis_registry& redis) noexcept
        : databases_(&databases),
          redis_(&redis) {}

    [[nodiscard]] constexpr bool attached() const noexcept {
        return databases_ != nullptr;
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

private:
    explicit constexpr worker_client_registry_view(std::nullptr_t) noexcept {}

    db_registry* databases_{nullptr};
    // Read only by the Redis accessors.
    [[maybe_unused]] redis_registry* redis_{nullptr};
};

}  // namespace detail
}  // namespace ruvia
