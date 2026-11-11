#include "integration/worker_capabilities.h"

#include <exception>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>

#include "ruvia/core/operation_timeout.h"
#include "ruvia/web/detail/integration/worker_context_capabilities.h"
#include "ruvia/web/detail/integration/worker_state.h"
#include "ruvia/web/http_client.h"

#include "client/http_client_registry.h"
#include "context/context_services.h"

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

[[nodiscard]] const worker_handle& require_worker_capabilities_worker(const worker_handle& worker_value) {
    if (!worker_value.valid()) {
        throw std::invalid_argument("worker capabilities require a valid worker");
    }
    return worker_value;
}

[[nodiscard]] worker_capability_options require_worker_capability_options(worker_capability_options options) {
    if (options.connection_failure_ == nullptr) {
        throw std::invalid_argument("worker capabilities require a connection failure outlet");
    }
    return options;
}

[[nodiscard]] operation_options context_operation_options(const stop_token& stop_token_value) {
    return operation_options{.timeout_ = std::nullopt, .stop_token_ = stop_token_value};
}
}  // namespace

worker_capabilities::worker_capabilities(asio::io_context& io_context, const worker_handle& worker_value,
    worker_capability_definitions definitions,
    worker_capability_options options)
    : worker_(require_worker_capabilities_worker(worker_value)),
      redis_(io_context, definitions.redis_, worker_),
      databases_(io_context, worker_, definitions.databases_, &redis_),
      http_client_result_budget_domain_(
          std::make_shared<http_client_result_budget_domain>(options.http_client_result_budget_)),
      http_clients_(io_context, worker_,
          definitions.http_clients_, http_client_result_budget_domain_),
      worker_states_(definitions.worker_states_),
      rate_limiter_(
          options.default_rate_limit_, options.route_rate_limits_, options.rate_limit_capacity_),
      options_(require_worker_capability_options(std::move(options))),
      worker_services_{
          .worker_ = worker_,
          .clients_ = client_registries(),
          .rate_limiter_ = &rate_limiter_,
          .env_ = options_.env_,
          .max_decoded_body_bytes_ = options_.max_decoded_body_bytes_,
          .states_ = &worker_states_,
          .precompressed_static_files_ = options_.precompressed_static_files_,
          .file_io_timeout_ = options_.file_io_timeout_,
          .connection_failure_ = options_.connection_failure_,
          .trusted_proxies_ = options_.trusted_proxies_,
      } {}

task<void> worker_capabilities::start() {
    worker_states_.initialize();
    try {
        if (!redis_.empty()) {
            co_await redis_.connect();
        }
        if (!databases_.empty()) {
            co_await databases_.connect();
        }
    } catch (...) {
        close_now();
        worker_states_.shutdown();
        throw;
    }
}

void worker_capabilities::close_now() noexcept {
    http_clients_.close_now();
    databases_.close_now();
    redis_.close_now();
}

task<void> worker_capabilities::stop() {
    close_now();
    std::exception_ptr failure;
    try {
        co_await http_clients_.join();
    } catch (...) {
        failure = std::current_exception();
    }
    worker_states_.shutdown();
    if (failure != nullptr) {
        std::rethrow_exception(failure);
    }
}

context_services worker_capabilities::make_context_services(const stop_token& stop_token_value) {
    return context_services(worker_services_, stop_token_value);
}

worker_client_registry_view worker_capabilities::client_registries() noexcept {
    return worker_client_registry_view(http_clients_
#ifdef RUVIA_ENABLE_DATABASE
        ,
        databases_
#endif
#ifdef RUVIA_ENABLE_REDIS
        ,
        redis_
#endif
    );
}

const worker_state_registry& worker_capabilities::worker_states() const noexcept {
    return worker_states_;
}

void* worker_context_capabilities::worker_state_instance(const void* type_key) const {
    auto* instance = worker_states_ == nullptr ? nullptr : worker_states_->instance(type_key);
    if (instance == nullptr) {
        throw std::logic_error(
            "worker state type is not registered: call application::use_worker_state<state_type>() before application::run()");
    }
    return instance;
}

#ifdef RUVIA_ENABLE_DATABASE
db_handle worker_client_registry_view::db(::ruvia::operation_scope& operation_scope,
    request_io_metrics* io_metrics, const stop_token& stop_token_value) const {
    if (!attached()) {
        throw db_error(db_error::code_type::not_configured, std::nullopt, "database is not configured");
    }
    return databases_->get(operation_scope, io_metrics)
        .with_options(context_operation_options(stop_token_value));
}

db_handle worker_client_registry_view::db(std::string_view alias, ::ruvia::operation_scope& operation_scope,
    request_io_metrics* io_metrics, const stop_token& stop_token_value) const {
    if (!attached()) {
        throw db_error(db_error::code_type::not_configured, std::nullopt, "database is not configured");
    }
    return databases_->get(alias, operation_scope, io_metrics)
        .with_options(context_operation_options(stop_token_value));
}
#endif

#ifdef RUVIA_ENABLE_REDIS
redis_handle worker_client_registry_view::redis(::ruvia::operation_scope& operation_scope,
    request_io_metrics* io_metrics, const stop_token& stop_token_value) const {
    if (!attached()) {
        throw redis_error(redis_error::code_type::not_configured, "redis is not configured");
    }
    return redis_->get(operation_scope, io_metrics).with_options(context_operation_options(stop_token_value));
}

redis_handle worker_client_registry_view::redis(std::string_view alias, ::ruvia::operation_scope& operation_scope,
    request_io_metrics* io_metrics, const stop_token& stop_token_value) const {
    if (!attached()) {
        throw redis_error(redis_error::code_type::not_configured, "redis is not configured");
    }
    return redis_->get(alias, operation_scope, io_metrics)
        .with_options(context_operation_options(stop_token_value));
}
#endif

http_client_handle worker_client_registry_view::client(
    ::ruvia::operation_scope& operation_scope, const stop_token& stop_token_value) const {
    if (!attached()) {
        throw http_client_error(
            http_client_error::code_type::not_configured, "http client is not configured");
    }
    return http_clients_->get(operation_scope)
        .with_options(context_operation_options(stop_token_value));
}

http_client_handle worker_client_registry_view::client(std::string_view alias,
    ::ruvia::operation_scope& operation_scope, const stop_token& stop_token_value) const {
    if (!attached()) {
        throw http_client_error(
            http_client_error::code_type::not_configured, "http client is not configured");
    }
    return http_clients_->get(alias, operation_scope)
        .with_options(context_operation_options(stop_token_value));
}

}  // namespace ruvia::detail
