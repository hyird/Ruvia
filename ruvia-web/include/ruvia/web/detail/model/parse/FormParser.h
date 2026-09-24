#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "ruvia/core/Integer.h"
#include "ruvia/core/DecimalNumber.h"
#include "ruvia/core/memory/PmrResource.h"
#include "ruvia/http/UrlEncoding.h"
#include "ruvia/web/detail/model/Traits.h"

// Internal URL-encoded form parser for RUVIA_MODEL.

namespace ruvia::detail {

[[nodiscard]] inline bool hasFormEncoding(std::string_view value) noexcept {
    return hasUrlEncoding(value, UrlDecodeMode::kForm);
}

[[nodiscard]] inline bool validateFormEncoding(std::string_view body) noexcept {
    return validateUrlEncoding(body);
}

enum class FormValueEncoding : std::uint8_t { kUrlEncoded,
    kDecoded };

[[nodiscard]] inline std::optional<bool> parseFormBool(std::string_view decoded) noexcept {
    if (decoded == "true" || decoded == "1") {
        return true;
    }
    if (decoded == "false" || decoded == "0") {
        return false;
    }
    return std::nullopt;
}

template <typename NumberT>
[[nodiscard]] std::optional<NumberT> parseFormNumber(std::string_view decoded) {
    if (decoded.empty()) {
        return std::nullopt;
    }
    NumberT parsed{};
    if constexpr (std::is_floating_point_v<NumberT>) {
        const auto value = ruvia::parseDecimalNumber<NumberT>(decoded);
        if (!value) {
            return std::nullopt;
        }
        parsed = *value;
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
        const auto integer = parseInteger<NumberT>(decoded);
        if (!integer) {
            return std::nullopt;
        }
        parsed = *integer;
    }
    return parsed;
}

template <typename T>
[[nodiscard]] std::optional<T> parseFormValue(ResolvedPmrResourceTag, std::string_view input,
    FormValueEncoding encoding, std::pmr::memory_resource* resource,
    ModelStringStorage stringStorage = ModelStringStorage::kBorrowed) {
    using FieldT = std::remove_cvref_t<T>;

    std::optional<std::pmr::string> decodedStorage;
    auto decoded = input;
    if (encoding == FormValueEncoding::kUrlEncoded && hasFormEncoding(input)) {
        decodedStorage =
            decodeUrlComponent(input, {.mode = UrlDecodeMode::kForm, .resource = resource});
        if (!decodedStorage.has_value()) {
            return std::nullopt;
        }
        decoded = std::string_view(*decodedStorage);
    }

    if constexpr (isRuviaString<FieldT>) {
        if (!decodedStorage.has_value()) {
            if (stringStorage == ModelStringStorage::kOwned) {
                return FieldT(decoded, ::ruvia::ModelOptions{.resource = resource});
            }
            return ModelValueFactory::makeString(decoded, resource);
        }
        FieldT value = makeRequestValue<FieldT>(ResolvedPmrResourceTag{}, resource);
        value.assignOwned(std::move(*decodedStorage));
        return value;
    } else if constexpr (std::is_same_v<FieldT, std::string_view>) {
        if (decodedStorage.has_value()) {
            return std::nullopt;
        }
        return decoded;
    } else if constexpr (isRuviaScalar<FieldT>) {
        using ScalarT = ModelScalarValueT<FieldT>;
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
[[nodiscard]] std::optional<T> parseFormValue(std::string_view input, FormValueEncoding encoding,
    std::pmr::memory_resource* resource,
    ModelStringStorage stringStorage = ModelStringStorage::kBorrowed) {
    return parseFormValue<T>(
        ResolvedPmrResourceTag{}, input, encoding, pmrResourceOrDefault(resource), stringStorage);
}

}  // namespace ruvia::detail
