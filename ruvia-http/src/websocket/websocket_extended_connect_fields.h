#pragma once

#include <span>

#include "ruvia/http/http_header.h"
#include "ruvia/http/websocket_client_negotiation.h"

#include "parser/http_header_descriptor_storage.h"

namespace ruvia::detail {

// The one Extended CONNECT WebSocket bootstrap field list for HTTP/2 (RFC
// 8441) and HTTP/3 (RFC 9220): the negotiation's Sec-WebSocket-* request
// fields, then the application headers. The descriptors borrow both.
[[nodiscard]] inline http_header_descriptor_storage<http_header_view> stage_websocket_extended_connect_fields(
    const websocket_client_negotiation& negotiation, std::span<const http_header_view> headers) {
    const auto negotiation_fields = negotiation.request_fields();
    http_header_descriptor_storage<http_header_view> staged;
    staged.reserve(negotiation_fields.size() + headers.size());
    for (const auto& field : negotiation_fields) {
        staged.append(http_header_view{field.name(), field.value()});
    }
    for (const auto& field : headers) {
        staged.append(field);
    }
    return staged;
}

}  // namespace ruvia::detail
