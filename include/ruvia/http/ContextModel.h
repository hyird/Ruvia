#pragma once

#include "ruvia/http/Context.h"
#include "ruvia/http/ModelJson.h"
#include "ruvia/http/ModelTypes.h"

namespace ruvia {

template <typename T>
Task<T> Context::json() const {
    static_assert(JsonBody<T>::value, "JSON body type must use RUVIA_MODEL");
    if (!requestContentTypeMatches("application/json")) {
        detail::throwInvalidJsonContentType();
    }
    const auto requestBody = co_await body();
    auto parsed = JsonBody<T>::parse(requestBody, resource());
    if (!parsed) {
        detail::throwInvalidJsonBody();
    }
    co_return std::move(*parsed);
}

template <typename T>
Task<T> Context::form() const {
    static_assert(FormBody<T>::value, "form body type must use RUVIA_MODEL");
    if (!requestContentTypeMatches("application/x-www-form-urlencoded")) {
        detail::throwInvalidFormContentType();
    }
    const auto requestBody = co_await body();
    auto parsed = FormBody<T>::parse(requestBody, resource());
    if (!parsed) {
        detail::throwInvalidFormBody();
    }
    co_return std::move(*parsed);
}

template <typename T>
inline HttpResponse Context::json(
    const T& value,
    std::uint16_t statusCode,
    std::string_view statusText) const {
    std::pmr::string body(allocator<char>());
    appendJson(body, value);
    return jsonSerialized(body, statusCode, statusText);
}

}  // namespace ruvia
