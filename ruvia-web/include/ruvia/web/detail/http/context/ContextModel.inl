#pragma once

// Model-backed inline definitions for the public Context API.

#include "ruvia/web/ModelJson.h"
#include "ruvia/web/ModelObject.h"
#include "ruvia/web/ModelTypes.h"

namespace ruvia {

template <typename T>
inline HttpResponse Context::json(const T& value) const {
    std::pmr::string body(allocator<char>());
    appendJson(body, value);
    return jsonSerialized(body);
}

}  // namespace ruvia
