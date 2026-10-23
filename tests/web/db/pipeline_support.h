#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include "ruvia/core/task.h"
#include "ruvia/web/db/db_handle.h"
#include "ruvia/web/db/db_types.h"

#include "backend_client_fixture.h"

// Callers shared by the pipelining tests of every driver.
namespace pipeline_test {

using peer_type = ruvia::test::backend_startup_peer;

// What one concurrent caller observed. Failures are recorded rather than
// thrown so every caller in a scope finishes on its own.
struct outcome final {
    std::string value_;
    std::uint64_t affected_rows_{0};
    std::optional<std::uint64_t> insert_id_;
    std::optional<ruvia::db_error::code_type> error_;
    std::string sql_state_;
    // The statement was refused for its shape (rows where none were wanted,
    // or none where rows were).
    bool refused_{false};
};

inline void record_failure(const ruvia::db_error& error, outcome& result) {
    result.error_ = error.code();
    result.sql_state_ = error.sql_state().value_or("");
}

inline ruvia::task<void> query(ruvia::db_handle handle, std::string_view sql, outcome& result) {
    try {
        const ruvia::db_rows rows = co_await handle.query(sql);
        result.value_ = std::string(rows.front()["v"].as<std::string_view>().value_or(""));
    } catch (const ruvia::db_error& error) {
        record_failure(error, result);
    } catch (const std::invalid_argument&) {
        result.refused_ = true;
    }
}

inline ruvia::task<void> query_id(ruvia::db_handle handle, std::string_view sql, std::int64_t id, outcome& result) {
    try {
        const ruvia::db_rows rows = co_await handle.query(sql, id);
        result.value_ = std::string(rows.front()["v"].as<std::string_view>().value_or(""));
    } catch (const ruvia::db_error& error) {
        record_failure(error, result);
    }
}

inline ruvia::task<void> execute(ruvia::db_handle handle, std::string_view sql, outcome& result) {
    try {
        const auto executed = co_await handle.execute(sql);
        result.affected_rows_ = executed.affected_rows();
        result.insert_id_ = executed.last_insert_id();
    } catch (const ruvia::db_error& error) {
        record_failure(error, result);
    } catch (const std::invalid_argument&) {
        result.refused_ = true;
    }
}

inline ruvia::task<void> begin_and_commit(ruvia::db_handle handle, outcome& result) {
    try {
        auto transaction = co_await handle.begin_transaction();
        co_await transaction.commit();
        result.value_ = "committed";
    } catch (const ruvia::db_error& error) {
        record_failure(error, result);
    }
}

}  // namespace pipeline_test
