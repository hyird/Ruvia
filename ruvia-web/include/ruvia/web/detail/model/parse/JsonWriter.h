#pragma once

#include <charconv>
#include <cmath>
#include <cstddef>
#include <limits>
#include <memory_resource>
#include <string>
#include <system_error>
#include <type_traits>

#include "ruvia/web/detail/json/JsonEscape.h"
#include "ruvia/web/detail/model/ModelBinary.h"
#include "ruvia/web/detail/model/ModelSchema.h"
#include "ruvia/web/detail/model/Traits.h"

namespace ruvia::detail {

struct ModelJsonAccess final {
    template <typename ModelT>
    [[nodiscard]] static std::size_t sizeHint(const ModelT& model);

    template <typename ModelT>
    static void append(std::pmr::string& output, const ModelT& model);
};

template <typename ValueT>
[[nodiscard]] std::size_t jsonSizeHintValue(const ValueT& value) {
    using T = std::remove_cvref_t<ValueT>;
    if constexpr (isRuviaScalar<T>) {
        return jsonSizeHintValue(value.value);
    } else if constexpr (std::is_same_v<T, bool>) {
        return value ? 4 : 5;
    } else if constexpr (std::is_integral_v<T>) {
        return static_cast<std::size_t>(std::numeric_limits<T>::digits10) + 3;
    } else if constexpr (std::is_floating_point_v<T>) {
        return 32;
    } else if constexpr (isRuviaBytes<T>) {
        return ruvia::base64EncodedSize(value.size()) + 2;
    } else if constexpr (isRuviaString<T>) {
        return jsonStringSizeHint(value.view());
    } else if constexpr (isModel<T>) {
        return ModelJsonAccess::sizeHint(value);
    } else if constexpr (isRuviaArray<T> || isRuviaBoxedArray<T>) {
        std::size_t size = 2;
        bool first = true;
        for (const auto& item : value) {
            if (!first) {
                ++size;
            }
            first = false;
            size += jsonSizeHintValue(item);
        }
        return size;
    } else {
        static_assert(
            alwaysFalse<T>, "JSON output must use Ruvia scalar types or RUVIA_MODEL");
    }
}

template <typename ValueT>
void appendJsonValue(std::pmr::string& output, const ValueT& value);

template <typename SequenceT>
void appendJsonSequence(std::pmr::string& output, const SequenceT& value) {
    output.push_back('[');
    bool first = true;
    for (const auto& item : value) {
        if (!first) {
            output.push_back(',');
        }
        first = false;
        appendJsonValue(output, item);
    }
    output.push_back(']');
}

template <typename ValueT>
void appendJsonValue(std::pmr::string& output, const ValueT& value) {
    using T = std::remove_cvref_t<ValueT>;
    if constexpr (isRuviaScalar<T>) {
        appendJsonValue(output, value.value);
    } else if constexpr (std::is_same_v<T, bool>) {
        output.append(value ? "true" : "false");
    } else if constexpr (std::is_integral_v<T> || std::is_floating_point_v<T>) {
        if constexpr (std::is_floating_point_v<T>) {
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
    } else if constexpr (isRuviaBytes<T>) {
        appendModelBinary(output, value);
    } else if constexpr (isRuviaString<T>) {
        appendJsonString(output, value.view());
    } else if constexpr (isModel<T>) {
        ModelJsonAccess::append(output, value);
    } else if constexpr (isRuviaArray<T> || isRuviaBoxedArray<T>) {
        appendJsonSequence(output, value);
    } else {
        static_assert(
            alwaysFalse<T>, "JSON output must use Ruvia scalar types or RUVIA_MODEL");
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
    visitModelFields(value, model_access::schema<model_type>(),
        [&](const auto&, const auto& slot) {
            const auto& field_value = slot.value();
            serialized_field_kind kind;
            if (field_value && !(slot.omitEmpty() && isEmptyValue(*field_value))) {
                kind = serialized_field_kind::value;
            } else if (!field_value && (slot.isNull() || slot.emitNull())) {
                kind = serialized_field_kind::null;
            } else {
                return;
            }
            visitor(slot, kind, !first);
            first = false;
        });
}

}  // namespace model

template <typename ModelT>
std::size_t ModelJsonAccess::sizeHint(const ModelT& value) {
    std::size_t size = 2;
    model::visit_serialized_fields(value, [&size](const auto& slot, auto kind, bool comma) {
        size += std::size_t{comma} + jsonStringSizeHint(slot.wireName()) + 1;
        size += kind == model::serialized_field_kind::value
                    ? jsonSizeHintValue(*slot.value())
                    : 4;
    });
    return size;
}

template <typename ModelT>
void ModelJsonAccess::append(std::pmr::string& output, const ModelT& value) {
    output.push_back('{');
    model::visit_serialized_fields(value, [&output](const auto& slot, auto kind, bool comma) {
        if (comma) {
            output.push_back(',');
        }
        appendJsonString(output, slot.wireName());
        output.push_back(':');
        if (kind == model::serialized_field_kind::value) {
            appendJsonValue(output, *slot.value());
        } else {
            output.append("null");
        }
    });
    output.push_back('}');
}

}  // namespace ruvia::detail
