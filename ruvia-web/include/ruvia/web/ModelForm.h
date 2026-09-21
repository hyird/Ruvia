#pragma once

#include <memory_resource>
#include <optional>
#include <string_view>

#include "ruvia/web/detail/model/Traits.h"
#include "ruvia/web/detail/model/parse/JsonParser.h"

namespace ruvia {

template <typename T>
    requires detail::isModel<T>
[[nodiscard]] std::optional<T> fromForm(std::string_view body, ModelParseOptions options = {}) {
    return detail::ModelParseAccess::parseFormOwned<T>(
        body, detail::pmrResourceOrDefault(options.resource));
}

}  // namespace ruvia
