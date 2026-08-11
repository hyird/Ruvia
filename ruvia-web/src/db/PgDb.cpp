#include "ruvia/web/detail/db/DbRegistry.h"

#include "ruvia/web/detail/db/DbPostgreSql.h"
#include "ruvia/web/detail/db/DbResultAccess.h"
#include "ruvia/web/detail/db/DbUtils.h"

#include <libpq-fe.h>

#include <stdexcept>
#include <utility>

namespace ruvia::detail {
namespace {

void freePostgreSqlResult(void* result) noexcept {
    PQclear(static_cast<PGresult*>(result));
}

class PostgreSqlResultOwner final {
public:
    explicit PostgreSqlResultOwner(PGresult* result) noexcept
        : result_(result) {}

    ~PostgreSqlResultOwner() {
        reset();
    }

    PostgreSqlResultOwner(const PostgreSqlResultOwner&) = delete;
    PostgreSqlResultOwner& operator=(const PostgreSqlResultOwner&) = delete;

    [[nodiscard]] PGresult* get() const noexcept {
        return result_;
    }

    [[nodiscard]] PGresult& operator*() const noexcept {
        return *result_;
    }

    [[nodiscard]] PGresult* release() noexcept {
        return std::exchange(result_, nullptr);
    }

    void reset() noexcept {
        if (result_ != nullptr) {
            PQclear(result_);
            result_ = nullptr;
        }
    }

private:
    PGresult* result_;
};

void clearRemainingPostgreSqlResults(PGconn* connection) noexcept {
    while (auto* remaining = PQgetResult(connection)) {
        PQclear(remaining);
    }
}

[[nodiscard]] bool successfulResultStatus(ExecStatusType status) noexcept {
    return status == PGRES_COMMAND_OK || status == PGRES_TUPLES_OK || status == PGRES_EMPTY_QUERY;
}

void materializeBorrowedResult(DbRows& output, PGresult& result, std::pmr::memory_resource* resource) {
    const auto rowCount = static_cast<std::size_t>(PQntuples(&result));
    const auto fieldCount = static_cast<std::size_t>(PQnfields(&result));
    auto& rows = DbResultAccess::rows(output);
    auto& fields = DbResultAccess::fields(output);
    auto& columnNames = DbResultAccess::columnNames(output);
    rows.reserve(rowCount);
    fields.reserve(rowCount * fieldCount);
    columnNames.reserve(fieldCount);
    for (std::size_t field = 0; field < fieldCount; ++field) {
        columnNames.emplace_back(PQfname(&result, static_cast<int>(field)));
    }
    for (std::size_t row = 0; row < rowCount; ++row) {
        const auto rowStart = fields.size();
        for (std::size_t field = 0; field < fieldCount; ++field) {
            const auto rowIndex = static_cast<int>(row);
            const auto fieldIndex = static_cast<int>(field);
            if (PQgetisnull(&result, rowIndex, fieldIndex) != 0) {
                fields.push_back(DbResultAccess::nullField(resource));
                continue;
            }
            fields.push_back(DbResultAccess::borrowedField(std::string_view(PQgetvalue(&result, rowIndex, fieldIndex), static_cast<std::size_t>(PQgetlength(&result, rowIndex, fieldIndex))), resource));
        }
        rows.push_back(DbResultAccess::borrowedRow(
            fields.data() + rowStart,
            fieldCount,
            columnNames.data(),
            columnNames.size(),
            resource));
    }
}

[[nodiscard]] DbRow materializeOwnedSingleRow(PGresult& result, std::pmr::memory_resource* resource) {
    auto row = DbResultAccess::ownedRow(resource);
    auto& fields = DbResultAccess::ownedFields(row);
    auto& columnNames = DbResultAccess::ownedColumnNames(row);
    const auto fieldCount = static_cast<std::size_t>(PQnfields(&result));
    fields.reserve(fieldCount);
    columnNames.reserve(fieldCount);
    for (std::size_t field = 0; field < fieldCount; ++field) {
        const auto fieldIndex = static_cast<int>(field);
        columnNames.emplace_back(PQfname(&result, fieldIndex));
        if (PQgetisnull(&result, 0, fieldIndex) != 0) {
            fields.push_back(DbResultAccess::nullField(resource));
            continue;
        }
        fields.push_back(DbResultAccess::ownedField(std::string_view(PQgetvalue(&result, 0, fieldIndex), static_cast<std::size_t>(PQgetlength(&result, 0, fieldIndex))), resource));
    }
    return row;
}

}  // namespace

Task<DbRows> PostgreSqlPool::query(std::pmr::string sql, std::pmr::vector<DbValue> params, std::pmr::memory_resource* resource, OperationOptions options) {
    return executeDbQuery(*this, std::move(sql), std::move(params), resource, std::move(options));
}

Task<DbExecResult> PostgreSqlPool::execute(std::pmr::string sql, std::pmr::vector<DbValue> params, std::pmr::memory_resource* resource, OperationOptions options) {
    return executeDbCommand(*this, std::move(sql), std::move(params), resource, std::move(options));
}

Task<DbRows> PostgreSqlPool::queryOnSlot(ConnectionSlot& slot, const std::pmr::string& sql, std::span<const DbValue> params, std::pmr::memory_resource* resource, const OperationTimeout& operationTimeout) {
    throwIfCancelled(slot);
    if (!slot.connected) {
        co_await connectUnlocked(slot, operationTimeout);
    }
    const OperationTimeout deadline = operationTimeout.constrainedBy(config_.queryTimeout);
    co_await sendQuery(slot, sql, params, deadline, false);

    auto output = DbResultAccess::makeResult(resource);
    bool retainedTupleResult = false;
    while (true) {
        co_await waitUntilResultReady(slot, deadline);
        PostgreSqlResultOwner result(PQgetResult(slot.connection));
        if (result.get() == nullptr) {
            break;
        }
        const auto status = PQresultStatus(result.get());
        if (!successfulResultStatus(status)) {
            auto error = postgreSqlError(
                *slot.connection,
                "PostgreSQL query",
                DbError::Code::kStatementFailed,
                result.get());
            result.reset();
            clearRemainingPostgreSqlResults(slot.connection);
            throw error;
        }

        if (status == PGRES_TUPLES_OK) {
            if (retainedTupleResult) {
                result.reset();
                throw DbError(
                    DbError::Code::kProtocolError,
                    DbDriver::kPostgreSql,
                    "PostgreSQL returned multiple tuple results");
            }
            materializeBorrowedResult(output, *result, resource);
            DbResultAccess::ownRawResult(output, result.release(), &freePostgreSqlResult);
            retainedTupleResult = true;
        }
    }
    if (!retainedTupleResult) {
        throw std::invalid_argument("query() requires row-producing SQL");
    }
    co_return output;
}

Task<DbExecResult> PostgreSqlPool::executeOnSlot(ConnectionSlot& slot, const std::pmr::string& sql, std::span<const DbValue> params, std::pmr::memory_resource*, const OperationTimeout& operationTimeout) {
    throwIfCancelled(slot);
    if (!slot.connected) {
        co_await connectUnlocked(slot, operationTimeout);
    }
    const OperationTimeout deadline = operationTimeout.constrainedBy(config_.queryTimeout);
    co_await sendQuery(slot, sql, params, deadline, false);

    std::uint64_t affectedRows = 0;
    while (true) {
        co_await waitUntilResultReady(slot, deadline);
        PostgreSqlResultOwner result(PQgetResult(slot.connection));
        if (result.get() == nullptr) {
            break;
        }
        const auto status = PQresultStatus(result.get());
        if (!successfulResultStatus(status)) {
            auto error = postgreSqlError(
                *slot.connection,
                "PostgreSQL execute",
                DbError::Code::kStatementFailed,
                result.get());
            result.reset();
            clearRemainingPostgreSqlResults(slot.connection);
            throw error;
        }
        if (status == PGRES_TUPLES_OK) {
            result.reset();
            clearRemainingPostgreSqlResults(slot.connection);
            throw std::invalid_argument("execute() does not accept row-producing SQL");
        }
        affectedRows = postgreSqlAffectedRows(*result);
    }
    co_return DbResultAccess::makeExecResult(affectedRows);
}

Task<DbStreamResult> PostgreSqlPool::stream(std::pmr::string sql, std::pmr::vector<DbValue> params, std::pmr::memory_resource* resource, OperationOptions options) {
    if (sql.empty()) {
        throw std::invalid_argument("SQL must not be empty");
    }
    const OperationTimeout operationTimeout(options.timeout);
    const auto slotIndex = co_await acquireSlot(operationTimeout, options.stopToken);
    DbSlotCancellationGuard cancellation(*this, slotIndex, options.stopToken);
    try {
        auto& slot = slots_[slotIndex];
        throwIfCancelled(slot);
        if (!slot.connected) {
            co_await connectUnlocked(slot, operationTimeout);
        }
        const OperationTimeout deadline = operationTimeout.constrainedBy(config_.queryTimeout);
        co_await sendQuery(slot, sql, std::span<const DbValue>(params), deadline, true);
        co_return DbStreamResult(DbPoolRef{this}, slotIndex, nullptr, resource, std::move(options));
    } catch (...) {
        closeSlot(slots_[slotIndex]);
        cancellation.finish();
        releaseSlot(slotIndex);
        throw;
    }
}

Task<std::optional<DbRow>> PostgreSqlPool::readStreamRow(std::size_t slotIndex, void*, std::pmr::memory_resource* resource, const OperationOptions& options) {
    if (slotIndex >= slots_.size()) {
        co_return std::nullopt;
    }
    auto& slot = slots_[slotIndex];
    DbSlotCancellationGuard cancellation(*this, slotIndex, options.stopToken);
    bool slotReleased = false;
    try {
        throwIfCancelled(slot);
        const OperationTimeout deadline = OperationTimeout(options.timeout).constrainedBy(config_.queryTimeout);
        co_await waitUntilResultReady(slot, deadline);
        PostgreSqlResultOwner result(PQgetResult(slot.connection));
        if (result.get() == nullptr) {
            slotReleased = true;
            cancellation.finish();
            releaseSlot(slotIndex);
            co_return std::nullopt;
        }
        const auto status = PQresultStatus(result.get());
        if (status == PGRES_SINGLE_TUPLE) {
            auto row = materializeOwnedSingleRow(*result, resource);
            co_return row;
        }
        if (status == PGRES_TUPLES_OK || status == PGRES_COMMAND_OK || status == PGRES_EMPTY_QUERY) {
            result.reset();
            while (true) {
                PostgreSqlResultOwner remaining(PQgetResult(slot.connection));
                if (remaining.get() == nullptr) {
                    break;
                }
                const auto remainingStatus = PQresultStatus(remaining.get());
                if (!successfulResultStatus(remainingStatus)) {
                    auto error = postgreSqlError(
                        *slot.connection,
                        "PostgreSQL stream",
                        DbError::Code::kStatementFailed,
                        remaining.get());
                    throw error;
                }
            }
            slotReleased = true;
            cancellation.finish();
            releaseSlot(slotIndex);
            if (status == PGRES_COMMAND_OK || status == PGRES_EMPTY_QUERY) {
                throw std::invalid_argument("queryStream() requires row-producing SQL");
            }
            co_return std::nullopt;
        }
        auto error = postgreSqlError(
            *slot.connection,
            "PostgreSQL stream",
            DbError::Code::kStatementFailed,
            result.get());
        throw error;
    } catch (...) {
        if (!slotReleased) {
            closeSlot(slot);
            cancellation.finish();
            releaseSlot(slotIndex);
        }
        throw;
    }
}

Task<void> PostgreSqlPool::closeStream(std::size_t slot, void*, std::pmr::memory_resource*, const OperationOptions& options) {
    if (slot < slots_.size()) {
        DbSlotCancellationGuard cancellation(*this, slot, options.stopToken);
        // Abandoning a libpq single-row result still requires draining the whole
        // command. Closing is bounded and keeps the worker non-blocking.
        closeSlot(slots_[slot]);
        cancellation.finish();
        releaseSlot(slot);
        if (options.stopToken.stopRequested()) {
            throw DbError(DbError::Code::kCancelled, DbDriver::kPostgreSql, "database operation cancelled");
        }
    }
    co_return;
}

void PostgreSqlPool::abortStream(std::size_t slot, void*) noexcept {
    if (slot >= slots_.size()) {
        return;
    }
    closeSlot(slots_[slot]);
    releaseSlot(slot);
}

Task<void> PostgreSqlPool::executeControl(ConnectionSlot& slot, std::string_view sql, std::pmr::memory_resource* resource, const OperationTimeout& operationTimeout) {
    const std::pmr::string command(sql, resource_);
    (void)co_await executeOnSlot(slot, command, {}, resource, operationTimeout);
}

Task<DbRows> PostgreSqlPool::queryOnTransactionSlot(std::size_t slot, std::pmr::string sql, std::pmr::vector<DbValue> params, std::pmr::memory_resource* resource, const OperationOptions& options) {
    return queryOnDbTransactionSlot(*this, slot, std::move(sql), std::move(params), resource, options);
}

Task<DbExecResult> PostgreSqlPool::executeOnTransactionSlot(std::size_t slot, std::pmr::string sql, std::pmr::vector<DbValue> params, std::pmr::memory_resource* resource, const OperationOptions& options) {
    return executeOnDbTransactionSlot(*this, slot, std::move(sql), std::move(params), resource, options);
}

Task<DbTransaction> PostgreSqlPool::beginTransaction(std::pmr::memory_resource* resource, OperationOptions options) {
    const OperationTimeout operationTimeout(options.timeout);
    const auto slotIndex = co_await acquireSlot(operationTimeout, options.stopToken);
    DbSlotCancellationGuard cancellation(*this, slotIndex, options.stopToken);
    try {
        auto& slot = slots_[slotIndex];
        throwIfCancelled(slot);
        if (!slot.connected) {
            co_await connectUnlocked(slot, operationTimeout);
        }
        co_await executeControl(slot, "BEGIN", resource, operationTimeout);
    } catch (...) {
        closeSlot(slots_[slotIndex]);
        cancellation.finish();
        releaseSlot(slotIndex);
        throw;
    }
    co_return DbTransaction(DbPoolRef{this}, slotIndex, resource, std::move(options));
}

Task<void> PostgreSqlPool::commitTransaction(std::size_t slot, std::pmr::memory_resource* resource, const OperationOptions& options) {
    return finishDbTransaction(*this, slot, "COMMIT", resource, options);
}

Task<void> PostgreSqlPool::rollbackTransaction(std::size_t slot, std::pmr::memory_resource* resource, const OperationOptions& options) {
    return finishDbTransaction(*this, slot, "ROLLBACK", resource, options);
}

void PostgreSqlPool::abortTransaction(std::size_t slot) noexcept {
    if (slot >= slots_.size()) {
        return;
    }
    closeSlot(slots_[slot]);
    releaseSlot(slot);
}

}  // namespace ruvia::detail
