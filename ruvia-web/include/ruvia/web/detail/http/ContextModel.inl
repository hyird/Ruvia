#pragma once

// Model-backed inline definitions for the public Context API.

#include "ruvia/web/ModelJson.h"
#include "ruvia/web/ModelObject.h"
#include "ruvia/web/ModelTypes.h"

namespace ruvia {

inline Task<JsonValue> ContextRequest::json() const {
    if (!context_->requestContentTypeMatches("application/json")) {
        detail::throwInvalidJsonContentType();
    }
    const auto requestBody = co_await text();
    auto parsed = JsonValue::parse(requestBody, context_->resource());
    if (!parsed) {
        detail::throwInvalidJsonBody();
    }
    co_return std::move(*parsed);
}

template <typename T>
Task<T> ContextRequest::json() const {
    static_assert(JsonBody<T>::value, "JSON body type must use RUVIA_REQUEST_MODEL");
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
    static_assert(FormBody<T>::value, "form body type must use RUVIA_REQUEST_MODEL");
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
