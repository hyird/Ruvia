#pragma once

#include <cstddef>
#include <optional>

#include "ruvia/http/ProtocolByteLimit.h"
#include "ruvia/web/detail/router/RouteModes.h"

namespace ruvia::detail {

// The byte ceiling that applies to a request body given its route mode: stream
// routes use the optional stream limit, every other route uses the required
// positive buffered limit. The protocol-facing result has no numeric sentinel.
// `routeLimit` is a ceiling the route itself declared (0 for none). It can only
// tighten the server's: a route must not be able to raise the deployment-wide
// bound, and it makes an otherwise unlimited stream route bounded.
[[nodiscard]] inline ProtocolByteLimit requestBodyByteLimit(RequestBodyMode bodyMode,
    const std::optional<std::size_t>& maxStreamBodyBytes, std::size_t maxBufferedBodyBytes,
    std::size_t routeLimit = 0) {
    if (bodyMode != RequestBodyMode::kStream) {
        const auto limit = routeLimit != 0 && routeLimit < maxBufferedBodyBytes
                               ? routeLimit
                               : maxBufferedBodyBytes;
        return ProtocolByteLimit::limited(limit);
    }
    if (!maxStreamBodyBytes.has_value()) {
        return routeLimit != 0 ? ProtocolByteLimit::limited(routeLimit)
                               : ProtocolByteLimit::unlimited();
    }
    const auto limit =
        routeLimit != 0 && routeLimit < *maxStreamBodyBytes ? routeLimit : *maxStreamBodyBytes;
    return ProtocolByteLimit::limited(limit);
}

}  // namespace ruvia::detail
