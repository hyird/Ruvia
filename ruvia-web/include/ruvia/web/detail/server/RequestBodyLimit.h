#pragma once

#include <cstddef>
#include <optional>

#include "ruvia/http/ProtocolByteLimit.h"
#include "ruvia/web/detail/router/RouteModes.h"

namespace ruvia::detail {

// The byte ceiling that applies to a request body given its route mode: stream
// routes use the optional stream limit, every other route uses the required
// positive buffered limit. The protocol-facing result has no numeric sentinel.
[[nodiscard]] inline ProtocolByteLimit requestBodyByteLimit(
    RequestBodyMode bodyMode,
    const std::optional<std::size_t>& maxStreamBodyBytes,
    std::size_t maxBufferedBodyBytes) {
    if (bodyMode != RequestBodyMode::kStream) {
        return ProtocolByteLimit::limited(maxBufferedBodyBytes);
    }
    return maxStreamBodyBytes.has_value()
        ? ProtocolByteLimit::limited(*maxStreamBodyBytes)
        : ProtocolByteLimit::unlimited();
}

}  // namespace ruvia::detail
