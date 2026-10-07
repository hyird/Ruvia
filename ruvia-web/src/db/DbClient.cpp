#include "ruvia/web/db/DbClient.h"

#include <memory>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "ruvia/web/detail/db/DbClientState.h"

namespace ruvia::detail {

DbClientState::DbClientState(EventLoop loop, const DbConfig& config)
    : loop_(client_lifecycle<DbClientState>::require_loop(std::move(loop))),
      worker_(loop_.handle()),
      memory_(),
      databases_(loop_.ioContext(), worker_, memory_.resource(), config),
      lifecycle_(*this, loop_, worker_) {}

DbClientState::DbClientState(EventLoop loop, const DbConfig& config,
    const RedisHandle& cache_store, const DbCacheConfig& cache_policy)
    : loop_(client_lifecycle<DbClientState>::require_loop(std::move(loop))),
      worker_(loop_.handle()),
      memory_(),
      databases_(loop_.ioContext(), worker_, memory_.resource(), config, cache_store, cache_policy),
      lifecycle_(*this, loop_, worker_) {}

void DbClientState::throw_not_ready() {
    throw std::logic_error("database client is not connected");
}

DbHandle DbClientState::handle(OperationOptions options) {
    lifecycle_.require_ready();
    return databases_.get(lifecycle_.operation_scope()).withOptions(lifecycle_.options(std::move(options)));
}

}  // namespace ruvia::detail

namespace ruvia {

DbClient::DbClient(EventLoop loop, const DbConfig& config)
    : state_(std::make_shared<detail::DbClientState>(std::move(loop), config)) {
    state_->bindStop();
}

DbClient::DbClient(EventLoop loop, const DbConfig& config,
    const RedisHandle& cache_store, const DbCacheConfig& cache_policy)
    : state_(std::make_shared<detail::DbClientState>(std::move(loop), config, cache_store, cache_policy)) {
    state_->bindStop();
}

DbClient::~DbClient() {
    state_->requestClose();
}

Task<void> DbClient::connect() & {
    return state_->connect();
}

DbHandle DbClient::withOptions(OperationOptions options) const& {
    return state_->handle(std::move(options));
}

ScopedOperation<DbRows> DbClient::query(
    std::string_view sql, std::span<const DbValue> params) const& {
    return withOptions({}).query(sql, params);
}

ScopedOperation<DbExecResult> DbClient::execute(
    std::string_view sql, std::span<const DbValue> params) const& {
    return withOptions({}).execute(sql, params);
}

ScopedOperation<DbStreamResult> DbClient::queryStream(
    std::string_view sql, std::span<const DbValue> params) const& {
    return withOptions({}).queryStream(sql, params);
}

ScopedOperation<DbTransaction> DbClient::beginTransaction(DbTransactionOptions options) const& {
    return withOptions({}).beginTransaction(std::move(options));
}

void DbClient::close() noexcept {
    state_->requestClose();
}

Task<void> DbClient::shutdown() & {
    return state_->shutdown();
}

const WorkerHandle& DbClient::worker() const& noexcept {
    return state_->worker();
}

}  // namespace ruvia
