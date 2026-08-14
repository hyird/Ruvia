#pragma once

// Model-backed inline definitions for the public Context API.

#include "ruvia/web/ModelJson.h"
#include "ruvia/web/ModelObject.h"
#include "ruvia/web/ModelTypes.h"

namespace ruvia {

template <typename T>
    requires detail::isResponseModel<T>
inline HttpResponse Context::json(const T& value) const {
    auto body = toJson(value, resource());
    return jsonSerialized(body);
}

}  // namespace ruvia
