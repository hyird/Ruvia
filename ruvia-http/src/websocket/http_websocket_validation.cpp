#include <array>
#include <cstddef>
#include <optional>

#include "ruvia/http/detail/field/http_connection_fields.h"
#include "ruvia/http/detail/field/http_field_list_grammar.h"
#include "ruvia/http/detail/parser/http_parser_syntax.h"
#include "ruvia/http/http1_request_body_plan.h"
#include "ruvia/http/http_base64.h"
#include "ruvia/http/http_request.h"
#include "ruvia/http/http_response.h"
#include "ruvia/http/websocket_handshake.h"

#include "request/http_request_access.h"
#include "websocket/http_websocket_frame_codec.h"
#include "websocket/http_websocket_handshake_fields.h"

namespace ruvia::detail {
namespace {

// A Sec-WebSocket-Key is the canonical padded base64 of a 16-byte nonce
// (RFC 6455 section 4.1), so exactly 24 characters.
[[nodiscard]] std::optional<std::array<std::uint8_t, 16>> decode_websocket_key(
    std::string_view key) noexcept {
    key = http_trim_ows(key);
    std::array<std::uint8_t, 16> nonce{};
    if (key.size() != 24) {
        return std::nullopt;
    }
    const auto decoded =
        http_base64_decode(key, nonce, http_base64_charset::standard, http_base64_padding::required);
    if (!decoded || *decoded != nonce.size()) {
        return std::nullopt;
    }
    return nonce;
}

}  // namespace

}  // namespace ruvia::detail

namespace ruvia {

http_protocol_error websocket_handshake_failure::protocol_error() const noexcept {
    switch (kind_) {
        case kind_type::invalid_request:
            return http_protocol_error(http_status::bad_request, "invalid_websocket_handshake",
                "invalid WebSocket handshake");
        case kind_type::unsupported_version:
            // RFC 6455 section 4.4 requires Sec-websocket-Version and uses 400
            // in its example. 426 would instead require HTTP Upgrade fields.
            return http_protocol_error(http_status::bad_request, "unsupported_websocket_version",
                "unsupported WebSocket version");
        case kind_type::unsupported_subprotocol:
            return http_protocol_error(http_status::bad_request,
                "unsupported_websocket_subprotocol", "no supported WebSocket subprotocol offered");
    }
    return http_protocol_error(
        http_status::bad_request, "invalid_websocket_handshake", "invalid WebSocket handshake");
}

void websocket_handshake_failure::apply_required_response_headers(http_response& response) const {
    if (kind_ == kind_type::unsupported_version) {
        response.header_stable_view("Sec-WebSocket-Version", "13");
    }
}

websocket_handshake_validation_result validate_websocket_handshake(const http_request& request,
    const http1_request_body_plan& body_plan, websocket_handshake_validation_options options) noexcept {
    if (request.known_method() != http_known_method::get ||
        request.protocol_version() != http_protocol_version::http11 ||
        // RFC 6455 does not forbid Content-Length on the HTTP Upgrade request.
        // The parser-owned framing plan is the authoritative distinction:
        // Content-Length: 0 carries no content, while a positive length or
        // chunked coding still has bytes that must be consumed before the
        // connection can change protocols.
        body_plan.requires_consumption()) {
        return websocket_handshake_validation_result::make_invalid_request();
    }

    detail::http_connection_options connection_options;
    detail::http_upgrade_protocols upgrade_protocols;
    detail::websocket_request_header_validator websocket_headers(detail::http_field_list_role::recipient);
    std::string_view key;
    std::size_t key_count = 0;
    bool websocket_upgrade = false;

    const auto headers = request.headers();
    for (std::size_t i = 0; i < headers.size(); ++i) {
        const auto kind = detail::http_request_access::header_kind(request, i);
        const auto& header_value = headers[i];
        if (kind == detail::request_header_kind::connection) {
            if (connection_options.parse_field(
                    header_value.value(), detail::http_connection_field_role::request_recipient) !=
                detail::http_field_list_parse_status::ok) {
                return websocket_handshake_validation_result::make_invalid_request();
            }
        } else if (kind == detail::request_header_kind::upgrade) {
            if (upgrade_protocols.parse_field(header_value.value(), detail::http_field_list_role::recipient,
                    [&websocket_upgrade](const detail::http_upgrade_protocol& protocol) noexcept {
                        if (protocol.version_.empty() &&
                            http_ascii_equals_ignore_case(protocol.name_, "websocket")) {
                            websocket_upgrade = true;
                        }
                        return true;
                    }) != detail::http_field_list_parse_status::ok) {
                return websocket_handshake_validation_result::make_invalid_request();
            }
        } else if (kind == detail::request_header_kind::sec_websocket_key) {
            key = header_value.value();
            ++key_count;
        } else if (!websocket_headers.observe_header(kind, header_value.value())) {
            return websocket_handshake_validation_result::make_invalid_request();
        }
    }

    if (!connection_options.upgrade() || !upgrade_protocols.has_protocol() || !websocket_upgrade ||
        key_count != 1 || !detail::decode_websocket_key(key).has_value()) {
        return websocket_handshake_validation_result::make_invalid_request();
    }
    return websocket_headers.finish(options);
}

}  // namespace ruvia
