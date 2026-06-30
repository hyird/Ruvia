#pragma once

#include <memory_resource>
#include <string_view>
#include <utility>

#include "ruvia/http/detail/model/RequestFieldVisitors.h"

namespace ruvia::detail {

template <typename Visitor>
[[nodiscard]] bool visitRequestJsonFields(const RequestObject& body, Visitor&& visitor) {
    return visitJsonObjectFields(
        ResolvedPmrResourceTag{},
        body.view(),
        body.resource(),
        std::forward<Visitor>(visitor));
}

template <typename Visitor>
[[nodiscard]] bool visitRequestFormFields(const RequestObject& body, Visitor&& visitor) {
    return visitFormObjectFields(
        ResolvedPmrResourceTag{},
        body.view(),
        body.resource(),
        std::forward<Visitor>(visitor));
}

}  // namespace ruvia::detail
