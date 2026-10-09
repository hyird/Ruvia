#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string_view>

#include "ruvia/web/db/db_migration.h"
#include "ruvia/web/detail/db/db_sql_scan.h"

namespace ruvia::detail {

// PostgreSQL's lock_timeout is serialized as an integer number of
// milliseconds.  Do not duration_cast() an arbitrary seconds value: the
// seconds-to-milliseconds multiplication can overflow before the result is
// handed to the backend.  This helper is deliberately checked rather than
// saturating; silently making an enormous configured timeout finite changes
// the migration lock contract.
[[nodiscard]] inline std::uint64_t postgres_lock_timeout_milliseconds(std::chrono::seconds timeout) {
    if (timeout.count() <= 0) {
        throw std::invalid_argument("database migration lock timeout must be greater than zero");
    }
    constexpr auto milliseconds_per_second = std::int64_t{1000};
    constexpr auto max_seconds = std::chrono::milliseconds::max().count() / milliseconds_per_second;
    if (timeout.count() > max_seconds) {
        throw std::invalid_argument(
            "database migration lock timeout cannot be represented as PostgreSQL milliseconds");
    }
    return static_cast<std::uint64_t>(timeout.count()) *
           static_cast<std::uint64_t>(milliseconds_per_second);
}

// A migration table name is a SQL identifier that cannot be parameterized, so it
// is restricted to the selected backend's identifier byte limit and
// [A-Za-z0-9_] before being quoted -- the sole defense against SQL injection
// via a misconfigured table name.
[[nodiscard]] inline bool is_valid_migration_table_name(
    std::string_view name, db_driver driver) noexcept {
    const auto max_bytes = driver == db_driver::postgresql ? 63U : 64U;
    if (name.empty() || name.size() > max_bytes) {
        return false;
    }
    for (const auto ch : name) {
        const auto c = static_cast<unsigned char>(ch);
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '_') {
            continue;
        }
        return false;
    }
    return true;
}

// MariaDB's collations are PAD SPACE, including the binary one the migrations
// table pins, so "v1" and "v1 " are one id there and two everywhere else. An id
// wrapped in whitespace is a typo in every case that matters, so it is refused
// rather than quietly folded.
[[nodiscard]] inline bool has_surrounding_whitespace(std::string_view id) noexcept {
    return !id.empty() && (is_sql_whitespace(id.front()) || is_sql_whitespace(id.back()));
}

// Two migration ids are the same id to the schema table if they differ only in
// ASCII letter case. The applied-migration lookup compares them with the
// column's collation, and MariaDB's default (utf8mb4_general_ci) is
// case-insensitive: "v1" and "V1" collide there while PostgreSQL keeps them
// apart, so one list would produce two different schemas. New tables pin a
// binary collation, but a table created before that still compares loosely, so
// the ambiguity is refused at the source instead.
[[nodiscard]] inline bool migration_ids_collide(
    std::string_view left, std::string_view right) noexcept {
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
[[nodiscard]] inline std::size_t skip_migration_trailing_comment(
    std::string_view sql, std::size_t index, db_driver driver) noexcept {
    if (index >= sql.size()) {
        return index;
    }
    if (driver == db_driver::mariadb && sql[index] == '#') {
        return skip_sql_line_comment(sql, index + 1, false);
    }
    if (driver == db_driver::mariadb
            ? is_mariadb_double_dash_comment(sql, index)
            : (sql[index] == '-' && index + 1 < sql.size() && sql[index + 1] == '-')) {
        return skip_sql_line_comment(sql, index + 2, driver == db_driver::postgresql);
    }
    if (sql[index] == '/' && index + 1 < sql.size() && sql[index + 1] == '*') {
        return driver == db_driver::postgresql ? skip_postgresql_block_comment(sql, index)
                                               : skip_sql_block_comment(sql, index);
    }
    return index;
}

[[nodiscard]] inline bool has_trailing_sql_only(
    std::string_view sql, std::size_t after, db_driver driver) noexcept {
    for (auto index = after; index < sql.size();) {
        if (!is_sql_whitespace(sql[index])) {
            const auto next_value = skip_migration_trailing_comment(sql, index, driver);
            if (next_value == index) {
                return false;
            }
            index = next_value;
            continue;
        }
        ++index;
    }
    return true;
}

[[nodiscard]] inline bool has_migration_statement_text(
    std::string_view sql, db_driver driver) noexcept {
    for (auto index = std::size_t{0}; index < sql.size();) {
        if (is_sql_whitespace(sql[index]) || sql[index] == ';') {
            ++index;
            continue;
        }
        const auto next_value = skip_migration_trailing_comment(sql, index, driver);
        if (next_value != index) {
            index = next_value;
            continue;
        }
        return true;
    }
    return false;
}

[[nodiscard]] inline std::size_t find_migration_statement_separator(
    std::string_view sql, db_driver driver) {
    switch (driver) {
        case db_driver::mariadb:
            return find_sql_syntax_byte(sql, ';');
        case db_driver::postgresql:
            return find_postgresql_syntax_byte(sql, ';');
        default:
            throw std::invalid_argument("database driver is invalid");
    }
}

// Validates a developer-supplied migration list before it is applied: every id
// must be non-empty, at most 190 bytes (the indexed schema column width), have
// non-empty single-statement SQL, and be unique -- a duplicate id would run the
// wrong migration.
inline void validate_migration_list(std::span<const db_migration> migrations, db_driver driver) {
    for (std::size_t i = 0; i < migrations.size(); ++i) {
        const auto& migration = migrations[i];
        switch (migration.atomicity()) {
            case db_migration_atomicity::transactional:
            case db_migration_atomicity::unwrapped:
                break;
            default:
                throw std::invalid_argument("database migration atomicity is invalid");
        }
        if (migration.id().empty()) {
            throw std::invalid_argument("database migration id must not be empty");
        }
        if (migration.id().size() > 190) {
            throw std::invalid_argument("database migration id must not exceed 190 bytes");
        }
        if (has_surrounding_whitespace(migration.id())) {
            throw std::invalid_argument(
                "database migration id must not begin or end with whitespace");
        }
        if (!has_migration_statement_text(migration.sql(), driver)) {
            throw std::invalid_argument("database migration SQL must not be empty");
        }
        const auto separator = find_migration_statement_separator(migration.sql(), driver);
        if (separator != std::string_view::npos &&
            !has_trailing_sql_only(migration.sql(), separator + 1, driver)) {
            throw std::invalid_argument(
                "database migration must contain exactly one SQL statement");
        }
        for (std::size_t j = i + 1; j < migrations.size(); ++j) {
            if (migration_ids_collide(migrations[j].id(), migration.id())) {
                throw std::invalid_argument(
                    "database migration ids must be unique, including case");
            }
        }
    }
}

inline void validate_migration_list(std::span<const db_migration> migrations) {
    validate_migration_list(migrations, db_driver::postgresql);
}

}  // namespace ruvia::detail
