#include "ruvia/web/redis/RedisClient.h"

#include <memory>
#include <stdexcept>
#include <utility>

#include "ruvia/web/detail/redis/RedisClientState.h"

namespace ruvia::detail {

RedisClientState::RedisClientState(EventLoop loop, const RedisConfig& config)
    : loop_(client_lifecycle<RedisClientState>::require_loop(std::move(loop))),
      worker_(loop_.handle()),
      memory_(),
      runtime_(loop_.ioContext(), worker_, RedisConfigStorage(config, memory_.resource()), memory_.resource()),
      lifecycle_(*this, loop_, worker_) {}

void RedisClientState::throw_not_ready() {
    throw std::logic_error("redis client is not connected");
}

RedisHandle RedisClientState::handle(OperationOptions options) {
    lifecycle_.require_ready();
    return runtime_.handle(lifecycle_.operation_scope(), lifecycle_.options(std::move(options)));
}

}  // namespace ruvia::detail

namespace ruvia {

RedisClient::RedisClient(EventLoop loop, const RedisConfig& config)
    : state_(std::make_shared<detail::RedisClientState>(std::move(loop), config)) {
    state_->bindStop();
}

RedisClient::~RedisClient() {
    state_->requestClose();
}

Task<void> RedisClient::connect() & {
    return state_->connect();
}

RedisHandle RedisClient::withOptions(OperationOptions options) const& {
    return state_->handle(std::move(options));
}

void RedisClient::close() noexcept {
    state_->requestClose();
}

Task<void> RedisClient::shutdown() & {
    return state_->shutdown();
}

const WorkerHandle& RedisClient::worker() const& noexcept {
    return state_->worker();
}

}  // namespace ruvia
