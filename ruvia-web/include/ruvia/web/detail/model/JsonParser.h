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

// Internal JSON value parser for RUVIA_MODEL.

namespace ruvia::detail {

template <typename T>
[[nodiscard]] std::optional<T> parseJsonValue(
    std::string_view& input,
    std::pmr::memory_resource* resource,
    std::size_t depth = 0,
    ModelStringStorage stringStorage = ModelStringStorage::kBorrowed);

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
[[nodiscard]] std::optional<SequenceT> parseJsonSequenceValue(
    std::string_view& input,
    std::pmr::memory_resource* resource,
    std::size_t depth,
    ModelStringStorage stringStorage) {
    using Traits = JsonSequenceValueTraits<std::remove_cvref_t<SequenceT>>;
    using ElementT = typename Traits::value_type;

    if (depth > kMaxJsonDepth) {
        return std::nullopt;
    }
    auto remaining = input;
    if (!consumeJsonChar(remaining, '[')) {
        return std::nullopt;
    }

    SequenceT value = makeRequestValue<SequenceT>(resource);
    skipJsonWhitespace(remaining);
    if (!remaining.empty() && remaining.front() == ']') {
        remaining.remove_prefix(1);
        input = remaining;
        return value;
    }

    for (;;) {
        auto element = parseJsonValue<ElementT>(
            remaining, resource, depth + 1, stringStorage);
        if (!element.has_value()) {
            return std::nullopt;
        }
        Traits::emplace(value, std::move(*element));

        skipJsonWhitespace(remaining);
        if (!remaining.empty() && remaining.front() == ']') {
            remaining.remove_prefix(1);
            input = remaining;
            return value;
        }
        if (!consumeJsonChar(remaining, ',')) {
            return std::nullopt;
        }
    }
}

template <typename T>
[[nodiscard]] std::optional<T> parseJsonValue(
    std::string_view& input,
    std::pmr::memory_resource* resource,
    std::size_t depth,
    ModelStringStorage stringStorage) {
    using FieldT = std::remove_cvref_t<T>;
    if (depth > kMaxJsonDepth) {
        return std::nullopt;
    }
    auto remaining = input;
    if constexpr (isRuviaString<FieldT>) {
        const auto parsed = parseJsonString(remaining);
        if (!parsed.has_value()) {
            return std::nullopt;
        }
        if (parsed->encoding() == JsonStringEncoding::kLiteral) {
            input = remaining;
            if (stringStorage == ModelStringStorage::kOwned) {
                return FieldT(parsed->raw(), resource);
            }
            return ModelValueFactory::makeString(parsed->raw(), resource);
        }
        auto decoded = decodeJsonString(parsed->raw(), resource);
        if (!decoded.has_value()) {
            return std::nullopt;
        }
        FieldT value = makeRequestValue<FieldT>(resource);
        value.assignOwned(std::move(*decoded));
        input = remaining;
        return value;
    } else if constexpr (std::is_same_v<FieldT, std::string_view>) {
        const auto parsed = parseJsonString(remaining);
        if (!parsed.has_value() ||
            parsed->encoding() != JsonStringEncoding::kLiteral) {
            return std::nullopt;
        }
        input = remaining;
        return parsed->raw();
    } else if constexpr (isRuviaArray<FieldT> || isRuviaList<FieldT>) {
        auto parsed = parseJsonSequenceValue<FieldT>(
            remaining, resource, depth, stringStorage);
        if (!parsed.has_value()) {
            return std::nullopt;
        }
        input = remaining;
        return parsed;
    } else if constexpr (isRuviaScalar<FieldT>) {
        using ScalarT = typename RuviaScalarTraits<FieldT>::value_type;
        ScalarT parsed{};
        if constexpr (std::is_same_v<ScalarT, bool>) {
            if (consumeJsonLiteral(remaining, "true")) {
                parsed = true;
            } else if (consumeJsonLiteral(remaining, "false")) {
                parsed = false;
            } else {
                return std::nullopt;
            }
        } else {
            if (!parseJsonNumberValue(remaining, parsed)) {
                return std::nullopt;
            }
        }
        input = remaining;
        return FieldT(parsed);
    } else if constexpr (JsonBody<FieldT>::value) {
        const auto objectStart = remaining;
        if (!skipJsonObject(remaining, depth + 1)) {
            return std::nullopt;
        }
        const auto object = objectStart.substr(
            0,
            objectStart.size() - remaining.size());
        auto nested = JsonBody<FieldT>::parseDepth(
            object,
            resource,
            depth + 1,
            stringStorage);
        if (!nested.has_value()) {
            return std::nullopt;
        }
        input = remaining;
        return nested;
    } else {
        static_assert(alwaysFalse<FieldT>, "RUVIA_MODEL JSON field type is not supported");
    }
}

}  // namespace ruvia::detail
