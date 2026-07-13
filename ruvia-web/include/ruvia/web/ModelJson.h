#pragma once

#include "ruvia/web/ModelTypes.h"
#include "ruvia/web/detail/model/JsonWriter.h"
#include "ruvia/core/memory/PmrResource.h"

#include <memory_resource>
#include <string>

namespace ruvia {

template <typename T>
void appendJson(std::pmr::string& output, const T& value) {
    output.reserve(output.size() + detail::jsonSizeHintValue(value));
    detail::appendJsonValue(output, value);
}

template <typename T>
[[nodiscard]] std::pmr::string toJson(
    const T& value,
    std::pmr::memory_resource* resource = nullptr) {
    std::pmr::string output(detail::pmrResourceOrDefault(resource));
    output.reserve(detail::jsonSizeHintValue(value));
    detail::appendJsonValue(output, value);
    return output;
}

}  // namespace ruvia
