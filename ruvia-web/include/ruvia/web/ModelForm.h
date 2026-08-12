#pragma once

#include "ruvia/web/ModelObject.h"

#include <memory_resource>
#include <optional>
#include <string_view>

namespace ruvia {

template <typename T>
    requires FormBody<T>::value
[[nodiscard]] std::optional<T> fromForm(
    std::string_view body,
    std::pmr::memory_resource* resource = nullptr) {
    return detail::ModelParseAccess::parseFormOwned<T>(
        body, detail::pmrResourceOrDefault(resource));
}

}  // namespace ruvia
