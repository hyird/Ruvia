#pragma once

#include "ruvia/web/db/DbTypes.h"

#include <memory_resource>
#include <string>
#include <utility>

namespace ruvia::detail {

struct DbValueAccess final {
    [[nodiscard]] static DbValue ownedString(std::pmr::string value) {
        return DbValue(std::move(value));
    }

    [[nodiscard]] static DbValueType type(const DbValue& value) noexcept {
        return value.type();
    }

    [[nodiscard]] static std::string_view text(const DbValue& value) noexcept {
        return value.text();
    }

    [[nodiscard]] static std::int64_t signedValue(const DbValue& value) noexcept {
        return value.signedValue();
    }

    [[nodiscard]] static std::uint64_t unsignedValue(const DbValue& value) noexcept {
        return value.unsignedValue();
    }

    [[nodiscard]] static double doubleValue(const DbValue& value) noexcept {
        return value.doubleValue();
    }

    [[nodiscard]] static bool boolValue(const DbValue& value) noexcept {
        return value.boolValue();
    }
};

}  // namespace ruvia::detail
