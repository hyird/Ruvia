#pragma once

// Model-backed templates for the request-only public facade. These depend only
// on ContextRequest's narrow bridge, so ContextRequest.h is self-contained and
// does not require the complete response/state Context definition.

#include "ruvia/web/ModelJson.h"
#include "ruvia/web/ModelObject.h"

namespace ruvia {

template <typename T>
Task<T> ContextRequest::json() const {
    static_assert(JsonBody<T>::value, "JSON body type must use RUVIA_REQUEST_MODEL");
    if (!contentTypeMatches("application/json")) {
        detail::throwInvalidJsonContentType();
    }
    const auto requestBody = co_await text();
    auto parsed = JsonBody<T>::parse(requestBody, resource());
    if (!parsed) {
        detail::throwInvalidJsonBody();
    }
    co_return std::move(*parsed);
}

template <typename T>
Task<T> ContextRequest::form() const {
    static_assert(FormBody<T>::value, "form body type must use RUVIA_REQUEST_MODEL");
    if (!contentTypeMatches("application/x-www-form-urlencoded")) {
        detail::throwInvalidFormContentType();
    }
    const auto requestBody = co_await text();
    auto parsed = FormBody<T>::parse(requestBody, resource());
    if (!parsed) {
        detail::throwInvalidFormBody();
    }
    co_return std::move(*parsed);
}

template <typename T>
inline const T& ContextRequest::valid() const {
    return validatedModels().get<T>();
}

}  // namespace ruvia
