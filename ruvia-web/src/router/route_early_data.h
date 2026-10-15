#pragma once

#include "ruvia/http/http_known_method.h"

#include "router/route_entry.h"
#include "router/route_resolution.h"

namespace ruvia::detail {

// RFC 8470 replay policy for a request carried by, or dispatched on behalf of,
// TLS early data: only content-free GET/HEAD requests resolved to a buffered
// route whose handler chain explicitly opted into replay may run.
[[nodiscard]] inline bool early_data_request_allowed(
    http_known_method method, bool has_content, const route_resolution& resolution) noexcept {
    const auto* resolved = resolution.resolved();
    return (method == http_known_method::get || method == http_known_method::head) && !has_content &&
           resolved != nullptr && resolved->route().endpoint().buffered() != nullptr &&
           resolved->route().endpoint().buffered()->replay_safe();
}

}  // namespace ruvia::detail
