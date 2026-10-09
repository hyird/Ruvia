#pragma once

#include <cmath>
#include <memory_resource>
#include <stdexcept>
#include <string>
#include <string_view>

#include "ruvia/web/db/db_entity.h"
#include "ruvia/web/detail/db/db_utils.h"

namespace ruvia::detail {

inline void require_db_dialect(db_driver driver) {
    if (driver != db_driver::postgresql && driver != db_driver::mariadb) {
        throw std::invalid_argument("SQL compilation requires an explicit database driver");
    }
}

inline void append_db_identifier(std::pmr::string& output, std::string_view name, db_driver driver) {
    require_db_dialect(driver);
    const auto limit = driver == db_driver::postgresql ? std::size_t{63} : std::size_t{64};
    if (name.empty() || name.size() > limit || name.find('\0') != std::string_view::npos) {
        throw std::invalid_argument("invalid or overlong database identifier");
    }
    const char quote = driver == db_driver::postgresql ? '"' : '`';
    output.push_back(quote);
    for (const char ch : name) {
        output.push_back(ch);
        if (ch == quote) {
            output.push_back(quote);
        }
    }
    output.push_back(quote);
}

inline void append_db_qualified_identifier(std::pmr::string& output, std::string_view name, db_driver driver) {
    while (true) {
        const auto dot = name.find('.');
        append_db_identifier(output, name.substr(0, dot), driver);
        if (dot == std::string_view::npos) {
            return;
        }
        output.push_back('.');
        name.remove_prefix(dot + 1);
    }
}

inline void append_db_string_literal(std::pmr::string& output, std::string_view value, db_driver driver) {
    require_db_dialect(driver);
    if (value.find('\0') != std::string_view::npos) {
        throw std::invalid_argument("database text cannot contain NUL");
    }
    if (driver == db_driver::mariadb) {
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

inline void append_db_literal(std::pmr::string& output, const db_value& value, db_driver driver) {
    require_db_dialect(driver);
    switch (db_value_access::type(value)) {
        case db_value_type::null:
            output += "NULL";
            return;
        case db_value_type::string:
            append_db_string_literal(output, db_value_access::text(value), driver);
            return;
        case db_value_type::signed_value:
            append_db_number(output, db_value_access::signed_value(value));
            return;
        case db_value_type::unsigned_value:
            append_db_number(output, db_value_access::unsigned_value(value));
            return;
        case db_value_type::double_value:
            append_db_number(output, db_value_access::get_double_value(value));
            return;
        case db_value_type::bool_value:
            output += db_value_access::get_bool_value(value) ? "TRUE" : "FALSE";
            return;
    }
    std::terminate();
}

enum class db_sql_type_usage : std::uint8_t { column,
    cast };

inline void append_db_dollar_body(std::pmr::string& output, std::string_view body) {
    std::pmr::string delimiter("$ruvia$", output.get_allocator().resource());
    for (std::uint64_t suffix = 1; body.find(delimiter) != std::string_view::npos; ++suffix) {
        delimiter = "$ruvia";
        append_db_number(delimiter, suffix);
        delimiter += '$';
    }
    output += delimiter;
    output += body;
    output += delimiter;
}

inline void append_db_type_name(std::pmr::string& output, db_data_type type,
    std::string_view custom_name, std::size_t length, unsigned precision, unsigned scale,
    bool array_value, db_driver driver, db_sql_type_usage usage = db_sql_type_usage::column) {
    require_db_dialect(driver);
    const bool pg = driver == db_driver::postgresql;
    if ((array_value || !custom_name.empty()) && !pg) {
        throw std::invalid_argument("array and named database types require PostgreSQL");
    }
    if (!custom_name.empty()) {
        if (type != db_data_type::inferred || length != 0 || precision != 0 || scale != 0) {
            throw std::invalid_argument("a named database type cannot also specify scalar type modifiers");
        }
        append_db_qualified_identifier(output, custom_name, driver);
        if (array_value) {
            output += "[]";
        }
        return;
    }
    if (length != 0 && type != db_data_type::char_value && type != db_data_type::varchar) {
        throw std::invalid_argument("length requires CHAR or VARCHAR");
    }
    if ((precision != 0 || scale != 0) && type != db_data_type::numeric) {
        throw std::invalid_argument("precision and scale require NUMERIC");
    }
    if (scale > precision || (precision > (pg ? 1000U : 65U))) {
        throw std::invalid_argument("invalid database numeric precision or scale");
    }
    const bool maria_cast = !pg && usage == db_sql_type_usage::cast;
    if (type == db_data_type::char_value) {
        if (length == 0) {
            throw std::invalid_argument("CHAR requires a positive length");
        }
        if (pg && length > 10485760) {
            throw std::invalid_argument("PostgreSQL CHAR length exceeds the supported maximum");
        }
        if (!pg && !maria_cast && length > 255) {
            throw std::invalid_argument("MariaDB CHAR length exceeds the supported maximum");
        }
    }
    switch (type) {
        case db_data_type::boolean:
            output += maria_cast ? "UNSIGNED" : "BOOLEAN";
            break;
        case db_data_type::small_int:
            output += maria_cast ? "SIGNED" : "SMALLINT";
            break;
        case db_data_type::integer:
            output += maria_cast ? "SIGNED" : "INTEGER";
            break;
        case db_data_type::big_int:
            output += maria_cast ? "SIGNED" : "BIGINT";
            break;
        case db_data_type::real:
            output += maria_cast ? "DOUBLE" : "REAL";
            break;
        case db_data_type::double_value:
            output += pg ? "DOUBLE PRECISION" : "DOUBLE";
            break;
        case db_data_type::text:
            output += maria_cast ? "CHAR" : "TEXT";
            break;
        case db_data_type::char_value:
            output += "CHAR(";
            append_db_number(output, static_cast<std::uint64_t>(length));
            output.push_back(')');
            break;
        case db_data_type::varchar:
            if (length == 0) {
                throw std::invalid_argument("VARCHAR requires a positive length");
            }
            output += maria_cast ? "CHAR(" : "VARCHAR(";
            append_db_number(output, static_cast<std::uint64_t>(length));
            output.push_back(')');
            break;
        case db_data_type::numeric:
            output += "DECIMAL";
            if (precision != 0) {
                output.push_back('(');
                append_db_number(output, static_cast<std::uint64_t>(precision));
                output.push_back(',');
                append_db_number(output, static_cast<std::uint64_t>(scale));
                output.push_back(')');
            }
            break;
        case db_data_type::date:
            output += "DATE";
            break;
        case db_data_type::timestamp:
            output += pg ? "TIMESTAMP" : "DATETIME";
            break;
        case db_data_type::json:
            if (maria_cast) {
                throw std::invalid_argument("MariaDB does not support CAST AS JSON");
            }
            output += "JSON";
            break;
        case db_data_type::jsonb:
        case db_data_type::uuid:
        case db_data_type::timestamp_tz:
        case db_data_type::interval:
        case db_data_type::inet:
        case db_data_type::cidr:
        case db_data_type::bytea:
            if (!pg) {
                throw std::invalid_argument("selected database type requires PostgreSQL");
            }
            switch (type) {
                case db_data_type::jsonb:
                    output += "JSONB";
                    break;
                case db_data_type::uuid:
                    output += "UUID";
                    break;
                case db_data_type::timestamp_tz:
                    output += "TIMESTAMPTZ";
                    break;
                case db_data_type::interval:
                    output += "INTERVAL";
                    break;
                case db_data_type::inet:
                    output += "INET";
                    break;
                case db_data_type::cidr:
                    output += "CIDR";
                    break;
                case db_data_type::bytea:
                    output += "BYTEA";
                    break;
                default:
                    std::terminate();
            }
            break;
        case db_data_type::array:
        case db_data_type::inferred:
            throw std::invalid_argument("SQL requires an explicit scalar type; use the array flag for arrays");
    }
    if (array_value) {
        output += "[]";
    }
}

}  // namespace ruvia::detail
