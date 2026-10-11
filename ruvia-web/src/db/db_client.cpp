#include "ruvia/web/db/db_client.h"

#include <memory>
#include <stdexcept>
#include <utility>

#include "db/db_client_state.h"

namespace ruvia::detail {

db_client_state::db_client_state(event_loop loop, const db_config& config)
    : loop_(client_lifecycle<db_client_state>::require_loop(std::move(loop))),
      worker_(loop_.handle()),
      memory_(),
      databases_(loop_.io_context(), worker_, memory_.resource(), config),
      lifecycle_(*this, loop_, worker_) {}

#ifdef RUVIA_ENABLE_REDIS
db_client_state::db_client_state(event_loop loop, const db_config& config,
    const redis_handle& cache_store, const db_cache_config& cache_policy)
    : loop_(client_lifecycle<db_client_state>::require_loop(std::move(loop))),
      worker_(loop_.handle()),
      memory_(),
      databases_(loop_.io_context(), worker_, memory_.resource(), config, cache_store, cache_policy),
      lifecycle_(*this, loop_, worker_) {}
#endif

void db_client_state::throw_not_ready() {
    throw std::logic_error("database client is not connected");
}

db_handle db_client_state::handle(operation_options options) {
    lifecycle_.require_ready();
    return databases_.get(lifecycle_.operation_scope()).with_options(lifecycle_.options(std::move(options)));
}

}  // namespace ruvia::detail

namespace ruvia {

db_client::db_client(event_loop loop, const db_config& config)
    : state_(std::make_shared<detail::db_client_state>(std::move(loop), config)) {
    state_->bind_stop();
}

#ifdef RUVIA_ENABLE_REDIS
db_client::db_client(event_loop loop, const db_config& config,
    const redis_handle& cache_store, const db_cache_config& cache_policy)
    : state_(std::make_shared<detail::db_client_state>(std::move(loop), config, cache_store, cache_policy)) {
    state_->bind_stop();
}
#endif

db_client::~db_client() {
    state_->request_close();
}

task<void> db_client::connect() & {
    return state_->connect();
}

db_handle db_client::with_options(operation_options options) const& {
    return state_->handle(std::move(options));
}

db_handle db_client::handle() const& {
    return state_->handle({});
}

void db_client::close() noexcept {
    state_->request_close();
}

task<void> db_client::shutdown() & {
    return state_->shutdown();
}

const worker_handle& db_client::worker() const& noexcept {
    return state_->worker();
}

}  // namespace ruvia
