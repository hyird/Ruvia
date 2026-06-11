#pragma once

#include <charconv>
#include <cstddef>
#include <memory_resource>
#include <string_view>
#include <system_error>
#include <type_traits>

#include "ruvia/http/detail/model/JsonParser.h"
#include "ruvia/http/detail/model/Traits.h"
#include "ruvia/http/UrlEncoding.h"

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

[[nodiscard]] inline std::string_view decodedFormView(
    std::string_view input,
    std::pmr::string& scratch) {
    if (!hasFormEncoding(input)) {
        return input;
    }
    if (!decodeFormComponent(input, scratch)) {
        return {};
    }
    return scratch;
}

[[nodiscard]] inline std::pmr::memory_resource* formResource(std::pmr::memory_resource* resource) noexcept {
    return resource == nullptr ? std::pmr::get_default_resource() : resource;
}

template <typename T>
[[nodiscard]] bool parseFormValue(
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
            std::pmr::string decoded(formResource(resource));
            input = decodedFormView(input, decoded);
            if (input == "true" || input == "1") {
                value.value = true;
                return true;
            }
            if (input == "false" || input == "0") {
                value.value = false;
                return true;
            }
            return false;
        } else {
            std::pmr::string decoded(formResource(resource));
            input = decodedFormView(input, decoded);
            if (hasFormEncoding(input) || input.empty()) {
                return false;
            }
            ScalarT parsed{};
            const auto [ptr, ec] = std::from_chars(input.data(), input.data() + input.size(), parsed);
            if (ec != std::errc{} || ptr != input.data() + input.size()) {
                return false;
            }
            value.value = parsed;
            return true;
        }
    } else if constexpr (std::is_same_v<FieldT, bool>) {
        std::pmr::string decoded(formResource(resource));
        input = decodedFormView(input, decoded);
        if (input == "true" || input == "1") {
            value = true;
            return true;
        }
        if (input == "false" || input == "0") {
            value = false;
            return true;
        }
        return false;
    } else if constexpr (std::is_integral_v<FieldT> || std::is_floating_point_v<FieldT>) {
        std::pmr::string decoded(formResource(resource));
        input = decodedFormView(input, decoded);
        if (hasFormEncoding(input) || input.empty()) {
            return false;
        }
        const auto [ptr, ec] = std::from_chars(input.data(), input.data() + input.size(), value);
        return ec == std::errc{} && ptr == input.data() + input.size();
    } else {
        static_assert(alwaysFalse<FieldT>, "RUVIA_MODEL form field type is not supported");
    }
}

}  // namespace ruvia::detail
