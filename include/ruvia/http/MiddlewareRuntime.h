#pragma once

#include <concepts>
#include <memory>
#include <memory_resource>
#include <type_traits>

#include "ruvia/http/MiddlewareDescriptor.h"

namespace ruvia {

namespace detail {

[[nodiscard]] std::pmr::memory_resource* middlewareRuntimeResource() noexcept;

template <typename T>
concept TaskHttpResponse = std::same_as<std::remove_cvref_t<T>, Task<HttpResponse>>;

template <typename MiddlewareT>
concept AwaitableHandleMiddleware = requires(
    MiddlewareT middleware,
    Context& context,
    const Next& next) {
    { middleware.handle(context, next) } -> TaskHttpResponse;
};

template <typename MiddlewareT>
[[nodiscard]] Task<HttpResponse> invokeMiddleware(
    void* target,
    Context& context,
    const Next& next) {
    auto* middleware = static_cast<MiddlewareT*>(target);
    if constexpr (AwaitableHandleMiddleware<MiddlewareT>) {
        return middleware->handle(context, next);
    } else {
        static_assert(
            AwaitableHandleMiddleware<MiddlewareT>,
            "middleware must implement async handle(Context&, const ruvia::Next&)");
    }
}

template <typename MiddlewareT>
[[nodiscard]] void* createMiddleware() {
    auto* resource = middlewareRuntimeResource();
    auto* storage = resource->allocate(sizeof(MiddlewareT), alignof(MiddlewareT));
    auto* middleware = static_cast<MiddlewareT*>(storage);
    try {
        std::construct_at(middleware);
    } catch (...) {
        resource->deallocate(storage, sizeof(MiddlewareT), alignof(MiddlewareT));
        throw;
    }
    return middleware;
}

template <typename MiddlewareT>
void destroyMiddleware(void* target) noexcept {
    if (target == nullptr) {
        return;
    }
    auto* middleware = static_cast<MiddlewareT*>(target);
    std::destroy_at(middleware);
    middlewareRuntimeResource()->deallocate(middleware, sizeof(MiddlewareT), alignof(MiddlewareT));
}

template <typename MiddlewareT>
[[nodiscard]] ControllerMiddlewareDescriptor makeMiddlewareDescriptor();

}  // namespace detail

template <typename MiddlewareT>
class Middleware {
public:
    using RuviaMiddlewareType = MiddlewareT;

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
