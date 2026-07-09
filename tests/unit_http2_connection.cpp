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
    (void)conn.feed(std::string_view(settings, sizeof(settings)));
    conn.consumeOutput(conn.pendingOutput().size());
}

using ruvia::detail::Http2SubmitResult;

// Handshake but declare a small peer SETTINGS_INITIAL_WINDOW_SIZE so freshly created
// streams start with a tiny send window (to exercise flow-control backpressure).
void handshakeWithWindow(Http2Connection& conn, std::uint32_t window) {
    char s[9 + 6];
    ruvia::detail::http2EncodeFrameHeader(s, 6, Http2FrameType::kSettings, 0, 0);
    s[9] = 0;
    s[10] = 4;  // SETTINGS_INITIAL_WINDOW_SIZE
    s[11] = static_cast<char>((window >> 24) & 0xFF);
    s[12] = static_cast<char>((window >> 16) & 0xFF);
    s[13] = static_cast<char>((window >> 8) & 0xFF);
    s[14] = static_cast<char>(window & 0xFF);
    (void)conn.feed(std::string_view(s, sizeof(s)));
    conn.consumeOutput(conn.pendingOutput().size());
}

// Feed a complete GET on stream 1, drain its events and any output, leaving stream 1
// open (half-closed remote) and ready to receive a response.
void driveGetRequest(Http2Connection& conn, std::pmr::memory_resource* res) {
    std::pmr::string block(res);
    encodeGetRequest(block);
    const auto h = headersFrame(
        res, 1, ruvia::detail::kHttp2FlagEndHeaders | ruvia::detail::kHttp2FlagEndStream,
        std::string_view(block.data(), block.size()));
    (void)conn.feed(std::string_view(h.data(), h.size()));
    while (conn.nextEvent().kind != Http2Event::Kind::kNone) {
    }
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
    (void)conn.feed(std::string_view(settings, sizeof(settings)));
    conn.consumeOutput(conn.pendingOutput().size());  // drain the SETTINGS ACK

    char ping[9 + 8];
    ruvia::detail::http2EncodeFrameHeader(ping, 8, Http2FrameType::kPing, 0, 0);
    const char data[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    std::memcpy(ping + 9, data, 8);
    (void)conn.feed(std::string_view(ping, sizeof(ping)));

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

    // Self-dependent PRIORITY on a LIVE stream is a stream error -> RST_STREAM.
    driveGetRequest(conn, &resource);  // stream 1 open
    conn.consumeOutput(conn.pendingOutput().size());
    char live[9 + 5];
    ruvia::detail::http2EncodeFrameHeader(live, 5, Http2FrameType::kPriority, 0, 1);
    ruvia::detail::http2Write32(live + 9, 1);  // depends on stream 1 (itself)
    live[13] = 0;
    RUVIA_CHECK(conn.feed(std::string_view(live, sizeof(live))).status ==
                ruvia::detail::Http2FeedStatus::kOk);
    RUVIA_CHECK(!conn.closing());
    const auto rst = ruvia::detail::http2ParseFrameHeader(conn.pendingOutput().substr(0, 9));
    RUVIA_CHECK_EQ(rst.type, static_cast<std::uint8_t>(Http2FrameType::kRstStream));
    RUVIA_CHECK_EQ(rst.streamId, static_cast<std::uint32_t>(1));
    conn.consumeOutput(conn.pendingOutput().size());

    // Self-dependent PRIORITY on an IDLE stream is IGNORED (a deprecated advisory frame;
    // RST_STREAM on an idle stream would provoke a peer connection error). No output,
    // connection survives.
    char idle[9 + 5];
    ruvia::detail::http2EncodeFrameHeader(idle, 5, Http2FrameType::kPriority, 0, 7);
    ruvia::detail::http2Write32(idle + 9, 7);  // idle stream depends on itself
    idle[13] = 0;
    RUVIA_CHECK(conn.feed(std::string_view(idle, sizeof(idle))).status ==
                ruvia::detail::Http2FeedStatus::kOk);
    RUVIA_CHECK(!conn.closing());
    RUVIA_CHECK(conn.pendingOutput().empty());  // ignored: no RST, no GOAWAY
}

// A complete HEADERS frame (END_HEADERS + END_STREAM) decodes the request head and the
// sans-I/O core emits kMessageHead then kMessageEnd; the head is exposed via stream().
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
    RUVIA_CHECK(e1.kind == Http2Event::Kind::kMessageHead);
    RUVIA_CHECK_EQ(e1.streamId, static_cast<std::uint32_t>(1));
    const auto e2 = conn.nextEvent();
    RUVIA_CHECK(e2.kind == Http2Event::Kind::kMessageEnd);
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

    RUVIA_CHECK(conn.nextEvent().kind == Http2Event::Kind::kMessageHead);
    RUVIA_CHECK(conn.nextEvent().kind == Http2Event::Kind::kMessageEnd);
    auto* s = conn.stream(1);
    RUVIA_CHECK(s != nullptr && s->requestMethod() == ruvia::HttpMethod::kGet);
}

// Build a POST request head (no END_STREAM) with optional content-length; body follows.
std::pmr::string postHeadFrame(
    std::pmr::memory_resource* resource, std::string_view contentLength) {
    std::pmr::string block(resource);
    HpackEncoder::encodeHeader(block, ":method", "POST");
    HpackEncoder::encodeHeader(block, ":scheme", "https");
    HpackEncoder::encodeHeader(block, ":path", "/");
    HpackEncoder::encodeHeader(block, ":authority", "example.com");
    if (!contentLength.empty()) {
        HpackEncoder::encodeHeader(block, "content-length", contentLength);
    }
    return headersFrame(
        resource, 1, ruvia::detail::kHttp2FlagEndHeaders,
        std::string_view(block.data(), block.size()));
}

