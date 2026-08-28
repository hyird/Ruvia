#pragma once

#include "ruvia/web/ModelTypes.h"
#include "ruvia/web/detail/model/parse/JsonWriter.h"
#include "ruvia/core/memory/PmrResource.h"

#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>

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

template <typename T>
    requires JsonBody<T>::value
[[nodiscard]] std::optional<T> fromJson(std::string_view body, ModelParseOptions options = {}) {
    return detail::ModelJsonAccess::parseOwned<T>(body, detail::pmrResourceOrDefault(options.resource));
}

template <typename T>
    requires detail::isResponseModel<T>
[[nodiscard]] inline std::pmr::string toJson(const T& value, ModelSerializeOptions options = {}) {
    std::pmr::string output(detail::pmrResourceOrDefault(options.resource));
    output.reserve(detail::jsonSizeHintValue(value));
    detail::appendJsonValue(output, value);
    return output;
}

}  // namespace ruvia
