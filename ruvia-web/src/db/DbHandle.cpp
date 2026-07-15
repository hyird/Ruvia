#include "ruvia/web/db/Db.h"

#include "ruvia/web/detail/db/DbInternal.h"
#include "ruvia/web/detail/db/DbUtils.h"
#include "ruvia/core/memory/MemoryPool.h"

#include <stdexcept>
#include <utility>

namespace ruvia {
namespace {

struct PreparedDbStatement final {
    std::pmr::string sql;
    std::pmr::vector<DbValue> params;
};

[[nodiscard]] PreparedDbStatement prepareDbStatement(
    std::string_view sql,
    std::span<const DbValue> params,
    std::pmr::memory_resource* resource) {
    return PreparedDbStatement{
        std::pmr::string(sql, resource),
        detail::cloneDbValues(params, resource)};
}

[[noreturn]] void throwUnavailableDbBackend() {
    throw std::logic_error("database backend is not available");
}

Task<QueryResult> executePool(
    detail::DbPoolRef pool,
    std::pmr::string sql,
    std::pmr::vector<DbValue> params,
    std::pmr::memory_resource* resource) {
#ifdef RUVIA_ENABLE_MARIADB
    if (const auto* client = std::get_if<detail::MariaDbPool*>(&pool);
        client != nullptr && *client != nullptr) {
        return (*client)->execute(std::move(sql), std::move(params), resource);
    }
#endif
#ifdef RUVIA_ENABLE_POSTGRESQL
    if (const auto* client = std::get_if<detail::PostgreSqlPool*>(&pool);
        client != nullptr && *client != nullptr) {
        return (*client)->execute(std::move(sql), std::move(params), resource);
    }
#endif
    throwUnavailableDbBackend();
}

Task<DbStreamResult> streamPool(
    detail::DbPoolRef pool,
    std::pmr::string sql,
    std::pmr::vector<DbValue> params,
    std::pmr::memory_resource* resource) {
#ifdef RUVIA_ENABLE_MARIADB
    if (const auto* client = std::get_if<detail::MariaDbPool*>(&pool);
        client != nullptr && *client != nullptr) {
        return (*client)->stream(std::move(sql), std::move(params), resource);
    }
#endif
#ifdef RUVIA_ENABLE_POSTGRESQL
    if (const auto* client = std::get_if<detail::PostgreSqlPool*>(&pool);
        client != nullptr && *client != nullptr) {
        return (*client)->stream(std::move(sql), std::move(params), resource);
    }
#endif
    throwUnavailableDbBackend();
}

Task<DbTransaction> beginPoolTransaction(
    detail::DbPoolRef pool,
    std::pmr::memory_resource* resource) {
#ifdef RUVIA_ENABLE_MARIADB
    if (const auto* client = std::get_if<detail::MariaDbPool*>(&pool);
        client != nullptr && *client != nullptr) {
        return (*client)->beginTransaction(resource);
    }
#endif
#ifdef RUVIA_ENABLE_POSTGRESQL
    if (const auto* client = std::get_if<detail::PostgreSqlPool*>(&pool);
        client != nullptr && *client != nullptr) {
        return (*client)->beginTransaction(resource);
    }
#endif
    throwUnavailableDbBackend();
}

Task<std::optional<DbRow>> readPoolStream(
    detail::DbPoolRef pool,
    std::size_t slot,
    void* result,
    std::pmr::memory_resource* resource) {
#ifdef RUVIA_ENABLE_MARIADB
    if (const auto* client = std::get_if<detail::MariaDbPool*>(&pool);
        client != nullptr && *client != nullptr) {
        return (*client)->readStreamRow(slot, result, resource);
    }
#endif
#ifdef RUVIA_ENABLE_POSTGRESQL
    if (const auto* client = std::get_if<detail::PostgreSqlPool*>(&pool);
        client != nullptr && *client != nullptr) {
        return (*client)->readStreamRow(slot, result, resource);
    }
#endif
    throwUnavailableDbBackend();
}

Task<void> closePoolStream(
    detail::DbPoolRef pool,
    std::size_t slot,
    void* result,
    std::pmr::memory_resource* resource) {
#ifdef RUVIA_ENABLE_MARIADB
    if (const auto* client = std::get_if<detail::MariaDbPool*>(&pool);
        client != nullptr && *client != nullptr) {
        return (*client)->closeStream(slot, result, resource);
    }
#endif
#ifdef RUVIA_ENABLE_POSTGRESQL
    if (const auto* client = std::get_if<detail::PostgreSqlPool*>(&pool);
        client != nullptr && *client != nullptr) {
        return (*client)->closeStream(slot, result, resource);
    }
#endif
    throwUnavailableDbBackend();
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

Task<QueryResult> executeTransactionPool(
    detail::DbPoolRef pool,
    std::size_t slot,
    std::pmr::string sql,
    std::pmr::vector<DbValue> params,
    std::pmr::memory_resource* resource) {
#ifdef RUVIA_ENABLE_MARIADB
    if (const auto* client = std::get_if<detail::MariaDbPool*>(&pool);
        client != nullptr && *client != nullptr) {
        return (*client)->executeOnTransactionSlot(
            slot, std::move(sql), std::move(params), resource);
    }
#endif
#ifdef RUVIA_ENABLE_POSTGRESQL
    if (const auto* client = std::get_if<detail::PostgreSqlPool*>(&pool);
        client != nullptr && *client != nullptr) {
        return (*client)->executeOnTransactionSlot(
            slot, std::move(sql), std::move(params), resource);
    }
#endif
    throwUnavailableDbBackend();
}

Task<void> commitPoolTransaction(
    detail::DbPoolRef pool,
    std::size_t slot,
    std::pmr::memory_resource* resource) {
#ifdef RUVIA_ENABLE_MARIADB
    if (const auto* client = std::get_if<detail::MariaDbPool*>(&pool);
        client != nullptr && *client != nullptr) {
        return (*client)->commitTransaction(slot, resource);
    }
#endif
#ifdef RUVIA_ENABLE_POSTGRESQL
    if (const auto* client = std::get_if<detail::PostgreSqlPool*>(&pool);
        client != nullptr && *client != nullptr) {
        return (*client)->commitTransaction(slot, resource);
    }
#endif
    throwUnavailableDbBackend();
}

Task<void> rollbackPoolTransaction(
    detail::DbPoolRef pool,
    std::size_t slot,
    std::pmr::memory_resource* resource) {
#ifdef RUVIA_ENABLE_MARIADB
    if (const auto* client = std::get_if<detail::MariaDbPool*>(&pool);
        client != nullptr && *client != nullptr) {
        return (*client)->rollbackTransaction(slot, resource);
    }
#endif
#ifdef RUVIA_ENABLE_POSTGRESQL
    if (const auto* client = std::get_if<detail::PostgreSqlPool*>(&pool);
        client != nullptr && *client != nullptr) {
        return (*client)->rollbackTransaction(slot, resource);
    }
#endif
    throwUnavailableDbBackend();
}

void abortPoolTransaction(detail::DbPoolRef pool, std::size_t slot) noexcept {
#ifdef RUVIA_ENABLE_MARIADB
    if (const auto* client = std::get_if<detail::MariaDbPool*>(&pool);
        client != nullptr && *client != nullptr) {
        (*client)->abortTransaction(slot);
        return;
    }
#endif
#ifdef RUVIA_ENABLE_POSTGRESQL
    if (const auto* client = std::get_if<detail::PostgreSqlPool*>(&pool);
        client != nullptr && *client != nullptr) {
        (*client)->abortTransaction(slot);
    }
#endif
}

}  // namespace

DbHandle::DbHandle(
    detail::DbPoolRef client,
    std::pmr::memory_resource* resource) noexcept
    : client_(client),
      resource_(detail::pmrResourceOrDefault(resource)) {}

Task<QueryResult> DbHandle::query(std::string_view sql, std::span<const DbValue> params) const {
    return execute(sql, params);
}

Task<QueryResult> DbHandle::execute(std::string_view sql, std::span<const DbValue> params) const {
    auto statement = prepareDbStatement(sql, params, resource_);
    return executePrepared(std::move(statement.sql), std::move(statement.params));
}

Task<QueryResult> DbHandle::executePrepared(
    std::pmr::string sql,
    std::pmr::vector<DbValue> params) const {
    co_return co_await executePool(
        client_, std::move(sql), std::move(params), resource_);
}

Task<DbStreamResult> DbHandle::queryStream(
    std::string_view sql,
    std::span<const DbValue> params) const {
    auto statement = prepareDbStatement(sql, params, resource_);
    return queryStreamPrepared(std::move(statement.sql), std::move(statement.params));
}

Task<DbStreamResult> DbHandle::queryStreamPrepared(
    std::pmr::string sql,
    std::pmr::vector<DbValue> params) const {
    return streamPool(client_, std::move(sql), std::move(params), resource_);
}

Task<DbTransaction> DbHandle::beginTransaction() const {
    return beginPoolTransaction(client_, resource_);
}

DbStreamResult::Active::Active(
    detail::DbPoolRef client,
    std::size_t slot,
    void* result,
    std::pmr::memory_resource* resource) noexcept
    : client(client),
      slot(slot),
      result(result),
      resource(detail::pmrResourceOrDefault(resource)) {}

DbStreamResult::DbStreamResult(
    detail::DbPoolRef client,
    std::size_t slot,
    void* result,
    std::pmr::memory_resource* resource) noexcept
    : state_(std::in_place_type<Active>, client, slot, result, resource) {}

DbStreamResult::DbStreamResult(DbStreamResult&& other) noexcept
    : state_(std::move(other.state_)) {
    other.state_.emplace<Closed>();
}

DbStreamResult::~DbStreamResult() {
    reset();
}

bool DbStreamResult::active() const noexcept {
    return std::holds_alternative<Active>(state_);
}

Task<std::optional<DbRow>> DbStreamResult::read() {
    auto* active = std::get_if<Active>(&state_);
    if (active == nullptr) {
        co_return std::nullopt;
    }
    try {
        auto row = co_await readPoolStream(
            active->client, active->slot, active->result, active->resource);
        if (!row) {
            release();
        }
        co_return row;
    } catch (...) {
        release();
        throw;
    }
}

Task<void> DbStreamResult::close() {
    auto* active = std::get_if<Active>(&state_);
    if (active == nullptr) {
        co_return;
    }
    auto owned = std::move(*active);
    state_.emplace<Closed>();
    co_await closePoolStream(owned.client, owned.slot, owned.result, owned.resource);
}

void DbStreamResult::reset() noexcept {
    if (auto* active = std::get_if<Active>(&state_); active != nullptr) {
        abortPoolStream(active->client, active->slot, active->result);
    }
    state_.emplace<Closed>();
}

void DbStreamResult::release() noexcept {
    state_.emplace<Closed>();
}

DbTransaction::Active::Active(
    detail::DbPoolRef client,
    std::size_t slot,
    std::pmr::memory_resource* resource) noexcept
    : client(client),
      slot(slot),
      resource(detail::pmrResourceOrDefault(resource)) {}

DbTransaction::DbTransaction(
    detail::DbPoolRef client,
    std::size_t slot,
    std::pmr::memory_resource* resource) noexcept
    : state_(std::in_place_type<Active>, client, slot, resource) {}

DbTransaction::DbTransaction(DbTransaction&& other) noexcept
    : state_(std::move(other.state_)) {
    other.state_.emplace<Closed>();
}

DbTransaction::~DbTransaction() {
    reset();
}

bool DbTransaction::active() const noexcept {
    return std::holds_alternative<Active>(state_);
}

Task<QueryResult> DbTransaction::query(
    std::string_view sql,
    std::span<const DbValue> params) {
    return execute(sql, params);
}

Task<QueryResult> DbTransaction::execute(
    std::string_view sql,
    std::span<const DbValue> params) {
    const auto* active = std::get_if<Active>(&state_);
    if (active == nullptr) {
        throw std::logic_error("database transaction is not active");
    }
    auto statement = prepareDbStatement(sql, params, active->resource);
    return executePrepared(std::move(statement.sql), std::move(statement.params));
}

Task<QueryResult> DbTransaction::executePrepared(
    std::pmr::string sql,
    std::pmr::vector<DbValue> params) {
    auto* active = std::get_if<Active>(&state_);
    if (active == nullptr) {
        throw std::logic_error("database transaction is not active");
    }
    try {
        co_return co_await executeTransactionPool(
            active->client,
            active->slot,
            std::move(sql),
            std::move(params),
            active->resource);
    } catch (...) {
        state_.emplace<Closed>();
        throw;
    }
}

Task<void> DbTransaction::commit() {
    auto* active = std::get_if<Active>(&state_);
    if (active == nullptr) {
        throw std::logic_error("database transaction is not active");
    }
    auto owned = std::move(*active);
    state_.emplace<Closed>();
    return commitPoolTransaction(owned.client, owned.slot, owned.resource);
}

Task<void> DbTransaction::rollback() {
    auto* active = std::get_if<Active>(&state_);
    if (active == nullptr) {
        throw std::logic_error("database transaction is not active");
    }
    auto owned = std::move(*active);
    state_.emplace<Closed>();
    return rollbackPoolTransaction(owned.client, owned.slot, owned.resource);
}

void DbTransaction::reset() noexcept {
    if (auto* active = std::get_if<Active>(&state_); active != nullptr) {
        abortPoolTransaction(active->client, active->slot);
    }
    state_.emplace<Closed>();
}

}  // namespace ruvia
