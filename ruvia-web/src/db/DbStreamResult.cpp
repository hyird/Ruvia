#include "ruvia/web/db/Db.h"

#include <utility>
#include "ruvia/web/detail/db/DbRegistry.h"
#include "ruvia/web/detail/db/DbUtils.h"

// A streaming query result: the rows arrive one at a time from a pooled
// connection the result holds for as long as it is active, and every exit --
// exhaustion, explicit close, failure, or destruction -- must return that
// connection exactly once.

namespace ruvia {
namespace {

Task<std::optional<DbRow>> readPoolStream(detail::DbPoolRef pool, std::size_t slot, void* result,
    std::pmr::memory_resource* resource, const OperationOptions& options) {
#ifdef RUVIA_ENABLE_MARIADB
    if (const auto* client = std::get_if<detail::MariaDbPool*>(&pool);
        client != nullptr && *client != nullptr) {
        return (*client)->readStreamRow(slot, result, resource, options);
    }
#endif
#ifdef RUVIA_ENABLE_POSTGRESQL
    if (const auto* client = std::get_if<detail::PostgreSqlPool*>(&pool);
        client != nullptr && *client != nullptr) {
        return (*client)->readStreamRow(slot, result, resource, options);
    }
#endif
    detail::throwUnavailableDbBackend();
}

Task<void> closePoolStream(detail::DbPoolRef pool, std::size_t slot, void* result,
    std::pmr::memory_resource* resource, const OperationOptions& options) {
#ifdef RUVIA_ENABLE_MARIADB
    if (const auto* client = std::get_if<detail::MariaDbPool*>(&pool);
        client != nullptr && *client != nullptr) {
        return (*client)->closeStream(slot, result, resource, options);
    }
#endif
#ifdef RUVIA_ENABLE_POSTGRESQL
    if (const auto* client = std::get_if<detail::PostgreSqlPool*>(&pool);
        client != nullptr && *client != nullptr) {
        return (*client)->closeStream(slot, result, resource, options);
    }
#endif
    detail::throwUnavailableDbBackend();
}

void abortPoolStream(detail::DbPoolRef pool, std::size_t slot, void* result) noexcept {
#ifdef RUVIA_ENABLE_MARIADB
    if (const auto* client = std::get_if<detail::MariaDbPool*>(&pool);
        client != nullptr && *client != nullptr) {
        (*client)->abortStream(slot, result);
        return;
    }
#endif
#ifdef RUVIA_ENABLE_POSTGRESQL
    if (const auto* client = std::get_if<detail::PostgreSqlPool*>(&pool);
        client != nullptr && *client != nullptr) {
        (*client)->abortStream(slot, result);
    }
#endif
}

}  // namespace

DbStreamResult::Lease::Lease(detail::DbPoolRef client, std::size_t slot, void* result,
    std::pmr::memory_resource* resource, OperationOptions options) noexcept
    : client(client),
      slot(slot),
      result(result),
      resource(detail::pmrResourceOrDefault(resource)),
      options(std::move(options)) {}

class DbStreamResult::State final {
public:
    State(detail::DbPoolRef client, std::size_t slot, void* result,
        std::pmr::memory_resource* resource, OperationOptions options) noexcept
        : operation(Lease{client, slot, result, resource, std::move(options)}) {}

    ~State() {
        operation.reset(
            [](Lease& lease) noexcept { abortPoolStream(lease.client, lease.slot, lease.result); });
    }

    OperationState operation;
};

DbStreamResult::DbStreamResult(detail::DbPoolRef client, std::size_t slot, void* result,
    std::pmr::memory_resource* resource, OperationOptions options)
    : state_(detail::makePmrObject<State>(
          resource, client, slot, result, resource, std::move(options))) {}

DbStreamResult::DbStreamResult(DbStreamResult&& other) noexcept
    : detail::ScopedCapabilityNode(std::move(other)),
      state_(std::move(other.state_)) {}

DbStreamResult::~DbStreamResult() = default;

bool DbStreamResult::active() const noexcept {
    return state_ != nullptr && state_->operation.active();
}

void DbStreamResult::bindOperationScope(detail::ScopedOperationScope& scope) noexcept {
    bind(scope, &DbStreamResult::expireCapability);
}

void DbStreamResult::expireCapability(detail::ScopedCapabilityNode& capability) noexcept {
    auto& result = static_cast<DbStreamResult&>(capability);
    result.reset();
}

ScopedOperation<std::optional<DbRow>> DbStreamResult::read() & {
    requireActive();
    return detail::makeScopedOperation(
        operationScope(), readTask(OperationGuard(state_->operation)));
}

Task<std::optional<DbRow>> DbStreamResult::readTask(OperationGuard operation) {
    operation.start();
    auto& lease = operation.lease();
    auto row = co_await readPoolStream(
        lease.client, lease.slot, lease.result, lease.resource, lease.options);
    if (row) {
        operation.finishActive();
    } else {
        operation.finishClosed();
    }
    co_return row;
}

ScopedOperation<void> DbStreamResult::close() & {
    requireActive();
    return detail::makeScopedOperation(
        operationScope(), closeTask(OperationGuard(state_->operation)));
}

Task<void> DbStreamResult::closeTask(OperationGuard operation) {
    operation.start();
    auto& lease = operation.lease();
    co_await closePoolStream(lease.client, lease.slot, lease.result, lease.resource, lease.options);
    operation.finishClosed();
}

void DbStreamResult::reset() noexcept {
    if (state_ != nullptr) {
        state_->operation.reset(
            [](Lease& lease) noexcept { abortPoolStream(lease.client, lease.slot, lease.result); });
    }
}

}  // namespace ruvia
