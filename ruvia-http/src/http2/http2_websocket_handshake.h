#pragma once

#include <algorithm>
#include <cstddef>
#include <memory_resource>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/http/detail/parser/http_parser_syntax.h"
#include "ruvia/http/http_request.h"
#include "ruvia/http/websocket_handshake.h"

#include "http2/http2_field_list.h"
#include "http2/http2_header_list_size.h"
#include "http2/http2_header_rules.h"
#include "http2/http2_remote_receive_semantics.h"
#include "http2/http2_response_headers.h"
#include "http2/http2_stream_state.h"
#include "request/http_request_access.h"
#include "websocket/http_websocket_handshake_fields.h"
#include "websocket/websocket_server_negotiation.h"

namespace ruvia::detail {

[[nodiscard]] inline bool http2_is_pending_websocket_connect(const http2_stream_state& stream) noexcept {
    const auto* pending = stream.tunnel().pending();
    return pending != nullptr && pending->form() == http2_connect_form::extended &&
           stream.protocol_is_websocket();
}

[[nodiscard]] inline bool http2_websocket_connect_remote_head_ended(
    const http2_stream_state& stream) noexcept {
    const auto& remote = stream.remote_receive();
    const auto* pending_end = remote.connect_pending_end_stream();
    return remote.head_end_stream_pending() != nullptr ||
           (pending_end != nullptr && pending_end->ended_without_tunnel_data());
}

[[nodiscard]] inline bool http2_websocket_connect_content_acceptable(
    const http2_stream_state& stream) noexcept {
    const auto* known = stream.remote_content().allowed_known_length();
    return stream.remote_content().allowed_without_length() != nullptr ||
           (known != nullptr && known->declared_length() == 0);
}

[[nodiscard]] inline websocket_handshake_validation_result validate_http2_websocket_handshake(
    const http2_stream_state& stream, const http_request& request,
    websocket_handshake_validation_options options = {}) noexcept {
    if (!http2_is_pending_websocket_connect(stream) || !http2_remote_final_head_decoded(stream) ||
        http2_websocket_connect_remote_head_ended(stream) ||
        !http2_websocket_connect_content_acceptable(stream)) {
        return websocket_handshake_validation_result::make_invalid_request();
    }

    // RFC 8441 section 5: :protocol supersedes the RFC 6455 key exchange, so
    // Sec-WebSocket-Key and Sec-WebSocket-Accept are not processed here.
    websocket_request_header_validator validator(http_field_list_role::recipient);
    const auto headers = request.headers();
    for (std::size_t i = 0; i < headers.size(); ++i) {
        const auto kind = http_request_access::header_kind(request, i);
        if (!validator.observe_header(kind, headers[i].value())) {
            return websocket_handshake_validation_result::make_invalid_request();
        }
    }
    return validator.finish(options);
}

inline void http2_append_websocket_handshake_headers(
    http2_field_list& fields, const websocket_server_negotiation& negotiation) {
    fields.clear();
    const auto fields_value = negotiation.response_headers();
    const bool has_date = std::ranges::any_of(fields_value, [](const http_header& field) {
        return field.name() == "date";
    });
    const auto date = has_date ? std::string_view{} : cached_date_value();
    const auto subprotocol = negotiation.subprotocol();
    const auto extensions = negotiation.extensions();
    http2_header_list_size section_size;
    if (!section_size.add(":status", "200") ||
        (!date.empty() && !section_size.add("date", date)) ||
        (!subprotocol.empty() && !section_size.add("sec-websocket-protocol", subprotocol)) ||
        (!extensions.empty() && !section_size.add("sec-websocket-extensions", extensions))) {
        throw std::invalid_argument("WebSocket response headers exceed HTTP/2 header list limit");
    }
    for (const auto& field : fields_value) {
        if (!section_size.add(field.name(), field.value())) {
            throw std::invalid_argument("WebSocket response headers exceed HTTP/2 header list limit");
        }
    }
    fields.reserve(fields_value.size() + 4);
    fields.add(":status", "200");
    if (!date.empty()) {
        fields.add_owned_value("date", date);
    }
    if (!subprotocol.empty()) {
        fields.add("sec-websocket-protocol", subprotocol);
    }
    if (!extensions.empty()) {
        fields.add("sec-websocket-extensions", extensions);
    }
    for (const auto& field : fields_value) {
        fields.add(field.name(), field.value());
    }
}

}  // namespace ruvia::detail
