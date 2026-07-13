#pragma once

// Model-backed inline definitions for the public Context API.

#include "ruvia/web/ModelJson.h"
#include "ruvia/web/ModelObject.h"
#include "ruvia/web/ModelTypes.h"

namespace ruvia {

template <typename T>
inline HttpResponse Context::json(
    const T& value,
    std::optional<std::uint16_t> statusCode) const {
    std::pmr::string body(allocator<char>());
    appendJson(body, value);
    return jsonSerialized(body, statusCode);
}

template <typename T>
inline HttpResponse Context::json(
    const T& value,
    std::optional<std::uint16_t> statusCode,
    std::span<const HttpHeaderView> headers) const {
    auto response = json(value, statusCode);
    applyExplicitResponseHeaders(response, headers);
    return response;
}

template <typename T>
inline HttpResponse Context::json(const T& value, ResponseInit init) const {
    auto response = json(value, init.status);
    applyExplicitResponseHeaders(response, init.headers);
    return response;
}

}  // namespace ruvia
