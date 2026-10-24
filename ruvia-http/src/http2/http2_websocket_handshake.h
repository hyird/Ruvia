#pragma once

#include <algorithm>
#include <cstddef>
#include <memory_resource>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/http/detail/parser/http_parser_syntax.h"
#include "ruvia/http/hpack.h"
#include "ruvia/http/http_request.h"
#include "ruvia/http/websocket_handshake.h"

#include "http2/http2_hpack.h"
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

[[nodiscard]] inline websocket_handshake_validation_result validate_http2_websocket_handshake(
    const http2_stream_state& stream, const http_request& request,
    websocket_handshake_validation_options options = {}) noexcept {
    if (!http2_is_pending_websocket_connect(stream) || !http2_remote_final_head_decoded(stream) ||
        http2_remote_peer_half_closed(stream) ||
        stream.remote_content().allowed_without_length() == nullptr) {
        return websocket_handshake_validation_result::make_invalid_request();
    }

    // RFC 8441 section 5: :protocol supersedes the RFC 6455 key exchange, so
    // Sec-WebSocket-Key and Sec-WebSocket-Accept are not processed here.
    websocket_request_header_validator validator(http_field_list_role::recipient);
    const auto headers = request.headers();
    for (std::size_t i = 0; i < headers.size(); ++i) {
        const auto kind = static_cast<request_header_kind>(http_request_access::header_kind(request, i));
        if (!validator.observe_header(kind, headers[i].value())) {
            return websocket_handshake_validation_result::make_invalid_request();
        }
    }
    return validator.finish(options);
}

inline void http2_encode_websocket_handshake_headers(
    std::pmr::string& header_block, const websocket_server_negotiation& negotiation) {
    try {
        header_block.clear();
        hpack_encoder::encode_status(header_block, http_status::ok);
        const auto fields_value = negotiation.response_headers();
        const bool has_date = std::ranges::any_of(fields_value, [](const http_header& field) {
            return field.name() == "date";
        });
        if (!has_date) {
            if (const auto date = cached_date_value(); !date.empty()) {
                hpack_encoder::encode_header_with_name_index(
                    header_block, hpack_static_index::date, date);
            }
        }
        if (!negotiation.subprotocol().empty()) {
            hpack_encoder::encode_header(
                header_block, "sec-websocket-protocol", negotiation.subprotocol());
        }
        if (!negotiation.extensions().empty()) {
            hpack_encoder::encode_header(
                header_block, "sec-websocket-extensions", negotiation.extensions());
        }
        for (const auto& field : fields_value) {
            hpack_encoder::encode_header(header_block, field.name(), field.value());
        }
    } catch (...) {
        header_block.clear();
        throw;
    }
}

}  // namespace ruvia::detail
