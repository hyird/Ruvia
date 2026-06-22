#pragma once

#include "ruvia/http/ModelTypes.h"
#include "ruvia/http/detail/model/JsonWriter.h"

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
    std::pmr::memory_resource* resource = std::pmr::get_default_resource()) {
    std::pmr::string output(resource);
    output.reserve(detail::jsonSizeHintValue(value));
    detail::appendJsonValue(output, value);
    return output;
}

}  // namespace ruvia
