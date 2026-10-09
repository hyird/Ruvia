#include "ruvia/web/db/db_client.h"

#include <memory>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "db/db_client_state.h"

namespace ruvia::detail {

db_client_state::db_client_state(event_loop loop, const db_config& config)
    : loop_(client_lifecycle<db_client_state>::require_loop(std::move(loop))),
      worker_(loop_.handle()),
      memory_(),
      databases_(loop_.io_context(), worker_, memory_.resource(), config),
      lifecycle_(*this, loop_, worker_) {}

db_client_state::db_client_state(event_loop loop, const db_config& config,
    const redis_handle& cache_store, const db_cache_config& cache_policy)
    : loop_(client_lifecycle<db_client_state>::require_loop(std::move(loop))),
      worker_(loop_.handle()),
      memory_(),
      databases_(loop_.io_context(), worker_, memory_.resource(), config, cache_store, cache_policy),
      lifecycle_(*this, loop_, worker_) {}

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

db_client::db_client(event_loop loop, const db_config& config,
    const redis_handle& cache_store, const db_cache_config& cache_policy)
    : state_(std::make_shared<detail::db_client_state>(std::move(loop), config, cache_store, cache_policy)) {
    state_->bind_stop();
}

db_client::~db_client() {
    state_->request_close();
}

task<void> db_client::connect() & {
    return state_->connect();
}

db_handle db_client::with_options(operation_options options) const& {
    return state_->handle(std::move(options));
}

scoped_operation<db_rows> db_client::query(
    std::string_view sql, std::span<const db_value> params) const& {
    return with_options({}).query(sql, params);
}

scoped_operation<db_exec_result> db_client::execute(
    std::string_view sql, std::span<const db_value> params) const& {
    return with_options({}).execute(sql, params);
}

scoped_operation<db_stream_result> db_client::query_stream(
    std::string_view sql, std::span<const db_value> params) const& {
    return with_options({}).query_stream(sql, params);
}

scoped_operation<db_transaction> db_client::begin_transaction(db_transaction_options options) const& {
    return with_options({}).begin_transaction(std::move(options));
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
