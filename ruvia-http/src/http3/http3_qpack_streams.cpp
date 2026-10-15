#include "ruvia/http/http3_qpack_streams.h"

#include <variant>

#include "ruvia/http/http3_qpack.h"
#include "ruvia/http/http3_var_int.h"

namespace ruvia {

std::variant<std::monostate, http3_qpack_stream_error> http3_qpack_encoder_stream_validator::consume(
    std::span<const char> bytes_value, bool fin) noexcept {
    // With a permanently empty table, the only legal peer encoder instruction
    // is Set Dynamic Table Capacity(0), encoded as the single octet 0x20.
    // All other first octets already identify an invalid instruction, so no
    // partial-instruction storage or deferred validation is necessary.
    for (char byte : bytes_value) {
        if (static_cast<unsigned char>(byte) != 0x20U) {
            return http3_qpack_stream_error::encoder_stream_error;
        }
    }
    if (fin) {
        return http3_qpack_stream_error::closed_critical_stream;
    }
    return {};
}

std::variant<std::monostate, http3_qpack_stream_error> http3_qpack_decoder_stream_validator::consume(
    std::span<const char> bytes_value, bool fin) noexcept {
    // Cancellation can be sent even when there are no dynamic references.
    // It contains a 6-bit prefixed stream ID and can span multiple feeds.
    for (char byte : bytes_value) {
        if (instruction_size_ == 0 && (static_cast<unsigned char>(byte) & 0xc0U) != 0x40U) {
            return http3_qpack_stream_error::decoder_stream_error;
        }
        if (instruction_size_ == instruction_.size()) {
            return http3_qpack_stream_error::decoder_stream_error;
        }
        instruction_[instruction_size_++] = byte;
        const auto decoded = decode_http3_qpack_integer(
            std::span<const char>(instruction_).first(instruction_size_), 6);
        if ((decoded.index() != 0)) {
            if (std::get<1>(decoded) != http3_qpack_error::need_more_data ||
                instruction_size_ == instruction_.size()) {
                return http3_qpack_stream_error::decoder_stream_error;
            }
            continue;
        }
        if (std::get<0>(decoded).value_ > http3_var_int_max) {
            return http3_qpack_stream_error::decoder_stream_error;
        }
        instruction_size_ = 0;
    }
    if (fin) {
        return instruction_size_ != 0
                   ? http3_qpack_stream_error::decoder_stream_error
                   : http3_qpack_stream_error::closed_critical_stream;
    }
    return {};
}

}  // namespace ruvia
