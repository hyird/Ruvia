#include "ruvia/web/redis/redis_client.h"

#include <memory>
#include <stdexcept>
#include <utility>

#include "redis/redis_client_state.h"

namespace ruvia::detail {

redis_client_state::redis_client_state(event_loop loop, const redis_config& config)
    : loop_(client_lifecycle<redis_client_state>::require_loop(std::move(loop))),
      worker_(loop_.handle()),
      memory_(),
      runtime_(loop_.io_context(), worker_, redis_config_storage(config, memory_.resource()), memory_.resource()),
      lifecycle_(*this, loop_, worker_) {}

void redis_client_state::throw_not_ready() {
    throw std::logic_error("redis client is not connected");
}

redis_handle redis_client_state::handle(operation_options options) {
    lifecycle_.require_ready();
    return runtime_.handle(lifecycle_.operation_scope(), lifecycle_.options(std::move(options)));
}

}  // namespace ruvia::detail

namespace ruvia {

redis_client::redis_client(event_loop loop, const redis_config& config)
    : state_(std::make_shared<detail::redis_client_state>(std::move(loop), config)) {
    state_->bind_stop();
}

redis_client::~redis_client() {
    state_->request_close();
}

task<void> redis_client::connect() & {
    return state_->connect();
}

redis_handle redis_client::with_options(operation_options options) const& {
    return state_->handle(std::move(options));
}

void redis_client::close() noexcept {
    state_->request_close();
}

task<void> redis_client::shutdown() & {
    return state_->shutdown();
}

const worker_handle& redis_client::worker() const& noexcept {
    return state_->worker();
}

}  // namespace ruvia
