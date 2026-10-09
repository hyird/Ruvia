#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <variant>

namespace ruvia {

enum class Http3QpackStreamError : std::uint8_t {
    kEncoderStreamError,
    kDecoderStreamError,
    // The caller maps this critical-stream termination to H3_CLOSED_CRITICAL_STREAM.
    kClosedCriticalStream,
};

// Bounded sans-I/O validator for the peer's QPACK encoder stream when the
// negotiated maximum dynamic table capacity is zero.
class Http3QpackEncoderStreamValidator final {
public:
    [[nodiscard]] std::variant<std::monostate, Http3QpackStreamError> consume(
        std::span<const char> bytes, bool fin = false) noexcept;
};

// At capacity zero, Section Acknowledgment and Insert Count Increment cannot
// occur, but Stream Cancellation is still permitted (RFC 9204 §2.2.2.2).
class Http3QpackDecoderStreamValidator final {
public:
    [[nodiscard]] std::variant<std::monostate, Http3QpackStreamError> consume(
        std::span<const char> bytes, bool fin = false) noexcept;

private:
    std::array<char, 11> instruction_{};
    std::size_t instructionSize_{0};
};

}  // namespace ruvia
