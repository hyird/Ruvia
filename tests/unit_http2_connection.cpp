#include "test_harness.h"

#include <cstdint>
#include <cstring>
#include <memory_resource>
#include <string_view>

#include "net/http2/Http2Connection.h"
#include "net/http2/Http2FrameCodec.h"
#include "net/http2/Http2Hpack.h"
#include "net/http2/Http2WindowUpdate.h"

namespace {

using ruvia::detail::Http2Connection;
using ruvia::detail::Http2Event;
using ruvia::detail::Http2FrameType;
using ruvia::detail::HpackEncoder;

// Encode a minimal valid GET request header block (HPACK literals) into `block`.
void encodeGetRequest(std::pmr::string& block) {
    HpackEncoder::encodeHeader(block, ":method", "GET");
    HpackEncoder::encodeHeader(block, ":scheme", "https");
    HpackEncoder::encodeHeader(block, ":path", "/");
    HpackEncoder::encodeHeader(block, ":authority", "example.com");
}

// Frame a HEADERS block on `streamId` with the given flags into a fed-ready buffer.
std::pmr::string headersFrame(
    std::pmr::memory_resource* resource, std::uint32_t streamId, std::uint8_t flags,
    std::string_view block) {
    std::pmr::string frame(resource);
    char hdr[9];
    ruvia::detail::http2EncodeFrameHeader(
        hdr, static_cast<std::uint32_t>(block.size()), Http2FrameType::kHeaders, flags, streamId);
    frame.append(hdr, 9);
    frame.append(block.data(), block.size());
    return frame;
}

// Feed the peer's empty SETTINGS frame and drain the resulting ACK, leaving the
// connection ready for post-handshake frames.
void handshake(Http2Connection& conn) {
    char settings[9];
    ruvia::detail::http2EncodeFrameHeader(settings, 0, Http2FrameType::kSettings, 0, 0);
    conn.feed(std::string_view(settings, sizeof(settings)));
    conn.consumeOutput(conn.pendingOutput().size());
}

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

// After the handshake, a PING is echoed back with the ACK flag and the same payload.
RUVIA_TEST(http2_connection_feed_ping_echoes_ack) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);

    char settings[9];
    ruvia::detail::http2EncodeFrameHeader(settings, 0, Http2FrameType::kSettings, 0, 0);
    conn.feed(std::string_view(settings, sizeof(settings)));
    conn.consumeOutput(conn.pendingOutput().size());  // drain the SETTINGS ACK

    char ping[9 + 8];
    ruvia::detail::http2EncodeFrameHeader(ping, 8, Http2FrameType::kPing, 0, 0);
    const char data[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    std::memcpy(ping + 9, data, 8);
    conn.feed(std::string_view(ping, sizeof(ping)));

    const auto out = conn.pendingOutput();
    RUVIA_CHECK_EQ(out.size(), static_cast<std::size_t>(9 + 8));
    const auto ack = ruvia::detail::http2ParseFrameHeader(out.substr(0, 9));
    RUVIA_CHECK_EQ(ack.type, static_cast<std::uint8_t>(Http2FrameType::kPing));
    RUVIA_CHECK((ack.flags & ruvia::detail::kHttp2FlagAck) != 0);
    RUVIA_CHECK(out.substr(9, 8) == std::string_view(data, 8));
}

// A valid connection-level WINDOW_UPDATE just opens the send window: no error, no
// output frame.
RUVIA_TEST(http2_connection_feed_connection_window_update_ok) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshake(conn);

    char wu[ruvia::detail::kHttp2WindowUpdateFrameBytes];
    ruvia::detail::http2WriteWindowUpdate(wu, 0, 1000);
    const auto result = conn.feed(std::string_view(wu, sizeof(wu)));

    RUVIA_CHECK(result.status == ruvia::detail::Http2FeedStatus::kOk);
    RUVIA_CHECK(!conn.closing());
    RUVIA_CHECK(conn.pendingOutput().empty());
}

// A zero-increment connection WINDOW_UPDATE is a protocol error (GOAWAY).
RUVIA_TEST(http2_connection_feed_zero_window_update_goaway) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshake(conn);

    char wu[ruvia::detail::kHttp2WindowUpdateFrameBytes];
    ruvia::detail::http2WriteWindowUpdate(wu, 0, 0);
    const auto result = conn.feed(std::string_view(wu, sizeof(wu)));

    RUVIA_CHECK(result.status == ruvia::detail::Http2FeedStatus::kError);
    RUVIA_CHECK(conn.closing());
    const auto goaway = ruvia::detail::http2ParseFrameHeader(conn.pendingOutput().substr(0, 9));
    RUVIA_CHECK_EQ(goaway.type, static_cast<std::uint8_t>(Http2FrameType::kGoaway));
}

