#pragma once

#include "ruvia/http/http_request.h"
#include "ruvia/http/websocket_handshake.h"

#include "http2/http2_stream_state.h"
#include "websocket/http_websocket_handshake_fields.h"

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
    return validate_extended_connect_websocket_headers(request, options);
}

}  // namespace ruvia::detail
