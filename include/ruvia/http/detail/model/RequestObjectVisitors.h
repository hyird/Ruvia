#pragma once

#include <memory_resource>
#include <string_view>
#include <utility>

#include "ruvia/http/detail/model/RequestFieldVisitors.h"

namespace ruvia::detail {

template <typename Visitor>
[[nodiscard]] bool visitRequestJsonFields(const RequestObject& body, Visitor&& visitor) {
    return visitJsonObjectFields(body.view(), body.resource(), std::forward<Visitor>(visitor));
}

template <typename Visitor>
[[nodiscard]] bool visitRequestFormFields(const RequestObject& body, Visitor&& visitor) {
    return visitFormObjectFields(body.view(), body.resource(), std::forward<Visitor>(visitor));
}

}  // namespace ruvia::detail
