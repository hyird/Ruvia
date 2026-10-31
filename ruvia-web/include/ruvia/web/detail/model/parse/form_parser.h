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

#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/http/url_encoding.h"
#include "ruvia/web/detail/util/decimal_number.h"
#include "ruvia/web/integer.h"
#include "ruvia/web/model.h"

// Internal URL-encoded form parser for RUVIA_MODEL.

namespace ruvia::detail {

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

template <typename number_type>
[[nodiscard]] std::optional<number_type> parse_form_number(std::string_view decoded) {
    if (decoded.empty()) {
        return std::nullopt;
    }
    number_type parsed_value{};
    if constexpr (std::is_floating_point_v<number_type>) {
        const auto value = detail::parse_decimal_number<number_type>(decoded);
        if (!value) {
            return std::nullopt;
        }
        parsed_value = *value;
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
        const auto integer = parse_integer<number_type>(decoded);
        if ((integer.index() != 0)) {
            return std::nullopt;
        }
        parsed_value = std::get<0>(integer);
    }
    return parsed_value;
}

template <typename t_type>
[[nodiscard]] std::optional<t_type> parse_form_value(resolved_pmr_resource_tag, std::string_view input,
    form_value_encoding encoding, std::pmr::memory_resource* resource, model_string_storage string_storage) {
    using field_type = std::remove_cvref_t<t_type>;

    std::optional<std::pmr::string> decoded_storage;
    auto decoded = input;
    if (encoding == form_value_encoding::url_encoded && ruvia::has_url_encoding(input, url_decode_mode::form)) {
        decoded_storage =
            ruvia::decode_url_component(input, {.mode_ = url_decode_mode::form, .resource_ = resource});
        if (!decoded_storage.has_value()) {
            return std::nullopt;
        }
        decoded = std::string_view(*decoded_storage);
    }

    if constexpr (is_ruvia_string<field_type>) {
        if (!decoded_storage.has_value()) {
            if (string_storage == model_string_storage::owned) {
                return field_type(decoded, ::ruvia::model_options{.resource_ = resource});
            }
            return model_value_factory::make_string(decoded, resource);
        }
        field_type value = make_request_value<field_type>(resolved_pmr_resource_tag{}, resource);
        value.assign_owned(std::move(*decoded_storage));
        return value;
    } else if constexpr (std::is_same_v<field_type, std::string_view>) {
        if (decoded_storage.has_value()) {
            return std::nullopt;
        }
        return decoded;
    } else if constexpr (is_ruvia_scalar<field_type>) {
        using scalar_type = model_scalar_value_type<field_type>;
        if constexpr (std::is_same_v<scalar_type, bool>) {
            const auto parsed_value = parse_form_bool(decoded);
            if (!parsed_value.has_value()) {
                return std::nullopt;
            }
            return field_type(*parsed_value);
        } else {
            const auto parsed_value = parse_form_number<scalar_type>(decoded);
            if (!parsed_value.has_value()) {
                return std::nullopt;
            }
            return field_type(*parsed_value);
        }
    } else {
        return std::nullopt;
    }
}

}  // namespace ruvia::detail
