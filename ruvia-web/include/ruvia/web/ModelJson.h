#pragma once

#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>

#include "ruvia/core/memory/PmrResource.h"
#include "ruvia/web/ModelTypes.h"
#include "ruvia/web/detail/model/Traits.h"
#include "ruvia/web/detail/model/parse/JsonParser.h"
#include "ruvia/web/detail/model/parse/JsonWriter.h"

namespace ruvia {

namespace detail {
class RequestBindings;
}

// A request-scoped view of one validated JSON body. The typed value and the
// exact original bytes share the same middleware scope, allowing JSONB
// passthrough without a parse/serialize round trip.
template <typename T>
class ValidatedJson final {
public:
    [[nodiscard]] const T& value() const noexcept {
        return *value_;
    }

    [[nodiscard]] std::string_view raw() const noexcept {
        return raw_;
    }

private:
    friend class detail::RequestBindings;

    ValidatedJson(const T& value, std::string_view raw) noexcept
        : value_(&value),
          raw_(raw) {}

    const T* value_;
    std::string_view raw_;
};

// Owns parsed data in options.resource. Supports models, Ruvia scalar values,
// strings, bytes and arrays of these types. Request middleware evaluates field
// rules separately; the codec only checks the complete document's structure.
template <typename T>
    requires detail::isModelJsonValue<T>
[[nodiscard]] std::optional<T> fromJson(std::string_view body, ModelParseOptions options = {}) {
    return detail::parseJsonDocument<T>(body, detail::pmrResourceOrDefault(options.resource),
        detail::ModelStringStorage::kOwned);
}

// Serializes current values without invoking field rules or initializers.
template <typename T>
    requires detail::isModelJsonValue<T>
[[nodiscard]] inline std::pmr::string toJson(const T& value, ModelSerializeOptions options = {}) {
    std::pmr::string output(detail::pmrResourceOrDefault(options.resource));
    // MSVC reserve(n) can leave no room for the trailing NUL, so resize(n)
    // allocates again. One extra byte stays in the same allocation.
    output.reserve(detail::jsonSizeHintValue(value) + 1);
    detail::appendJsonValue(output, value);
    return output;
}

}  // namespace ruvia
