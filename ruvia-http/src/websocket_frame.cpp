#include "websocket_frame.h"

#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

#include "borrowed_text.h"
#include "http_io_buffer.h"
#include "http_protocol.h"
#include "utf8.h"
#include "websocket_connection.h"
#include "websocket_deflate.h"

namespace ruvia::detail {

websocket_encoded_close_payload::websocket_encoded_close_payload(websocket_close_code code, std::string_view reason) noexcept :
    size_(static_cast<std::uint8_t>(reason.size() + 2)) {
    const auto code_value = code.value();
    bytes_[0] = static_cast<char>((code_value >> 8) & 0xFF);
    bytes_[1] = static_cast<char>(code_value & 0xFF);
    if (!reason.empty()) {
        std::memcpy(bytes_.data() + 2, reason.data(), reason.size());
    }
}

websocket_close_payload_encode_result encode_websocket_close_payload(websocket_close_code code, std::string_view reason) noexcept {
    if (reason.size() > 123) {
        return websocket_close_payload_encode_result(websocket_close_payload_encode_failure(websocket_close_payload_encode_error::reason_too_large));
    }
    if (!::ruvia::is_valid_utf8(reason)) {
        return websocket_close_payload_encode_result(websocket_close_payload_encode_failure(websocket_close_payload_encode_error::invalid_reason));
    }
    return websocket_close_payload_encode_result(websocket_encoded_close_payload(code, reason));
}

std::optional<websocket_protocol_failure> websocket_close_payload_failure(std::string_view payload_value) noexcept {
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
    if (!::ruvia::is_valid_utf8(payload_value.substr(2))) {
        return websocket_protocol_failure::invalid_payload_data;
    }
    return std::nullopt;
}

void websocket_inbound_assembler::release_completed(websocket_receive_storage& storage) noexcept {
    if (std::holds_alternative<websocket_inbound_idle>(state_)) {
        http_clear_string_retaining_small(message_, websocket_retained_buffer_bytes);
        storage.settle(websocket_receive_storage::slot::reassembly, message_);
    }
}

websocket_inbound_result websocket_inbound_assembler::accept(const websocket_frame_view& frame, protocol_byte_limit message_limit, websocket_receive_storage& storage) {
    if (is_websocket_control_frame_kind(frame.kind())) {
        const auto opcode = static_cast<websocket_opcode>(frame.kind());
        return websocket_inbound_result::make_control_frame(opcode, frame.payload());
    }
    if (frame.kind() == websocket_frame_kind::continuation) {
        const auto* fragmented = std::get_if<websocket_inbound_fragmented>(&state_);
        if (fragmented == nullptr) {
            return websocket_inbound_result::make_failure(websocket_protocol_failure::protocol_error);
        }
        if (message_limit.addition_exceeds(message_.size(), frame.payload().size()) || !storage.reserve(websocket_receive_storage::slot::reassembly, message_, message_.size() + frame.payload().size())) {
            return websocket_inbound_result::make_failure(websocket_protocol_failure::message_too_large);
        }
        message_.append(frame.payload().data(), frame.payload().size());
        if (!frame.final()) {
            return websocket_inbound_result::make_continue();
        }
        const auto opcode = fragmented->opcode();
        const auto encoding = fragmented->encoding();
        state_.template emplace<websocket_inbound_idle>();
        const auto message = websocket_message::borrow(opcode, std::string_view(message_));
        if (encoding == websocket_inbound_content_encoding::per_message_deflate) {
            return websocket_inbound_result::make_message(message, websocket_inbound_content_encoding::per_message_deflate);
        }
        if (opcode == websocket_opcode::text && !::ruvia::is_valid_utf8(message_)) {
            return websocket_inbound_result::make_failure(websocket_protocol_failure::invalid_payload_data);
        }
        return websocket_inbound_result::make_message(message, websocket_inbound_content_encoding::identity);
    }
    if (frame.kind() == websocket_frame_kind::text || frame.kind() == websocket_frame_kind::binary) {
        if (std::get_if<websocket_inbound_fragmented>(&state_) != nullptr) {
            return websocket_inbound_result::make_failure(websocket_protocol_failure::protocol_error);
        }
        // websocket_try_read_frame already bounded this first frame by the message limit.
        const auto opcode = static_cast<websocket_opcode>(frame.kind());
        if (frame.final()) {
            const auto message = websocket_message::borrow(opcode, frame.payload());
            if (frame.compressed()) {
                return websocket_inbound_result::make_message(message, websocket_inbound_content_encoding::per_message_deflate);
            }
            if (opcode == websocket_opcode::text && !::ruvia::is_valid_utf8(frame.payload())) {
                return websocket_inbound_result::make_failure(websocket_protocol_failure::invalid_payload_data);
            }
            return websocket_inbound_result::make_message(message, websocket_inbound_content_encoding::identity);
        }
        if (!storage.reserve(websocket_receive_storage::slot::reassembly, message_, frame.payload().size())) {
            return websocket_inbound_result::make_failure(websocket_protocol_failure::message_too_large);
        }
        message_.assign(frame.payload().data(), frame.payload().size());
        state_.template emplace<websocket_inbound_fragmented>(opcode, frame.compressed() ? websocket_inbound_content_encoding::per_message_deflate : websocket_inbound_content_encoding::identity);
    }
    return websocket_inbound_result::make_continue();
}

}  // namespace ruvia::detail
