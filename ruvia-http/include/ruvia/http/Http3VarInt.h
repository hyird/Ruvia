#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>

namespace ruvia {

inline constexpr std::uint64_t kHttp3VarIntMax = (std::uint64_t{1} << 62) - 1;
inline constexpr std::size_t kHttp3VarIntMaxBytes = 8;

enum class Http3CodecError : std::uint8_t {
    kNeedMoreData,
    kValueOutOfRange,
    kOutputTooSmall
};

struct Http3VarInt final {
    std::uint64_t value{0};
    std::size_t encodedBytes{0};
};

[[nodiscard]] inline constexpr std::expected<Http3VarInt, Http3CodecError> decodeHttp3VarInt(
    std::span<const char> input) noexcept {
    if (input.empty()) {
        return std::unexpected(Http3CodecError::kNeedMoreData);
    }
    const auto first = static_cast<std::uint8_t>(input[0]);
    const auto encodedBytes = std::size_t{1} << (first >> 6);
    if (input.size() < encodedBytes) {
        return std::unexpected(Http3CodecError::kNeedMoreData);
    }
    std::uint64_t value = first & 0x3fU;
    for (std::size_t i = 1; i < encodedBytes; ++i) {
        value = (value << 8) | static_cast<std::uint8_t>(input[i]);
    }
    return Http3VarInt{.value = value, .encodedBytes = encodedBytes};
}

[[nodiscard]] inline constexpr std::expected<std::size_t, Http3CodecError> encodeHttp3VarInt(
    std::span<char> output, std::uint64_t value) noexcept {
    if (value > kHttp3VarIntMax) {
        return std::unexpected(Http3CodecError::kValueOutOfRange);
    }
    const std::size_t size = value < (std::uint64_t{1} << 6)    ? 1
                             : value < (std::uint64_t{1} << 14) ? 2
                             : value < (std::uint64_t{1} << 30) ? 4
                                                                : 8;
    if (output.size() < size) {
        return std::unexpected(Http3CodecError::kOutputTooSmall);
    }
    for (std::size_t i = size; i > 0; --i) {
        output[i - 1] = static_cast<char>(value & 0xffU);
        value >>= 8;
    }
    const auto prefix = static_cast<std::uint8_t>(size == 1 ? 0 : size == 2 ? 1
                                                              : size == 4   ? 2
                                                                            : 3);
    output[0] = static_cast<char>(static_cast<std::uint8_t>(output[0]) | (prefix << 6));
    return size;
}

[[nodiscard]] inline constexpr std::size_t http3VarIntEncodedSize(std::uint64_t value) noexcept {
    return value < (std::uint64_t{1} << 6)    ? 1
           : value < (std::uint64_t{1} << 14) ? 2
           : value < (std::uint64_t{1} << 30) ? 4
                                              : 8;
}

}  // namespace ruvia
