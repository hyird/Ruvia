#include "integration/worker_capabilities.h"

#include <memory>
#include <stdexcept>
#include <utility>

#include "context/context_services.h"

namespace ruvia::detail {
namespace {

[[nodiscard]] const worker_handle& require_worker_capabilities_worker(const worker_handle& worker_value) {
    if (!worker_value.valid()) {
        throw std::invalid_argument("worker capabilities require a valid worker");
    }
    return worker_value;
}

}  // namespace

worker_capabilities::worker_capabilities(asio::io_context& io_context, const worker_handle& worker_value,
    std::pmr::memory_resource* resource, worker_capability_definitions definitions,
    worker_capability_options options)
    : worker_(require_worker_capabilities_worker(worker_value)),
      redis_(io_context, resource, definitions.redis_, worker_),
      databases_(io_context, worker_, resource, definitions.databases_, &redis_),
      http_client_result_budget_domain_(
          std::make_shared<http_client_result_budget_domain>(options.http_client_result_budget_)),
      http_clients_(io_context, worker_, resource, definitions.http_clients_,
          http_client_result_budget_domain_),
      worker_states_(resource, definitions.worker_states_),
      rate_limiter_(
          options.default_rate_limit_, options.route_rate_limits_, options.rate_limit_capacity_, resource),
      options_(std::move(options)) {}

task<void> worker_capabilities::connect() {
    try {
        if (!redis_.empty()) {
            co_await redis_.connect();
        }
        if (!databases_.empty()) {
            co_await databases_.connect();
        }
    } catch (...) {
        close_now();
        throw;
    }
}

task<void> worker_capabilities::join() {
    co_await http_clients_.join();
}

void worker_capabilities::close_now() noexcept {
    http_clients_.close_now();
    databases_.close_now();
    redis_.close_now();
}

void worker_capabilities::initialize_worker_state() {
    worker_states_.initialize();
}

void worker_capabilities::shutdown_worker_state() noexcept {
    worker_states_.shutdown();
}

context_services worker_capabilities::make_context_services(const stop_token& stop_token_value) {
    return context_services(context_worker_services{
                                .worker_ = worker_,
                                .clients_ = client_registries(),
                                .rate_limiter_ = &rate_limiter_,
                                .env_ = options_.env_,
                                .max_decoded_body_bytes_ = options_.max_decoded_body_bytes_,
                                .states_ = &worker_states_,
                                .blocking_pool_ = options_.blocking_pool_,
                                .precompressed_static_files_ = options_.precompressed_static_files_,
                                .trusted_proxies_ = options_.trusted_proxies_,
                            },
        stop_token_value);
}

worker_client_registry_view worker_capabilities::client_registries() noexcept {
    return worker_client_registry_view(databases_, redis_, http_clients_);
}

const worker_state_registry& worker_capabilities::worker_states() const noexcept {
    return worker_states_;
}

rate_limiter_type& worker_capabilities::rate_limiter() noexcept {
    return rate_limiter_;
}

blocking_pool* worker_capabilities::get_blocking_pool() const noexcept {
    return options_.blocking_pool_;
}

}  // namespace ruvia::detail
