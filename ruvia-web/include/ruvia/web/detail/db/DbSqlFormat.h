#pragma once

#include <cmath>
#include <memory_resource>
#include <stdexcept>
#include <string>
#include <string_view>

#include "ruvia/web/db/DbEntity.h"
#include "ruvia/web/detail/db/DbUtils.h"

namespace ruvia::detail {

inline void requireDbDialect(DbDriver driver) {
    if (driver != DbDriver::kPostgreSql && driver != DbDriver::kMariaDb) {
        throw std::invalid_argument("SQL compilation requires an explicit database driver");
    }
}

inline void appendDbIdentifier(std::pmr::string& output, std::string_view name, DbDriver driver) {
    requireDbDialect(driver);
    const auto limit = driver == DbDriver::kPostgreSql ? std::size_t{63} : std::size_t{64};
    if (name.empty() || name.size() > limit || name.find('\0') != std::string_view::npos) {
        throw std::invalid_argument("invalid or overlong database identifier");
    }
    const char quote = driver == DbDriver::kPostgreSql ? '"' : '`';
    output.push_back(quote);
    for (const char ch : name) {
        output.push_back(ch);
        if (ch == quote) {
            output.push_back(quote);
        }
    }
    output.push_back(quote);
}

inline void appendDbQualifiedIdentifier(std::pmr::string& output, std::string_view name, DbDriver driver) {
    while (true) {
        const auto dot = name.find('.');
        appendDbIdentifier(output, name.substr(0, dot), driver);
        if (dot == std::string_view::npos) {
            return;
        }
        output.push_back('.');
        name.remove_prefix(dot + 1);
    }
}

inline void appendDbStringLiteral(std::pmr::string& output, std::string_view value, DbDriver driver) {
    requireDbDialect(driver);
    if (value.find('\0') != std::string_view::npos) {
        throw std::invalid_argument("database text cannot contain NUL");
    }
    if (driver == DbDriver::kMariaDb) {
        // Hex text is independent of NO_BACKSLASH_ESCAPES and ANSI_QUOTES.
        // It also avoids allowing connection SQL modes to change DDL values.
        constexpr char digits[] = "0123456789abcdef";
        output += "CONVERT(X'";
        for (const auto ch : value) {
            const auto byte = static_cast<unsigned char>(ch);
            output.push_back(digits[byte >> 4]);
            output.push_back(digits[byte & 15]);
        }
        output += "' USING utf8mb4)";
        return;
    }
    output += "E'";
    for (const char ch : value) {
        if (ch == '\\' || ch == '\'') {
            output.push_back(ch);
        }
        output.push_back(ch);
    }
    output.push_back('\'');
}

inline void appendDbLiteral(std::pmr::string& output, const DbValue& value, DbDriver driver) {
    requireDbDialect(driver);
    switch (DbValueAccess::type(value)) {
        case DbValueType::kNull:
            output += "NULL";
            return;
        case DbValueType::kString:
            appendDbStringLiteral(output, DbValueAccess::text(value), driver);
            return;
        case DbValueType::kSigned:
            appendDbNumber(output, DbValueAccess::signedValue(value));
            return;
        case DbValueType::kUnsigned:
            appendDbNumber(output, DbValueAccess::unsignedValue(value));
            return;
        case DbValueType::kDouble:
            appendDbNumber(output, DbValueAccess::doubleValue(value));
            return;
        case DbValueType::kBool:
            output += DbValueAccess::boolValue(value) ? "TRUE" : "FALSE";
            return;
    }
    std::terminate();
}

enum class DbSqlTypeUsage : std::uint8_t { kColumn,
    kCast };

inline void appendDbDollarBody(std::pmr::string& output, std::string_view body) {
    std::pmr::string delimiter("$ruvia$", output.get_allocator().resource());
    for (std::uint64_t suffix = 1; body.find(delimiter) != std::string_view::npos; ++suffix) {
        delimiter = "$ruvia";
        appendDbNumber(delimiter, suffix);
        delimiter += '$';
    }
    output += delimiter;
    output += body;
    output += delimiter;
}

inline void appendDbTypeName(std::pmr::string& output, DbDataType type,
    std::string_view customName, std::size_t length, unsigned precision, unsigned scale,
    bool array, DbDriver driver, DbSqlTypeUsage usage = DbSqlTypeUsage::kColumn) {
    requireDbDialect(driver);
    const bool pg = driver == DbDriver::kPostgreSql;
    if ((array || !customName.empty()) && !pg) {
        throw std::invalid_argument("array and named database types require PostgreSQL");
    }
    if (!customName.empty()) {
        if (type != DbDataType::kInferred || length != 0 || precision != 0 || scale != 0) {
            throw std::invalid_argument("a named database type cannot also specify scalar type modifiers");
        }
        appendDbQualifiedIdentifier(output, customName, driver);
        if (array) {
            output += "[]";
        }
        return;
    }
    if (length != 0 && type != DbDataType::kVarchar) {
        throw std::invalid_argument("length requires VARCHAR");
    }
    if ((precision != 0 || scale != 0) && type != DbDataType::kNumeric) {
        throw std::invalid_argument("precision and scale require NUMERIC");
    }
    if (scale > precision || (precision > (pg ? 1000U : 65U))) {
        throw std::invalid_argument("invalid database numeric precision or scale");
    }
    const bool mariaCast = !pg && usage == DbSqlTypeUsage::kCast;
    switch (type) {
        case DbDataType::kBoolean:
            output += mariaCast ? "UNSIGNED" : "BOOLEAN";
            break;
        case DbDataType::kSmallInt:
            output += mariaCast ? "SIGNED" : "SMALLINT";
            break;
        case DbDataType::kInteger:
            output += mariaCast ? "SIGNED" : "INTEGER";
            break;
        case DbDataType::kBigInt:
            output += mariaCast ? "SIGNED" : "BIGINT";
            break;
        case DbDataType::kReal:
            output += mariaCast ? "DOUBLE" : "REAL";
            break;
        case DbDataType::kDouble:
            output += pg ? "DOUBLE PRECISION" : "DOUBLE";
            break;
        case DbDataType::kText:
            output += mariaCast ? "CHAR" : "TEXT";
            break;
        case DbDataType::kVarchar:
            if (length == 0) {
                throw std::invalid_argument("VARCHAR requires a positive length");
            }
            output += mariaCast ? "CHAR(" : "VARCHAR(";
            appendDbNumber(output, static_cast<std::uint64_t>(length));
            output.push_back(')');
            break;
        case DbDataType::kNumeric:
            output += "DECIMAL";
            if (precision != 0) {
                output.push_back('(');
                appendDbNumber(output, static_cast<std::uint64_t>(precision));
                output.push_back(',');
                appendDbNumber(output, static_cast<std::uint64_t>(scale));
                output.push_back(')');
            }
            break;
        case DbDataType::kDate:
            output += "DATE";
            break;
        case DbDataType::kTimestamp:
            output += pg ? "TIMESTAMP" : "DATETIME";
            break;
        case DbDataType::kJson:
            if (mariaCast) {
                throw std::invalid_argument("MariaDB does not support CAST AS JSON");
            }
            output += "JSON";
            break;
        case DbDataType::kJsonb:
        case DbDataType::kUuid:
        case DbDataType::kTimestampTz:
        case DbDataType::kInterval:
        case DbDataType::kInet:
        case DbDataType::kCidr:
        case DbDataType::kBytea:
            if (!pg) {
                throw std::invalid_argument("selected database type requires PostgreSQL");
            }
            switch (type) {
                case DbDataType::kJsonb:
                    output += "JSONB";
                    break;
                case DbDataType::kUuid:
                    output += "UUID";
                    break;
                case DbDataType::kTimestampTz:
                    output += "TIMESTAMPTZ";
                    break;
                case DbDataType::kInterval:
                    output += "INTERVAL";
                    break;
                case DbDataType::kInet:
                    output += "INET";
                    break;
                case DbDataType::kCidr:
                    output += "CIDR";
                    break;
                case DbDataType::kBytea:
                    output += "BYTEA";
                    break;
                default:
                    std::terminate();
            }
            break;
        case DbDataType::kArray:
        case DbDataType::kInferred:
            throw std::invalid_argument("SQL requires an explicit scalar type; use the array flag for arrays");
    }
    if (array) {
        output += "[]";
    }
}

}  // namespace ruvia::detail
