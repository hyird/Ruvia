#pragma once

// Model-backed inline definitions for the public Context API.

#include "ruvia/web/Json.h"
#include "ruvia/web/ModelJson.h"
#include "ruvia/web/ModelObject.h"
#include "ruvia/web/ModelTypes.h"

#include <utility>

namespace ruvia {

template <typename T>
inline HttpResponse Context::json(const T& value) const {
    std::pmr::string body(allocator<char>());
    appendJson(body, value);
    return jsonSerialized(body);
}

template <typename BuildFn>
inline HttpResponse Context::jsonObject(BuildFn&& build) const {
    std::pmr::string body(allocator<char>());
    {
        JsonObjectWriter writer(body);
        std::forward<BuildFn>(build)(writer);
    }
    return jsonSerialized(body);
}

template <typename BuildFn>
inline HttpResponse Context::jsonArray(BuildFn&& build) const {
    std::pmr::string body(allocator<char>());
    {
        JsonArrayWriter writer(body);
        std::forward<BuildFn>(build)(writer);
    }
    return jsonSerialized(body);
}

}  // namespace ruvia
