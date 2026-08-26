#include "test_harness.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

#include "ruvia/http/detail/http2/flow/Http2FlowControl.h"
#include "ruvia/http/detail/http2/frame/Http2FrameCodec.h"
#include "ruvia/http/detail/http2/frame/Http2FrameTypes.h"

namespace {

using ruvia::detail::http2ApplyWindowUpdate;
using ruvia::detail::http2EncodeFrameHeader;
using ruvia::detail::Http2ErrorCode;
using ruvia::detail::Http2FrameType;
using ruvia::detail::http2ParseFrameHeader;
using ruvia::detail::http2Read16;
using ruvia::detail::http2Read24;
using ruvia::detail::http2Read31;
using ruvia::detail::http2Read32;
using ruvia::detail::Http2SettingId;
using ruvia::detail::http2WindowUpdateIncrement;
using ruvia::detail::Http2WindowUpdateResult;
using ruvia::detail::http2Write16;
using ruvia::detail::http2Write32;
using ruvia::detail::http2WriteFrameHeader;
using ruvia::detail::http2WriteGoawayPayload;
using ruvia::detail::http2WriteSettingsEntry;
using ruvia::detail::http2WriteWindowUpdate;
using ruvia::detail::kHttp2FrameHeaderBytes;

const unsigned char* bytes(const char* p) noexcept {
    return reinterpret_cast<const unsigned char*>(p);
}

}  // namespace

RUVIA_TEST(frame_header_encode_parse_round_trip) {
    char buf[kHttp2FrameHeaderBytes];
    http2EncodeFrameHeader(buf, 0x123456, Http2FrameType::kHeaders, 0x25, 0x0A);
    const auto header = http2ParseFrameHeader(std::string_view(buf, kHttp2FrameHeaderBytes));
    RUVIA_CHECK_EQ(header.length, std::uint32_t{0x123456});
    RUVIA_CHECK(header.type == static_cast<std::uint8_t>(Http2FrameType::kHeaders));
    RUVIA_CHECK_EQ(header.flags, std::uint8_t{0x25});
    RUVIA_CHECK_EQ(header.streamId, std::uint32_t{0x0A});
}

RUVIA_TEST(frame_header_masks_reserved_stream_id_bit) {
    char buf[kHttp2FrameHeaderBytes];
    // The top (reserved) bit of the stream id must be cleared on the wire.
    http2EncodeFrameHeader(buf, 8, Http2FrameType::kSettings, 0, 0x8000000A);
    const auto header = http2ParseFrameHeader(std::string_view(buf, kHttp2FrameHeaderBytes));
    RUVIA_CHECK_EQ(header.streamId, std::uint32_t{0x0A});
}

RUVIA_TEST(frame_write_helpers_advance_the_cursor) {
    char buf[kHttp2FrameHeaderBytes];
    char* end = http2WriteFrameHeader(buf, 0, Http2FrameType::kData, 0, 1);
    RUVIA_CHECK(static_cast<std::size_t>(end - buf) == kHttp2FrameHeaderBytes);

    char two[2];
    char* endTwo = http2Write16(two, 0x1234);
    RUVIA_CHECK(endTwo - two == 2);
    RUVIA_CHECK_EQ(static_cast<unsigned>(static_cast<unsigned char>(two[0])), 0x12u);  // big-endian
    RUVIA_CHECK_EQ(static_cast<unsigned>(static_cast<unsigned char>(two[1])), 0x34u);
    RUVIA_CHECK_EQ(http2Read16(bytes(two)), std::uint16_t{0x1234});

    char four[4];
    char* endFour = http2Write32(four, 0x12345678);
    RUVIA_CHECK(endFour - four == 4);
    RUVIA_CHECK_EQ(static_cast<unsigned>(static_cast<unsigned char>(four[0])), 0x12u);
    RUVIA_CHECK_EQ(http2Read32(bytes(four)), std::uint32_t{0x12345678});
}

RUVIA_TEST(frame_settings_entry_serialization) {
    char buf[6];
    char* end = http2WriteSettingsEntry(buf, Http2SettingId::kMaxConcurrentStreams, 100);
    RUVIA_CHECK(end - buf == 6);
    RUVIA_CHECK_EQ(
        http2Read16(bytes(buf)), static_cast<std::uint16_t>(Http2SettingId::kMaxConcurrentStreams));
    RUVIA_CHECK_EQ(http2Read32(bytes(buf) + 2), std::uint32_t{100});
}

