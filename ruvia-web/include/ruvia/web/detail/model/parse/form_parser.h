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
#include <variant>

#include "ruvia/core/decimal_number.h"
#include "ruvia/core/integer.h"
#include "ruvia/core/memory/pmr_resource.h"
#include "ruvia/http/url_encoding.h"
#include "ruvia/web/detail/model/traits.h"

// Internal URL-encoded form parser for RUVIA_MODEL.

namespace ruvia::detail {

[[nodiscard]] inline bool has_form_encoding(std::string_view value) noexcept {
    return ruvia::has_url_encoding(value, url_decode_mode::form);
}

[[nodiscard]] inline bool validate_form_encoding(std::string_view body) noexcept {
    return ruvia::validate_url_encoding(body);
}

enum class form_value_encoding : std::uint8_t { url_encoded,
    decoded };

[[nodiscard]] inline std::optional<bool> parse_form_bool(std::string_view decoded) noexcept {
    if (decoded == "true" || decoded == "1") {
        return true;
    }
    if (decoded == "false" || decoded == "0") {
        return false;
    }
    return std::nullopt;
}

template <typename number_t_type>
[[nodiscard]] std::optional<number_t_type> parse_form_number(std::string_view decoded) {
    if (decoded.empty()) {
        return std::nullopt;
    }
    number_t_type parsed_value{};
    if constexpr (std::is_floating_point_v<number_t_type>) {
        const auto value = ruvia::parse_decimal_number<number_t_type>(decoded);
        if ((value.index() != 0)) {
            return std::nullopt;
        }
        parsed_value = std::get<0>(value);
        // Floating parsers accept "inf"/"nan", but the rest of the pipeline
        // cannot round-trip them: the JSON number grammar rejects them on input,
        // the model JSON writer replaces them with null, and the finite number
        // formatter throws. Reject them here so a bound floating field is always
        // a finite value rather than one that silently changes or aborts the
        // response when serialized.
        if (!std::isfinite(parsed_value)) {
            return std::nullopt;
        }
    } else {
        const auto integer = parse_integer<number_t_type>(decoded);
        if ((integer.index() != 0)) {
            return std::nullopt;
        }
        parsed_value = std::get<0>(integer);
    }
    return parsed_value;
}

template <typename t_type>
[[nodiscard]] std::optional<t_type> parse_form_value(resolved_pmr_resource_tag, std::string_view input,
    form_value_encoding encoding, std::pmr::memory_resource* resource,
    model_string_storage string_storage = model_string_storage::borrowed) {
    using field_t_type = std::remove_cvref_t<t_type>;

    std::optional<std::pmr::string> decoded_storage;
    auto decoded = input;
    if (encoding == form_value_encoding::url_encoded && has_form_encoding(input)) {
        decoded_storage =
            ruvia::decode_url_component(input, {.mode_ = url_decode_mode::form, .resource_ = resource});
        if (!decoded_storage.has_value()) {
            return std::nullopt;
        }
        decoded = std::string_view(*decoded_storage);
    }

    if constexpr (is_ruvia_string<field_t_type>) {
        if (!decoded_storage.has_value()) {
            if (string_storage == model_string_storage::owned) {
                return field_t_type(decoded, ::ruvia::model_options{.resource_ = resource});
            }
            return model_value_factory::make_string(decoded, resource);
        }
        field_t_type value = make_request_value<field_t_type>(resolved_pmr_resource_tag{}, resource);
        value.assign_owned(std::move(*decoded_storage));
        return value;
    } else if constexpr (std::is_same_v<field_t_type, std::string_view>) {
        if (decoded_storage.has_value()) {
            return std::nullopt;
        }
        return decoded;
    } else if constexpr (is_ruvia_scalar<field_t_type>) {
        using scalar_t_type = model_scalar_value_t_type<field_t_type>;
        if constexpr (std::is_same_v<scalar_t_type, bool>) {
            const auto parsed_value = parse_form_bool(decoded);
            if (!parsed_value.has_value()) {
                return std::nullopt;
            }
            return field_t_type(*parsed_value);
        } else {
            const auto parsed_value = parse_form_number<scalar_t_type>(decoded);
            if (!parsed_value.has_value()) {
                return std::nullopt;
            }
            return field_t_type(*parsed_value);
        }
    } else {
        return std::nullopt;
    }
}

template <typename t_type>
[[nodiscard]] std::optional<t_type> parse_form_value(std::string_view input, form_value_encoding encoding,
    std::pmr::memory_resource* resource,
    model_string_storage string_storage = model_string_storage::borrowed) {
    return parse_form_value<t_type>(
        resolved_pmr_resource_tag{}, input, encoding, pmr_resource_or_default(resource), string_storage);
}

}  // namespace ruvia::detail
