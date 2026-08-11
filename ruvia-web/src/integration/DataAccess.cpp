#include "ruvia/web/DataAccess.h"

#include <memory>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <utility>

#include "ruvia/web/detail/db/DbRegistry.h"
#include "ruvia/web/detail/integration/DataAccessServiceState.h"
#include "ruvia/web/detail/redis/RedisRegistry.h"

namespace ruvia {

DataAccessContext::DataAccessContext(std::shared_ptr<detail::DataAccessServiceState> state) noexcept
    : state_(std::move(state)) {}

const WorkerHandle& DataAccessContext::worker() const& noexcept {
    return state_->worker();
}

std::pmr::memory_resource* DataAccessContext::resource() const noexcept {
    return state_->resource();
}

StopToken DataAccessContext::stopToken() const noexcept {
    return state_->stopToken();
}

#ifdef RUVIA_ENABLE_DATABASE
DbHandle DataAccessContext::db() const {
    state_->requireConnectedOnWorker();
    return state_->access().databases().get(resource(), operationScope_).withOptions(
        DbOperationOptions{.timeout = std::nullopt, .stopToken = stopToken()});
}

DbHandle DataAccessContext::db(std::string_view alias) const {
    state_->requireConnectedOnWorker();
    return state_->access().databases().get(alias, resource(), operationScope_).withOptions(
        DbOperationOptions{.timeout = std::nullopt, .stopToken = stopToken()});
}
#endif

#ifdef RUVIA_ENABLE_REDIS
RedisHandle DataAccessContext::redis() const {
    state_->requireConnectedOnWorker();
    return state_->access().redis().get(resource(), operationScope_).withOptions(
        RedisOperationOptions{.timeout = std::nullopt, .stopToken = stopToken()});
}

RedisHandle DataAccessContext::redis(std::string_view alias) const {
    state_->requireConnectedOnWorker();
    return state_->access().redis().get(alias, resource(), operationScope_).withOptions(
        RedisOperationOptions{.timeout = std::nullopt, .stopToken = stopToken()});
}
#endif

DataAccessService::DataAccessService(EventLoop loop, DataAccessOptions options)
    : state_(std::make_shared<detail::DataAccessServiceState>(std::move(loop), std::move(options))) {
    state_->bindStop();
}

DataAccessService::~DataAccessService() {
    state_->requestClose();
}

std::future<void> DataAccessService::connect() {
    return state_->scheduleConnect();
}

DataAccessPostResult DataAccessService::postTask(MoveOnlyFunction<Task<void>(DataAccessContext&)> task) {
    return state_->post(std::move(task));
}

void DataAccessService::close() {
    if (!state_->worker().isCurrent()) {
        throw std::logic_error("data access service must close on its bound event loop");
    }
    state_->closeOnWorker();
}

DataAccessStats DataAccessService::stats() const noexcept {
    return state_->stats();
}

const WorkerHandle& DataAccessService::worker() const& noexcept {
    return state_->worker();
}

}  // namespace ruvia
