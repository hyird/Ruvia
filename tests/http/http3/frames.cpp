#include <array>
#include <cstdint>
#include <span>
#include <string_view>

#include "ruvia/http/Http3Frames.h"
#include "ruvia/http/Http3VarInt.h"

#include "test_harness.h"

namespace {

using ruvia::decodeHttp3Frame;
using ruvia::decodeHttp3FrameHeader;
using ruvia::decodeHttp3VarInt;
using ruvia::encodeHttp3Frame;
using ruvia::encodeHttp3VarInt;
using ruvia::Http3CodecError;
using ruvia::isHttp3GreaseFrameType;
using ruvia::kHttp3VarIntMax;

}  // namespace

RUVIA_TEST(http3_varint_encodes_and_decodes_all_wire_widths) {
    constexpr std::array<std::uint64_t, 8> values{0, 63, 64, 16383, 16384, (1ULL << 30) - 1,
        1ULL << 30, kHttp3VarIntMax};
    constexpr std::array<std::size_t, 8> widths{1, 1, 2, 2, 4, 4, 8, 8};
    for (std::size_t i = 0; i < values.size(); ++i) {
        std::array<char, 8> bytes{};
        const auto written = encodeHttp3VarInt(bytes, values[i]);
        RUVIA_CHECK(written.has_value());
        if (!written) {
            continue;
        }
        RUVIA_CHECK_EQ(*written, widths[i]);
        const auto decoded = decodeHttp3VarInt(std::span<const char>(bytes).first(*written));
        RUVIA_CHECK(decoded.has_value());
        if (decoded) {
            RUVIA_CHECK_EQ(decoded->value, values[i]);
            RUVIA_CHECK_EQ(decoded->encodedBytes, widths[i]);
        }
    }
}

RUVIA_TEST(http3_varint_reports_truncation_and_bounds_without_writing) {
    constexpr std::array<char, 1> twoBytePrefix{static_cast<char>(0x40)};
    RUVIA_CHECK(decodeHttp3VarInt(twoBytePrefix).error() == Http3CodecError::kNeedMoreData);
    std::array<char, 8> output{};
    output.fill('#');
    const auto before = output;
    RUVIA_CHECK(encodeHttp3VarInt(output, kHttp3VarIntMax + 1).error() ==
                Http3CodecError::kValueOutOfRange);
    RUVIA_CHECK(encodeHttp3VarInt(std::span<char>(output).first(1), 64).error() ==
                Http3CodecError::kOutputTooSmall);
    RUVIA_CHECK_EQ(output, before);
}

RUVIA_TEST(http3_frame_codec_reports_invalid_type_before_output_capacity) {
    std::array<char, 1> output{'#'};
    const auto result = encodeHttp3Frame(output, kHttp3VarIntMax + 1, {});
    RUVIA_CHECK(!result.has_value());
    if (!result) {
        RUVIA_CHECK(result.error() == Http3CodecError::kValueOutOfRange);
    }
    RUVIA_CHECK_EQ(output[0], '#');
}

RUVIA_TEST(http3_frame_codec_round_trips_opaque_payloads) {
    constexpr std::string_view payload = "headers-or-data";
    std::array<char, 64> wire{};
    const auto written = encodeHttp3Frame(wire, 0x1, payload);
    RUVIA_CHECK(written.has_value());
    if (!written) {
        return;
    }
    const auto decoded = decodeHttp3Frame(std::span<const char>(wire).first(*written));
    RUVIA_CHECK(decoded.has_value());
    if (decoded) {
        RUVIA_CHECK_EQ(decoded->type, std::uint64_t{0x1});
        RUVIA_CHECK_EQ(std::string_view(decoded->payload.data(), decoded->payload.size()), payload);
        RUVIA_CHECK_EQ(decoded->encodedBytes, *written);
    }
}

RUVIA_TEST(http3_frame_codec_distinguishes_truncation_and_keeps_unknown_frames_skippable) {
    constexpr std::array<char, 3> shortHeader{0x1, 0x4, 0x01};
    RUVIA_CHECK(decodeHttp3FrameHeader(std::span<const char>(shortHeader).first(1)).error() ==
                Http3CodecError::kNeedMoreData);
    RUVIA_CHECK(decodeHttp3Frame(shortHeader).error() == Http3CodecError::kNeedMoreData);

    // Unknown extension frame type 0x21 (an HTTP/3 grease type) is surfaced as
    // an opaque frame; consuming encodedBytes skips it without decoding payload.
    constexpr std::array<char, 5> greaseFrame{0x21, 0x01, 'x', 0x01, 0x00};
    const auto frame = decodeHttp3Frame(greaseFrame);
    RUVIA_CHECK(frame.has_value());
    RUVIA_CHECK(isHttp3GreaseFrameType(0x21));
    RUVIA_CHECK(isHttp3GreaseFrameType(0x40));
    RUVIA_CHECK(!isHttp3GreaseFrameType(0x22));
    if (frame) {
        RUVIA_CHECK_EQ(frame->type, std::uint64_t{0x21});
        RUVIA_CHECK_EQ(std::string_view(frame->payload.data(), frame->payload.size()),
            std::string_view("x"));
        RUVIA_CHECK_EQ(frame->encodedBytes, std::size_t{3});
        const auto next = decodeHttp3Frame(std::span<const char>(greaseFrame).subspan(frame->encodedBytes));
        RUVIA_CHECK(next.has_value());
        if (next) {
            RUVIA_CHECK_EQ(next->type, std::uint64_t{1});
        }
    }
}
