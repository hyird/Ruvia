#include "ruvia/web/detail/db/DbInternal.h"

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

[[nodiscard]] bool successfulResultStatus(ExecStatusType status) noexcept {
    return status == PGRES_COMMAND_OK || status == PGRES_TUPLES_OK ||
        status == PGRES_EMPTY_QUERY;
}

void materializeBorrowedResult(
    QueryResult& output,
    PGresult& result,
    std::pmr::memory_resource* resource) {
    const auto rowCount = static_cast<std::size_t>(PQntuples(&result));
    const auto fieldCount = static_cast<std::size_t>(PQnfields(&result));
    auto& rows = DbResultAccess::rows(output);
    auto& fields = DbResultAccess::fields(output);
    rows.reserve(rowCount);
    fields.reserve(rowCount * fieldCount);
    for (std::size_t row = 0; row < rowCount; ++row) {
        const auto rowStart = fields.size();
        for (std::size_t field = 0; field < fieldCount; ++field) {
            const auto rowIndex = static_cast<int>(row);
            const auto fieldIndex = static_cast<int>(field);
            if (PQgetisnull(&result, rowIndex, fieldIndex) != 0) {
                fields.push_back(DbResultAccess::nullField(resource));
                continue;
            }
            fields.push_back(DbResultAccess::borrowedField(
                std::string_view(
                    PQgetvalue(&result, rowIndex, fieldIndex),
                    static_cast<std::size_t>(PQgetlength(&result, rowIndex, fieldIndex))),
                resource));
        }
        rows.push_back(DbResultAccess::borrowedRow(
            fields.data() + rowStart, fieldCount, resource));
    }
}

[[nodiscard]] DbRow materializeOwnedSingleRow(
    PGresult& result,
    std::pmr::memory_resource* resource) {
    auto row = DbResultAccess::ownedRow(resource);
    auto& fields = DbResultAccess::ownedFields(row);
    const auto fieldCount = static_cast<std::size_t>(PQnfields(&result));
    fields.reserve(fieldCount);
    for (std::size_t field = 0; field < fieldCount; ++field) {
        const auto fieldIndex = static_cast<int>(field);
        if (PQgetisnull(&result, 0, fieldIndex) != 0) {
            fields.push_back(DbResultAccess::nullField(resource));
            continue;
        }
        fields.push_back(DbResultAccess::ownedField(
            std::string_view(
                PQgetvalue(&result, 0, fieldIndex),
                static_cast<std::size_t>(PQgetlength(&result, 0, fieldIndex))),
            resource));
    }
    return row;
}

}  // namespace

Task<QueryResult> PostgreSqlPool::execute(
    std::pmr::string sql,
    std::pmr::vector<DbValue> params,
    std::pmr::memory_resource* resource) {
    if (sql.empty()) {
        throw std::invalid_argument("SQL must not be empty");
    }
    const auto slotIndex = co_await acquireSlot();
    SlotGuard guard(*this, slotIndex);
    try {
        co_return co_await executeOnSlot(
            slots_[slotIndex],
            sql,
            std::span<const DbValue>(params),
            resource);
    } catch (...) {
        closeSlot(slots_[slotIndex]);
        throw;
    }
}

Task<QueryResult> PostgreSqlPool::executeOnSlot(
    ConnectionSlot& slot,
    const std::pmr::string& sql,
    std::span<const DbValue> params,
    std::pmr::memory_resource* resource) {
    if (!slot.connected) {
        co_await connectUnlocked(slot);
    }
    OperationTimeout deadline(config_.queryTimeout);
    co_await sendQuery(slot, sql, params, deadline, false);

    auto output = DbResultAccess::makeResult(resource);
    bool retainedTupleResult = false;
    while (true) {
        co_await waitUntilResultReady(slot, deadline);
        auto* result = PQgetResult(slot.connection);
        if (result == nullptr) {
            break;
        }
        const auto status = PQresultStatus(result);
        if (!successfulResultStatus(status)) {
            auto error = postgreSqlError(*slot.connection, "PostgreSQL query", result);
            PQclear(result);
            while (auto* remaining = PQgetResult(slot.connection)) {
                PQclear(remaining);
            }
            throw error;
        }

        DbResultAccess::setAffectedRows(output, postgreSqlAffectedRows(*result));
        if (status == PGRES_TUPLES_OK) {
            if (retainedTupleResult) {
                PQclear(result);
                throw std::runtime_error("PostgreSQL returned multiple tuple results");
            }
            materializeBorrowedResult(output, *result, resource);
            DbResultAccess::ownRawResult(output, result, &freePostgreSqlResult);
            retainedTupleResult = true;
        } else {
            PQclear(result);
        }
    }
    co_return output;
}

