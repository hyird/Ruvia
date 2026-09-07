#pragma once

#include "ruvia/web/detail/http/context/RequestBindings.h"

namespace ruvia {

template <std::size_t N>
inline HttpResponse Context::body(const char (&value)[N]) const {
    const auto size = N > 0 && value[N - 1] == '\0' ? N - 1 : N;
    return bodyStaticView(std::string_view(value, size));
}

template <std::size_t N>
inline HttpResponse Context::text(const char (&body)[N]) const {
    const auto size = N > 0 && body[N - 1] == '\0' ? N - 1 : N;
    return textStaticView(std::string_view(body, size));
}

template <std::size_t N>
inline HttpResponse Context::html(const char (&body)[N]) const {
    const auto size = N > 0 && body[N - 1] == '\0' ? N - 1 : N;
    return htmlStaticView(std::string_view(body, size));
}

template <typename T>
inline RequestStateBinding<T> Context::bindRequestState(const T& value) {
    return requestBindings().bindState(value);
}

template <typename T>
inline const std::remove_cvref_t<T>& Context::requestState() const {
    return requestBindings().getState<T>();
}

template <typename T>
inline const std::remove_cvref_t<T>* Context::tryRequestState() const noexcept {
    return requestBindings().tryGetState<T>();
}

}  // namespace ruvia

namespace ruvia::detail {

template <typename T>
inline RequestBindingHandle<T> bindValidatedModel(Context& context, const T& model) {
    return context.requestBindings().bindValidated(model);
}

template <typename T>
inline RequestBindingHandle<T> bindValidatedJsonModel(
    Context& context, const T& model, std::string_view rawJson) {
    return context.requestBindings().bindValidated(model, rawJson);
}

}  // namespace ruvia::detail