// Frame a DATA payload on `streamId` with the given flags.
std::pmr::string dataFrame(
    std::pmr::memory_resource* resource, std::uint32_t streamId, std::uint8_t flags,
    std::string_view body) {
    std::pmr::string frame(resource);
    char hdr[9];
    ruvia::detail::http2EncodeFrameHeader(
        hdr, static_cast<std::uint32_t>(body.size()), Http2FrameType::kData, flags, streamId);
    frame.append(hdr, 9);
    frame.append(body.data(), body.size());
    return frame;
}

// A DATA frame after the head yields a kMessageBodyChunk carrying the bytes, then
// (on END_STREAM) kMessageEnd; the core also credits the peer back with WINDOW_UPDATE.
RUVIA_TEST(http2_connection_feed_data_emits_body_chunk_and_end) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshake(conn);

    const auto h = postHeadFrame(&resource, "");
    (void)conn.feed(std::string_view(h.data(), h.size()));
    RUVIA_CHECK(conn.nextEvent().kind == Http2Event::Kind::kMessageHead);
    RUVIA_CHECK(conn.nextEvent().kind == Http2Event::Kind::kNone);

    const char body[5] = {'h', 'e', 'l', 'l', 'o'};
    const auto d = dataFrame(
        &resource, 1, ruvia::detail::kHttp2FlagEndStream, std::string_view(body, 5));
    (void)conn.feed(std::string_view(d.data(), d.size()));

    const auto chunk = conn.nextEvent();
    RUVIA_CHECK(chunk.kind == Http2Event::Kind::kMessageBodyChunk);
    RUVIA_CHECK_EQ(chunk.streamId, static_cast<std::uint32_t>(1));
    RUVIA_CHECK(chunk.bytes == std::string_view(body, 5));
    const auto end = conn.nextEvent();
    RUVIA_CHECK(end.kind == Http2Event::Kind::kMessageEnd);
    RUVIA_CHECK_EQ(end.streamId, static_cast<std::uint32_t>(1));

    const auto wu = ruvia::detail::http2ParseFrameHeader(conn.pendingOutput().substr(0, 9));
    RUVIA_CHECK_EQ(wu.type, static_cast<std::uint8_t>(Http2FrameType::kWindowUpdate));
}

// A DATA/END_STREAM that falls short of a declared content-length is a protocol error:
// the core RST_STREAMs the stream and does NOT emit kMessageEnd.
RUVIA_TEST(http2_connection_feed_data_short_of_content_length_resets) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshake(conn);

    const auto h = postHeadFrame(&resource, "10");  // promises 10 bytes
    (void)conn.feed(std::string_view(h.data(), h.size()));
    RUVIA_CHECK(conn.nextEvent().kind == Http2Event::Kind::kMessageHead);

    const char body[5] = {'s', 'h', 'o', 'r', 't'};  // only 5, with END_STREAM
    const auto d = dataFrame(
        &resource, 1, ruvia::detail::kHttp2FlagEndStream, std::string_view(body, 5));
    (void)conn.feed(std::string_view(d.data(), d.size()));

    RUVIA_CHECK(conn.nextEvent().kind == Http2Event::Kind::kMessageBodyChunk);
    // The length mismatch aborts the stream: kStreamClosed (never kMessageEnd), and
    // the (unpinned) stream is removed from the table.
    RUVIA_CHECK(conn.nextEvent().kind == Http2Event::Kind::kStreamClosed);
    RUVIA_CHECK(conn.nextEvent().kind == Http2Event::Kind::kNone);
    RUVIA_CHECK(conn.stream(1) == nullptr);
}

// submitResponseHead emits a HEADERS block (END_HEADERS, no END_STREAM when a body
// follows); submitData then sends the buffered body as a terminal DATA frame.
RUVIA_TEST(http2_connection_submit_response_head_and_body) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshake(conn);
    driveGetRequest(conn, &resource);

    ruvia::HttpResponse resp(&resource);
    resp.status(200);
    resp.setBodyCopy("hello");
    (void)conn.submitResponseHead(1, resp, /*bodyForbidden=*/false);

    const auto head = conn.pendingOutput();
    const auto hd = ruvia::detail::http2ParseFrameHeader(head.substr(0, 9));
    RUVIA_CHECK_EQ(hd.type, static_cast<std::uint8_t>(Http2FrameType::kHeaders));
    RUVIA_CHECK((hd.flags & ruvia::detail::kHttp2FlagEndHeaders) != 0);
    RUVIA_CHECK((hd.flags & ruvia::detail::kHttp2FlagEndStream) == 0);
    conn.consumeOutput(head.size());

    const auto r = conn.submitData(1, "hello", /*endStream=*/true);
    RUVIA_CHECK(r == Http2SubmitResult::kOk);
    const auto body = conn.pendingOutput();
    const auto dd = ruvia::detail::http2ParseFrameHeader(body.substr(0, 9));
    RUVIA_CHECK_EQ(dd.type, static_cast<std::uint8_t>(Http2FrameType::kData));
    RUVIA_CHECK_EQ(dd.length, static_cast<std::uint32_t>(5));
    RUVIA_CHECK((dd.flags & ruvia::detail::kHttp2FlagEndStream) != 0);
}

