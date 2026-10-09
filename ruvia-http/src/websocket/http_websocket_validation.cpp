#include <array>
#include <cstddef>
#include <cstring>
#include <optional>
#include <utility>

#include "ruvia/http/detail/field/header_token_utils.h"
#include "ruvia/http/detail/field/http_connection_fields.h"
#include "ruvia/http/detail/parser/http_parser_syntax.h"
#include "ruvia/http/detail/response/http_response_header_state.h"
#include "ruvia/http/http1_request_body_plan.h"
#include "ruvia/http/http_request.h"
#include "ruvia/http/websocket_handshake.h"

#include "request/http_request_access.h"
#include "websocket/http_websocket_close_payload.h"
#include "websocket/http_websocket_frame_codec.h"
#include "websocket/http_websocket_handshake_fields.h"
#include "websocket/http_websocket_payload_validation.h"

namespace ruvia::detail {
namespace {

[[nodiscard]] bool websocket_header_equals(
    std::string_view value, std::string_view expected) noexcept {
    return detail::http_ascii_equals_ignore_case(detail::http_trim_ows(value), expected);
}

[[nodiscard]] std::optional<std::uint8_t> base64_value(char c) noexcept {
    if (c >= 'A' && c <= 'Z') {
        return static_cast<std::uint8_t>(c - 'A');
    }
    if (c >= 'a' && c <= 'z') {
        return static_cast<std::uint8_t>(26 + c - 'a');
    }
    if (c >= '0' && c <= '9') {
        return static_cast<std::uint8_t>(52 + c - '0');
    }
    if (c == '+') {
        return 62;
    }
    if (c == '/') {
        return 63;
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<std::array<std::uint8_t, 16>> decode_websocket_key(
    std::string_view key) noexcept {
    key = detail::http_trim_ows(key);
    if (key.size() != 24) {
        return std::nullopt;
    }

    std::array<std::uint8_t, 16> nonce{};
    std::size_t out = 0;
    const auto emit = [&nonce, &out](std::uint8_t byte) noexcept {
        if (out < nonce.size()) {
            nonce[out] = byte;
        }
        ++out;
    };
    for (std::size_t i = 0; i < key.size(); i += 4) {
        std::array<std::uint8_t, 4> values{};
        std::size_t padding = 0;
        for (std::size_t j = 0; j < 4; ++j) {
            const auto ch = key[i + j];
            if (ch == '=') {
                values[j] = 0;
                ++padding;
                continue;
            }
            if (padding != 0) {
                return std::nullopt;
            }
            const auto value = base64_value(ch);
            if (!value) {
                return std::nullopt;
            }
            values[j] = *value;
        }
        if (padding > 2 || (padding != 0 && i + 4 != key.size())) {
            return std::nullopt;
        }
        // RFC 4648 requires unused bits in the final base64 quantum to be
        // zero.  Without this check, multiple non-canonical strings decode
        // to the same nonce and are incorrectly accepted as websocket keys.
        if ((padding == 2 && (values[1] & 0x0FU) != 0) ||
            (padding == 1 && (values[2] & 0x03U) != 0)) {
            return std::nullopt;
        }
        const auto triple = (static_cast<std::uint32_t>(values[0]) << 18) |
                            (static_cast<std::uint32_t>(values[1]) << 12) |
                            (static_cast<std::uint32_t>(values[2]) << 6) |
                            static_cast<std::uint32_t>(values[3]);
        emit(static_cast<std::uint8_t>((triple >> 16) & 0xFF));
        if (padding < 2) {
            emit(static_cast<std::uint8_t>((triple >> 8) & 0xFF));
        }
        if (padding < 1) {
            emit(static_cast<std::uint8_t>(triple & 0xFF));
        }
    }
    if (out != 16) {
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
            return http_protocol_error(http_status::bad_request, "invalid WebSocket handshake");
        case kind_type::unsupported_version:
            // RFC 6455 section 4.4 requires Sec-websocket-Version and uses 400
            // in its example. 426 would instead require HTTP Upgrade fields,
            // which HTTP/2 and HTTP/3 forbid.
            return http_protocol_error(
                http_status::bad_request, "unsupported WebSocket version");
    }
    return http_protocol_error(http_status::bad_request, "invalid WebSocket handshake");
}

void websocket_handshake_failure::apply_required_response_headers(http_response& response) const {
    if (kind_ == kind_type::unsupported_version) {
        response.header_stable_view("Sec-WebSocket-Version", "13");
    }
}

websocket_handshake_validation_result validate_websocket_handshake(
    const http_request& request, const http1_request_body_plan& body_plan) noexcept {
    detail::http_connection_options connection_options;
    detail::http_upgrade_protocols upgrade_protocols;
    std::string_view key;
    std::string_view version;
    std::size_t key_count = 0;
    std::size_t version_count = 0;
    bool websocket_upgrade = false;

    const auto headers = request.headers();
    for (std::size_t i = 0; i < headers.size(); ++i) {
        const auto kind = detail::http_request_access::header_kind(request, i);
        const auto& header_value = headers[i];
        if (kind == static_cast<std::uint8_t>(detail::request_header_kind::connection)) {
            if (connection_options.parse_field(
                    header_value.value(), detail::http_field_list_role::recipient) !=
                detail::http_field_list_parse_status::ok) {
                return websocket_handshake_validation_result::make_invalid_request();
            }
        } else if (kind == static_cast<std::uint8_t>(detail::request_header_kind::upgrade)) {
            if (upgrade_protocols.parse_field(header_value.value(), detail::http_field_list_role::recipient,
                    [&websocket_upgrade](const detail::http_upgrade_protocol& protocol) noexcept {
                        if (protocol.version_.empty() &&
                            detail::http_ascii_equals_ignore_case(protocol.name_, "websocket")) {
                            websocket_upgrade = true;
                        }
                        return true;
                    }) != detail::http_field_list_parse_status::ok) {
                return websocket_handshake_validation_result::make_invalid_request();
            }
        } else if (kind == static_cast<std::uint8_t>(detail::request_header_kind::sec_websocket_key)) {
            key = header_value.value();
            ++key_count;
        } else if (kind == static_cast<std::uint8_t>(detail::request_header_kind::sec_websocket_version)) {
            version = header_value.value();
            ++version_count;
        }
    }

    if (request.known_method() != http_known_method::get ||
        request.protocol_version() != http_protocol_version::http11 || !connection_options.upgrade() ||
        !upgrade_protocols.has_protocol() || !websocket_upgrade ||
        // RFC 6455 does not forbid Content-Length on the HTTP Upgrade request.
        // The parser-owned framing plan is the authoritative distinction:
        // Content-Length: 0 carries no content, while a positive length or
        // chunked coding still has bytes that must be consumed before the
        // connection can change protocols.
        body_plan.requires_consumption() || !detail::websocket_subprotocol_offers_valid(request) ||
        !detail::websocket_extension_offers_valid(request) || key_count != 1 ||
        !detail::decode_websocket_key(key).has_value() || version_count != 1) {
        return websocket_handshake_validation_result::make_invalid_request();
    }
    if (!detail::websocket_header_equals(version, "13")) {
        return websocket_handshake_validation_result::make_unsupported_version();
    }
    return websocket_handshake_validation_result::make_accepted();
}

}  // namespace ruvia

namespace ruvia::detail {

bool is_valid_websocket_close_code(std::uint16_t code) noexcept {
    if (code == 1000 || code == 1001 || code == 1002 || code == 1003 ||
        (code >= 1007 && code <= 1014)) {
        return true;
    }
    return code >= 3000 && code <= 4999;
}

bool is_valid_utf8(std::string_view value) noexcept {
    std::uint32_t codepoint = 0;
    std::uint32_t remaining = 0;
    std::uint32_t min_value = 0;
    for (const auto ch : value) {
        const auto byte = static_cast<unsigned char>(ch);
        if (remaining == 0) {
            if (byte <= 0x7F) {
                continue;
            }
            if (byte >= 0xC2 && byte <= 0xDF) {
                codepoint = byte & 0x1FU;
                remaining = 1;
                min_value = 0x80;
            } else if (byte >= 0xE0 && byte <= 0xEF) {
                codepoint = byte & 0x0FU;
                remaining = 2;
                min_value = 0x800;
            } else if (byte >= 0xF0 && byte <= 0xF4) {
                codepoint = byte & 0x07U;
                remaining = 3;
                min_value = 0x10000;
            } else {
                return false;
            }
        } else {
            if ((byte & 0xC0U) != 0x80U) {
                return false;
            }
            codepoint = (codepoint << 6) | (byte & 0x3FU);
            --remaining;
            if (remaining == 0 && (codepoint < min_value || codepoint > 0x10FFFF ||
                                      (codepoint >= 0xD800 && codepoint <= 0xDFFF))) {
                return false;
            }
        }
    }
    return remaining == 0;
}

websocket_encoded_close_payload::websocket_encoded_close_payload(
    std::uint16_t code, std::string_view reason) noexcept
    : size_(static_cast<std::uint8_t>(reason.size() + 2)) {
    bytes_[0] = static_cast<char>((code >> 8) & 0xFF);
    bytes_[1] = static_cast<char>(code & 0xFF);
    if (!reason.empty()) {
        std::memcpy(bytes_.data() + 2, reason.data(), reason.size());
    }
}

websocket_close_payload_encode_result encode_websocket_close_payload(
    std::uint16_t code, std::string_view reason) noexcept {
    if (!is_valid_websocket_close_code(code)) {
        return websocket_close_payload_encode_result(
            websocket_close_payload_encode_failure(websocket_close_payload_encode_error::invalid_code));
    }
    if (reason.size() > 123) {
        return websocket_close_payload_encode_result(
            websocket_close_payload_encode_failure(websocket_close_payload_encode_error::reason_too_large));
    }
    if (!is_valid_utf8(reason)) {
        return websocket_close_payload_encode_result(
            websocket_close_payload_encode_failure(websocket_close_payload_encode_error::invalid_reason));
    }
    return websocket_close_payload_encode_result(websocket_encoded_close_payload(code, reason));
}

std::optional<websocket_protocol_failure> websocket_close_payload_failure(
    std::string_view payload_value) noexcept {
    // Incoming Close frame (RFC 6455 §5.5.1). A malformed frame is a protocol
    // error (close 1002); an otherwise-valid frame whose reason is not valid
    // UTF-8 is invalid payload data (close 1007, §8.1).
    if (payload_value.size() == 1) {
        return websocket_protocol_failure::protocol_error;
    }
    if (payload_value.size() < 2) {
        return std::nullopt;
    }
    const auto code = static_cast<std::uint16_t>(
        (static_cast<unsigned char>(payload_value[0]) << 8) | static_cast<unsigned char>(payload_value[1]));
    if (!is_valid_websocket_close_code(code)) {
        return websocket_protocol_failure::protocol_error;
    }
    if (!is_valid_utf8(payload_value.substr(2))) {
        return websocket_protocol_failure::invalid_payload_data;
    }
    return std::nullopt;
}

}  // namespace ruvia::detail
