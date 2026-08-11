#pragma once

#include "ruvia/web/ModelObject.h"

#include <memory_resource>
#include <optional>
#include <string_view>

namespace ruvia {

template <typename T>
[[nodiscard]] std::optional<T> fromForm(
    std::string_view body,
    std::pmr::memory_resource* resource = nullptr) {
    static_assert(FormBody<T>::value, "fromForm<T> requires a RUVIA_MODEL");
    return detail::ModelParseAccess::parseFormOwned<T>(
        body, detail::pmrResourceOrDefault(resource));
}

}  // namespace ruvia