// submitStreamingResponseHead emits HEADERS with NO Content-Length and leaves the
// stream open; subsequent submitData chunks stream the body, the last with END_STREAM.
RUVIA_TEST(http2_connection_submit_streaming_response_head_and_chunks) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshake(conn);
    driveGetRequest(conn, &resource);

    ruvia::HttpResponse resp(&resource);
    resp.status(200);
    (void)conn.submitStreamingResponseHead(1, resp, /*bodyForbidden=*/false);

    const auto head = conn.pendingOutput();
    const auto hd = ruvia::detail::http2ParseFrameHeader(head.substr(0, 9));
    RUVIA_CHECK_EQ(hd.type, static_cast<std::uint8_t>(Http2FrameType::kHeaders));
    RUVIA_CHECK((hd.flags & ruvia::detail::kHttp2FlagEndHeaders) != 0);
    RUVIA_CHECK((hd.flags & ruvia::detail::kHttp2FlagEndStream) == 0);  // stays open
    conn.consumeOutput(head.size());

    (void)conn.submitData(1, "chunk1", /*endStream=*/false);
    (void)conn.submitData(1, "chunk2", /*endStream=*/true);
    const auto body = conn.pendingOutput();
    const auto d1 = ruvia::detail::http2ParseFrameHeader(body.substr(0, 9));
    RUVIA_CHECK_EQ(d1.type, static_cast<std::uint8_t>(Http2FrameType::kData));
    RUVIA_CHECK_EQ(d1.length, static_cast<std::uint32_t>(6));
    RUVIA_CHECK((d1.flags & ruvia::detail::kHttp2FlagEndStream) == 0);
    const auto d2 = ruvia::detail::http2ParseFrameHeader(body.substr(9 + 6, 9));
    RUVIA_CHECK_EQ(d2.type, static_cast<std::uint8_t>(Http2FrameType::kData));
    RUVIA_CHECK((d2.flags & ruvia::detail::kHttp2FlagEndStream) != 0);
}

// A body larger than the send window is partially sent and the remainder buffered
// (kBlocked). A WINDOW_UPDATE drains the rest with END_STREAM and reports the stream
// unblocked -- the sans-I/O equivalent of nghttp2 defer/resume.
RUVIA_TEST(http2_connection_submit_data_blocks_then_drains_on_window) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshakeWithWindow(conn, 3);  // stream 1 starts with a 3-byte send window
    driveGetRequest(conn, &resource);

    const char body[5] = {'a', 'b', 'c', 'd', 'e'};
    const auto r1 = conn.submitData(1, std::string_view(body, 5), /*endStream=*/true);
    RUVIA_CHECK(r1 == Http2SubmitResult::kBlocked);

    const auto out1 = conn.pendingOutput();
    const auto d1 = ruvia::detail::http2ParseFrameHeader(out1.substr(0, 9));
    RUVIA_CHECK_EQ(d1.type, static_cast<std::uint8_t>(Http2FrameType::kData));
    RUVIA_CHECK_EQ(d1.length, static_cast<std::uint32_t>(3));            // only 3 fit
    RUVIA_CHECK((d1.flags & ruvia::detail::kHttp2FlagEndStream) == 0);   // not terminal
    conn.consumeOutput(out1.size());

    char wu[ruvia::detail::kHttp2WindowUpdateFrameBytes];
    ruvia::detail::http2WriteWindowUpdate(wu, 1, 10);  // reopen stream 1's window
    (void)conn.feed(std::string_view(wu, sizeof(wu)));

    const auto out2 = conn.pendingOutput();
    const auto d2 = ruvia::detail::http2ParseFrameHeader(out2.substr(0, 9));
    RUVIA_CHECK_EQ(d2.type, static_cast<std::uint8_t>(Http2FrameType::kData));
    RUVIA_CHECK_EQ(d2.length, static_cast<std::uint32_t>(2));            // remaining 2
    RUVIA_CHECK((d2.flags & ruvia::detail::kHttp2FlagEndStream) != 0);   // now terminal

    const auto unblocked = conn.takeUnblockedStreams();
    RUVIA_CHECK_EQ(unblocked.size(), static_cast<std::size_t>(1));
    RUVIA_CHECK_EQ(unblocked[0], static_cast<std::uint32_t>(1));
}

// submitReset emits a RST_STREAM and marks the stream reset so no further response
// bytes are produced for it.
RUVIA_TEST(http2_connection_submit_reset_emits_rst) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshake(conn);
    driveGetRequest(conn, &resource);

    (void)conn.submitReset(1, 0x8 /* CANCEL */);
    const auto out = conn.pendingOutput();
    const auto r = ruvia::detail::http2ParseFrameHeader(out.substr(0, 9));
    RUVIA_CHECK_EQ(r.type, static_cast<std::uint8_t>(Http2FrameType::kRstStream));
    RUVIA_CHECK_EQ(r.streamId, static_cast<std::uint32_t>(1));
    RUVIA_CHECK(conn.stream(1)->isReset());
}

// A pinned stream (handler in flight) is NOT freed by a peer RST_STREAM: it stays in
// the table (so the handler's request views survive) but is marked reset, and
// kStreamClosed is emitted so the owner can drop the response. unpin then frees it.
RUVIA_TEST(http2_connection_pinned_stream_survives_peer_reset) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshake(conn);
    driveGetRequest(conn, &resource);  // stream 1 created + decoded
    RUVIA_CHECK(conn.stream(1) != nullptr);

    conn.pinStream(1);

    char rst[9 + 4];
    ruvia::detail::http2EncodeFrameHeader(rst, 4, Http2FrameType::kRstStream, 0, 1);
    ruvia::detail::http2Write32(rst + 9, 8 /* CANCEL */);
    (void)conn.feed(std::string_view(rst, sizeof(rst)));

    auto* s = conn.stream(1);
    RUVIA_CHECK(s != nullptr);   // kept alive because pinned
    RUVIA_CHECK(s->isReset());   // but marked reset
    bool sawClosed = false;
    for (;;) {
        const auto event = conn.nextEvent();
        if (event.kind == Http2Event::Kind::kNone) break;
        if (event.kind == Http2Event::Kind::kStreamClosed && event.streamId == 1) sawClosed = true;
    }
    RUVIA_CHECK(sawClosed);

    conn.unpinStream(1);
    RUVIA_CHECK(conn.stream(1) == nullptr);  // freed once the handler finished
}

