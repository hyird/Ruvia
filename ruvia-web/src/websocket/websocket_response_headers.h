#pragma once

#include "ruvia/web/context.h"

#include "context/context_access.h"
#include "http/inline_http_header_views.h"

namespace ruvia::detail {

// The context retains header storage through handshake construction. The
// protocol plan copies the views before this temporary pointer table is freed.
[[nodiscard]] inline inline_http_header_views<> websocket_response_headers(context& context_value) {
    return inline_http_header_views<>(
        context_access::handshake_headers(context_value), context_value.pool());
}

}  // namespace ruvia::detail
