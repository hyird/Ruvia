#pragma once

#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>

#include "ruvia/core/memory/pmr_resource.h"
#include "ruvia/web/detail/model/parse/json_parser.h"
#include "ruvia/web/detail/model/parse/json_writer.h"
#include "ruvia/web/detail/model/traits.h"
#include "ruvia/web/model_types.h"

namespace ruvia {

namespace detail {
class request_bindings;
}

// A request-scoped view of one validated JSON body. The typed value and the
// exact original bytes share the same middleware scope, allowing JSONB
// passthrough without a parse/serialize round trip.
template <typename t_type>
class validated_json final {
public:
    [[nodiscard]] const t_type& value() const noexcept {
        return *value_;
    }

    [[nodiscard]] std::string_view raw() const noexcept {
        return raw_;
    }

private:
    friend class detail::request_bindings;

    validated_json(const t_type& value, std::string_view raw) noexcept
        : value_(&value),
          raw_(raw) {}

    const t_type* value_;
    std::string_view raw_;
};

// Owns parsed data in options.resource. Supports models, Ruvia scalar values,
// strings, bytes and arrays of these types. Request middleware evaluates field
// rules separately; the codec only checks the complete document's structure.
template <typename t_type>
    requires detail::is_model_json_value<t_type>
[[nodiscard]] std::optional<t_type> from_json(std::string_view body, model_parse_options options = {}) {
    return detail::parse_json_document<t_type>(body, detail::pmr_resource_or_default(options.resource_),
        detail::model_string_storage::owned, options);
}

// Serializes current values without invoking field rules or initializers.
template <typename t_type>
    requires detail::is_model_json_value<t_type>
[[nodiscard]] inline std::pmr::string to_json(const t_type& value, model_serialize_options options = {}) {
    std::pmr::string output(detail::pmr_resource_or_default(options.resource_));
    // MSVC reserve(n) can leave no room for the trailing NUL, so resize(n)
    // allocates again. One extra byte stays in the same allocation.
    output.reserve(detail::json_size_hint_value(value) + 1);
    detail::append_json_value(output, value);
    return output;
}

}  // namespace ruvia