RUVIA_TEST(frame_window_update_serialization) {
    char buf[kHttp2FrameHeaderBytes + 4];
    // A high bit set on the increment must be masked off (reserved bit).
    char* end = http2WriteWindowUpdate(buf, 7, 0x80000005);
    RUVIA_CHECK(static_cast<std::size_t>(end - buf) == kHttp2FrameHeaderBytes + 4);
    const auto header = http2ParseFrameHeader(std::string_view(buf, kHttp2FrameHeaderBytes));
    RUVIA_CHECK(header.type == static_cast<std::uint8_t>(Http2FrameType::kWindowUpdate));
    RUVIA_CHECK_EQ(header.length, std::uint32_t{4});
    RUVIA_CHECK_EQ(header.streamId, std::uint32_t{7});
    RUVIA_CHECK_EQ(http2Read32(bytes(buf) + kHttp2FrameHeaderBytes), std::uint32_t{5});
}

RUVIA_TEST(frame_goaway_payload_serialization) {
    char buf[8];
    // The last-stream-id reserved bit is masked; the error code follows.
    char* end = http2WriteGoawayPayload(buf, 0x8000000F, Http2ErrorCode::kProtocolError);
    RUVIA_CHECK(end - buf == 8);
    RUVIA_CHECK_EQ(http2Read32(bytes(buf)), std::uint32_t{0x0F});
    RUVIA_CHECK_EQ(
        http2Read32(bytes(buf) + 4), static_cast<std::uint32_t>(Http2ErrorCode::kProtocolError));
}

RUVIA_TEST(http2_big_endian_readers) {
    const unsigned char data[] = {0x12, 0x34, 0x56, 0x78};
    RUVIA_CHECK_EQ(http2Read16(data), std::uint16_t{0x1234});
    RUVIA_CHECK_EQ(http2Read24(data), std::uint32_t{0x123456});
    RUVIA_CHECK_EQ(http2Read32(data), std::uint32_t{0x12345678});
    RUVIA_CHECK_EQ(http2Read31(data), std::uint32_t{0x12345678});  // high bit already 0
    // http2Read31 masks the reserved top bit; http2Read32 keeps it.
    const unsigned char high[] = {0xFF, 0xFF, 0xFF, 0xFF};
    RUVIA_CHECK_EQ(http2Read31(high), std::uint32_t{0x7fffffff});
    RUVIA_CHECK_EQ(http2Read32(high), std::uint32_t{0xffffffff});
}

RUVIA_TEST(http2_window_update_increment_masks_reserved_bit) {
    const std::string payload = std::string("\xff\xff\xff\xff", 4);
    RUVIA_CHECK_EQ(http2WindowUpdateIncrement(payload), std::uint32_t{0x7fffffff});
}

RUVIA_TEST(http2_apply_window_update) {
    std::int32_t window = 100;
    RUVIA_CHECK(http2ApplyWindowUpdate(window, 50) == Http2WindowUpdateResult::kOk);
    RUVIA_CHECK_EQ(window, 150);
    // A zero increment is a distinct (protocol-error) result.
    RUVIA_CHECK(http2ApplyWindowUpdate(window, 0) == Http2WindowUpdateResult::kZeroIncrement);
    RUVIA_CHECK_EQ(window, 150);
}

RUVIA_TEST(http2_window_update_overflow_is_flow_control_error) {
    constexpr auto kMax = std::numeric_limits<std::int32_t>::max();  // 2^31 - 1
    // A WINDOW_UPDATE pushing the window past 2^31-1 must be rejected (RFC 7540 6.9.1)...
    std::int32_t nearMax = kMax - 10;
    RUVIA_CHECK(http2ApplyWindowUpdate(nearMax, 20) == Http2WindowUpdateResult::kOverflow);
    RUVIA_CHECK_EQ(nearMax, kMax - 10);  // window is unchanged on overflow
    // ...but reaching exactly 2^31-1 is allowed.
    std::int32_t exact = kMax - 10;
    RUVIA_CHECK(http2ApplyWindowUpdate(exact, 10) == Http2WindowUpdateResult::kOk);
    RUVIA_CHECK_EQ(exact, kMax);
}
