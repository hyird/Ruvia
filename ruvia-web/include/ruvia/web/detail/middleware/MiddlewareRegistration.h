#pragma once

#include <memory_resource>
#include <tuple>
#include <type_traits>
#include <utility>

#include "ruvia/core/memory/PmrObject.h"
#include "ruvia/web/Context.h"
#include "ruvia/web/Middleware.h"
#include "ruvia/web/detail/util/RegistrationResource.h"
#include "ruvia/web/detail/http/context/RequestBindings.h"
#include "ruvia/web/detail/middleware/MiddlewareDescriptor.h"

namespace ruvia::detail {

template <typename MiddlewareT>
concept VoidHandleMiddleware = requires { static_cast<Task<void> (MiddlewareT::*)(Context&, Next&)>(&MiddlewareT::handle); };

template <typename MiddlewareT>
concept ResponseHandleMiddleware = requires { static_cast<Task<HttpResponse> (MiddlewareT::*)(Context&, Next&)>(&MiddlewareT::handle); };

template <typename MiddlewareT>
[[nodiscard]] Task<void> invokeResponseMiddleware(void* target, Context& context, Next& next) {
    auto* middleware = static_cast<MiddlewareT*>(target);
    auto response = co_await middleware->handle(context, next);
    context.respond(std::move(response));
}

template <typename MiddlewareT>
[[nodiscard]] Task<void> invokeMiddleware(void* target, Context& context, Next& next) {
    auto* middleware = static_cast<MiddlewareT*>(target);
    if constexpr (VoidHandleMiddleware<MiddlewareT>) {
        return middleware->handle(context, next);
    } else if constexpr (ResponseHandleMiddleware<MiddlewareT>) {
        return invokeResponseMiddleware<MiddlewareT>(target, context, next);
    } else {
        static_assert(VoidHandleMiddleware<MiddlewareT> || ResponseHandleMiddleware<MiddlewareT>,
            "middleware must implement async Task<void> or Task<HttpResponse> handle(Context&, "
            "ruvia::Next&)");
    }
}

// Constructs the middleware from the arguments use<T>(args...) captured. The
// tuple lives on the registration resource for the process lifetime, so it is
// still readable every time the router materializes an instance -- including
// across a stop()/run() cycle, which rebuilds instances from the same
// descriptors. Registering without arguments is the empty-tuple case, not a
// separate path: std::apply then calls the default constructor.
template <typename MiddlewareT, typename ArgsT>
[[nodiscard]] void* createMiddleware(const void* args) {
    return std::apply([](const auto&... values) { return static_cast<void*>(constructPmrObject<MiddlewareT>(registrationResource(), values...)); }, *static_cast<const ArgsT*>(args));
}

template <typename MiddlewareT>
void destroyMiddleware(void* target) noexcept {
    destroyPmrObject(static_cast<MiddlewareT*>(target), registrationResource());
}

template <typename MiddlewareT>
[[nodiscard]] const void* middlewareValidatedModelTypeKey() noexcept {
    if constexpr (requires { typename MiddlewareT::RuviaValidationBody; }) {
        return requestBindingTypeKey<typename MiddlewareT::RuviaValidationBody>();
    } else {
        return nullptr;
    }
}

template <typename MiddlewareT>
[[nodiscard]] constexpr bool middlewareUsesRouteRateLimit() noexcept {
    if constexpr (requires { MiddlewareT::ruviaUsesRouteRateLimit; }) {
        return MiddlewareT::ruviaUsesRouteRateLimit;
    } else {
        return false;
    }
}

// Whether this middleware is meaningful on a request that matched no route.
// A property of the middleware, not of where it is registered: security headers
// and request ids belong on a 404 response just as much as on a 200, while a
// validator or an authorization check has nothing to act on. Declared as
//     static constexpr bool ruviaRunsOnUnmatchedRequests = true;
template <typename MiddlewareT>
[[nodiscard]] constexpr bool middlewareRunsOnUnmatchedRequests() noexcept {
    if constexpr (requires { MiddlewareT::ruviaRunsOnUnmatchedRequests; }) {
        return MiddlewareT::ruviaRunsOnUnmatchedRequests;
    } else {
        return false;
    }
}

// Registers one middleware type together with the arguments every instance of
// it is constructed from. Arguments are decayed and copied once, at
// registration; a middleware is built per router materialization, so they must
// stay readable for the process lifetime rather than the caller's scope.
// Registering without arguments is the zero-argument case of this, so there is
// one descriptor shape and one construction path.
// Copies text onto the process registration resource so a descriptor can hold a
// view of it. The caller's argument may be a temporary; the descriptor outlives
// every route table built from it.
[[nodiscard]] inline std::string_view retainRegistrationText(std::string_view text) {
    if (text.empty()) {
        return {};
    }
    const auto* stored = constructPmrObject<std::pmr::string>(registrationResource(), text, registrationResource());
    return std::string_view(*stored);
}

template <typename MiddlewareT, typename... Args>
[[nodiscard]] ControllerMiddlewareDescriptor makeMiddlewareDescriptor(Args&&... args) {
    static_assert(std::is_base_of_v<Middleware<MiddlewareT>, MiddlewareT>, "middleware must derive from ruvia::Middleware<MiddlewareT>");
    static_assert(std::is_final_v<MiddlewareT>, "middleware must be final");
    static_assert(std::is_constructible_v<MiddlewareT, const std::decay_t<Args>&...>, "middleware is not constructible from the arguments passed to use<T>(); a middleware registered without arguments must be default constructible");

    using ArgsT = std::tuple<std::decay_t<Args>...>;
    const auto* stored = constructPmrObject<ArgsT>(registrationResource(), std::forward<Args>(args)...);
    return ControllerMiddlewareDescriptor(&invokeMiddleware<MiddlewareT>, &createMiddleware<MiddlewareT, ArgsT>, &destroyMiddleware<MiddlewareT>, stored, middlewareValidatedModelTypeKey<MiddlewareT>(), middlewareUsesRouteRateLimit<MiddlewareT>(), middlewareRunsOnUnmatchedRequests<MiddlewareT>());
}

}  // namespace ruvia::detail
