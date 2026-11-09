#pragma once

#include <array>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/http/url_encoding.h"
#include "ruvia/web/detail/decimal_number.h"
#include "ruvia/web/integer.h"
#include "ruvia/web/model.h"

// Internal URL-encoded form parser for RUVIA_MODEL.

namespace ruvia {
class request_name_value_list;
}  // namespace ruvia

namespace ruvia::detail {

// application/x-www-form-urlencoded body whose names and values are still
// percent-encoded.
struct form_body_input final {
    std::string_view body_;
    model_string_storage string_storage_{model_string_storage::borrowed};
};

// Borrowed input for form-style schema materialization: an encoded body, or
// request fields (query, path, headers, cookies) whose values are already
// decoded. Never a dynamic public model.
using model_input = std::variant<form_body_input, std::reference_wrapper<const request_name_value_list>>;

// Visits each name/value pair of a URL-encoded form body with the name decoded
// and the value still encoded. Form decoding keeps malformed '%' sequences
// literally (WHATWG), so the body needs no separate percent-encoding pass and
// never fails.
template <typename visitor_type>
    requires std::invocable<visitor_type&, std::string_view, std::string_view>
void visit_form_body_fields(std::string_view body, std::pmr::memory_resource* resource, visitor_type&& visitor) {
    auto& visitor_ref = visitor;
    std::pmr::string decoded_name(resource);
    (void)ruvia::visit_url_encoded_pairs(body, [&](std::string_view name, std::string_view value) {
        visitor_ref(*ruvia::decode_url_component(name, decoded_name, url_decode_mode::form), value);
    });
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

// Form mode of decode_url_component always yields a value (WHATWG keeps a
// malformed '%' literally) and returns `input` itself when nothing needs
// decoding, so a decoded view's address tells whether `storage` holds it.
[[nodiscard]] inline std::string_view decode_form_value(
    std::string_view input, form_value_encoding encoding, std::pmr::string& storage) {
    if (encoding == form_value_encoding::decoded) {
        return input;
    }
    return *ruvia::decode_url_component(input, storage, url_decode_mode::form);
}

template <typename t_type>
    requires is_form_field<t_type>
[[nodiscard]] std::optional<t_type> parse_form_value(resolved_pmr_resource_tag, std::string_view input,
    form_value_encoding encoding, std::pmr::memory_resource* resource, model_string_storage string_storage) {
    using field_type = std::remove_cvref_t<t_type>;

    if constexpr (is_ruvia_scalar<field_type>) {
        // Decoded scalar text only lives until conversion; typical values stay
        // in this stack buffer.
        std::array<std::byte, 128> stack_bytes;
        std::pmr::monotonic_buffer_resource stack_resource(stack_bytes.data(), stack_bytes.size(), resource);
        std::pmr::string decoded_storage(&stack_resource);
        const auto decoded = decode_form_value(input, encoding, decoded_storage);
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
        std::pmr::string decoded_storage(resource);
        if (decode_form_value(input, encoding, decoded_storage).data() != input.data()) {
            field_type value = make_request_value<field_type>(resolved_pmr_resource_tag{}, resource);
            value.assign_owned(std::move(decoded_storage));
            return value;
        }
        if (string_storage == model_string_storage::owned) {
            return field_type(input, ::ruvia::model_options{.resource_ = resource});
        }
        return model_value_factory::make_string(input, resource);
    }
}

}  // namespace ruvia::detail
