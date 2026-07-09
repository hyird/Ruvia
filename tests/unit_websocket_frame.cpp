#include "test_harness.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>

#include "net/ws/HttpWebSocketUtils.h"
#include "ruvia/http/WebSocketProtocol.h"

namespace {

using ruvia::WebSocketOpcode;
using ruvia::detail::decodeMaskedWebSocketPayload;
using ruvia::detail::decodeWebSocketFrameStart;
using ruvia::detail::encodeWebSocketFrameHeader;
using ruvia::detail::isInvalidWebSocketControlFrame;
using ruvia::detail::WebSocketFrameHeader;
using ruvia::detail::WebSocketFrameStart;
using ruvia::detail::webSocketFrameLengthExceedsLimit;

}  // namespace

// RFC 6455 §5.2: server-received client frames must be masked (second byte high
// bit set), RSV2/RSV3 must be clear, and reserved opcodes are rejected.
RUVIA_TEST(ws_frame_start_accepts_valid_masked_frames) {
    WebSocketFrameStart frame;
    RUVIA_CHECK(decodeWebSocketFrameStart(0x81, 0x80, frame, false));  // FIN + text, masked
    RUVIA_CHECK(frame.fin && !frame.continuation && !frame.rsv1);
    RUVIA_CHECK(frame.opcode == WebSocketOpcode::kText);
    RUVIA_CHECK(decodeWebSocketFrameStart(0x82, 0x80, frame, false));  // binary
    RUVIA_CHECK(frame.opcode == WebSocketOpcode::kBinary);
    RUVIA_CHECK(decodeWebSocketFrameStart(0x88, 0x80, frame, false));  // close
    RUVIA_CHECK(frame.opcode == WebSocketOpcode::kClose);
    RUVIA_CHECK(decodeWebSocketFrameStart(0x89, 0x80, frame, false));  // ping
    RUVIA_CHECK(decodeWebSocketFrameStart(0x8A, 0x80, frame, false));  // pong
    RUVIA_CHECK(decodeWebSocketFrameStart(0x80, 0x80, frame, false));  // continuation, FIN
    RUVIA_CHECK(frame.continuation);
    RUVIA_CHECK(decodeWebSocketFrameStart(0x01, 0x80, frame, false));  // non-FIN text (fragment start)
    RUVIA_CHECK(!frame.fin);
}

RUVIA_TEST(ws_frame_start_rejects_malformed) {
    WebSocketFrameStart frame;
    RUVIA_CHECK(!decodeWebSocketFrameStart(0x81, 0x00, frame, false));  // not masked
    RUVIA_CHECK(!decodeWebSocketFrameStart(0x91, 0x80, frame, false));  // RSV2 set
    RUVIA_CHECK(!decodeWebSocketFrameStart(0xA1, 0x80, frame, false));  // RSV3 set
    RUVIA_CHECK(!decodeWebSocketFrameStart(0x83, 0x80, frame, false));  // reserved opcode 0x3
    RUVIA_CHECK(!decodeWebSocketFrameStart(0x87, 0x80, frame, false));  // reserved opcode 0x7
    RUVIA_CHECK(!decodeWebSocketFrameStart(0x8B, 0x80, frame, false));  // reserved control 0xB
    RUVIA_CHECK(!decodeWebSocketFrameStart(0x8F, 0x80, frame, false));  // reserved control 0xF
}

RUVIA_TEST(ws_frame_start_rsv1_rules) {
    // RSV1 (compression) is valid only on the first data frame when negotiated.
    WebSocketFrameStart frame;
    RUVIA_CHECK(!decodeWebSocketFrameStart(0xC1, 0x80, frame, false));  // rsv1 but not allowed
    RUVIA_CHECK(decodeWebSocketFrameStart(0xC1, 0x80, frame, true));    // rsv1 on text, allowed
    RUVIA_CHECK(frame.rsv1);
    RUVIA_CHECK(!decodeWebSocketFrameStart(0xC0, 0x80, frame, true));   // rsv1 on continuation -> reject
    RUVIA_CHECK(!decodeWebSocketFrameStart(0xC8, 0x80, frame, true));   // rsv1 on control frame -> reject
}

