#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>

namespace ruvia {

enum class Http3QpackError : std::uint8_t {
    kNeedMoreData,
    kIntegerOverflow,
    kInvalidIndex,
    kInvalidHuffman,
    kOutputTooSmall,
};

struct Http3QpackStaticEntry final {
    std::string_view name;
    std::string_view value;
};

struct Http3QpackInteger final {
    std::uint64_t value{0};
    std::size_t encodedBytes{0};
};

// RFC 9204 static-table lookup. Valid indices are 0 through 98.
[[nodiscard]] std::expected<Http3QpackStaticEntry, Http3QpackError> http3QpackStaticEntry(
    std::uint64_t index) noexcept;

// Decodes/encodes a prefixed integer at the start of a field. `prefixBits` is 1..8;
// encode's `prefix` supplies bits outside the integer prefix.
[[nodiscard]] std::expected<Http3QpackInteger, Http3QpackError> decodeHttp3QpackInteger(
    std::span<const char> input, std::uint8_t prefixBits) noexcept;
[[nodiscard]] std::expected<std::size_t, Http3QpackError> encodeHttp3QpackInteger(
    std::span<char> output, std::uint8_t prefixBits, std::uint8_t prefix,
    std::uint64_t value) noexcept;

// Decodes a QPACK string literal into `output`; the returned size includes the
// first-byte Huffman flag and the prefixed length. Huffman strings use HPACK's
// identical Huffman code table (RFC 7541 Appendix B).
[[nodiscard]] std::expected<std::size_t, Http3QpackError> decodeHttp3QpackString(
    std::span<const char> input, std::pmr::string& output);
// Encodes a non-Huffman string literal. QPACK Huffman encoding is intentionally
// not provided by this primitive.
[[nodiscard]] std::expected<std::size_t, Http3QpackError> encodeHttp3QpackString(
    std::span<char> output, std::string_view value) noexcept;

}  // namespace ruvia
