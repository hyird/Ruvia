#pragma once

#include <cstddef>
#include <memory_resource>
#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>

#include "ruvia/web/detail/model/Traits.h"
#include "ruvia/web/detail/json/JsonNumber.h"
#include "ruvia/web/detail/json/JsonScanner.h"
#include "ruvia/web/detail/json/JsonString.h"
#include "ruvia/web/detail/model/JsonWriter.h"

// Internal JSON model value parser layer for RUVIA_MODEL.

namespace ruvia::detail {

template <typename T>
[[nodiscard]] std::optional<T> parseJsonValue(
    std::string_view& input,
    std::pmr::memory_resource* resource,
    std::size_t depth = 0);

template <typename SequenceT>
struct JsonSequenceValueTraits;

template <typename ValueT>
struct JsonSequenceValueTraits<std::pmr::vector<ValueT>> {
    using value_type = ValueT;

    static void emplace(std::pmr::vector<ValueT>& value, ValueT&& element) {
        value.emplace_back(std::move(element));
    }
};

template <typename ValueT>
struct JsonSequenceValueTraits<List<ValueT>> {
    using value_type = ValueT;

    static void emplace(List<ValueT>& value, ValueT&& element) {
        value.emplaceMove(std::move(element));
    }
};

template <typename SequenceT>
[[nodiscard]] bool parseJsonSequenceValue(
    std::string_view& input,
    SequenceT& value,
    std::pmr::memory_resource* resource,
    std::size_t depth) {
    using Traits = JsonSequenceValueTraits<std::remove_cvref_t<SequenceT>>;
    using ElementT = typename Traits::value_type;

    if (depth > kMaxJsonDepth) {
        return false;
    }
    if (!consumeJsonChar(input, '[')) {
        return false;
    }

    value.clear();
    skipJsonWhitespace(input);
    if (!input.empty() && input.front() == ']') {
        input.remove_prefix(1);
        return true;
    }

    for (;;) {
        auto element = parseJsonValue<ElementT>(input, resource, depth + 1);
        if (!element) {
            return false;
        }
        Traits::emplace(value, std::move(*element));

        skipJsonWhitespace(input);
        if (!input.empty() && input.front() == ']') {
            input.remove_prefix(1);
            return true;
        }
        if (!consumeJsonChar(input, ',')) {
            return false;
        }
    }
}

template <typename VectorT>
[[nodiscard]] bool parseJsonArrayValue(
    std::string_view& input,
    VectorT& value,
    std::pmr::memory_resource* resource,
    std::size_t depth) {
    return parseJsonSequenceValue(input, value, resource, depth);
}

template <typename ListT>
[[nodiscard]] bool parseJsonListValue(
    std::string_view& input,
    ListT& value,
    std::pmr::memory_resource* resource,
    std::size_t depth) {
    return parseJsonSequenceValue(input, value, resource, depth);
}

template <typename T>
[[nodiscard]] bool parseJsonValue(
    std::string_view& input,
    T& value,
    std::pmr::memory_resource* resource,
    std::size_t depth = 0) {
    using FieldT = std::remove_cvref_t<T>;
    if (depth > kMaxJsonDepth) {
        return false;
    }
    if constexpr (isRuviaString<FieldT>) {
        std::string_view raw;
        bool escaped = false;
        if (!parseJsonStringRaw(input, raw, escaped)) {
            return false;
        }
        if (!escaped) {
            value.assignView(raw);
            return true;
        }
        return decodeJsonString(raw, value.resetOwned());
    } else if constexpr (std::is_same_v<FieldT, std::string_view>) {
        return parseJsonStringView(input, value);
    } else if constexpr (isRuviaArray<FieldT>) {
        return parseJsonArrayValue(input, value, resource, depth);
    } else if constexpr (isRuviaList<FieldT>) {
        return parseJsonListValue(input, value, resource, depth);
    } else if constexpr (isRuviaScalar<FieldT>) {
        using ScalarT = typename RuviaScalarTraits<FieldT>::value_type;
        ScalarT parsed{};
        if constexpr (std::is_same_v<ScalarT, bool>) {
            if (consumeJsonLiteral(input, "true")) {
                value.value = true;
                return true;
            }
            if (consumeJsonLiteral(input, "false")) {
                value.value = false;
                return true;
            }
            return false;
        } else {
            if (!parseJsonNumberValue(input, parsed)) {
                return false;
            }
            value.value = parsed;
            return true;
        }
    } else if constexpr (JsonBody<FieldT>::value) {
        std::string_view object = input;
        if (!skipJsonObject(input, depth + 1)) {
            return false;
        }
        object = object.substr(0, object.size() - input.size());
        if (auto nested = JsonBody<FieldT>::parseDepth(object, resource, depth + 1); nested) {
            value = std::move(*nested);
            return true;
        }
        return false;
    } else {
        static_assert(alwaysFalse<FieldT>, "RUVIA_MODEL JSON getter type is not supported");
    }
}

template <typename T>
[[nodiscard]] std::optional<T> parseJsonValue(
    std::string_view& input,
    std::pmr::memory_resource* resource,
    std::size_t depth) {
    using FieldT = std::remove_cvref_t<T>;
    if (depth > kMaxJsonDepth) {
        return std::nullopt;
    }
    if constexpr (JsonBody<FieldT>::value) {
        std::string_view object = input;
        if (!skipJsonObject(input, depth + 1)) {
            return std::nullopt;
        }
        object = object.substr(0, object.size() - input.size());
        return JsonBody<FieldT>::parseDepth(object, resource, depth + 1);
    } else {
        FieldT value = makeRequestValue<FieldT>(resource);
        if (!parseJsonValue(input, value, resource, depth)) {
            return std::nullopt;
        }
        return value;
    }
}

}  // namespace ruvia::detail