// Unpinning a stream that completed normally (no RST) frees it too.
RUVIA_TEST(http2_connection_unpin_frees_completed_stream) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshake(conn);
    driveGetRequest(conn, &resource);
    conn.pinStream(1);
    RUVIA_CHECK(conn.stream(1) != nullptr);
    conn.unpinStream(1);
    RUVIA_CHECK(conn.stream(1) == nullptr);
}

// RFC 8441 Extended CONNECT: a CONNECT + :protocol=websocket head emits kMessageHead
// with NO kMessageEnd (the tunnel stays open), and the stream carries the
// extendedConnectWebSocket mark for the owner's route policy. After the owner marks the
// tunnel, submitWebSocketHandshake answers 200 WITHOUT END_STREAM, tunnel DATA flows as
// kMessageBodyChunk events (no content-length required), and submitData carries frames
// back on the still-open stream.
RUVIA_TEST(http2_connection_websocket_tunnel_handshake_and_data) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshake(conn);

    std::pmr::string block(&resource);
    HpackEncoder::encodeHeader(block, ":method", "CONNECT");
    HpackEncoder::encodeHeader(block, ":protocol", "websocket");
    HpackEncoder::encodeHeader(block, ":scheme", "https");
    HpackEncoder::encodeHeader(block, ":path", "/ws");
    HpackEncoder::encodeHeader(block, ":authority", "example.com");
    HpackEncoder::encodeHeader(block, "sec-websocket-version", "13");
    const auto h = headersFrame(
        &resource, 1, ruvia::detail::kHttp2FlagEndHeaders,
        std::string_view(block.data(), block.size()));
    (void)conn.feed(std::string_view(h.data(), h.size()));

    bool sawHeaders = false;
    bool sawEnd = false;
    for (;;) {
        const auto event = conn.nextEvent();
        if (event.kind == Http2Event::Kind::kNone) break;
        if (event.kind == Http2Event::Kind::kMessageHead && event.streamId == 1) sawHeaders = true;
        if (event.kind == Http2Event::Kind::kMessageEnd) sawEnd = true;
    }
    RUVIA_CHECK(sawHeaders);
    RUVIA_CHECK(!sawEnd);  // the tunnel must stay open

    auto* stream = conn.stream(1);
    RUVIA_CHECK(stream != nullptr);
    RUVIA_CHECK(stream->extendedConnectWebSocket());

    // Owner route policy admitted a WebSocket route: mark the tunnel and answer 200.
    stream->markWebSocketTunnel();
    conn.consumeOutput(conn.pendingOutput().size());
    (void)conn.submitWebSocketHandshake(1, "chat");

    const auto out = conn.pendingOutput();
    RUVIA_CHECK(out.size() > 9);
    const auto head = ruvia::detail::http2ParseFrameHeader(out.substr(0, 9));
    RUVIA_CHECK_EQ(head.type, static_cast<std::uint8_t>(Http2FrameType::kHeaders));
    RUVIA_CHECK_EQ(head.streamId, static_cast<std::uint32_t>(1));
    RUVIA_CHECK((head.flags & ruvia::detail::kHttp2FlagEndHeaders) != 0);
    RUVIA_CHECK((head.flags & ruvia::detail::kHttp2FlagEndStream) == 0);  // stream open
    conn.consumeOutput(out.size());

    // Inbound tunnel bytes (a would-be masked frame) surface as body chunks even with
    // no content-length: the tunnel is exempt from body accounting.
    char data[9 + 4];
    ruvia::detail::http2EncodeFrameHeader(data, 4, Http2FrameType::kData, 0, 1);
    std::memcpy(data + 9, "\x81\x80\x01\x02", 4);
    (void)conn.feed(std::string_view(data, sizeof(data)));
    bool sawChunk = false;
    for (;;) {
        const auto event = conn.nextEvent();
        if (event.kind == Http2Event::Kind::kNone) break;
        if (event.kind == Http2Event::Kind::kMessageBodyChunk && event.streamId == 1 &&
            event.bytes.size() == 4) {
            sawChunk = true;
        }
    }
    RUVIA_CHECK(sawChunk);

    // Outbound tunnel frames ride submitData on the still-open stream.
    conn.consumeOutput(conn.pendingOutput().size());
    RUVIA_CHECK(conn.submitData(1, "\x81\x02hi", false) == Http2SubmitResult::kOk);
    const auto frameOut = conn.pendingOutput();
    const auto dataHead = ruvia::detail::http2ParseFrameHeader(frameOut.substr(0, 9));
    RUVIA_CHECK_EQ(dataHead.type, static_cast<std::uint8_t>(Http2FrameType::kData));
    RUVIA_CHECK((dataHead.flags & ruvia::detail::kHttp2FlagEndStream) == 0);
    RUVIA_CHECK(frameOut.substr(9) == std::string_view("\x81\x02hi"));
}

