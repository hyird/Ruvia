#pragma once

#include <memory_resource>
#include <string_view>
#include <utility>

#include "ruvia/web/ModelObject.h"
#include "ruvia/web/detail/model/parse/RequestFieldVisitors.h"

namespace ruvia::detail {

template <typename Visitor>
[[nodiscard]] bool visitModelInputJsonFields(const ModelInput& input, Visitor&& visitor) {
    return visitJsonObjectFields(
        ResolvedPmrResourceTag{}, input.view(), input.resource(), std::forward<Visitor>(visitor));
}

template <typename Visitor>
[[nodiscard]] bool visitModelInputFormFields(const ModelInput& input, Visitor&& visitor) {
    if (input.kind() == ModelInputKind::kFormFields) {
        const auto* const fields = input.fields();
        return fields == nullptr ? true
                                 : visitDecodedFormFields(*fields, std::forward<Visitor>(visitor));
    }
    return visitFormObjectFields(
        ResolvedPmrResourceTag{}, input.view(), input.resource(), std::forward<Visitor>(visitor));
}

}  // namespace ruvia::detail
