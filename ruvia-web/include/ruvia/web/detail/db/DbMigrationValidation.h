#pragma once

#include "ruvia/web/db/DbMigration.h"
#include "ruvia/web/detail/db/DbSqlScan.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string_view>

namespace ruvia::detail {

// PostgreSQL's lock_timeout is serialized as an integer number of
// milliseconds.  Do not duration_cast() an arbitrary seconds value: the
// seconds-to-milliseconds multiplication can overflow before the result is
// handed to the backend.  This helper is deliberately checked rather than
// saturating; silently making an enormous configured timeout finite changes
// the migration lock contract.
[[nodiscard]] inline std::uint64_t postgresLockTimeoutMilliseconds(std::chrono::seconds timeout) {
    if (timeout.count() <= 0) {
        throw std::invalid_argument("database migration lock timeout must be greater than zero");
    }
    constexpr auto kMillisecondsPerSecond = std::int64_t{1000};
    constexpr auto kMaxSeconds = std::chrono::milliseconds::max().count() / kMillisecondsPerSecond;
    if (timeout.count() > kMaxSeconds) {
        throw std::invalid_argument("database migration lock timeout cannot be represented as PostgreSQL milliseconds");
    }
    return static_cast<std::uint64_t>(timeout.count()) * static_cast<std::uint64_t>(kMillisecondsPerSecond);
}

// A migration table name is a SQL identifier that cannot be parameterized, so it
// is restricted to the selected backend's identifier byte limit and
// [A-Za-z0-9_] before being quoted -- the sole defense against SQL injection
// via a misconfigured table name.
[[nodiscard]] inline bool isValidMigrationTableName(std::string_view name, DbDriver driver) noexcept {
    const auto maxBytes = driver == DbDriver::kPostgreSql ? 63U : 64U;
    if (name.empty() || name.size() > maxBytes) {
        return false;
    }
    for (const auto ch : name) {
        const auto c = static_cast<unsigned char>(ch);
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_') {
            continue;
        }
        return false;
    }
    return true;
}

[[nodiscard]] inline constexpr bool isSqlWhitespace(char character) noexcept {
    return character == ' ' || character == '\t' || character == '\r' || character == '\n';
}

// MariaDB's collations are PAD SPACE, including the binary one the migrations
// table pins, so "v1" and "v1 " are one id there and two everywhere else. An id
// wrapped in whitespace is a typo in every case that matters, so it is refused
// rather than quietly folded.
[[nodiscard]] inline bool hasSurroundingWhitespace(std::string_view id) noexcept {
    return !id.empty() && (isSqlWhitespace(id.front()) || isSqlWhitespace(id.back()));
}

// Two migration ids are the same id to the schema table if they differ only in
// ASCII letter case. The applied-migration lookup compares them with the
// column's collation, and MariaDB's default (utf8mb4_general_ci) is
// case-insensitive: "v1" and "V1" collide there while PostgreSQL keeps them
// apart, so one list would produce two different schemas. New tables pin a
// binary collation, but a table created before that still compares loosely, so
// the ambiguity is refused at the source instead.
[[nodiscard]] inline bool migrationIdsCollide(std::string_view left, std::string_view right) noexcept {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t i = 0; i < left.size(); ++i) {
        auto a = static_cast<unsigned char>(left[i]);
        auto b = static_cast<unsigned char>(right[i]);
        if (a >= 'A' && a <= 'Z') {
            a = static_cast<unsigned char>(a - 'A' + 'a');
        }
        if (b >= 'A' && b <= 'Z') {
            b = static_cast<unsigned char>(b - 'A' + 'a');
        }
        if (a != b) {
            return false;
        }
    }
    return true;
}

// A migration is one statement. Both backends enforce that anyway -- libpq's
// extended protocol rejects multiple commands outright and the MariaDB
// connection never enables CLIENT_MULTI_STATEMENTS -- but they report it as a
// backend syntax error pointing at the second statement, which reads like the
// SQL is wrong rather than the packaging. A trailing separator is accepted by
// both, so only a separator with statement text after it is refused.
[[nodiscard]] inline bool hasTrailingSqlOnly(std::string_view sql, std::size_t after) noexcept {
    for (auto index = after; index < sql.size(); ++index) {
        if (!isSqlWhitespace(sql[index])) {
            return false;
        }
    }
    return true;
}

// Validates a developer-supplied migration list before it is applied: every id
// must be non-empty, at most 190 bytes (the indexed schema column width), have
// non-empty single-statement SQL, and be unique -- a duplicate id would run the
// wrong migration.
inline void validateMigrationList(std::span<const DbMigration> migrations) {
    for (std::size_t i = 0; i < migrations.size(); ++i) {
        const auto& migration = migrations[i];
        if (migration.id().empty()) {
            throw std::invalid_argument("database migration id must not be empty");
        }
        if (migration.id().size() > 190) {
            throw std::invalid_argument("database migration id must not exceed 190 bytes");
        }
        if (hasSurroundingWhitespace(migration.id())) {
            throw std::invalid_argument("database migration id must not begin or end with whitespace");
        }
        if (migration.sql().empty()) {
            throw std::invalid_argument("database migration SQL must not be empty");
        }
        const auto separator = findSqlSyntaxByte(migration.sql(), ';');
        if (separator != std::string_view::npos && !hasTrailingSqlOnly(migration.sql(), separator + 1)) {
            throw std::invalid_argument("database migration must contain exactly one SQL statement");
        }
        for (std::size_t j = i + 1; j < migrations.size(); ++j) {
            if (migrationIdsCollide(migrations[j].id(), migration.id())) {
                throw std::invalid_argument("database migration ids must be unique, including case");
            }
        }
    }
}

}  // namespace ruvia::detail