namespace {

using ruvia::detail::Http2Role;

// Byte shuttle between two cores (no sockets): move pending output of `from` into
// `to`, draining `to`'s events into the collectors first would lose them -- so the
// caller passes a per-hop event sink invoked after every feed.
template <typename OnEvent>
void shuttleOnce(Http2Connection& from, Http2Connection& to, OnEvent&& onEvent) {
    while (from.wantsWrite()) {
        const auto out = from.pendingOutput();
        std::pmr::string copy(out.data(), out.size(), std::pmr::get_default_resource());
        from.consumeOutput(out.size());
        (void)to.feed(std::string_view(copy.data(), copy.size()));
        for (;;) {
            const auto event = to.nextEvent();
            if (event.kind == Http2Event::Kind::kNone) {
                break;
            }
            onEvent(event);
        }
    }
}

}  // namespace

// Client role end-to-end against the server core with ZERO I/O: the client core opens
// stream 1, sends a GET, the server core dispatches a 200 "pong", and the client core
// surfaces the response head (status via the stream state), body chunk, and end.
RUVIA_TEST(http2_connection_client_role_get_round_trip) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection server(&resource);
    server.expectClientPreface();
    server.queueLocalSettings();
    Http2Connection client(&resource, {}, Http2Role::kClient);
    client.queueClientPreface();

    std::string clientBody;
    std::uint16_t status = 0;
    bool clientSawHead = false;
    bool clientSawEnd = false;
    const auto onServerEvent = [&](const Http2Event& event) {
        if (event.kind == Http2Event::Kind::kMessageEnd) {
            ruvia::HttpResponse response(&resource);
            response.status(200);
            response.setBodyCopy("pong");
            (void)server.submitResponseHead(event.streamId, response, /*bodyForbidden=*/false);
            (void)server.submitData(event.streamId, "pong", /*endStream=*/true);
        }
    };
    const auto onClientEvent = [&](const Http2Event& event) {
        if (event.kind == Http2Event::Kind::kMessageHead) {
            clientSawHead = true;
            if (auto* stream = client.stream(event.streamId)) {
                status = stream->responseStatus();
            }
        } else if (event.kind == Http2Event::Kind::kMessageBodyChunk) {
            clientBody.append(event.bytes.data(), event.bytes.size());
        } else if (event.kind == Http2Event::Kind::kMessageEnd) {
            clientSawEnd = true;
        }
    };

    const auto streamId = client.openLocalStream();
    RUVIA_CHECK_EQ(streamId, static_cast<std::uint32_t>(1));
    client.pinStream(streamId);
    (void)client.submitRequestHead(streamId, "GET", "http", "example.com", "/", {}, /*endStream=*/true);

    for (int round = 0; round < 4; ++round) {
        shuttleOnce(client, server, onServerEvent);
        shuttleOnce(server, client, onClientEvent);
    }

    RUVIA_CHECK(clientSawHead);
    RUVIA_CHECK_EQ(status, static_cast<std::uint16_t>(200));
    RUVIA_CHECK(clientBody == "pong");
    RUVIA_CHECK(clientSawEnd);
    auto* stream = client.stream(streamId);
    RUVIA_CHECK(stream != nullptr);
    // content-length was decoded into the stream (auto CL from the server head).
    RUVIA_CHECK(stream->hasContentLength());
    client.unpinStream(streamId);
    RUVIA_CHECK(client.stream(streamId) == nullptr);
}

// Client role POST: the request body flows through submitData with END_STREAM, the
// server core buffers it (owner-side append) and answers; both directions complete.
RUVIA_TEST(http2_connection_client_role_post_round_trip) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection server(&resource);
    server.expectClientPreface();
    server.queueLocalSettings();
    Http2Connection client(&resource, {}, Http2Role::kClient);
    client.queueClientPreface();

    std::string serverBody;
    std::string clientBody;
    bool clientSawEnd = false;
    const auto onServerEvent = [&](const Http2Event& event) {
        if (event.kind == Http2Event::Kind::kMessageBodyChunk) {
            serverBody.append(event.bytes.data(), event.bytes.size());
        } else if (event.kind == Http2Event::Kind::kMessageEnd) {
            ruvia::HttpResponse response(&resource);
            response.status(200);
            response.setBodyCopy(serverBody);
            (void)server.submitResponseHead(event.streamId, response, false);
            (void)server.submitData(
                event.streamId, std::string_view(serverBody.data(), serverBody.size()), true);
        }
    };
    const auto onClientEvent = [&](const Http2Event& event) {
        if (event.kind == Http2Event::Kind::kMessageBodyChunk) {
            clientBody.append(event.bytes.data(), event.bytes.size());
        } else if (event.kind == Http2Event::Kind::kMessageEnd) {
            clientSawEnd = true;
        }
    };

    const auto streamId = client.openLocalStream();
    client.pinStream(streamId);
    const ruvia::HttpHeaderView headers[] = {ruvia::HttpHeaderView{"content-length", "5"}};
    (void)client.submitRequestHead(streamId, "POST", "http", "example.com", "/echo",
        std::span<const ruvia::HttpHeaderView>(headers, 1), /*endStream=*/false);
    RUVIA_CHECK(client.submitData(streamId, "hello", /*endStream=*/true) == Http2SubmitResult::kOk);

    for (int round = 0; round < 4; ++round) {
        shuttleOnce(client, server, onServerEvent);
        shuttleOnce(server, client, onClientEvent);
    }

    RUVIA_CHECK(serverBody == "hello");
    RUVIA_CHECK(clientBody == "hello");
    RUVIA_CHECK(clientSawEnd);
}

