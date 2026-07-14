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
    std::pmr::memory_resource* resource,
    RequestMemory* requestMemory) {
#ifdef RUVIA_ENABLE_MARIADB
    if (const auto* client = std::get_if<detail::MariaDbPool*>(&pool);
        client != nullptr && *client != nullptr) {
        return (*client)->beginTransaction(resource, requestMemory);
    }
#endif
#ifdef RUVIA_ENABLE_POSTGRESQL
    if (const auto* client = std::get_if<detail::PostgreSqlPool*>(&pool);
        client != nullptr && *client != nullptr) {
        return (*client)->beginTransaction(resource, requestMemory);
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
    std::pmr::memory_resource* resource,
    RequestMemory* requestMemory) noexcept
    : client_(client),
      resource_(detail::pmrResourceOrDefault(resource)),
      requestMemory_(requestMemory) {}

QueryResult DbHandle::mountResult(QueryResult result) const {
    if (requestMemory_ == nullptr) {
        return result;
    }
    const auto& mounted = requestMemory_->emplace<QueryResult>(std::move(result));
    QueryResult view(resource_);
    view.mounted_ = &mounted;
    return view;
}

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
    auto result = co_await executePool(client_, std::move(sql), std::move(params), resource_);
    co_return mountResult(std::move(result));
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
    return beginPoolTransaction(client_, resource_, requestMemory_);
}

DbStreamResult::DbStreamResult(
    detail::DbPoolRef client,
    std::size_t slot,
    void* result,
    std::pmr::memory_resource* resource,
    bool active) noexcept
    : client_(client),
      slot_(slot),
      result_(result),
      resource_(detail::pmrResourceOrDefault(resource)),
      active_(active) {}

DbStreamResult::DbStreamResult(DbStreamResult&& other) noexcept
    : client_(std::exchange(other.client_, {})),
      slot_(std::exchange(other.slot_, 0)),
      result_(std::exchange(other.result_, nullptr)),
      resource_(std::exchange(other.resource_, nullptr)),
      active_(std::exchange(other.active_, false)) {}

DbStreamResult& DbStreamResult::operator=(DbStreamResult&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    reset();
    client_ = std::exchange(other.client_, {});
    slot_ = std::exchange(other.slot_, 0);
    result_ = std::exchange(other.result_, nullptr);
    resource_ = std::exchange(other.resource_, nullptr);
    active_ = std::exchange(other.active_, false);
    return *this;
}

DbStreamResult::~DbStreamResult() {
    reset();
}

bool DbStreamResult::active() const noexcept {
    return active_;
}

Task<std::optional<DbRow>> DbStreamResult::read() {
    if (!active_ || detail::dbPoolRefEmpty(client_)) {
        co_return std::nullopt;
    }
    try {
        auto row = co_await readPoolStream(client_, slot_, result_, resource_);
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
    if (!active_ || detail::dbPoolRefEmpty(client_)) {
        co_return;
    }
    active_ = false;
    auto client = client_;
    const auto slot = slot_;
    auto* result = result_;
    auto* resource = resource_;
    client_ = {};
    slot_ = 0;
    result_ = nullptr;
    co_await closePoolStream(client, slot, result, resource);
}

void DbStreamResult::reset() noexcept {
    if (active_ && !detail::dbPoolRefEmpty(client_)) {
        abortPoolStream(client_, slot_, result_);
    }
    client_ = {};
    slot_ = 0;
    result_ = nullptr;
    active_ = false;
}

void DbStreamResult::release() noexcept {
    client_ = {};
    slot_ = 0;
    result_ = nullptr;
    active_ = false;
}

DbTransaction::DbTransaction(
    detail::DbPoolRef client,
    std::size_t slot,
    std::pmr::memory_resource* resource,
    RequestMemory* requestMemory) noexcept
    : client_(client),
      slot_(slot),
      resource_(detail::pmrResourceOrDefault(resource)),
      requestMemory_(requestMemory),
      active_(true) {}

DbTransaction::DbTransaction(DbTransaction&& other) noexcept
    : client_(std::exchange(other.client_, {})),
      slot_(std::exchange(other.slot_, 0)),
      resource_(std::exchange(other.resource_, nullptr)),
      requestMemory_(std::exchange(other.requestMemory_, nullptr)),
      active_(std::exchange(other.active_, false)) {}

DbTransaction& DbTransaction::operator=(DbTransaction&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    reset();
    client_ = std::exchange(other.client_, {});
    slot_ = std::exchange(other.slot_, 0);
    resource_ = std::exchange(other.resource_, nullptr);
    requestMemory_ = std::exchange(other.requestMemory_, nullptr);
    active_ = std::exchange(other.active_, false);
    return *this;
}

DbTransaction::~DbTransaction() {
    reset();
}

bool DbTransaction::active() const noexcept {
    return active_;
}

QueryResult DbTransaction::mountResult(QueryResult result) const {
    if (requestMemory_ == nullptr) {
        return result;
    }
    const auto& mounted = requestMemory_->emplace<QueryResult>(std::move(result));
    QueryResult view(resource_);
    view.mounted_ = &mounted;
    return view;
}

Task<QueryResult> DbTransaction::query(
    std::string_view sql,
    std::span<const DbValue> params) {
    return execute(sql, params);
}

Task<QueryResult> DbTransaction::execute(
    std::string_view sql,
    std::span<const DbValue> params) {
    if (!active_ || detail::dbPoolRefEmpty(client_)) {
        throw std::logic_error("database transaction is not active");
    }
    auto statement = prepareDbStatement(sql, params, resource_);
    return executePrepared(std::move(statement.sql), std::move(statement.params));
}

Task<QueryResult> DbTransaction::executePrepared(
    std::pmr::string sql,
    std::pmr::vector<DbValue> params) {
    if (!active_ || detail::dbPoolRefEmpty(client_)) {
        throw std::logic_error("database transaction is not active");
    }
    try {
        auto result = co_await executeTransactionPool(
            client_, slot_, std::move(sql), std::move(params), resource_);
        co_return mountResult(std::move(result));
    } catch (...) {
        client_ = {};
        slot_ = 0;
        active_ = false;
        throw;
    }
}

Task<void> DbTransaction::commit() {
    if (!active_ || detail::dbPoolRefEmpty(client_)) {
        throw std::logic_error("database transaction is not active");
    }
    active_ = false;
    auto client = client_;
    const auto slot = slot_;
    auto* resource = resource_;
    client_ = {};
    slot_ = 0;
    return commitPoolTransaction(client, slot, resource);
}

Task<void> DbTransaction::rollback() {
    if (!active_ || detail::dbPoolRefEmpty(client_)) {
        throw std::logic_error("database transaction is not active");
    }
    active_ = false;
    auto client = client_;
    const auto slot = slot_;
    auto* resource = resource_;
    client_ = {};
    slot_ = 0;
    return rollbackPoolTransaction(client, slot, resource);
}

void DbTransaction::reset() noexcept {
    if (active_ && !detail::dbPoolRefEmpty(client_)) {
        abortPoolTransaction(client_, slot_);
    }
    client_ = {};
    slot_ = 0;
    active_ = false;
}

}  // namespace ruvia
