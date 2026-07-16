#pragma once

// Model-backed templates for the request-only public facade. These depend only
// on ContextRequest's narrow bridge, so ContextRequest.h is self-contained and
// does not require the complete response/state Context definition.

#include "ruvia/web/ModelJson.h"
#include "ruvia/web/ModelObject.h"

namespace ruvia {

template <typename T>
Task<T> ContextRequest::jsonModelTask(const Context* context) {
    static_assert(JsonBody<T>::value, "JSON body type must use RUVIA_REQUEST_MODEL");
    if (!contextContentTypeMatches(context, "application/json")) {
        detail::throwInvalidJsonContentType();
    }
    const auto requestBody = co_await contextTextTask(context);
    auto parsed = JsonBody<T>::parse(requestBody, contextResource(context));
    if (!parsed) {
        detail::throwInvalidJsonBody();
    }
    co_return std::move(*parsed);
}

template <typename T>
ScopedOperation<T> ContextRequest::json() const {
    return detail::makeScopedOperation(
        contextOperationScope(context_), jsonModelTask<T>(context_));
}

template <typename T>
Task<T> ContextRequest::formModelTask(const Context* context) {
    static_assert(FormBody<T>::value, "form body type must use RUVIA_REQUEST_MODEL");
    if (!contextContentTypeMatches(context, "application/x-www-form-urlencoded")) {
        detail::throwInvalidFormContentType();
    }
    const auto requestBody = co_await contextTextTask(context);
    auto parsed = FormBody<T>::parse(requestBody, contextResource(context));
    if (!parsed) {
        detail::throwInvalidFormBody();
    }
    co_return std::move(*parsed);
}

template <typename T>
ScopedOperation<T> ContextRequest::form() const {
    return detail::makeScopedOperation(
        contextOperationScope(context_), formModelTask<T>(context_));
}

template <typename T>
inline const T& ContextRequest::valid() const {
    return validatedModels().get<T>();
}

}  // namespace ruvia
