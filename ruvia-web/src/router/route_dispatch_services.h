#pragma once

#include <cstdint>

#include "ruvia/web/context.h"
#include "ruvia/web/error_handlers.h"

#include "context/context_access.h"
#include "context/context_services.h"
#include "router/route_table.h"

namespace ruvia {

// Every dispatch path hands the context the same three things: the table it may
// re-enter, and the error and not-found handlers that apply at this scope.
[[nodiscard]] inline detail::context_services with_route_handlers(detail::context_services services,
    const detail::route_table& routes_value, detail::http_error_handler_ref_type error_handler,
    detail::http_not_found_handler_ref_type not_found_handler) noexcept {
    return services.with_routes(routes_value)
        .with_error_handler(error_handler)
        .with_not_found_handler(not_found_handler);
}

// The context a matched route runs in, carrying its captured parameters and the
// route identity that scopes a rate limit.
[[nodiscard]] inline context make_route_context(request_memory& memory, const http_request& request,
    const detail::resolved_route& resolved, detail::context_services services) {
    const auto& route = resolved.route();
    const auto route_rate_limit_scope = reinterpret_cast<std::uintptr_t>(&route);
    const auto values = resolved.match().values();
    if (values.empty()) {
        return detail::context_access::make(
            memory, request, route.path(), route_rate_limit_scope, services);
    }

    return detail::context_access::make(memory, request, route.path(), route.param_names().data(),
        values.data(), values.size(), route_rate_limit_scope, services);
}

}  // namespace ruvia
