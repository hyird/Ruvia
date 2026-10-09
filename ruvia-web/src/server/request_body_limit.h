#pragma once

#include <cstddef>
#include <optional>

#include "ruvia/http/protocol_byte_limit.h"
#include "ruvia/web/detail/router/route_modes.h"

namespace ruvia::detail {

// The byte ceiling that applies to a request body given its route mode: stream
// routes use the optional stream limit, every other route uses the required
// positive buffered limit. The protocol-facing result has no numeric sentinel.
// `route_limit` is a ceiling the route itself declared (0 for none). It can only
// tighten the server's: a route must not be able to raise the deployment-wide
// bound, and it makes an otherwise unlimited stream route bounded.
[[nodiscard]] inline protocol_byte_limit request_body_byte_limit(request_body_mode body_mode,
    const std::optional<std::size_t>& max_stream_body_bytes, std::size_t max_buffered_body_bytes,
    std::size_t route_limit = 0) {
    if (body_mode != request_body_mode::stream) {
        const auto limit = route_limit != 0 && route_limit < max_buffered_body_bytes
                               ? route_limit
                               : max_buffered_body_bytes;
        return protocol_byte_limit::limited(limit);
    }
    if (!max_stream_body_bytes.has_value()) {
        return route_limit != 0 ? protocol_byte_limit::limited(route_limit)
                                : protocol_byte_limit::unlimited();
    }
    const auto limit =
        route_limit != 0 && route_limit < *max_stream_body_bytes ? route_limit : *max_stream_body_bytes;
    return protocol_byte_limit::limited(limit);
}

}  // namespace ruvia::detail
