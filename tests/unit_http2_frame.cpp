#include "test_harness.h"

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

#include "ruvia/http/detail/http2/Http2FrameCodec.h"
#include "ruvia/http/detail/http2/Http2FlowControl.h"

namespace {

using ruvia::detail::Http2WindowUpdateResult;
using ruvia::detail::http2ApplyWindowUpdate;
using ruvia::detail::http2Read16;
using ruvia::detail::http2Read24;
using ruvia::detail::http2Read31;
using ruvia::detail::http2Read32;
using ruvia::detail::http2WindowUpdateIncrement;

}  // namespace

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