Task<DbStreamResult> PostgreSqlPool::stream(
    std::pmr::string sql,
    std::pmr::vector<DbValue> params,
    std::pmr::memory_resource* resource) {
    if (sql.empty()) {
        throw std::invalid_argument("SQL must not be empty");
    }
    const auto slotIndex = co_await acquireSlot();
    try {
        auto& slot = slots_[slotIndex];
        if (!slot.connected) {
            co_await connectUnlocked(slot);
        }
        OperationTimeout deadline(config_.queryTimeout);
        co_await sendQuery(
            slot,
            sql,
            std::span<const DbValue>(params),
            deadline,
            true);
        co_return DbStreamResult(DbPoolRef{this}, slotIndex, nullptr, resource);
    } catch (...) {
        closeSlot(slots_[slotIndex]);
        releaseSlot(slotIndex);
        throw;
    }
}

Task<std::optional<DbRow>> PostgreSqlPool::readStreamRow(
    std::size_t slotIndex,
    void*,
    std::pmr::memory_resource* resource) {
    if (slotIndex >= slots_.size()) {
        co_return std::nullopt;
    }
    auto& slot = slots_[slotIndex];
    try {
        OperationTimeout deadline(config_.queryTimeout);
        co_await waitUntilResultReady(slot, deadline);
        auto* result = PQgetResult(slot.connection);
        if (result == nullptr) {
            releaseSlot(slotIndex);
            co_return std::nullopt;
        }
        const auto status = PQresultStatus(result);
        if (status == PGRES_SINGLE_TUPLE) {
            auto row = materializeOwnedSingleRow(*result, resource);
            PQclear(result);
            co_return row;
        }
        if (status == PGRES_TUPLES_OK || status == PGRES_COMMAND_OK ||
            status == PGRES_EMPTY_QUERY) {
            PQclear(result);
            while (auto* remaining = PQgetResult(slot.connection)) {
                const auto remainingStatus = PQresultStatus(remaining);
                if (!successfulResultStatus(remainingStatus)) {
                    auto error = postgreSqlError(
                        *slot.connection, "PostgreSQL stream", remaining);
                    PQclear(remaining);
                    throw error;
                }
                PQclear(remaining);
            }
            releaseSlot(slotIndex);
            co_return std::nullopt;
        }
        auto error = postgreSqlError(*slot.connection, "PostgreSQL stream", result);
        PQclear(result);
        throw error;
    } catch (...) {
        closeSlot(slot);
        releaseSlot(slotIndex);
        throw;
    }
}

Task<void> PostgreSqlPool::closeStream(
    std::size_t slot,
    void*,
    std::pmr::memory_resource*) {
    if (slot < slots_.size()) {
        // Abandoning a libpq single-row result still requires draining the whole
        // command. Closing is bounded and keeps the worker non-blocking.
        closeSlot(slots_[slot]);
        releaseSlot(slot);
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

Task<void> PostgreSqlPool::executeControl(
    ConnectionSlot& slot,
    std::string_view sql,
    std::pmr::memory_resource* resource) {
    const std::pmr::string command(sql, resource_);
    (void)co_await executeOnSlot(slot, command, {}, resource);
}

Task<QueryResult> PostgreSqlPool::executeOnTransactionSlot(
    std::size_t slot,
    std::pmr::string sql,
    std::pmr::vector<DbValue> params,
    std::pmr::memory_resource* resource) {
    if (slot >= slots_.size()) {
        throw std::logic_error("database transaction slot is invalid");
    }
    try {
        co_return co_await executeOnSlot(
            slots_[slot],
            sql,
            std::span<const DbValue>(params),
            resource);
    } catch (...) {
        closeSlot(slots_[slot]);
        releaseSlot(slot);
        throw;
    }
}

Task<DbTransaction> PostgreSqlPool::beginTransaction(
    std::pmr::memory_resource* resource) {
    const auto slotIndex = co_await acquireSlot();
    try {
        auto& slot = slots_[slotIndex];
        if (!slot.connected) {
            co_await connectUnlocked(slot);
        }
        co_await executeControl(slot, "BEGIN", resource);
    } catch (...) {
        closeSlot(slots_[slotIndex]);
        releaseSlot(slotIndex);
        throw;
    }
    co_return DbTransaction(DbPoolRef{this}, slotIndex, resource);
}

Task<void> PostgreSqlPool::commitTransaction(
    std::size_t slot,
    std::pmr::memory_resource* resource) {
    if (slot >= slots_.size()) {
        throw std::logic_error("database transaction slot is invalid");
    }
    try {
        co_await executeControl(slots_[slot], "COMMIT", resource);
    } catch (...) {
        closeSlot(slots_[slot]);
        releaseSlot(slot);
        throw;
    }
    releaseSlot(slot);
}

Task<void> PostgreSqlPool::rollbackTransaction(
    std::size_t slot,
    std::pmr::memory_resource* resource) {
    if (slot >= slots_.size()) {
        throw std::logic_error("database transaction slot is invalid");
    }
    try {
        co_await executeControl(slots_[slot], "ROLLBACK", resource);
    } catch (...) {
        closeSlot(slots_[slot]);
        releaseSlot(slot);
        throw;
    }
    releaseSlot(slot);
}

void PostgreSqlPool::abortTransaction(std::size_t slot) noexcept {
    if (slot >= slots_.size()) {
        return;
    }
    closeSlot(slots_[slot]);
    releaseSlot(slot);
}

}  // namespace ruvia::detail
