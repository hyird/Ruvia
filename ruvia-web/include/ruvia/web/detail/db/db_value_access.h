#pragma once

#include <memory_resource>
#include <string>
#include <utility>

#include "ruvia/web/db/db_types.h"

namespace ruvia::detail {

struct db_value_access final {
    [[nodiscard]] static db_value owned_string(std::pmr::string value) {
        return db_value(std::move(value));
    }

    [[nodiscard]] static db_value_type type(const db_value& value) noexcept {
        return value.type();
    }

    [[nodiscard]] static std::string_view text(const db_value& value) noexcept {
        return value.text();
    }

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
