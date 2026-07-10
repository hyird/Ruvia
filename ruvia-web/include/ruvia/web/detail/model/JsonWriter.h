#pragma once

#include "ruvia/http/JsonUtils.h"
#include "ruvia/web/detail/model/Traits.h"

#include <charconv>
#include <cmath>
#include <cstddef>
#include <limits>
#include <memory_resource>
#include <string>
#include <system_error>
#include <type_traits>

namespace ruvia::detail {

template <typename ValueT>
[[nodiscard]] std::size_t jsonSizeHintValue(const ValueT& value) {
    using T = std::remove_cvref_t<ValueT>;
    if constexpr (isRuviaScalar<T>) {
        return jsonSizeHintValue(value.value);
    } else if constexpr (std::is_same_v<T, bool>) {
        return value ? 4 : 5;
    } else if constexpr (std::is_integral_v<T>) {
        return static_cast<std::size_t>(std::numeric_limits<T>::digits10) + 3;
    } else if constexpr (std::is_floating_point_v<T>) {
        return 32;
    } else if constexpr (isRuviaString<T>) {
        return jsonStringSizeHint(value.view());
    } else if constexpr (requires { value.ruviaJsonSizeHint(); }) {
        return value.ruviaJsonSizeHint();
    } else if constexpr (isRuviaArray<T> || isRuviaList<T>) {
        std::size_t size = 2;
        bool first = true;
        for (const auto& item : value) {
            if (!first) {
                ++size;
            }
            first = false;
            size += jsonSizeHintValue(item);
        }
        return size;
    } else {
        static_assert(alwaysFalse<T>, "RUVIA_MODEL field type is not JSON serializable");
    }
}

template <typename ValueT>
void appendJsonValue(std::pmr::string& output, const ValueT& value);

template <typename SequenceT>
void appendJsonSequence(std::pmr::string& output, const SequenceT& value) {
    output.push_back('[');
    bool first = true;
    for (const auto& item : value) {
        if (!first) {
            output.push_back(',');
        }
        first = false;
        appendJsonValue(output, item);
    }
    output.push_back(']');
}

template <typename ValueT>
void appendJsonValue(std::pmr::string& output, const ValueT& value) {
    using T = std::remove_cvref_t<ValueT>;
    if constexpr (isRuviaScalar<T>) {
        appendJsonValue(output, value.value);
    } else if constexpr (std::is_same_v<T, bool>) {
        output.append(value ? "true" : "false");
    } else if constexpr (std::is_integral_v<T> || std::is_floating_point_v<T>) {
        if constexpr (std::is_floating_point_v<T>) {
            // JSON (RFC 8259) has no representation for infinity or NaN, and
            // std::to_chars would emit the bare tokens "inf"/"nan" — invalid JSON.
            // Serialize non-finite values as null, matching JSON.stringify.
            if (!std::isfinite(value)) {
                output.append("null");
                return;
            }
        }
        char buffer[64];
        const auto [ptr, ec] = std::to_chars(buffer, buffer + sizeof(buffer), value);
        if (ec == std::errc{}) {
            output.append(buffer, static_cast<std::size_t>(ptr - buffer));
        }
    } else if constexpr (isRuviaString<T>) {
        appendJsonString(output, value.view());
    } else if constexpr (requires { value.ruviaAppendJson(output); }) {
        value.ruviaAppendJson(output);
    } else if constexpr (isRuviaArray<T> || isRuviaList<T>) {
        appendJsonSequence(output, value);
    } else {
        static_assert(alwaysFalse<T>, "RUVIA_MODEL field type is not JSON serializable");
    }
}

}  // namespace ruvia::detail
