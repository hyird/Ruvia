#include "ruvia/http/Http3QpackStreams.h"

#include "ruvia/http/Http3Qpack.h"
#include "ruvia/http/Http3VarInt.h"

namespace ruvia {

std::expected<void, Http3QpackStreamError> Http3QpackEncoderStreamValidator::consume(
    std::span<const char> bytes, bool fin) noexcept {
    // With a permanently empty table, the only legal peer encoder instruction
    // is Set Dynamic Table Capacity(0), encoded as the single octet 0x20.
    // All other first octets already identify an invalid instruction, so no
    // partial-instruction storage or deferred validation is necessary.
    for (char byte : bytes) {
        if (static_cast<unsigned char>(byte) != 0x20U) {
            return std::unexpected(Http3QpackStreamError::kEncoderStreamError);
        }
    }
    if (fin) {
        return std::unexpected(Http3QpackStreamError::kClosedCriticalStream);
    }
    return {};
}

std::expected<void, Http3QpackStreamError> Http3QpackDecoderStreamValidator::consume(
    std::span<const char> bytes, bool fin) noexcept {
    // Cancellation can be sent even when there are no dynamic references.
    // It contains a 6-bit prefixed stream ID and can span multiple feeds.
    for (char byte : bytes) {
        if (instructionSize_ == 0 && (static_cast<unsigned char>(byte) & 0xc0U) != 0x40U) {
            return std::unexpected(Http3QpackStreamError::kDecoderStreamError);
        }
        if (instructionSize_ == instruction_.size()) {
            return std::unexpected(Http3QpackStreamError::kDecoderStreamError);
        }
        instruction_[instructionSize_++] = byte;
        const auto decoded = decodeHttp3QpackInteger(
            std::span<const char>(instruction_).first(instructionSize_), 6);
        if (!decoded) {
            if (decoded.error() != Http3QpackError::kNeedMoreData ||
                instructionSize_ == instruction_.size()) {
                return std::unexpected(Http3QpackStreamError::kDecoderStreamError);
            }
            continue;
        }
        if (decoded->value > kHttp3VarIntMax) {
            return std::unexpected(Http3QpackStreamError::kDecoderStreamError);
        }
        instructionSize_ = 0;
    }
    if (fin) {
        return std::unexpected(instructionSize_ != 0
                                   ? Http3QpackStreamError::kDecoderStreamError
                                   : Http3QpackStreamError::kClosedCriticalStream);
    }
    return {};
}

}  // namespace ruvia
