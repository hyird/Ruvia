#pragma once

#include "ruvia/web/ModelObject.h"

#include <memory_resource>
#include <optional>
#include <string_view>

namespace ruvia {

template <typename T>
    requires FormBody<T>::value
[[nodiscard]] std::optional<T> fromForm(std::string_view body, ModelParseOptions options = {}) {
    return detail::ModelParseAccess::parseFormOwned<T>(body, detail::pmrResourceOrDefault(options.resource));
}

}  // namespace ruvia