// RST_STREAM referencing an idle (never-opened) stream is a protocol error (GOAWAY).
RUVIA_TEST(http2_connection_feed_rst_on_idle_stream_goaway) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshake(conn);

    char frame[9 + 4];
    ruvia::detail::http2EncodeFrameHeader(frame, 4, Http2FrameType::kRstStream, 0, 1);
    ruvia::detail::http2Write32(frame + 9, 0);
    const auto result = conn.feed(std::string_view(frame, sizeof(frame)));

    RUVIA_CHECK(result.status == ruvia::detail::Http2FeedStatus::kError);
    RUVIA_CHECK(conn.closing());
    const auto g = ruvia::detail::http2ParseFrameHeader(conn.pendingOutput().substr(0, 9));
    RUVIA_CHECK_EQ(g.type, static_cast<std::uint8_t>(Http2FrameType::kGoaway));
}

// PRIORITY where a stream depends on itself is a stream error: the core RST_STREAMs it
// (but the connection survives).
RUVIA_TEST(http2_connection_feed_priority_self_dependency_resets) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshake(conn);

    char frame[9 + 5];
    ruvia::detail::http2EncodeFrameHeader(frame, 5, Http2FrameType::kPriority, 0, 1);
    ruvia::detail::http2Write32(frame + 9, 1);  // depends on stream 1 (itself)
    frame[13] = 0;                              // weight
    const auto result = conn.feed(std::string_view(frame, sizeof(frame)));

    RUVIA_CHECK(result.status == ruvia::detail::Http2FeedStatus::kOk);
    RUVIA_CHECK(!conn.closing());
    const auto rst = ruvia::detail::http2ParseFrameHeader(conn.pendingOutput().substr(0, 9));
    RUVIA_CHECK_EQ(rst.type, static_cast<std::uint8_t>(Http2FrameType::kRstStream));
    RUVIA_CHECK_EQ(rst.streamId, static_cast<std::uint32_t>(1));
}

// A complete HEADERS frame (END_HEADERS + END_STREAM) decodes the request head and the
// sans-I/O core emits kRequestHeaders then kRequestEnd; the head is exposed via stream().
RUVIA_TEST(http2_connection_feed_headers_emits_request_event) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshake(conn);

    std::pmr::string block(&resource);
    encodeGetRequest(block);
    const auto frame = headersFrame(
        &resource, 1,
        ruvia::detail::kHttp2FlagEndHeaders | ruvia::detail::kHttp2FlagEndStream,
        std::string_view(block.data(), block.size()));
    const auto result = conn.feed(std::string_view(frame.data(), frame.size()));

    RUVIA_CHECK(result.status == ruvia::detail::Http2FeedStatus::kOk);
    RUVIA_CHECK(!conn.closing());

    const auto e1 = conn.nextEvent();
    RUVIA_CHECK(e1.kind == Http2Event::Kind::kRequestHeaders);
    RUVIA_CHECK_EQ(e1.streamId, static_cast<std::uint32_t>(1));
    const auto e2 = conn.nextEvent();
    RUVIA_CHECK(e2.kind == Http2Event::Kind::kRequestEnd);
    RUVIA_CHECK_EQ(e2.streamId, static_cast<std::uint32_t>(1));
    RUVIA_CHECK(conn.nextEvent().kind == Http2Event::Kind::kNone);

    auto* s = conn.stream(1);
    RUVIA_CHECK(s != nullptr);
    RUVIA_CHECK(s->requestMethod() == ruvia::HttpMethod::kGet);
}

// A HEADERS frame WITHOUT END_HEADERS leaves the block open (awaiting CONTINUATION); a
// CONTINUATION carrying the rest with END_HEADERS completes the head and emits the event.
RUVIA_TEST(http2_connection_feed_headers_continuation_completes_head) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshake(conn);

    std::pmr::string first(&resource);
    HpackEncoder::encodeHeader(first, ":method", "GET");
    HpackEncoder::encodeHeader(first, ":scheme", "https");
    std::pmr::string second(&resource);
    HpackEncoder::encodeHeader(second, ":path", "/");
    HpackEncoder::encodeHeader(second, ":authority", "example.com");

    // HEADERS with END_STREAM but no END_HEADERS -> no event yet.
    const auto h = headersFrame(
        &resource, 1, ruvia::detail::kHttp2FlagEndStream,
        std::string_view(first.data(), first.size()));
    RUVIA_CHECK(conn.feed(std::string_view(h.data(), h.size())).status ==
                ruvia::detail::Http2FeedStatus::kOk);
    RUVIA_CHECK(conn.nextEvent().kind == Http2Event::Kind::kNone);

    // CONTINUATION with END_HEADERS -> head completes.
    char chdr[9];
    ruvia::detail::http2EncodeFrameHeader(
        chdr, static_cast<std::uint32_t>(second.size()), Http2FrameType::kContinuation,
        ruvia::detail::kHttp2FlagEndHeaders, 1);
    std::pmr::string cont(&resource);
    cont.append(chdr, 9);
    cont.append(second.data(), second.size());
    RUVIA_CHECK(conn.feed(std::string_view(cont.data(), cont.size())).status ==
                ruvia::detail::Http2FeedStatus::kOk);

    RUVIA_CHECK(conn.nextEvent().kind == Http2Event::Kind::kRequestHeaders);
    RUVIA_CHECK(conn.nextEvent().kind == Http2Event::Kind::kRequestEnd);
    auto* s = conn.stream(1);
    RUVIA_CHECK(s != nullptr && s->requestMethod() == ruvia::HttpMethod::kGet);
}
