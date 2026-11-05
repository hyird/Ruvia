#pragma once

#include <charconv>
#include <cmath>
#include <cstddef>
#include <limits>
#include <memory_resource>
#include <string>
#include <system_error>
#include <type_traits>

#include "ruvia/http/http_number_format.h"
#include "ruvia/web/detail/json/json_escape.h"
#include "ruvia/web/detail/model/model_binary.h"
#include "ruvia/web/detail/model/model_schema.h"
#include "ruvia/web/model.h"

namespace ruvia::detail {

struct model_json_access final {
    template <typename model_type>
    [[nodiscard]] static std::size_t size_hint(const model_type& model);

    template <typename model_type>
    static void append(std::pmr::string& output, const model_type& model);
};

template <typename value_type>
consteval std::size_t json_fixed_scalar_size_hint() noexcept {
    using t_type = std::remove_cvref_t<value_type>;
    if constexpr (is_ruvia_scalar<t_type>) {
        return json_fixed_scalar_size_hint<decltype(std::declval<t_type>().value_)>();
    } else if constexpr (std::is_same_v<t_type, bool>) {
        return 5;
    } else if constexpr (std::is_integral_v<t_type>) {
        return static_cast<std::size_t>(std::numeric_limits<t_type>::digits10) + 3;
    } else if constexpr (std::is_floating_point_v<t_type>) {
        return 32;
    } else {
        return 0;
    }
}

template <typename value_type>
[[nodiscard]] std::size_t json_size_hint_value(const value_type& value) {
    using t_type = std::remove_cvref_t<value_type>;
    if constexpr (json_fixed_scalar_size_hint<t_type>() != 0) {
        return json_fixed_scalar_size_hint<t_type>();
    } else if constexpr (is_ruvia_bytes<t_type>) {
        return http_base64_encoded_size(value.size()) + 2;
    } else if constexpr (is_ruvia_string<t_type>) {
        return json_string_size_hint(value.view());
    } else if constexpr (is_model<t_type>) {
        return model_json_access::size_hint(value);
    } else if constexpr (is_ruvia_json_value<t_type> || is_ruvia_json_object<t_type>) {
        return value.view().size();
    } else if constexpr (is_ruvia_array<t_type> || is_ruvia_boxed_array<t_type>) {
        if constexpr (is_ruvia_array<t_type>) {
            constexpr std::size_t item_hint = json_fixed_scalar_size_hint<typename t_type::value_type>();
            if constexpr (item_hint != 0) {
                return value.empty() ? 2 : 2 + value.size() * item_hint + (value.size() - 1);
            }
        }
        std::size_t size = 2;
        bool first = true;
        for (const auto& item : value) {
            if (!first) {
                ++size;
            }
            first = false;
            size += json_size_hint_value(item);
        }
        return size;
    } else {
        static_assert(
            always_false<t_type>, "JSON output must use Ruvia scalar types or RUVIA_MODEL");
    }
}

template <typename value_type>
void append_json_value(std::pmr::string& output, const value_type& value);

template <typename sequence_type>
void append_json_sequence(std::pmr::string& output, const sequence_type& value) {
    output.push_back('[');
    bool first = true;
    for (const auto& item : value) {
        if (!first) {
            output.push_back(',');
        }
        first = false;
        append_json_value(output, item);
    }
    output.push_back(']');
}

template <typename value_type>
void append_json_value(std::pmr::string& output, const value_type& value) {
    using t_type = std::remove_cvref_t<value_type>;
    if constexpr (is_ruvia_scalar<t_type>) {
        append_json_value(output, value.value_);
    } else if constexpr (std::is_same_v<t_type, bool>) {
        output.append(value ? "true" : "false");
    } else if constexpr (std::is_unsigned_v<t_type>) {
        http_append_unsigned(output, value);
    } else if constexpr (std::is_integral_v<t_type>) {
        http_append_signed(output, value);
    } else if constexpr (std::is_floating_point_v<t_type>) {
        // JSON (RFC 8259) has no representation for infinity or NaN, and
        // std::to_chars would emit the bare tokens "inf"/"nan" — invalid JSON.
        // Serialize non-finite values as null, matching JSON.stringify.
        if (!std::isfinite(value)) {
            output.append("null");
            return;
        }
        http_append_formatted_number(output, value, "JSON number cannot be formatted");
    } else if constexpr (is_ruvia_bytes<t_type>) {
        append_model_binary(output, value);
    } else if constexpr (is_ruvia_string<t_type>) {
        append_json_string(output, value.view());
    } else if constexpr (is_model<t_type>) {
        model_json_access::append(output, value);
    } else if constexpr (is_ruvia_json_value<t_type> || is_ruvia_json_object<t_type>) {
        output.append(value.view());
    } else if constexpr (is_ruvia_array<t_type> || is_ruvia_boxed_array<t_type>) {
        append_json_sequence(output, value);
    } else {
        static_assert(
            always_false<t_type>, "JSON output must use Ruvia scalar types or RUVIA_MODEL");
    }
}

namespace model {

enum class serialized_field_kind { value,
    null };

// Both serialization passes share emission and comma decisions, with no
// intermediate representation or runtime field lookup.
template <typename model_type, typename visitor_type>
void visit_serialized_fields(const model_type& value, visitor_type&& visitor) {
    bool first = true;
    visit_model_fields(value, model_access::schema<model_type>(),
        [&](const auto&, const auto& slot) {
            const auto& field_value = slot.value();
            serialized_field_kind kind;
            if (field_value && !(slot.omit_empty() && is_empty_value(*field_value))) {
                kind = serialized_field_kind::value;
            } else if (!field_value && (slot.is_null() || slot.emit_null())) {
                kind = serialized_field_kind::null;
            } else {
                return;
            }
            visitor(slot, kind, !first);
            first = false;
        });
}

}  // namespace model

template <typename model_type>
std::size_t model_json_access::size_hint(const model_type& value) {
    std::size_t size = 2;
    model::visit_serialized_fields(value, [&size](const auto& slot, auto kind, bool comma) {
        size += slot.json_member_key(comma).size();
        size += kind == model::serialized_field_kind::value
                    ? json_size_hint_value(*slot.value())
                    : 4;
    });
    return size;
}

template <typename model_type>
void model_json_access::append(std::pmr::string& output, const model_type& value) {
    output.push_back('{');
    model::visit_serialized_fields(value, [&output](const auto& slot, auto kind, bool comma) {
        output.append(slot.json_member_key(comma));
        if (kind == model::serialized_field_kind::value) {
            append_json_value(output, *slot.value());
        } else {
            output.append("null");
        }
    });
    output.push_back('}');
}

}  // namespace ruvia::detail
