#pragma once

#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>

#include "ruvia/core/memory/PmrResource.h"
#include "ruvia/http/UrlEncoding.h"
#include "ruvia/web/detail/DecimalNumber.h"
#include "ruvia/web/detail/model/Traits.h"

// Internal URL-encoded form parser for RUVIA_MODEL.

namespace ruvia::detail {

[[nodiscard]] inline bool hasFormEncoding(std::string_view value) noexcept {
    return hasUrlEncoding(value, UrlDecodeMode::kForm);
}

[[nodiscard]] inline bool validateFormEncoding(std::string_view body) noexcept {
    return validateUrlEncoding(body);
}

enum class FormValueEncoding : std::uint8_t {
    kUrlEncoded,
    kDecoded
};

[[nodiscard]] inline std::optional<bool> parseFormBool(
    std::string_view decoded) noexcept {
    if (decoded == "true" || decoded == "1") {
        return true;
    }
    if (decoded == "false" || decoded == "0") {
        return false;
    }
    return std::nullopt;
}

template <typename NumberT>
[[nodiscard]] std::optional<NumberT> parseFormNumber(
    std::string_view decoded) {
    if (decoded.empty()) {
        return std::nullopt;
    }
    NumberT parsed{};
    if constexpr (std::is_floating_point_v<NumberT>) {
        double value = 0;
        if (!parseDecimalNumber(decoded, value)) {
            return std::nullopt;
        }
        parsed = static_cast<NumberT>(value);
        // Floating parsers accept "inf"/"nan", but the rest of the pipeline
        // cannot round-trip them: the JSON number grammar rejects them on input,
        // the model JSON writer replaces them with null, and the finite number
        // formatter throws. Reject them here so a bound floating field is always
        // a finite value rather than one that silently changes or aborts the
        // response when serialized.
        if (!std::isfinite(parsed)) {
            return std::nullopt;
        }
    } else {
        const auto [ptr, ec] = std::from_chars(
            decoded.data(),
            decoded.data() + decoded.size(),
            parsed);
        if (ec != std::errc{} || ptr != decoded.data() + decoded.size()) {
            return std::nullopt;
        }
    }
    return parsed;
}

template <typename T>
[[nodiscard]] std::optional<T> parseFormValue(
    ResolvedPmrResourceTag,
    std::string_view input,
    FormValueEncoding encoding,
    std::pmr::memory_resource* resource) {
    using FieldT = std::remove_cvref_t<T>;

    std::optional<std::pmr::string> decodedStorage;
    auto decoded = input;
    if (encoding == FormValueEncoding::kUrlEncoded && hasFormEncoding(input)) {
        decodedStorage = decodeUrlComponent(
            input,
            UrlDecodeMode::kForm,
            resource);
        if (!decodedStorage.has_value()) {
            return std::nullopt;
        }
        decoded = std::string_view(*decodedStorage);
    }

    if constexpr (isRuviaString<FieldT>) {
        if (!decodedStorage.has_value()) {
            return ModelValueFactory::makeString(decoded, resource);
        }
        FieldT value = makeRequestValue<FieldT>(
            ResolvedPmrResourceTag{},
            resource);
        value.assignOwned(std::move(*decodedStorage));
        return value;
    } else if constexpr (std::is_same_v<FieldT, std::string_view>) {
        if (decodedStorage.has_value()) {
            return std::nullopt;
        }
        return decoded;
    } else if constexpr (isRuviaScalar<FieldT>) {
        using ScalarT = typename RuviaScalarTraits<FieldT>::value_type;
        if constexpr (std::is_same_v<ScalarT, bool>) {
            const auto parsed = parseFormBool(decoded);
            if (!parsed.has_value()) {
                return std::nullopt;
            }
            return FieldT(*parsed);
        } else {
            const auto parsed = parseFormNumber<ScalarT>(decoded);
            if (!parsed.has_value()) {
                return std::nullopt;
            }
            return FieldT(*parsed);
        }
    } else {
        return std::nullopt;
    }
}

template <typename T>
[[nodiscard]] std::optional<T> parseFormValue(
    std::string_view input,
    FormValueEncoding encoding,
    std::pmr::memory_resource* resource) {
    return parseFormValue<T>(
        ResolvedPmrResourceTag{},
        input,
        encoding,
        pmrResourceOrDefault(resource));
}

}  // namespace ruvia::detail
