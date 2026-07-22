#pragma once

#include <cstdint>

#include "ruvia/web/Context.h"
#include "ruvia/web/ErrorHandlers.h"
#include "ruvia/web/detail/http/context/ContextAccess.h"
#include "ruvia/web/detail/http/context/ContextServices.h"
#include "ruvia/web/detail/router/RouteTable.h"

namespace ruvia {

// Every dispatch path hands the Context the same three things: the table it may
// re-enter, and the error and not-found handlers that apply at this scope.
[[nodiscard]] inline detail::ContextServices withRouteHandlers(
    detail::ContextServices services,
    const detail::RouteTable& routes,
    HttpErrorHandler errorHandler,
    HttpNotFoundHandler notFoundHandler) noexcept {
    return services
        .withRoutes(routes)
        .withErrorHandler(errorHandler)
        .withNotFoundHandler(notFoundHandler);
}

// The Context a matched route runs in, carrying its captured parameters and the
// route identity that scopes a rate limit.
[[nodiscard]] inline Context makeRouteContext(
    RequestMemory& memory,
    const HttpRequest& request,
    const detail::ResolvedRoute& resolved,
    detail::ContextServices services) noexcept {
    const auto& route = resolved.route();
    const auto routeRateLimitScope = reinterpret_cast<std::uintptr_t>(&route);
    const auto values = resolved.match().values();
    if (values.empty()) {
        return detail::ContextAccess::make(
            memory,
            request,
            route.path(),
            routeRateLimitScope,
            services);
    }

    return detail::ContextAccess::make(
        memory,
        request,
        route.path(),
        route.paramNames().data(),
        values.data(),
        values.size(),
        routeRateLimitScope,
        services);
}

}  // namespace ruvia
