#pragma once

#include <concepts>
#include <memory>
#include <memory_resource>
#include <type_traits>
#include <utility>

#include "ruvia/web/Context.h"
#include "ruvia/web/detail/RegistrationResource.h"
#include "ruvia/web/MiddlewareDescriptor.h"
#include "ruvia/core/memory/PmrObject.h"

namespace ruvia {

namespace detail {

template <typename T>
concept TaskVoid = std::same_as<std::remove_cvref_t<T>, Task<void>>;

template <typename T>
concept TaskHttpResponse = std::same_as<std::remove_cvref_t<T>, Task<HttpResponse>>;

template <typename MiddlewareT>
concept VoidHandleMiddleware = requires {
    static_cast<Task<void> (MiddlewareT::*)(Context&, Next&)>(&MiddlewareT::handle);
};

template <typename MiddlewareT>
concept ResponseHandleMiddleware = requires {
    static_cast<Task<HttpResponse> (MiddlewareT::*)(Context&, Next&)>(&MiddlewareT::handle);
};

template <typename MiddlewareT>
[[nodiscard]] Task<void> invokeResponseMiddleware(
    void* target,
    Context& context,
    Next& next) {
    auto* middleware = static_cast<MiddlewareT*>(target);
    auto response = co_await middleware->handle(context, next);
    context.res(std::move(response));
}

template <typename MiddlewareT>
[[nodiscard]] Task<void> invokeMiddleware(
    void* target,
    Context& context,
    Next& next) {
    auto* middleware = static_cast<MiddlewareT*>(target);
    if constexpr (VoidHandleMiddleware<MiddlewareT>) {
        return middleware->handle(context, next);
    } else if constexpr (ResponseHandleMiddleware<MiddlewareT>) {
        return invokeResponseMiddleware<MiddlewareT>(target, context, next);
    } else {
        static_assert(
            VoidHandleMiddleware<MiddlewareT> || ResponseHandleMiddleware<MiddlewareT>,
            "middleware must implement async Task<void> or Task<HttpResponse> handle(Context&, ruvia::Next&)");
    }
}

template <typename MiddlewareT>
[[nodiscard]] void* createMiddleware() {
    return constructPmrObject<MiddlewareT>(registrationResource());
}

template <typename MiddlewareT>
void destroyMiddleware(void* target) noexcept {
    destroyPmrObject(static_cast<MiddlewareT*>(target), registrationResource());
}

template <typename MiddlewareT>
[[nodiscard]] ControllerMiddlewareDescriptor makeMiddlewareDescriptor();

}  // namespace detail

template <typename MiddlewareT>
class Middleware {
protected:
    constexpr Middleware() noexcept = default;
    ~Middleware() = default;
};

namespace detail {

template <typename MiddlewareT>
[[nodiscard]] ControllerMiddlewareDescriptor makeMiddlewareDescriptor() {
    static_assert(
        std::is_base_of_v<Middleware<MiddlewareT>, MiddlewareT>,
        "middleware must derive from ruvia::Middleware<MiddlewareT>");
    static_assert(std::is_final_v<MiddlewareT>, "middleware must be final");
    static_assert(std::is_default_constructible_v<MiddlewareT>, "middleware must be default constructible");

    return ControllerMiddlewareDescriptor(
        &invokeMiddleware<MiddlewareT>,
        &createMiddleware<MiddlewareT>,
        &destroyMiddleware<MiddlewareT>);
}

}  // namespace detail

}  // namespace ruvia
