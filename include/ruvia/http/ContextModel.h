#pragma once

#include "ruvia/http/Context.h"
#include "ruvia/http/ModelJson.h"
#include "ruvia/http/ModelTypes.h"

namespace ruvia {

template <typename T>
Task<T> ContextRequest::json() const {
    static_assert(JsonBody<T>::value, "JSON body type must use RUVIA_MODEL");
    if (!context_->requestContentTypeMatches("application/json")) {
        detail::throwInvalidJsonContentType();
    }
    const auto requestBody = co_await text();
    auto parsed = JsonBody<T>::parse(requestBody, context_->resource());
    if (!parsed) {
        detail::throwInvalidJsonBody();
    }
    co_return std::move(*parsed);
}

template <typename T>
Task<T> ContextRequest::form() const {
    static_assert(FormBody<T>::value, "form body type must use RUVIA_MODEL");
    if (!context_->requestContentTypeMatches("application/x-www-form-urlencoded")) {
        detail::throwInvalidFormContentType();
    }
    const auto requestBody = co_await text();
    auto parsed = FormBody<T>::parse(requestBody, context_->resource());
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

template <typename T>
inline HttpResponse Context::json(
    const T& value,
    std::uint16_t statusCode,
    std::span<const HttpHeaderView> headers) const {
    auto response = json(value, statusCode);
    applyExplicitResponseHeaders(response, headers);
    return response;
}

template <typename T>
inline HttpResponse Context::json(const T& value, ResponseInit init) const {
    auto response = json(value, init.status, init.statusText);
    applyExplicitResponseHeaders(response, init.headers);
    return response;
}

}  // namespace ruvia
