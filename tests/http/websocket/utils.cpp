#include "test_harness.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory_resource>
#include <string>
#include <string_view>

#include "ruvia/http/ProtocolByteLimit.h"
#include "ruvia/http/detail/websocket/frame/HttpWebSocketFrameCodec.h"
#include "ruvia/http/detail/websocket/frame/HttpWebSocketFrameReader.h"
#include "ruvia/http/WebSocketProtocol.h"

namespace {

using ruvia::ProtocolByteLimit;
using ruvia::WebSocketOpcode;

}  // namespace

RUVIA_TEST(websocket_raw_opcode_validity) {
    using ruvia::detail::isInvalidWebSocketRawOpcode;
    // Valid: 0x0 continuation, 0x1 text, 0x2 binary, 0x8-0xA control (RFC 6455 5.2).
    RUVIA_CHECK(!isInvalidWebSocketRawOpcode(0x0));
    RUVIA_CHECK(!isInvalidWebSocketRawOpcode(0x1));
    RUVIA_CHECK(!isInvalidWebSocketRawOpcode(0x2));
    RUVIA_CHECK(!isInvalidWebSocketRawOpcode(0x8));
    RUVIA_CHECK(!isInvalidWebSocketRawOpcode(0x9));
    RUVIA_CHECK(!isInvalidWebSocketRawOpcode(0xA));
    // Reserved non-control 0x3-0x7 and reserved control 0xB-0xF are invalid.
    for (std::uint8_t op = 0x3; op <= 0x7; ++op) {
        RUVIA_CHECK(isInvalidWebSocketRawOpcode(op));
    }
    for (std::uint8_t op = 0xB; op <= 0xF; ++op) {
        RUVIA_CHECK(isInvalidWebSocketRawOpcode(op));
    }
}

RUVIA_TEST(websocket_control_opcode_classification) {
    using ruvia::detail::isWebSocketControlOpcode;
    RUVIA_CHECK(!isWebSocketControlOpcode(WebSocketOpcode::kText));
    RUVIA_CHECK(!isWebSocketControlOpcode(WebSocketOpcode::kBinary));
    RUVIA_CHECK(isWebSocketControlOpcode(WebSocketOpcode::kClose));
    RUVIA_CHECK(isWebSocketControlOpcode(WebSocketOpcode::kPing));
    RUVIA_CHECK(isWebSocketControlOpcode(WebSocketOpcode::kPong));
}

RUVIA_TEST(websocket_frame_message_limit_exempts_control_frames) {
    using ruvia::detail::webSocketFrameExceedsMessageLimit;
    using ruvia::detail::WebSocketFrameKind;
    // Data frames are measured against the per-message size limit.
    const auto limit = ProtocolByteLimit::limited(64);
    RUVIA_CHECK(webSocketFrameExceedsMessageLimit(WebSocketFrameKind::kText, 100, limit));
    RUVIA_CHECK(webSocketFrameExceedsMessageLimit(WebSocketFrameKind::kBinary, 100, limit));
    RUVIA_CHECK(!webSocketFrameExceedsMessageLimit(WebSocketFrameKind::kText, 50, limit));

    // Control frames (Close/Ping/Pong) are capped at 125 by RFC 6455 5.5 and are
    // NOT subject to the message-size limit: a 100-byte Ping, or a Close carrying a
    // reason phrase, must pass even when maxMessageBytes is 64.
    RUVIA_CHECK(!webSocketFrameExceedsMessageLimit(WebSocketFrameKind::kPing, 100, limit));
    RUVIA_CHECK(!webSocketFrameExceedsMessageLimit(WebSocketFrameKind::kPong, 100, limit));
    RUVIA_CHECK(!webSocketFrameExceedsMessageLimit(WebSocketFrameKind::kClose, 100, limit));
    RUVIA_CHECK(!webSocketFrameExceedsMessageLimit(
        WebSocketFrameKind::kText, 1'000'000, ProtocolByteLimit::unlimited()));
}

RUVIA_TEST(websocket_message_size_limits) {
    using ruvia::detail::webSocketAppendExceedsLimit;
    using ruvia::detail::webSocketMessageExceedsLimit;
    const auto limit = ProtocolByteLimit::limited(100);
    RUVIA_CHECK(!webSocketMessageExceedsLimit(1'000'000, ProtocolByteLimit::unlimited()));
    RUVIA_CHECK(!webSocketMessageExceedsLimit(100, limit));  // exact fit is allowed
    RUVIA_CHECK(webSocketMessageExceedsLimit(101, limit));

    // Append accounting is overflow-safe (uses subtraction, never current+append).
    RUVIA_CHECK(!webSocketAppendExceedsLimit(70, 30, limit));  // 70+30 == 100 ok
    RUVIA_CHECK(webSocketAppendExceedsLimit(71, 30, limit));   // 71+30 > 100
    RUVIA_CHECK(webSocketAppendExceedsLimit(0, 101, limit));   // single append over limit
    RUVIA_CHECK(!webSocketAppendExceedsLimit(1'000'000, 1, ProtocolByteLimit::unlimited()));
    constexpr auto kMax = (std::numeric_limits<std::size_t>::max)();
    RUVIA_CHECK(webSocketAppendExceedsLimit(kMax - 10, 20, limit));  // no wraparound
}

