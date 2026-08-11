#include "ruvia/web/detail/db/DbRegistry.h"
#include "ruvia/core/detail/io/OperationDeadline.h"
#include "ruvia/web/detail/db/DbSql.h"
#include "ruvia/web/detail/db/DbUtils.h"
#include "ruvia/web/detail/db/DbResultAccess.h"

#include <mysql/mysql.h>

#include <cstdint>
#include <stdexcept>
#include <utility>

namespace ruvia {

Task<DbRows> detail::MariaDbPool::query(std::pmr::string sql, std::pmr::vector<DbValue> params, std::pmr::memory_resource* resource) {
    return executeDbQuery(*this, std::move(sql), std::move(params), resource);
}

Task<DbExecResult> detail::MariaDbPool::execute(std::pmr::string sql, std::pmr::vector<DbValue> params, std::pmr::memory_resource* resource) {
    return executeDbCommand(*this, std::move(sql), std::move(params), resource);
}

Task<DbStreamResult> detail::MariaDbPool::stream(std::pmr::string sql, std::pmr::vector<DbValue> params, std::pmr::memory_resource* resource) {
    if (sql.empty()) {
        throw std::invalid_argument("SQL must not be empty");
    }

    const auto slotIndex = co_await acquireSlot();
    bool slotReleased = false;
    try {
        auto& slot = slots_[slotIndex];
        if (!slot.connected) {
            co_await connectUnlocked(slot);
        }

        OperationTimeout deadline(config_.queryTimeout);
        std::pmr::string interpolatedSql(detail::pmrResourceOrDefault(resource));
        if (!params.empty()) {
            interpolatedSql = interpolateSql(*slot.connection, std::string_view(sql), std::span<const DbValue>(params), resource);
            sql = std::move(interpolatedSql);
        }

        co_await runMysqlQuery(slot, std::string_view(sql), deadline);
        auto* rawResult = mysql_use_result(slot.connection);
        if (rawResult == nullptr) {
            if (mysql_field_count(slot.connection) != 0) {
                throw mysqlError(*slot.connection, "mysql_use_result");
            }
            releaseSlot(slotIndex);
            slotReleased = true;
            throw std::invalid_argument("queryStream() requires row-producing SQL");
        }

        co_return DbStreamResult(DbPoolRef{this}, slotIndex, rawResult, resource);
    } catch (...) {
        if (!slotReleased) {
            closeSlot(slots_[slotIndex]);
            releaseSlot(slotIndex);
        }
        throw;
    }
}

Task<std::optional<DbRow>> detail::MariaDbPool::readStreamRow(std::size_t slot, void* result, std::pmr::memory_resource* resource) {
    if (slot >= slots_.size() || result == nullptr) {
        co_return std::nullopt;
    }

    auto* rawResult = static_cast<MYSQL_RES*>(result);
    try {
        OperationTimeout deadline(config_.queryTimeout);
        MYSQL_ROW row = nullptr;
        int status = mysql_fetch_row_start(&row, rawResult);
        while (status != 0) {
            status = mysql_fetch_row_cont(&row, rawResult, co_await waitForMysql(slots_[slot], status, deadline));
        }

        if (row != nullptr) {
            const auto fieldCount = static_cast<std::size_t>(mysql_num_fields(rawResult));
            const auto* lengths = mysql_fetch_lengths(rawResult);
            auto outputRow = DbResultAccess::ownedRow(resource);
            auto& outputFields = DbResultAccess::ownedFields(outputRow);
            outputFields.reserve(fieldCount);
            for (std::size_t i = 0; i < fieldCount; ++i) {
                if (row[i] == nullptr) {
                    outputFields.push_back(DbResultAccess::nullField(resource));
                    continue;
                }
                outputFields.push_back(DbResultAccess::ownedField(std::string_view(row[i], lengths[i]), resource));
            }
            co_return outputRow;
        }
    } catch (...) {
        closeSlot(slots_[slot]);
        releaseSlot(slot);
        throw;
    }

    // EOF is not a read failure. The close path owns slot release, including
    // its own failure path, so it must run outside the read-error guard.
    co_await closeStream(slot, result, resource);
    co_return std::nullopt;
}

Task<void> detail::MariaDbPool::closeStream(std::size_t slot, void* result, std::pmr::memory_resource*) {
    if (slot >= slots_.size() || result == nullptr) {
        co_return;
    }

    auto* rawResult = static_cast<MYSQL_RES*>(result);
    try {
        OperationTimeout deadline(config_.queryTimeout);
        int status = mysql_free_result_start(rawResult);
        while (status != 0) {
            status = mysql_free_result_cont(rawResult, co_await waitForMysql(slots_[slot], status, deadline));
        }
        releaseSlot(slot);
    } catch (...) {
        closeSlot(slots_[slot]);
        releaseSlot(slot);
        throw;
    }
}

void detail::MariaDbPool::abortStream(std::size_t slot, void*) noexcept {
    if (slot >= slots_.size()) {
        return;
    }
    closeSlot(slots_[slot]);
    releaseSlot(slot);
}

Task<DbRows> detail::MariaDbPool::queryOnTransactionSlot(std::size_t slot, std::pmr::string sql, std::pmr::vector<DbValue> params, std::pmr::memory_resource* resource) {
    return detail::queryOnDbTransactionSlot(*this, slot, std::move(sql), std::move(params), resource);
}

Task<DbExecResult> detail::MariaDbPool::executeOnTransactionSlot(std::size_t slot, std::pmr::string sql, std::pmr::vector<DbValue> params, std::pmr::memory_resource* resource) {
    return detail::executeOnDbTransactionSlot(*this, slot, std::move(sql), std::move(params), resource);
}

Task<DbRows> detail::MariaDbPool::queryOnSlot(ConnectionSlot& slot, std::string_view sql, std::span<const DbValue> params, std::pmr::memory_resource* resource) {
    if (sql.empty()) {
        throw std::invalid_argument("SQL must not be empty");
    }
    if (!slot.connected) {
        co_await connectUnlocked(slot);
    }
    OperationTimeout deadline(config_.queryTimeout);

    std::pmr::string interpolatedSql(detail::pmrResourceOrDefault(resource));
    if (!params.empty()) {
        interpolatedSql = interpolateSql(*slot.connection, sql, params, resource);
        sql = interpolatedSql;
    }

    auto& connection = *slot.connection;
    co_await runMysqlQuery(slot, sql, deadline);

    auto result = DbResultAccess::makeResult(resource);
    auto* rawResult = co_await storeMysqlResult(slot, deadline);
    if (rawResult == nullptr) {
        if (mysql_field_count(&connection) != 0) {
            throw mysqlError(connection, "mysql_store_result");
        }
        throw std::invalid_argument("query() requires row-producing SQL");
    }

    DbResultAccess::ownRawResult(result, rawResult, &freeStoredResult);
    const auto fieldCount = static_cast<std::size_t>(mysql_num_fields(rawResult));
    const auto rowCount = static_cast<std::size_t>(mysql_num_rows(rawResult));
    auto& resultRows = DbResultAccess::rows(result);
    auto& resultFields = DbResultAccess::fields(result);
    resultRows.reserve(rowCount);
    resultFields.reserve(rowCount * fieldCount);
    while (auto* row = mysql_fetch_row(rawResult)) {
        const auto* lengths = mysql_fetch_lengths(rawResult);
        const auto rowStart = resultFields.size();
        for (std::size_t i = 0; i < fieldCount; ++i) {
            if (row[i] == nullptr) {
                resultFields.push_back(DbResultAccess::nullField(resource));
                continue;
            }
            resultFields.push_back(DbResultAccess::borrowedField(std::string_view(row[i], lengths[i]), resource));
        }
        resultRows.push_back(DbResultAccess::borrowedRow(resultFields.data() + rowStart, fieldCount, resource));
    }

    co_return result;
}

Task<DbExecResult> detail::MariaDbPool::executeOnSlot(ConnectionSlot& slot, std::string_view sql, std::span<const DbValue> params, std::pmr::memory_resource* resource) {
    if (sql.empty()) {
        throw std::invalid_argument("SQL must not be empty");
    }
    if (!slot.connected) {
        co_await connectUnlocked(slot);
    }
    OperationTimeout deadline(config_.queryTimeout);

    std::pmr::string interpolatedSql(detail::pmrResourceOrDefault(resource));
    if (!params.empty()) {
        interpolatedSql = interpolateSql(*slot.connection, sql, params, resource);
        sql = interpolatedSql;
    }

    auto& connection = *slot.connection;
    co_await runMysqlQuery(slot, sql, deadline);
    const auto affectedRows = static_cast<std::uint64_t>(mysql_affected_rows(&connection));
    const auto insertId = static_cast<std::uint64_t>(mysql_insert_id(&connection));
    auto* rawResult = co_await storeMysqlResult(slot, deadline);
    if (rawResult != nullptr) {
        freeStoredResult(rawResult);
        throw std::invalid_argument("execute() does not accept row-producing SQL");
    }
    if (mysql_field_count(&connection) != 0) {
        throw mysqlError(connection, "mysql_store_result");
    }
    co_return DbResultAccess::makeExecResult(affectedRows, insertId == 0 ? std::nullopt : std::optional<std::uint64_t>(insertId));
}

Task<void> detail::MariaDbPool::executeControl(ConnectionSlot& slot, std::string_view sql, std::pmr::memory_resource* resource) {
    (void)co_await executeOnSlot(slot, sql, std::span<const DbValue>(), resource);
    co_return;
}

Task<DbTransaction> detail::MariaDbPool::beginTransaction(std::pmr::memory_resource* resource) {
    const auto slotIndex = co_await acquireSlot();
    try {
        auto& slot = slots_[slotIndex];
        if (!slot.connected) {
            co_await connectUnlocked(slot);
        }
        co_await executeControl(slot, "START TRANSACTION", resource);
    } catch (...) {
        closeSlot(slots_[slotIndex]);
        releaseSlot(slotIndex);
        throw;
    }

    co_return DbTransaction(DbPoolRef{this}, slotIndex, resource);
}

Task<void> detail::MariaDbPool::commitTransaction(std::size_t slot, std::pmr::memory_resource* resource) {
    return finishDbTransaction(*this, slot, "COMMIT", resource);
}

Task<void> detail::MariaDbPool::rollbackTransaction(std::size_t slot, std::pmr::memory_resource* resource) {
    return finishDbTransaction(*this, slot, "ROLLBACK", resource);
}

void detail::MariaDbPool::abortTransaction(std::size_t slot) noexcept {
    if (slot >= slots_.size()) {
        return;
    }

    closeSlot(slots_[slot]);
    releaseSlot(slot);
}

}  // namespace ruvia
