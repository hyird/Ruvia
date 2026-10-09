#include "integration/worker_client_registry_view.h"

#include <optional>

#include "ruvia/core/operation_options.h"
#include "ruvia/web/error.h"
#include "ruvia/web/http_client_handle.h"

#include "client/http_client_registry.h"

#ifdef RUVIA_ENABLE_DATABASE
#include "ruvia/web/db/db_handle.h"

#include "db/db_registry.h"
#endif

#ifdef RUVIA_ENABLE_REDIS
#include "ruvia/web/redis/redis_handle.h"

#include "redis/redis_registry.h"
#endif

namespace ruvia::detail {
namespace {

[[nodiscard]] operation_options context_operation_options(const stop_token& stop_token_value) {
    return operation_options{.timeout_ = std::nullopt, .stop_token_ = stop_token_value};
}

}  // namespace

#ifdef RUVIA_ENABLE_DATABASE
db_handle worker_client_registry_view::db(
    ::ruvia::operation_scope& operation_scope, const stop_token& stop_token_value) const {
    if (!attached()) {
        throw db_error(db_error::code_type::not_configured, std::nullopt, "database is not configured");
    }
    return databases_->get(operation_scope)
        .with_options(context_operation_options(stop_token_value));
}

db_handle worker_client_registry_view::db(std::string_view alias,
    ::ruvia::operation_scope& operation_scope, const stop_token& stop_token_value) const {
    if (!attached()) {
        throw db_error(db_error::code_type::not_configured, std::nullopt, "database is not configured");
    }
    return databases_->get(alias, operation_scope)
        .with_options(context_operation_options(stop_token_value));
}
#endif

#ifdef RUVIA_ENABLE_REDIS
redis_handle worker_client_registry_view::redis(
    ::ruvia::operation_scope& operation_scope, const stop_token& stop_token_value) const {
    if (!attached()) {
        throw redis_error(redis_error::code_type::not_configured, "redis is not configured");
    }
    return redis_->get(operation_scope).with_options(context_operation_options(stop_token_value));
}

redis_handle worker_client_registry_view::redis(std::string_view alias,
    ::ruvia::operation_scope& operation_scope, const stop_token& stop_token_value) const {
    if (!attached()) {
        throw redis_error(redis_error::code_type::not_configured, "redis is not configured");
    }
    return redis_->get(alias, operation_scope)
        .with_options(context_operation_options(stop_token_value));
}
#endif

http_client_handle worker_client_registry_view::get_http_client(
    ::ruvia::operation_scope& operation_scope, const stop_token& stop_token_value) const {
    if (!attached()) {
        throw http_client_error(
            http_client_error::code_type::not_configured, "http client is not configured");
    }
    return http_clients_->get(operation_scope)
        .with_options(context_operation_options(stop_token_value));
}

http_client_handle worker_client_registry_view::get_http_client(std::string_view alias,
    ::ruvia::operation_scope& operation_scope, const stop_token& stop_token_value) const {
    if (!attached()) {
        throw http_client_error(
            http_client_error::code_type::not_configured, "http client is not configured");
    }
    return http_clients_->get(alias, operation_scope)
        .with_options(context_operation_options(stop_token_value));
}

}  // namespace ruvia::detail
