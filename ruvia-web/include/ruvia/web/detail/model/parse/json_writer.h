#pragma once

#include <charconv>
#include <cmath>
#include <cstddef>
#include <limits>
#include <memory_resource>
#include <string>
#include <system_error>
#include <type_traits>

#include "ruvia/web/detail/json/json_escape.h"
#include "ruvia/web/detail/model/model_binary.h"
#include "ruvia/web/detail/model/model_schema.h"
#include "ruvia/web/detail/model/traits.h"

namespace ruvia::detail {

struct model_json_access final {
    template <typename model_t_type>
    [[nodiscard]] static std::size_t size_hint(const model_t_type& model);

    template <typename model_t_type>
    static void append(std::pmr::string& output, const model_t_type& model);
};

template <typename value_t_type>
[[nodiscard]] std::size_t json_size_hint_value(const value_t_type& value) {
    using t_type = std::remove_cvref_t<value_t_type>;
    if constexpr (is_ruvia_scalar<t_type>) {
        return json_size_hint_value(value.value_);
    } else if constexpr (std::is_same_v<t_type, bool>) {
        return value ? 4 : 5;
    } else if constexpr (std::is_integral_v<t_type>) {
        return static_cast<std::size_t>(std::numeric_limits<t_type>::digits10) + 3;
    } else if constexpr (std::is_floating_point_v<t_type>) {
        return 32;
    } else if constexpr (is_ruvia_bytes<t_type>) {
        return ruvia::base64_encoded_size(value.size()) + 2;
    } else if constexpr (is_ruvia_string<t_type>) {
        return json_string_size_hint(value.view());
    } else if constexpr (is_model<t_type>) {
        return model_json_access::size_hint(value);
    } else if constexpr (is_ruvia_array<t_type> || is_ruvia_boxed_array<t_type>) {
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

template <typename value_t_type>
void append_json_value(std::pmr::string& output, const value_t_type& value);

template <typename sequence_t_type>
void append_json_sequence(std::pmr::string& output, const sequence_t_type& value) {
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

template <typename value_t_type>
void append_json_value(std::pmr::string& output, const value_t_type& value) {
    using t_type = std::remove_cvref_t<value_t_type>;
    if constexpr (is_ruvia_scalar<t_type>) {
        append_json_value(output, value.value_);
    } else if constexpr (std::is_same_v<t_type, bool>) {
        output.append(value ? "true" : "false");
    } else if constexpr (std::is_integral_v<t_type> || std::is_floating_point_v<t_type>) {
        if constexpr (std::is_floating_point_v<t_type>) {
            // JSON (RFC 8259) has no representation for infinity or NaN, and
            // std::to_chars would emit the bare tokens "inf"/"nan" — invalid JSON.
            // Serialize non-finite values as null, matching JSON.stringify.
            if (!std::isfinite(value)) {
                output.append("null");
                return;
            }
        }
        char buffer[64];
        const auto [ptr, ec] = std::to_chars(buffer, buffer + sizeof(buffer), value);
        if (ec == std::errc{}) {
            output.append(buffer, static_cast<std::size_t>(ptr - buffer));
        }
    } else if constexpr (is_ruvia_bytes<t_type>) {
        append_model_binary(output, value);
    } else if constexpr (is_ruvia_string<t_type>) {
        append_json_string(output, value.view());
    } else if constexpr (is_model<t_type>) {
        model_json_access::append(output, value);
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

template <typename model_t_type>
std::size_t model_json_access::size_hint(const model_t_type& value) {
    std::size_t size = 2;
    model::visit_serialized_fields(value, [&size](const auto& slot, auto kind, bool comma) {
        size += std::size_t{comma} + json_string_size_hint(slot.wire_name()) + 1;
        size += kind == model::serialized_field_kind::value
                    ? json_size_hint_value(*slot.value())
                    : 4;
    });
    return size;
}

template <typename model_t_type>
void model_json_access::append(std::pmr::string& output, const model_t_type& value) {
    output.push_back('{');
    model::visit_serialized_fields(value, [&output](const auto& slot, auto kind, bool comma) {
        if (comma) {
            output.push_back(',');
        }
        append_json_string(output, slot.wire_name());
        output.push_back(':');
        if (kind == model::serialized_field_kind::value) {
            append_json_value(output, *slot.value());
        } else {
            output.append("null");
        }
    });
    output.push_back('}');
}

}  // namespace ruvia::detail
