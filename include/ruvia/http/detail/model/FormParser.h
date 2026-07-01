#pragma once

#include <charconv>
#include <cstddef>
#include <memory_resource>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>

#include "ruvia/http/detail/model/Traits.h"
#include "ruvia/http/UrlEncoding.h"
#include "ruvia/memory/PmrResource.h"

// Internal URL-encoded form parser layer for RUVIA_MODEL.

namespace ruvia::detail {

[[nodiscard]] inline bool hasFormEncoding(std::string_view value) noexcept {
    return hasUrlEncoding(value, UrlDecodeMode::kForm);
}

[[nodiscard]] inline bool validateFormEncoding(std::string_view body) noexcept {
    return validateUrlEncoding(body);
}

template <typename StringT>
[[nodiscard]] bool decodeFormComponent(std::string_view input, StringT& output) {
    return decodeUrlComponent(input, output, UrlDecodeMode::kForm);
}

template <typename Visitor>
[[nodiscard]] bool withDecodedFormView(
    ResolvedPmrResourceTag,
    std::string_view input,
    std::pmr::memory_resource* resource,
    Visitor&& visitor) {
    if (!hasFormEncoding(input)) {
        return std::forward<Visitor>(visitor)(input);
    }
    std::pmr::string scratch(resource);
    if (!decodeFormComponent(input, scratch)) {
        return false;
    }
    return std::forward<Visitor>(visitor)(std::string_view(scratch));
}

template <typename Visitor>
[[nodiscard]] bool withDecodedFormView(
    std::string_view input,
    std::pmr::memory_resource* resource,
    Visitor&& visitor) {
    return withDecodedFormView(
        ResolvedPmrResourceTag{},
        input,
        pmrResourceOrDefault(resource),
        std::forward<Visitor>(visitor));
}

[[nodiscard]] inline bool parseFormBool(std::string_view decoded, bool& value) noexcept {
    if (decoded == "true" || decoded == "1") {
        value = true;
        return true;
    }
    if (decoded == "false" || decoded == "0") {
        value = false;
        return true;
    }
    return false;
}

template <typename NumberT>
[[nodiscard]] bool parseFormNumber(std::string_view decoded, NumberT& value) {
    if (decoded.empty()) {
        return false;
    }
    NumberT parsed{};
    const auto [ptr, ec] = std::from_chars(decoded.data(), decoded.data() + decoded.size(), parsed);
    if (ec != std::errc{} || ptr != decoded.data() + decoded.size()) {
        return false;
    }
    value = parsed;
    return true;
}

template <typename T>
[[nodiscard]] bool parseFormValue(
    ResolvedPmrResourceTag,
    std::string_view input,
    T& value,
    std::pmr::memory_resource* resource) {
    using FieldT = std::remove_cvref_t<T>;
    if constexpr (isRuviaString<FieldT>) {
        if (!hasFormEncoding(input)) {
            value.assignView(input);
            return true;
        }
        (void)resource;
        return decodeFormComponent(input, value.resetOwned());
    } else if constexpr (std::is_same_v<FieldT, std::string_view>) {
        if (hasFormEncoding(input)) {
            return false;
        }
        value = input;
        return true;
    } else if constexpr (isRuviaScalar<FieldT>) {
        using ScalarT = typename RuviaScalarTraits<FieldT>::value_type;
        if constexpr (std::is_same_v<ScalarT, bool>) {
            return withDecodedFormView(
                ResolvedPmrResourceTag{},
                input,
                resource,
                [&value](std::string_view decoded) {
                    return parseFormBool(decoded, value.value);
                });
        } else {
            return withDecodedFormView(
                ResolvedPmrResourceTag{},
                input,
                resource,
                [&value](std::string_view decoded) {
                    return parseFormNumber(decoded, value.value);
                });
        }
    } else if constexpr (std::is_same_v<FieldT, bool>) {
        return withDecodedFormView(
            ResolvedPmrResourceTag{},
            input,
            resource,
            [&value](std::string_view decoded) {
                return parseFormBool(decoded, value);
            });
    } else if constexpr (std::is_integral_v<FieldT> || std::is_floating_point_v<FieldT>) {
        return withDecodedFormView(
            ResolvedPmrResourceTag{},
            input,
            resource,
            [&value](std::string_view decoded) {
                return parseFormNumber(decoded, value);
            });
    } else {
        static_assert(alwaysFalse<FieldT>, "RUVIA_MODEL form field type is not supported");
    }
}

template <typename T>
[[nodiscard]] bool parseDecodedFormValue(
    ResolvedPmrResourceTag,
    std::string_view input,
    T& value,
    std::pmr::memory_resource*) {
    using FieldT = std::remove_cvref_t<T>;
    if constexpr (isRuviaString<FieldT>) {
        value.assignView(input);
        return true;
    } else if constexpr (std::is_same_v<FieldT, std::string_view>) {
        value = input;
        return true;
    } else if constexpr (isRuviaScalar<FieldT>) {
        using ScalarT = typename RuviaScalarTraits<FieldT>::value_type;
        if constexpr (std::is_same_v<ScalarT, bool>) {
            return parseFormBool(input, value.value);
        } else {
            return parseFormNumber(input, value.value);
        }
    } else if constexpr (std::is_same_v<FieldT, bool>) {
        return parseFormBool(input, value);
    } else if constexpr (std::is_integral_v<FieldT> || std::is_floating_point_v<FieldT>) {
        return parseFormNumber(input, value);
    } else {
        static_assert(alwaysFalse<FieldT>, "RUVIA_MODEL form field type is not supported");
    }
}

template <typename T>
[[nodiscard]] bool parseFormValue(
    std::string_view input,
    T& value,
    std::pmr::memory_resource* resource) {
    return parseFormValue(
        ResolvedPmrResourceTag{},
        input,
        value,
        pmrResourceOrDefault(resource));
}

template <typename T>
[[nodiscard]] bool parseDecodedFormValue(
    std::string_view input,
    T& value,
    std::pmr::memory_resource* resource) {
    return parseDecodedFormValue(
        ResolvedPmrResourceTag{},
        input,
        value,
        pmrResourceOrDefault(resource));
}

}  // namespace ruvia::detail