RUVIA_TEST(websocket_frame_length_and_read_overflow_guards) {
    using ruvia::detail::webSocketFrameLengthExceedsLimit;
    using ruvia::detail::webSocketMaskedFrameReadSizeOverflows;
    constexpr auto kU64Max = (std::numeric_limits<std::uint64_t>::max)();

    const auto limit = ProtocolByteLimit::limited(1000);
    RUVIA_CHECK(!webSocketFrameLengthExceedsLimit(100, limit));
    RUVIA_CHECK(webSocketFrameLengthExceedsLimit(2000, limit));
    RUVIA_CHECK(!webSocketFrameLengthExceedsLimit(2000, ProtocolByteLimit::unlimited()));
    // A declared 64-bit length beyond any addressable size is over the limit.
    RUVIA_CHECK(webSocketFrameLengthExceedsLimit(kU64Max, limit));

    // header + 4-byte mask + payload must not overflow size_t.
    RUVIA_CHECK(!webSocketMaskedFrameReadSizeOverflows(100, 14));
    RUVIA_CHECK(webSocketMaskedFrameReadSizeOverflows(kU64Max, 14));
}

RUVIA_TEST(websocket_integer_readers) {
    using ruvia::detail::readWebSocketUint16;
    using ruvia::detail::readWebSocketUint64;

    const char be16[] = {static_cast<char>(0x12), static_cast<char>(0x34)};
    RUVIA_CHECK_EQ(readWebSocketUint16(be16), std::uint16_t{0x1234});

    // 64-bit big-endian, most-significant bit clear -> 0x0100 = 256.
    const char be64[] = {0, 0, 0, 0, 0, 0, static_cast<char>(0x01), 0};
    std::uint64_t value = 0;
    RUVIA_CHECK(readWebSocketUint64(be64, value));
    RUVIA_CHECK_EQ(value, std::uint64_t{256});

    // The MSB of a 64-bit length must be 0 (RFC 6455 5.2); otherwise rejected.
    const char msbSet[] = {static_cast<char>(0x80), 0, 0, 0, 0, 0, 0, 0};
    std::uint64_t rejected = 123;
    RUVIA_CHECK(!readWebSocketUint64(msbSet, rejected));
}

RUVIA_TEST(websocket_read_buffer_compaction) {
    using ruvia::detail::compactWebSocketReadBuffer;
    const auto make = [](std::string_view text) {
        std::pmr::string buffer(std::pmr::get_default_resource());
        buffer.assign(text.data(), text.size());
        return buffer;
    };

    // pendingCompactUntil == 0 -> no-op, offset untouched.
    {
        auto buffer = make("hello");
        std::size_t offset = 2;
        std::size_t pending = 0;
        compactWebSocketReadBuffer(buffer, offset, pending);
        RUVIA_CHECK_EQ(std::string_view(buffer), std::string_view("hello"));
        RUVIA_CHECK_EQ(offset, std::size_t{2});
    }
    // Fully consumed -> clear the buffer.
    {
        auto buffer = make("hello");
        std::size_t offset = 0;
        std::size_t pending = 5;
        compactWebSocketReadBuffer(buffer, offset, pending);
        RUVIA_CHECK(buffer.empty());
        RUVIA_CHECK_EQ(offset, std::size_t{0});
        RUVIA_CHECK_EQ(pending, std::size_t{0});
    }
    // Small prefix below half -> lazy: offset advances, bytes stay in place.
    {
        auto buffer = make("0123456789");
        std::size_t offset = 0;
        std::size_t pending = 3;
        compactWebSocketReadBuffer(buffer, offset, pending);
        RUVIA_CHECK_EQ(std::string_view(buffer), std::string_view("0123456789"));
        RUVIA_CHECK_EQ(offset, std::size_t{3});
        RUVIA_CHECK_EQ(pending, std::size_t{0});
    }
    // Consumed prefix >= remaining -> compact: tail moves to the front.
    {
        auto buffer = make("PREFIXtail");
        std::size_t offset = 0;
        std::size_t pending = 6;
        compactWebSocketReadBuffer(buffer, offset, pending);
        RUVIA_CHECK_EQ(std::string_view(buffer), std::string_view("tail"));
        RUVIA_CHECK_EQ(offset, std::size_t{0});
    }
}