// A 1xx interim head is validated and skipped (no events, stream not decoded); the
// following 200 head is the one surfaced. Hand-encoded server bytes drive the client.
RUVIA_TEST(http2_connection_client_role_interim_response_skipped) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection client(&resource, {}, Http2Role::kClient);
    client.queueClientPreface();
    client.consumeOutput(client.pendingOutput().size());

    const auto streamId = client.openLocalStream();
    client.pinStream(streamId);
    (void)client.submitRequestHead(streamId, "GET", "http", "example.com", "/", {}, true);
    client.consumeOutput(client.pendingOutput().size());

    // Server bytes: SETTINGS, then HEADERS(103), then HEADERS(200) + DATA END_STREAM.
    std::pmr::string bytes(&resource);
    {
        char settings[9];
        ruvia::detail::http2EncodeFrameHeader(settings, 0, Http2FrameType::kSettings, 0, 0);
        bytes.append(settings, sizeof(settings));
        std::pmr::string interim(&resource);
        HpackEncoder::encodeHeader(interim, ":status", "103");
        HpackEncoder::encodeHeader(interim, "link", "</style.css>; rel=preload");
        const auto interimFrame = headersFrame(
            &resource, streamId, ruvia::detail::kHttp2FlagEndHeaders,
            std::string_view(interim.data(), interim.size()));
        bytes.append(interimFrame.data(), interimFrame.size());
        std::pmr::string final_(&resource);
        HpackEncoder::encodeHeader(final_, ":status", "200");
        HpackEncoder::encodeHeader(final_, "content-length", "2");
        const auto finalFrame = headersFrame(
            &resource, streamId, ruvia::detail::kHttp2FlagEndHeaders,
            std::string_view(final_.data(), final_.size()));
        bytes.append(finalFrame.data(), finalFrame.size());
        char data[9 + 2];
        ruvia::detail::http2EncodeFrameHeader(data, 2, Http2FrameType::kData,
            ruvia::detail::kHttp2FlagEndStream, streamId);
        std::memcpy(data + 9, "ok", 2);
        bytes.append(data, sizeof(data));
    }
    (void)client.feed(std::string_view(bytes.data(), bytes.size()));

    int heads = 0;
    std::string body;
    bool end = false;
    for (;;) {
        const auto event = client.nextEvent();
        if (event.kind == Http2Event::Kind::kNone) break;
        if (event.kind == Http2Event::Kind::kMessageHead) ++heads;
        if (event.kind == Http2Event::Kind::kMessageBodyChunk) body.append(event.bytes.data(), event.bytes.size());
        if (event.kind == Http2Event::Kind::kMessageEnd) end = true;
    }
    RUVIA_CHECK_EQ(heads, 1);  // only the final head is surfaced
    RUVIA_CHECK(body == "ok");
    RUVIA_CHECK(end);
    auto* stream = client.stream(streamId);
    RUVIA_CHECK(stream != nullptr);
    RUVIA_CHECK_EQ(stream->responseStatus(), static_cast<std::uint16_t>(200));
    RUVIA_CHECK_EQ(static_cast<int>(stream->interimResponseCount()), 1);
    RUVIA_CHECK(!client.closing());
}

// Client role protocol errors: HEADERS on an odd stream never opened is a connection
// error, and HEADERS on an even (server-initiated) stream is one too (push disabled).
RUVIA_TEST(http2_connection_client_role_rejects_unexpected_streams) {
    std::pmr::monotonic_buffer_resource resource;
    {
        Http2Connection client(&resource, {}, Http2Role::kClient);
        client.queueClientPreface();
        client.consumeOutput(client.pendingOutput().size());
        char settings[9];
        ruvia::detail::http2EncodeFrameHeader(settings, 0, Http2FrameType::kSettings, 0, 0);
        (void)client.feed(std::string_view(settings, sizeof(settings)));
        std::pmr::string head(&resource);
        HpackEncoder::encodeHeader(head, ":status", "200");
        const auto idle = headersFrame(
            &resource, 5, ruvia::detail::kHttp2FlagEndHeaders,
            std::string_view(head.data(), head.size()));
        (void)client.feed(std::string_view(idle.data(), idle.size()));
        RUVIA_CHECK(client.closing());  // HEADERS on idle stream -> GOAWAY
    }
    {
        Http2Connection client(&resource, {}, Http2Role::kClient);
        client.queueClientPreface();
        client.consumeOutput(client.pendingOutput().size());
        char settings[9];
        ruvia::detail::http2EncodeFrameHeader(settings, 0, Http2FrameType::kSettings, 0, 0);
        (void)client.feed(std::string_view(settings, sizeof(settings)));
        std::pmr::string head(&resource);
        HpackEncoder::encodeHeader(head, ":status", "200");
        const auto even = headersFrame(
            &resource, 2, ruvia::detail::kHttp2FlagEndHeaders,
            std::string_view(head.data(), head.size()));
        (void)client.feed(std::string_view(even.data(), even.size()));
        RUVIA_CHECK(client.closing());  // no push: even ids are never valid
    }
}

