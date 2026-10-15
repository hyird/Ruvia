#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <variant>

namespace ruvia {

enum class http3_qpack_stream_error : std::uint8_t {
    encoder_stream_error,
    decoder_stream_error,
    // The caller maps this critical-stream termination to H3_CLOSED_CRITICAL_STREAM.
    closed_critical_stream,
};

// Bounded sans-I/O validator for the peer's QPACK encoder stream when the
// negotiated maximum dynamic table capacity is zero.
class http3_qpack_encoder_stream_validator final {
public:
    [[nodiscard]] std::variant<std::monostate, http3_qpack_stream_error> consume(
        std::span<const char> bytes, bool fin = false) noexcept;
};

// At capacity zero, Section Acknowledgment and Insert Count Increment cannot
// occur, but Stream Cancellation is still permitted (RFC 9204 §2.2.2.2).
class http3_qpack_decoder_stream_validator final {
public:
    [[nodiscard]] std::variant<std::monostate, http3_qpack_stream_error> consume(
        std::span<const char> bytes, bool fin = false) noexcept;

private:
    std::array<char, 11> instruction_{};
    std::size_t instruction_size_{0};
};

}  // namespace ruvia
