#include <array>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <span>
#include <string_view>
#include <variant>
#include <vector>

#include "ruvia/http/http2_connection.h"
#include "ruvia/http/http_client_response_head.h"
#include "ruvia/http/websocket_client_negotiation.h"

#include "parser/http_header_descriptor_storage.h"

namespace ruvia {

namespace {

// The one RFC 8441 request builder behind both submit_http2_websocket_client_request()
// overloads (owning and borrowed application headers).
template <class header_type>
std::variant<std::uint32_t, http2_request_head_submit_error> submit_websocket_extended_connect(
    http2_connection& connection, const websocket_client_negotiation& negotiation, std::string_view scheme,
    std::string_view authority, std::string_view target, std::span<const header_type> headers) {
    const auto negotiation_fields = negotiation.request_fields();
    detail::http_header_descriptor_storage<http_header_view> staged_headers(negotiation.resource());
    staged_headers.reserve(negotiation_fields.size() + headers.size());
    for (const auto& field : negotiation_fields) {
        staged_headers.append(http_header_view{field.name(), field.value()});
    }
    for (const auto& field : headers) {
        staged_headers.append(http_header_view{field.name(), field.value()});
    }
    return connection.submit_request_head(http2_extended_connect_request_head_view{
        .protocol_ = "websocket",
        .scheme_ = scheme,
        .authority_ = authority,
        .target_ = target,
        .headers_ = staged_headers.view(),
    });
}

}  // namespace

std::variant<std::uint32_t, http2_request_head_submit_error> submit_http2_websocket_client_request(
    http2_connection& connection, const websocket_client_negotiation& negotiation, std::string_view scheme,
    std::string_view authority, std::string_view target, std::span<const http_header> headers) {
    return submit_websocket_extended_connect(connection, negotiation, scheme, authority, target, headers);
}

std::variant<std::uint32_t, http2_request_head_submit_error> submit_http2_websocket_client_request(
    http2_connection& connection, const websocket_client_negotiation& negotiation, std::string_view scheme,
    std::string_view authority, std::string_view target, std::span<const http_header_view> headers) {
    return submit_websocket_extended_connect(connection, negotiation, scheme, authority, target, headers);
}

std::variant<websocket_client_negotiation_result_view, websocket_client_handshake_error>
validate_http2_websocket_client_response(
    const websocket_client_negotiation& negotiation, const http_client_response_head& head, bool stream_open) {
    if (!head.status().is_successful()) {
        return websocket_client_handshake_error::response_status;
    }
    if (!stream_open) {
        return websocket_client_handshake_error::stream_closed;
    }
    return detail::validate_websocket_client_selection(negotiation, head.headers(), true);
}

}  // namespace ruvia
