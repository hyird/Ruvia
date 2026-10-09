#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <variant>

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

[[nodiscard]] inline constexpr std::variant<Http3FrameHeader, Http3CodecError> decodeHttp3FrameHeader(
    std::span<const char> input) noexcept {
    const auto type = decodeHttp3VarInt(input);
    if ((type.index() != 0)) {
        return std::get<1>(type);
    }
    const auto length = decodeHttp3VarInt(input.subspan(std::get<0>(type).encodedBytes));
    if ((length.index() != 0)) {
        return std::get<1>(length);
    }
    return Http3FrameHeader{.type = std::get<0>(type).value,
        .length = std::get<0>(length).value,
        .encodedBytes = std::get<0>(type).encodedBytes + std::get<0>(length).encodedBytes};
}

[[nodiscard]] inline constexpr std::variant<std::size_t, Http3CodecError> encodeHttp3FrameHeader(
    std::span<char> output, std::uint64_t type, std::uint64_t length) noexcept {
    if (type > kHttp3VarIntMax || length > kHttp3VarIntMax) {
        return Http3CodecError::kValueOutOfRange;
    }
    const auto required = http3VarIntEncodedSize(type) + http3VarIntEncodedSize(length);
    if (output.size() < required) {
        return Http3CodecError::kOutputTooSmall;
    }
    const auto typeSize = encodeHttp3VarInt(output, type);
    const auto lengthSize = encodeHttp3VarInt(output.subspan(std::get<0>(typeSize)), length);
    return std::get<0>(typeSize) + std::get<0>(lengthSize);
}

[[nodiscard]] inline constexpr std::variant<Http3FrameView, Http3CodecError> decodeHttp3Frame(
    std::span<const char> input) noexcept {
    const auto header = decodeHttp3FrameHeader(input);
    if ((header.index() != 0)) {
        return std::get<1>(header);
    }
    if (std::get<0>(header).length > input.size() - std::get<0>(header).encodedBytes) {
        return Http3CodecError::kNeedMoreData;
    }
    const auto payloadLength = static_cast<std::size_t>(std::get<0>(header).length);
    return Http3FrameView{.type = std::get<0>(header).type,
        .payload = input.subspan(std::get<0>(header).encodedBytes, payloadLength),
        .encodedBytes = std::get<0>(header).encodedBytes + payloadLength};
}

[[nodiscard]] inline constexpr std::variant<std::size_t, Http3CodecError> encodeHttp3Frame(
    std::span<char> output, std::uint64_t type, std::span<const char> payload) noexcept {
    if (type > kHttp3VarIntMax) {
        return Http3CodecError::kValueOutOfRange;
    }
    if (payload.size() > kHttp3VarIntMax) {
        return Http3CodecError::kValueOutOfRange;
    }
    const auto headerSize = http3VarIntEncodedSize(type) + http3VarIntEncodedSize(payload.size());
    if (output.size() < headerSize || output.size() - headerSize < payload.size()) {
        return Http3CodecError::kOutputTooSmall;
    }
    const auto written = encodeHttp3FrameHeader(output, type, payload.size());
    if ((written.index() != 0)) {
        return std::get<1>(written);
    }
    for (std::size_t i = 0; i < payload.size(); ++i) {
        output[std::get<0>(written) + i] = payload[i];
    }
    return std::get<0>(written) + payload.size();
}

}  // namespace ruvia
