#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>

#include "ruvia/http/Http3VarInt.h"

namespace ruvia {

inline constexpr std::size_t kHttp3FrameHeaderMaxBytes = 2 * kHttp3VarIntMaxBytes;

enum class Http3FrameType : std::uint64_t {
    kData = 0x0,
    kHeaders = 0x1,
    kCancelPush = 0x3,
    kSettings = 0x4,
    kPushPromise = 0x5,
    kGoaway = 0x7,
    kMaxPushId = 0xd
};

struct Http3FrameHeader final {
    std::uint64_t type{0};
    std::uint64_t length{0};
    std::size_t encodedBytes{0};
};

struct Http3FrameView final {
    std::uint64_t type{0};
    std::span<const char> payload{};
    std::size_t encodedBytes{0};
};

[[nodiscard]] inline constexpr bool isHttp3GreaseFrameType(std::uint64_t type) noexcept {
    return type >= 0x21 && (type - 0x21) % 0x1f == 0;
}

[[nodiscard]] inline constexpr std::expected<Http3FrameHeader, Http3CodecError> decodeHttp3FrameHeader(
    std::span<const char> input) noexcept {
    const auto type = decodeHttp3VarInt(input);
    if (!type) {
        return std::unexpected(type.error());
    }
    const auto length = decodeHttp3VarInt(input.subspan(type->encodedBytes));
    if (!length) {
        return std::unexpected(length.error());
    }
    return Http3FrameHeader{.type = type->value,
        .length = length->value,
        .encodedBytes = type->encodedBytes + length->encodedBytes};
}

[[nodiscard]] inline constexpr std::expected<std::size_t, Http3CodecError> encodeHttp3FrameHeader(
    std::span<char> output, std::uint64_t type, std::uint64_t length) noexcept {
    if (type > kHttp3VarIntMax || length > kHttp3VarIntMax) {
        return std::unexpected(Http3CodecError::kValueOutOfRange);
    }
    const auto required = http3VarIntEncodedSize(type) + http3VarIntEncodedSize(length);
    if (output.size() < required) {
        return std::unexpected(Http3CodecError::kOutputTooSmall);
    }
    const auto typeSize = encodeHttp3VarInt(output, type);
    const auto lengthSize = encodeHttp3VarInt(output.subspan(*typeSize), length);
    return *typeSize + *lengthSize;
}

[[nodiscard]] inline constexpr std::expected<Http3FrameView, Http3CodecError> decodeHttp3Frame(
    std::span<const char> input) noexcept {
    const auto header = decodeHttp3FrameHeader(input);
    if (!header) {
        return std::unexpected(header.error());
    }
    if (header->length > input.size() - header->encodedBytes) {
        return std::unexpected(Http3CodecError::kNeedMoreData);
    }
    const auto payloadLength = static_cast<std::size_t>(header->length);
    return Http3FrameView{.type = header->type,
        .payload = input.subspan(header->encodedBytes, payloadLength),
        .encodedBytes = header->encodedBytes + payloadLength};
}

[[nodiscard]] inline constexpr std::expected<std::size_t, Http3CodecError> encodeHttp3Frame(
    std::span<char> output, std::uint64_t type, std::span<const char> payload) noexcept {
    if (type > kHttp3VarIntMax) {
        return std::unexpected(Http3CodecError::kValueOutOfRange);
    }
    if (payload.size() > kHttp3VarIntMax) {
        return std::unexpected(Http3CodecError::kValueOutOfRange);
    }
    const auto headerSize = http3VarIntEncodedSize(type) + http3VarIntEncodedSize(payload.size());
    if (output.size() < headerSize || output.size() - headerSize < payload.size()) {
        return std::unexpected(Http3CodecError::kOutputTooSmall);
    }
    const auto written = encodeHttp3FrameHeader(output, type, payload.size());
    if (!written) {
        return std::unexpected(written.error());
    }
    for (std::size_t i = 0; i < payload.size(); ++i) {
        output[*written + i] = payload[i];
    }
    return *written + payload.size();
}

}  // namespace ruvia
