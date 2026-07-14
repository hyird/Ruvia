#pragma once

#include "ruvia/web/db/DbMigration.h"

#include <cstddef>
#include <span>
#include <stdexcept>
#include <string_view>

namespace ruvia::detail {

// A migration table name is a SQL identifier that cannot be parameterized, so it
// is restricted to the selected backend's identifier byte limit and
// [A-Za-z0-9_] before being quoted -- the sole defense against SQL injection
// via a misconfigured table name.
[[nodiscard]] inline bool isValidMigrationTableName(
    std::string_view name,
    DbDriver driver) noexcept {
    const auto maxBytes = driver == DbDriver::kPostgreSql ? 63U : 64U;
    if (name.empty() || name.size() > maxBytes) {
        return false;
    }
    for (const auto ch : name) {
        const auto c = static_cast<unsigned char>(ch);
        if ((c >= 'a' && c <= 'z') ||
            (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') ||
            c == '_') {
            continue;
        }
        return false;
    }
    return true;
}

// Validates a developer-supplied migration list before it is applied: every id
// must be non-empty, at most 190 bytes (the indexed schema column width), have
// non-empty SQL, and be unique -- a duplicate id would run the wrong migration.
inline void validateMigrationList(std::span<const DbMigration> migrations) {
    for (std::size_t i = 0; i < migrations.size(); ++i) {
        const auto& migration = migrations[i];
        if (migration.id.empty()) {
            throw std::invalid_argument("database migration id must not be empty");
        }
        if (migration.id.size() > 190) {
            throw std::invalid_argument("database migration id must not exceed 190 bytes");
        }
        if (migration.sql.empty()) {
            throw std::invalid_argument("database migration SQL must not be empty");
        }
        for (std::size_t j = i + 1; j < migrations.size(); ++j) {
            if (migrations[j].id == migration.id) {
                throw std::invalid_argument("database migration ids must be unique");
            }
        }
    }
}

}  // namespace ruvia::detail
