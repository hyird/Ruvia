#pragma once

#include <memory_resource>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/http/HttpCommon.h"
#include "ruvia/http/detail/model/FormParser.h"
#include "ruvia/http/detail/json/JsonObjectFields.h"
#include "ruvia/memory/PmrResource.h"

namespace ruvia::detail {

template <typename Visitor>
[[nodiscard]] bool visitRawFormFields(std::string_view body, Visitor&& visitor) {
    return visitUrlEncodedPairs(body, std::forward<Visitor>(visitor));
}

[[nodiscard]] inline bool formFieldNameEquals(
    std::string_view encodedName,
    std::string_view field) noexcept {
    return urlComponentEquals(encodedName, field, UrlDecodeMode::kForm);
}

template <typename Visitor>
[[nodiscard]] bool visitFormObjectFields(
    ResolvedPmrResourceTag,
    std::string_view body,
    std::pmr::memory_resource* resource,
    Visitor&& visitor) {
    bool valid = true;
    auto& visitorRef = visitor;
    const bool completed = visitRawFormFields(body, [&](std::string_view name, std::string_view value) {
        if (!hasFormEncoding(name)) {
            return dispatchJsonObjectFieldVisitor(visitorRef, name, value);
        }

        std::pmr::string decodedName(resource);
        if (!decodeFormComponent(name, decodedName)) {
            valid = false;
            return false;
        }
        return dispatchJsonObjectFieldVisitor(visitorRef, std::string_view(decodedName), value);
    });
    return completed && valid;
}

template <typename Visitor>
[[nodiscard]] bool visitFormObjectFields(
    std::string_view body,
    std::pmr::memory_resource* resource,
    Visitor&& visitor) {
    return visitFormObjectFields(
        ResolvedPmrResourceTag{},
        body,
        pmrResourceOrDefault(resource),
        std::forward<Visitor>(visitor));
}

template <typename Visitor>
[[nodiscard]] bool visitDecodedFormFields(
    const RequestNameValueList& fields,
    Visitor&& visitor) {
    auto& visitorRef = visitor;
    for (const auto& field : fields) {
        if (!dispatchJsonObjectFieldVisitor(visitorRef, field.name(), field.value())) {
            break;
        }
    }
    return true;
}

}  // namespace ruvia::detail
