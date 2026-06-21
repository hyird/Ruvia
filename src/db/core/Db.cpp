#include "../DbInternal.h"
#include "DbPoolDeadline.h"
#include "DbSql.h"
#include "DbUtils.h"

#include <mysql/mysql.h>

#include <cstdint>
#include <stdexcept>
#include <utility>

namespace ruvia {

Task<QueryResult> detail::MariaDbPool::execute(
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
            std::string_view(sql.data(), sql.size()),
            std::span<const DbValue>(params.data(), params.size()),
            resource);
    } catch (...) {
        closeSlot(slots_[slotIndex]);
        throw;
    }
}

Task<DbStreamResult> detail::MariaDbPool::stream(
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

        OperationDeadline deadline(config_.queryTimeout);
        std::pmr::string interpolatedSql(detail::resolveDbResource(resource));
        if (!params.empty()) {
            interpolatedSql = interpolateSql(
                *slot.connection,
                std::string_view(sql.data(), sql.size()),
                std::span<const DbValue>(params.data(), params.size()),
                resource);
            sql = std::move(interpolatedSql);
        }

        co_await runMysqlQuery(slot, std::string_view(sql.data(), sql.size()), deadline);
        auto* rawResult = mysql_use_result(slot.connection);
        if (rawResult == nullptr) {
            if (mysql_field_count(slot.connection) != 0) {
                throw mysqlError(*slot.connection, "mysql_use_result");
            }
            releaseSlot(slotIndex);
            co_return DbStreamResult(*this, slotIndex, nullptr, resource);
        }

        co_return DbStreamResult(*this, slotIndex, rawResult, resource);
    } catch (...) {
        closeSlot(slots_[slotIndex]);
        releaseSlot(slotIndex);
        throw;
    }
}

Task<std::optional<DbRow>> detail::MariaDbPool::readStreamRow(
    std::size_t slot,
    void* result,
    std::pmr::memory_resource* resource) {
    if (slot >= slots_.size() || result == nullptr) {
        co_return std::nullopt;
    }

    auto* rawResult = static_cast<MYSQL_RES*>(result);
    try {
        OperationDeadline deadline(config_.queryTimeout);
        MYSQL_ROW row = nullptr;
        int status = mysql_fetch_row_start(&row, rawResult);
        while (status != 0) {
            status = mysql_fetch_row_cont(
                &row,
                rawResult,
                co_await waitForMysql(slots_[slot], status, deadline));
        }

        if (row == nullptr) {
            co_await closeStream(slot, result, resource);
            co_return std::nullopt;
        }

        const auto fieldCount = static_cast<std::size_t>(mysql_num_fields(rawResult));
        const auto* lengths = mysql_fetch_lengths(rawResult);
        DbRow outputRow(resource);
        outputRow.ownedFields_.reserve(fieldCount);
        for (std::size_t i = 0; i < fieldCount; ++i) {
            if (row[i] == nullptr) {
                outputRow.ownedFields_.emplace_back(nullptr, resource);
                continue;
            }
            outputRow.ownedFields_.emplace_back(std::string_view(row[i], lengths[i]), resource);
        }
        outputRow.refreshView();
        co_return outputRow;
    } catch (...) {
        closeSlot(slots_[slot]);
        releaseSlot(slot);
        throw;
    }
}

Task<void> detail::MariaDbPool::closeStream(
    std::size_t slot,
    void* result,
    std::pmr::memory_resource*) {
    if (slot >= slots_.size() || result == nullptr) {
        co_return;
    }

    auto* rawResult = static_cast<MYSQL_RES*>(result);
    try {
        OperationDeadline deadline(config_.queryTimeout);
        int status = mysql_free_result_start(rawResult);
        while (status != 0) {
            status = mysql_free_result_cont(
                rawResult,
                co_await waitForMysql(slots_[slot], status, deadline));
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

Task<QueryResult> detail::MariaDbPool::executeOnTransactionSlot(
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
            std::string_view(sql.data(), sql.size()),
            std::span<const DbValue>(params.data(), params.size()),
            resource);
    } catch (...) {
        closeSlot(slots_[slot]);
        releaseSlot(slot);
        throw;
    }
}

Task<QueryResult> detail::MariaDbPool::executeOnSlot(
    ConnectionSlot& slot,
    std::string_view sql,
    std::span<const DbValue> params,
    std::pmr::memory_resource* resource) {
    if (sql.empty()) {
        throw std::invalid_argument("SQL must not be empty");
    }
    if (!slot.connected) {
        co_await connectUnlocked(slot);
    }
    OperationDeadline deadline(config_.queryTimeout);

    std::pmr::string interpolatedSql(detail::resolveDbResource(resource));
    if (!params.empty()) {
        interpolatedSql = interpolateSql(
            *slot.connection,
            sql,
            params,
            resource);
        sql = std::string_view(interpolatedSql.data(), interpolatedSql.size());
    }

    auto& connection = *slot.connection;
    co_await runMysqlQuery(slot, sql, deadline);

    QueryResult result(resource);
    result.affectedRows_ = static_cast<std::uint64_t>(mysql_affected_rows(&connection));
    result.lastInsertId_ = static_cast<std::uint64_t>(mysql_insert_id(&connection));

    auto* rawResult = co_await storeMysqlResult(slot, deadline);
    if (rawResult == nullptr) {
        if (mysql_field_count(&connection) != 0) {
            throw mysqlError(connection, "mysql_store_result");
        }
        co_return result;
    }

    result.rawResult_ = rawResult;
    result.releaseRawResult_ = &freeStoredResult;
    const auto fieldCount = static_cast<std::size_t>(mysql_num_fields(rawResult));
    const auto rowCount = static_cast<std::size_t>(mysql_num_rows(rawResult));
    result.rows_.reserve(rowCount);
    result.fields_.reserve(rowCount * fieldCount);
    while (auto* row = mysql_fetch_row(rawResult)) {
        const auto* lengths = mysql_fetch_lengths(rawResult);
        const auto rowStart = result.fields_.size();
        for (std::size_t i = 0; i < fieldCount; ++i) {
            if (row[i] == nullptr) {
                result.fields_.emplace_back(nullptr, resource);
                continue;
            }
            result.fields_.push_back(DbField::borrowed(std::string_view(row[i], lengths[i]), resource));
        }
        result.rows_.push_back(DbRow(result.fields_.data() + rowStart, fieldCount, resource));
    }

    co_return result;
}

Task<void> detail::MariaDbPool::executeControl(
    ConnectionSlot& slot,
    std::string_view sql,
    std::pmr::memory_resource* resource) {
    (void)co_await executeOnSlot(slot, sql, std::span<const DbValue>(), resource);
    co_return;
}

Task<DbTransaction> detail::MariaDbPool::beginTransaction(
    std::pmr::memory_resource* resource,
    RequestMemory* requestMemory) {
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

    co_return DbTransaction(*this, slotIndex, resource, requestMemory);
}

Task<void> detail::MariaDbPool::commitTransaction(std::size_t slot, std::pmr::memory_resource* resource) {
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

Task<void> detail::MariaDbPool::rollbackTransaction(std::size_t slot, std::pmr::memory_resource* resource) {
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

void detail::MariaDbPool::abortTransaction(std::size_t slot) noexcept {
    if (slot >= slots_.size()) {
        return;
    }

    closeSlot(slots_[slot]);
    releaseSlot(slot);
}

void detail::MariaDbPool::finishTransaction(std::size_t slot) noexcept {
    releaseSlot(slot);
}

}  // namespace ruvia
