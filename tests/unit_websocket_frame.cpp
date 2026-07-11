#include "test_harness.h"

#include <array>
#include <cstddef>
#include <concepts>
#include <cstdint>
#include <limits>
#include <memory_resource>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "ruvia/http/detail/websocket/HttpWebSocketUtils.h"
#include "ruvia/http/WebSocketProtocol.h"

namespace {

using ruvia::WebSocketOpcode;
using ruvia::detail::decodeMaskedWebSocketPayload;
using ruvia::detail::decodeWebSocketFrameStart;
using ruvia::detail::encodeWebSocketFrameHeader;
using ruvia::detail::isInvalidWebSocketControlFrame;
using ruvia::detail::WebSocketFrameHeader;
using ruvia::detail::WebSocketFrameReadResult;
using ruvia::detail::WebSocketFrameStart;
using ruvia::detail::WebSocketProtocolFailure;
using ruvia::detail::webSocketFrameLengthExceedsLimit;
using ruvia::detail::webSocketTryReadFrame;

template <typename T>
concept HasFrameReadStatusField = requires(const T& result) {
    result.status;
};

template <typename T>
concept HasFrameReadStatusAccessor = requires(const T& result) {
    result.status();
};

template <typename T>
concept HasRequiredBytesField = requires(const T& result) {
    result.requiredBytes;
};

template <typename T>
concept HasCleanEofAllowedField = requires(const T& result) {
    result.cleanEofAllowed;
};

template <typename T>
concept HasFrameReadError = requires(const T& result) {
    { result.error() } -> std::same_as<WebSocketProtocolFailure>;
};

static_assert(!std::default_initializable<WebSocketFrameReadResult>);
static_assert(std::same_as<
    decltype(std::declval<const WebSocketFrameReadResult&>().needInput()),
    const ruvia::detail::WebSocketFrameNeedInput*>);
static_assert(std::same_as<
    decltype(std::declval<const WebSocketFrameReadResult&>().frame()),
    const ruvia::detail::WebSocketFrameView*>);
static_assert(std::same_as<
    decltype(std::declval<const WebSocketFrameReadResult&>().failure()),
    const ruvia::detail::WebSocketFrameReadFailure*>);
static_assert(!HasFrameReadStatusField<WebSocketFrameReadResult>);
static_assert(!HasFrameReadStatusAccessor<WebSocketFrameReadResult>);
static_assert(!HasRequiredBytesField<WebSocketFrameReadResult>);
static_assert(!HasCleanEofAllowedField<WebSocketFrameReadResult>);
static_assert(!HasFrameReadError<WebSocketFrameReadResult>);
static_assert(HasFrameReadError<ruvia::detail::WebSocketFrameReadFailure>);

std::pmr::string maskedFrame(
    unsigned char first,
    std::string_view payload) {
    constexpr std::array<unsigned char, 4> mask{0x12, 0x34, 0x56, 0x78};
    std::pmr::string bytes(std::pmr::get_default_resource());
    bytes.reserve(2 + mask.size() + payload.size());
    bytes.push_back(static_cast<char>(first));
    bytes.push_back(static_cast<char>(0x80U | payload.size()));
    for (const auto byte : mask) {
        bytes.push_back(static_cast<char>(byte));
    }
    for (std::size_t index = 0; index < payload.size(); ++index) {
        const auto byte = static_cast<unsigned char>(payload[index]);
        bytes.push_back(static_cast<char>(byte ^ mask[index & 3U]));
    }
    return bytes;
}

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

RUVIA_TEST(ws_frame_reader_needs_input_without_sentinel_metadata) {
    std::pmr::string input(std::pmr::get_default_resource());
    std::size_t offset = 0;
    std::size_t pendingCompactUntil = 0;

    const auto empty = webSocketTryReadFrame(
        input, offset, pendingCompactUntil, 1024, false);
    RUVIA_CHECK(empty.needInput() != nullptr);
    RUVIA_CHECK(empty.frame() == nullptr);
    RUVIA_CHECK(empty.failure() == nullptr);

    input.push_back(static_cast<char>(0x81));
    const auto partial = webSocketTryReadFrame(
        input, offset, pendingCompactUntil, 1024, false);
    RUVIA_CHECK(partial.needInput() != nullptr);
    RUVIA_CHECK(partial.frame() == nullptr);
    RUVIA_CHECK(partial.failure() == nullptr);
    RUVIA_CHECK_EQ(offset, std::size_t{0});
    RUVIA_CHECK_EQ(pendingCompactUntil, std::size_t{0});
}

RUVIA_TEST(ws_frame_reader_returns_one_unmasked_borrowed_frame) {
    auto input = maskedFrame(0x81, "hi");
    std::size_t offset = 0;
    std::size_t pendingCompactUntil = 0;

    const auto result = webSocketTryReadFrame(
        input, offset, pendingCompactUntil, 1024, false);
    RUVIA_CHECK(result.needInput() == nullptr);
    RUVIA_CHECK(result.failure() == nullptr);
    RUVIA_CHECK(result.frame() != nullptr);
    RUVIA_CHECK(result.frame()->opcode == WebSocketOpcode::kText);
    RUVIA_CHECK(result.frame()->fin);
    RUVIA_CHECK(!result.frame()->continuation);
    RUVIA_CHECK_EQ(result.frame()->payload, std::string_view("hi"));
    RUVIA_CHECK_EQ(offset, input.size());
    RUVIA_CHECK_EQ(pendingCompactUntil, input.size());
}

RUVIA_TEST(ws_frame_reader_reports_typed_wire_failures) {
    std::size_t offset = 0;
    std::size_t pendingCompactUntil = 0;
    std::pmr::string unmasked(
        std::string_view("\x81\x02hi", 4),
        std::pmr::get_default_resource());
    const auto maskFailure = webSocketTryReadFrame(
        unmasked, offset, pendingCompactUntil, 1024, false);
    RUVIA_CHECK(maskFailure.failure() != nullptr);
    RUVIA_CHECK(
        maskFailure.failure()->error() ==
        WebSocketProtocolFailure::kProtocolError);
    RUVIA_CHECK(maskFailure.frame() == nullptr);

    auto tooLarge = maskedFrame(0x82, "123456");
    offset = 0;
    pendingCompactUntil = 0;
    const auto sizeFailure = webSocketTryReadFrame(
        tooLarge, offset, pendingCompactUntil, 5, false);
    RUVIA_CHECK(sizeFailure.failure() != nullptr);
    RUVIA_CHECK(
        sizeFailure.failure()->error() ==
        WebSocketProtocolFailure::kMessageTooLarge);

    const std::string invalidClose(
        "\x03\xe8\xc0\x80", 4);  // 1000 plus invalid UTF-8 reason
    auto close = maskedFrame(0x88, invalidClose);
    offset = 0;
    pendingCompactUntil = 0;
    const auto closeFailure = webSocketTryReadFrame(
        close, offset, pendingCompactUntil, 1024, false);
    RUVIA_CHECK(closeFailure.failure() != nullptr);
    RUVIA_CHECK(
        closeFailure.failure()->error() ==
        WebSocketProtocolFailure::kInvalidPayloadData);
}
