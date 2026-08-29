#pragma once

#include "ruvia/web/detail/json/JsonEscape.h"
#include "ruvia/web/detail/model/Traits.h"

#include <charconv>
#include <cmath>
#include <cstddef>
#include <limits>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>

namespace ruvia::detail {

struct ModelJsonAccess final {
    template <typename ModelT>
    [[nodiscard]] static std::optional<ModelT> parseOwned(
        std::string_view body, std::pmr::memory_resource* resource) {
        return ModelT::ruviaParseJsonBodyOwned(body, resource);
    }

    template <typename ModelT>
    [[nodiscard]] static std::size_t sizeHint(const ModelT& model) {
        return model.ruviaJsonSizeHint();
    }

    template <typename ModelT>
    static void append(std::pmr::string& output, const ModelT& model) {
        model.ruviaAppendJson(output);
    }
};

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
    } else if constexpr (isResponseModel<T>) {
        return ModelJsonAccess::sizeHint(value);
    } else if constexpr (isRuviaArray<T> || isRuviaBoxedArray<T>) {
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
        static_assert(
            alwaysFalse<T>, "JSON output must use Ruvia scalar types or RUVIA_RESPONSE_MODEL");
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
    } else if constexpr (isResponseModel<T>) {
        ModelJsonAccess::append(output, value);
    } else if constexpr (isRuviaArray<T> || isRuviaBoxedArray<T>) {
        appendJsonSequence(output, value);
    } else {
        static_assert(
            alwaysFalse<T>, "JSON output must use Ruvia scalar types or RUVIA_RESPONSE_MODEL");
    }
}

}  // namespace ruvia::detail
