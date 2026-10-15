#pragma once

#include <memory_resource>
#include <string>
#include <utility>
#include <variant>

#include "ruvia/web/db/db_types.h"

namespace ruvia::detail {

struct db_value_access final {
    [[nodiscard]] static db_value owned_string(std::pmr::string value) {
        return db_value(std::in_place_type<std::pmr::string>, std::move(value));
    }

    [[nodiscard]] static db_value_type type(const db_value& value) noexcept {
        return value.type();
    }

    [[nodiscard]] static std::string_view text(const db_value& value) noexcept {
        return value.text();
    }

    // Prepared statement parameters own their text. Requiring that alternative
    // gives native text protocols a checked, NUL-terminated borrowing boundary.
    [[nodiscard]] static const std::pmr::string& owned_text(const db_value& value) {
        return std::get<std::pmr::string>(value.storage_);
    }
    static const std::pmr::string& owned_text(const db_value&&) = delete;

    [[nodiscard]] static std::int64_t signed_value(const db_value& value) noexcept {
        return value.signed_value();
    }

    [[nodiscard]] static std::uint64_t unsigned_value(const db_value& value) noexcept {
        return value.unsigned_value();
    }

    [[nodiscard]] static double get_double_value(const db_value& value) noexcept {
        return value.get_double_value();
    }

    [[nodiscard]] static bool get_bool_value(const db_value& value) noexcept {
        return value.get_bool_value();
    }
};

}  // namespace ruvia::detail