RUVIA_TEST(ws_control_frame_rules) {
    const WebSocketFrameStart close{.opcode = WebSocketOpcode::kClose, .fin = true};
    RUVIA_CHECK(!isInvalidWebSocketControlFrame(close, 0));
    RUVIA_CHECK(!isInvalidWebSocketControlFrame(close, 125));
    RUVIA_CHECK(isInvalidWebSocketControlFrame(close, 126));  // control payload must be <= 125
    const WebSocketFrameStart fragmentedPing{.opcode = WebSocketOpcode::kPing, .fin = false};
    RUVIA_CHECK(isInvalidWebSocketControlFrame(fragmentedPing, 10));  // control frames must be FIN
    const WebSocketFrameStart data{.opcode = WebSocketOpcode::kText, .fin = false};
    RUVIA_CHECK(!isInvalidWebSocketControlFrame(data, 1000000));  // data frames are not control frames
}

RUVIA_TEST(ws_encode_frame_header_length_boundaries) {
    WebSocketFrameHeader header{};
    const auto u8 = [&header](std::size_t index) {
        return static_cast<unsigned char>(header[index]);
    };
    // <=125: 1-byte length, no mask bit (server frames are unmasked).
    RUVIA_CHECK_EQ(encodeWebSocketFrameHeader(header, WebSocketOpcode::kText, 125), std::size_t{2});
    RUVIA_CHECK_EQ(u8(0), 0x81U);
    RUVIA_CHECK_EQ(u8(1), 125U);
    // 126..0xFFFF: 126 marker + 16-bit length.
    RUVIA_CHECK_EQ(encodeWebSocketFrameHeader(header, WebSocketOpcode::kBinary, 126), std::size_t{4});
    RUVIA_CHECK_EQ(u8(0), 0x82U);
    RUVIA_CHECK_EQ(u8(1), 126U);
    RUVIA_CHECK_EQ(u8(2), 0U);
    RUVIA_CHECK_EQ(u8(3), 126U);
    RUVIA_CHECK_EQ(encodeWebSocketFrameHeader(header, WebSocketOpcode::kText, 65535), std::size_t{4});
    // >0xFFFF: 127 marker + 64-bit length. 65536 = 0x0000000000010000.
    RUVIA_CHECK_EQ(encodeWebSocketFrameHeader(header, WebSocketOpcode::kText, 65536), std::size_t{10});
    RUVIA_CHECK_EQ(u8(1), 127U);
    RUVIA_CHECK_EQ(u8(2), 0U);
    RUVIA_CHECK_EQ(u8(7), 1U);  // the 0x10000 bit
    RUVIA_CHECK_EQ(u8(8), 0U);
    RUVIA_CHECK_EQ(u8(9), 0U);
}

RUVIA_TEST(ws_mask_unmask_round_trip_all_tail_sizes) {
    const char mask[4] = {0x12, 0x34, 0x56, 0x78};
    // XOR masking is involutive, so masking twice restores the payload; cover every
    // tail remainder (0..3) past the 4-byte-unrolled body.
    for (std::size_t n = 0; n <= 10; ++n) {
        std::string original(n, '\0');
        for (std::size_t i = 0; i < n; ++i) {
            original[i] = static_cast<char>('A' + static_cast<int>(i));
        }
        std::string buffer = original;
        decodeMaskedWebSocketPayload(buffer.data(), buffer.size(), mask);
        if (n > 0) {
            RUVIA_CHECK(buffer != original);
        }
        decodeMaskedWebSocketPayload(buffer.data(), buffer.size(), mask);
        RUVIA_CHECK_EQ(buffer, original);
    }
    // Known value: 'A' (0x41) ^ 0x12 == 0x53.
    std::string one("A");
    decodeMaskedWebSocketPayload(one.data(), one.size(), mask);
    RUVIA_CHECK_EQ(static_cast<unsigned char>(one[0]), 0x53U);
}

RUVIA_TEST(ws_frame_length_limit) {
    RUVIA_CHECK(!webSocketFrameLengthExceedsLimit(100, 1000));       // within limit
    RUVIA_CHECK(webSocketFrameLengthExceedsLimit(1001, 1000));       // over limit
    RUVIA_CHECK(!webSocketFrameLengthExceedsLimit(1000000, 0));      // 0 == unlimited
    RUVIA_CHECK(webSocketFrameLengthExceedsLimit(
        std::numeric_limits<std::uint64_t>::max(), 1000));          // enormous payload over limit
}