// Graceful drain (RFC 9113 §6.8): beginDrain emits GOAWAY(NO_ERROR) at the last
// accepted stream id; streams already open keep working, HEADERS for a higher id are
// refused with RST_STREAM(REFUSED_STREAM), and beginDrain is idempotent.
RUVIA_TEST(http2_connection_begin_drain_refuses_new_streams) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshake(conn);
    driveGetRequest(conn, &resource);  // stream 1 open (half-closed remote)
    conn.consumeOutput(conn.pendingOutput().size());

    conn.beginDrain();
    // GOAWAY(NO_ERROR, lastStreamId=1) emitted, connection NOT closing.
    const auto goaway = conn.pendingOutput();
    RUVIA_CHECK(goaway.size() >= 9);
    const auto gh = ruvia::detail::http2ParseFrameHeader(goaway.substr(0, 9));
    RUVIA_CHECK_EQ(gh.type, static_cast<std::uint8_t>(Http2FrameType::kGoaway));
    RUVIA_CHECK(!conn.closing());
    RUVIA_CHECK(conn.draining());
    conn.consumeOutput(goaway.size());

    // A new stream ABOVE the advertised id (3) is refused with RST_STREAM.
    std::pmr::string block(&resource);
    encodeGetRequest(block);
    const auto h = headersFrame(
        &resource, 3, ruvia::detail::kHttp2FlagEndHeaders | ruvia::detail::kHttp2FlagEndStream,
        std::string_view(block.data(), block.size()));
    (void)conn.feed(std::string_view(h.data(), h.size()));
    bool refusedEnd = false;
    while (conn.nextEvent().kind != Http2Event::Kind::kNone) {
        // stream 3 must NOT surface as a request (it was refused)
    }
    const auto rst = conn.pendingOutput();
    RUVIA_CHECK(rst.size() >= 9);
    const auto rh = ruvia::detail::http2ParseFrameHeader(rst.substr(0, 9));
    RUVIA_CHECK_EQ(rh.type, static_cast<std::uint8_t>(Http2FrameType::kRstStream));
    RUVIA_CHECK_EQ(rh.streamId, static_cast<std::uint32_t>(3));
    RUVIA_CHECK(!conn.closing());  // drain refuses new streams WITHOUT closing
    (void)refusedEnd;
    conn.consumeOutput(rst.size());

    // Stream 1 (opened before the drain) can still be answered.
    ruvia::HttpResponse response(&resource);
    response.status(200);
    response.setBodyCopy("ok");
    (void)conn.submitResponseHead(1, response, /*bodyForbidden=*/false);
    RUVIA_CHECK(conn.submitData(1, "ok", true) == Http2SubmitResult::kOk);
    RUVIA_CHECK(conn.pendingOutput().size() > 9);  // response frames produced
    conn.consumeOutput(conn.pendingOutput().size());

    conn.beginDrain();  // idempotent: no further GOAWAY
    RUVIA_CHECK(conn.pendingOutput().empty());
}

// A streaming consumer's banked receive-window debt (deferStreamWindowRelease) must
// return to the CONNECTION window when the stream is removed, even if the owner never
// calls releaseStreamWindow -- otherwise the connection window shrinks permanently.
RUVIA_TEST(http2_connection_window_debt_flushed_on_removal) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshake(conn);

    // Open stream 1 with a body (POST, content-length) and mark it deferred-release.
    std::pmr::string block(&resource);
    HpackEncoder::encodeHeader(block, ":method", "POST");
    HpackEncoder::encodeHeader(block, ":scheme", "https");
    HpackEncoder::encodeHeader(block, ":path", "/");
    HpackEncoder::encodeHeader(block, ":authority", "example.com");
    HpackEncoder::encodeHeader(block, "content-length", "5");
    const auto h = headersFrame(
        &resource, 1, ruvia::detail::kHttp2FlagEndHeaders,
        std::string_view(block.data(), block.size()));
    (void)conn.feed(std::string_view(h.data(), h.size()));
    while (conn.nextEvent().kind != Http2Event::Kind::kNone) {}
    conn.deferStreamWindowRelease(1);
    conn.consumeOutput(conn.pendingOutput().size());

    // Feed 5 body bytes: the receive-window credit is BANKED (deferred), so NO
    // per-frame WINDOW_UPDATE is emitted.
    char data[9 + 5];
    ruvia::detail::http2EncodeFrameHeader(data, 5, Http2FrameType::kData, 0, 1);
    std::memcpy(data + 9, "hello", 5);
    (void)conn.feed(std::string_view(data, sizeof(data)));
    while (conn.nextEvent().kind != Http2Event::Kind::kNone) {}
    RUVIA_CHECK(conn.pendingOutput().empty());  // debt banked, not advertised

    // Peer RST_STREAM removes the (unpinned) stream. The banked 5 bytes must be
    // returned to the CONNECTION window via a stream-0 WINDOW_UPDATE.
    char rst[9 + 4];
    ruvia::detail::http2EncodeFrameHeader(rst, 4, Http2FrameType::kRstStream, 0, 1);
    ruvia::detail::http2Write32(rst + 9, 8 /* CANCEL */);
    (void)conn.feed(std::string_view(rst, sizeof(rst)));
    while (conn.nextEvent().kind != Http2Event::Kind::kNone) {}

    bool sawConnWindowUpdate = false;
    auto out = conn.pendingOutput();
    while (out.size() >= 9) {
        const auto fh = ruvia::detail::http2ParseFrameHeader(out.substr(0, 9));
        if (fh.type == static_cast<std::uint8_t>(Http2FrameType::kWindowUpdate) && fh.streamId == 0) {
            const auto inc = ruvia::detail::http2WindowUpdateIncrement(out.substr(9, 4));
            if (inc == 5) sawConnWindowUpdate = true;
        }
        out = out.substr(9 + fh.length);
    }
    RUVIA_CHECK(sawConnWindowUpdate);  // connection window self-healed on removal
    RUVIA_CHECK(conn.stream(1) == nullptr);
}

