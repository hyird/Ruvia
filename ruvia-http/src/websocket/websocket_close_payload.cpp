#include "websocket/websocket_close_payload.h"

#include <cstring>

#include "ruvia/http/http_utf8.h"

namespace ruvia::detail {

websocket_encoded_close_payload::websocket_encoded_close_payload(
    websocket_close_code code, std::string_view reason) noexcept
    : size_(static_cast<std::uint8_t>(reason.size() + 2)) {
    const auto code_value = code.value();
    bytes_[0] = static_cast<char>((code_value >> 8) & 0xFF);
    bytes_[1] = static_cast<char>(code_value & 0xFF);
    if (!reason.empty()) {
        std::memcpy(bytes_.data() + 2, reason.data(), reason.size());
    }
}

websocket_close_payload_encode_result encode_websocket_close_payload(
    websocket_close_code code, std::string_view reason) noexcept {
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
    // A malformed frame is a protocol error (close 1002); an otherwise-valid
    // frame whose reason is not valid UTF-8 is invalid payload data (close
    // 1007, RFC 6455 section 8.1).
    if (payload_value.size() == 1) {
        return websocket_protocol_failure::protocol_error;
    }
    if (payload_value.size() < 2) {
        return std::nullopt;
    }
    if (!websocket_close_code::is_valid_value(read_websocket_uint16(payload_value.data()))) {
        return websocket_protocol_failure::protocol_error;
    }
    if (!is_valid_utf8(payload_value.substr(2))) {
        return websocket_protocol_failure::invalid_payload_data;
    }
    return std::nullopt;
}

}  // namespace ruvia::detail
