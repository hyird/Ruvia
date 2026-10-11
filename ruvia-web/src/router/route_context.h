#pragma once

#include <cstdint>

#include "ruvia/web/context.h"

#include "context/context_access.h"
#include "context/context_services.h"
#include "router/route_table.h"

namespace ruvia {

// The context a matched route runs in, carrying its captured parameters and the
// route identity that scopes a rate limit.
[[nodiscard]] inline context make_route_context(request_memory& memory, const http_request& request,
    const detail::resolved_route& resolved, detail::context_services services) {
    const auto& route = resolved.route();
    const auto values = resolved.match().values();
    return detail::context_access::make(memory, request, route.path(), route.param_names().data(),
        values.data(), values.size(), reinterpret_cast<std::uintptr_t>(&route), services);
}

}  // namespace ruvia
