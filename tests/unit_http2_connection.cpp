#include "test_harness.h"

#include <cstdint>
#include <memory_resource>
#include <string_view>

#include "net/http2/Http2Connection.h"
#include "net/http2/Http2FrameCodec.h"

namespace {

using ruvia::detail::Http2Connection;
using ruvia::detail::Http2FrameType;

}  // namespace

// The sans-I/O core produces a SETTINGS frame (stream 0) into its outbound buffer,
// and consumeOutput drains it. Exercises the core with zero asio / zero I/O.
RUVIA_TEST(http2_connection_queue_local_settings_emits_settings_frame) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);

    RUVIA_CHECK(conn.pendingOutput().empty());
    RUVIA_CHECK(!conn.wantsWrite());

    conn.queueLocalSettings();

    const auto out = conn.pendingOutput();
    RUVIA_CHECK(out.size() >= 9);  // at least one frame header
    RUVIA_CHECK(conn.wantsWrite());

    const auto header = ruvia::detail::http2ParseFrameHeader(out.substr(0, 9));
    RUVIA_CHECK_EQ(header.type, static_cast<std::uint8_t>(Http2FrameType::kSettings));
    RUVIA_CHECK_EQ(header.streamId, static_cast<std::uint32_t>(0));

    conn.consumeOutput(out.size());
    RUVIA_CHECK(conn.pendingOutput().empty());
    RUVIA_CHECK(!conn.wantsWrite());
}

// feed() drives the SETTINGS handshake with zero I/O: feed the peer's empty
// SETTINGS frame and the core must emit a SETTINGS ACK.
RUVIA_TEST(http2_connection_feed_settings_emits_ack) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);

    char frame[9];
    ruvia::detail::http2EncodeFrameHeader(frame, 0, Http2FrameType::kSettings, 0, 0);
    const auto result = conn.feed(std::string_view(frame, sizeof(frame)));

    RUVIA_CHECK_EQ(result.consumed, static_cast<std::size_t>(9));
    RUVIA_CHECK(result.status == ruvia::detail::Http2FeedStatus::kOk);

    const auto out = conn.pendingOutput();
    RUVIA_CHECK(out.size() >= 9);
    const auto ack = ruvia::detail::http2ParseFrameHeader(out.substr(0, 9));
    RUVIA_CHECK_EQ(ack.type, static_cast<std::uint8_t>(Http2FrameType::kSettings));
    RUVIA_CHECK((ack.flags & ruvia::detail::kHttp2FlagAck) != 0);
    RUVIA_CHECK_EQ(ack.length, static_cast<std::uint32_t>(0));
}

// A non-SETTINGS first frame is a protocol error (GOAWAY emitted, feed reports error).
RUVIA_TEST(http2_connection_feed_rejects_non_settings_first_frame) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);

    char frame[9];
    ruvia::detail::http2EncodeFrameHeader(frame, 0, Http2FrameType::kPing, 0, 0);
    const auto result = conn.feed(std::string_view(frame, sizeof(frame)));

    RUVIA_CHECK(result.status == ruvia::detail::Http2FeedStatus::kError);
    RUVIA_CHECK(conn.closing());
    const auto out = conn.pendingOutput();
    RUVIA_CHECK(out.size() >= 9);
    const auto goaway = ruvia::detail::http2ParseFrameHeader(out.substr(0, 9));
    RUVIA_CHECK_EQ(goaway.type, static_cast<std::uint8_t>(Http2FrameType::kGoaway));
}