// Server-role trailers: a trailing HEADERS block WITHOUT END_STREAM is a protocol
// error on that stream (RFC 9113 §8.1) -- the core RSTs and closes it, no kMessageEnd.
RUVIA_TEST(http2_connection_server_trailers_without_end_stream_rejected) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshake(conn);

    // Open stream 1 with a body (POST, no END_STREAM on HEADERS).
    std::pmr::string block(&resource);
    HpackEncoder::encodeHeader(block, ":method", "POST");
    HpackEncoder::encodeHeader(block, ":scheme", "https");
    HpackEncoder::encodeHeader(block, ":path", "/");
    HpackEncoder::encodeHeader(block, ":authority", "example.com");
    const auto h = headersFrame(
        &resource, 1, ruvia::detail::kHttp2FlagEndHeaders,
        std::string_view(block.data(), block.size()));
    (void)conn.feed(std::string_view(h.data(), h.size()));
    while (conn.nextEvent().kind != Http2Event::Kind::kNone) {}
    conn.consumeOutput(conn.pendingOutput().size());

    // A trailer HEADERS block with END_HEADERS but NO END_STREAM -> stream error.
    std::pmr::string trailer(&resource);
    HpackEncoder::encodeHeader(trailer, "x-checksum", "abc");
    const auto t = headersFrame(
        &resource, 1, ruvia::detail::kHttp2FlagEndHeaders,  // deliberately no END_STREAM
        std::string_view(trailer.data(), trailer.size()));
    (void)conn.feed(std::string_view(t.data(), t.size()));

    bool sawClosed = false;
    bool sawEnd = false;
    for (;;) {
        const auto event = conn.nextEvent();
        if (event.kind == Http2Event::Kind::kNone) break;
        if (event.kind == Http2Event::Kind::kStreamClosed) sawClosed = true;
        if (event.kind == Http2Event::Kind::kMessageEnd) sawEnd = true;
    }
    RUVIA_CHECK(sawClosed);
    RUVIA_CHECK(!sawEnd);            // never completes the request
    RUVIA_CHECK(!conn.closing());    // stream error, not connection error
    const auto rst = ruvia::detail::http2ParseFrameHeader(conn.pendingOutput().substr(0, 9));
    RUVIA_CHECK_EQ(rst.type, static_cast<std::uint8_t>(Http2FrameType::kRstStream));
    RUVIA_CHECK(conn.stream(1) == nullptr);  // removed, not leaked
}

// submitTrailers queued behind a window-blocked body: the trailer HEADERS must be
// emitted AFTER the deferred DATA drains (RFC 9113 §8.1), carrying END_STREAM in place
// of it -- never ahead of the body bytes.
RUVIA_TEST(http2_connection_trailers_wait_for_blocked_body) {
    std::pmr::monotonic_buffer_resource resource;
    Http2Connection conn(&resource);
    handshakeWithWindow(conn, 4);  // tiny 4-byte stream send window
    driveGetRequest(conn, &resource);
    conn.consumeOutput(conn.pendingOutput().size());

    // Streaming response head (no content-length), then an 8-byte body: only 4 fit the
    // window, so 4 bytes are deferred -> kBlocked.
    ruvia::HttpResponse head(&resource);
    head.status(200);
    (void)conn.submitStreamingResponseHead(1, head, /*bodyForbidden=*/false);
    conn.consumeOutput(conn.pendingOutput().size());
    RUVIA_CHECK(conn.submitData(1, "AAAABBBB", /*endStream=*/false) == Http2SubmitResult::kBlocked);

    // First 4 bytes went out as DATA (no END_STREAM).
    auto out = conn.pendingOutput();
    const auto d1 = ruvia::detail::http2ParseFrameHeader(out.substr(0, 9));
    RUVIA_CHECK_EQ(d1.type, static_cast<std::uint8_t>(Http2FrameType::kData));
    RUVIA_CHECK_EQ(d1.length, static_cast<std::uint32_t>(4));
    RUVIA_CHECK((d1.flags & ruvia::detail::kHttp2FlagEndStream) == 0);
    conn.consumeOutput(out.size());

    // Queue trailers while the remaining 4 bytes are still window-blocked.
    std::pmr::string trailer(&resource);
    HpackEncoder::encodeHeader(trailer, "x-checksum", "ok");
    (void)conn.submitTrailers(1, std::string_view(trailer.data(), trailer.size()));
    RUVIA_CHECK(conn.pendingOutput().empty());  // nothing emitted yet (still blocked)

    // Peer WINDOW_UPDATE reopens the window: the deferred DATA drains, THEN the trailer
    // HEADERS(END_STREAM) follows -- in that order.
    char wu[9 + 4];
    ruvia::detail::http2EncodeFrameHeader(wu, 4, Http2FrameType::kWindowUpdate, 0, 1);
    ruvia::detail::http2Write32(wu + 9, 100);
    (void)conn.feed(std::string_view(wu, sizeof(wu)));
    while (conn.nextEvent().kind != Http2Event::Kind::kNone) {}

    out = conn.pendingOutput();
    const auto d2 = ruvia::detail::http2ParseFrameHeader(out.substr(0, 9));
    RUVIA_CHECK_EQ(d2.type, static_cast<std::uint8_t>(Http2FrameType::kData));
    RUVIA_CHECK_EQ(d2.length, static_cast<std::uint32_t>(4));  // the remaining body
    RUVIA_CHECK((d2.flags & ruvia::detail::kHttp2FlagEndStream) == 0);  // NOT on the DATA
    out = out.substr(9 + d2.length);
    const auto th = ruvia::detail::http2ParseFrameHeader(out.substr(0, 9));
    RUVIA_CHECK_EQ(th.type, static_cast<std::uint8_t>(Http2FrameType::kHeaders));  // trailer
    RUVIA_CHECK((th.flags & ruvia::detail::kHttp2FlagEndStream) != 0);  // END_STREAM here
}
