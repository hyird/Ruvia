#include "test_harness.h"

#include <cstdint>
#include <memory_resource>
#include <string_view>

#include "ruvia/http/detail/websocket/WsConnection.h"

namespace {

using ruvia::WebSocketOpcode;
using ruvia::detail::WsConnection;
using ruvia::detail::WsEvent;
using ruvia::detail::WsFeedStatus;

// Build a masked client->server frame (RFC 6455 §5.1): FIN/opcode byte, MASK bit + a
// short length (<=125 for tests), a 4-byte mask, then the masked payload.
std::pmr::string maskedFrame(
    std::pmr::memory_resource* res, std::uint8_t opcode, std::string_view payload, bool fin = true,
    bool rsv1 = false) {
    std::pmr::string f(res);
    f.push_back(static_cast<char>((fin ? 0x80U : 0U) | (rsv1 ? 0x40U : 0U) | opcode));
    f.push_back(static_cast<char>(0x80U | static_cast<std::uint8_t>(payload.size())));
    const unsigned char mask[4] = {0x11, 0x22, 0x33, 0x44};
    f.append(reinterpret_cast<const char*>(mask), 4);
    for (std::size_t i = 0; i < payload.size(); ++i) {
        f.push_back(static_cast<char>(static_cast<unsigned char>(payload[i]) ^ mask[i % 4]));
    }
    return f;
}

WsFeedStatus feedBytes(
    WsConnection& connection,
    std::pmr::string& input,
    std::string_view bytes) {
    input.append(bytes.data(), bytes.size());
    return connection.feed();
}

}  // namespace

// A single masked Text frame is delivered as one complete kMessage event, unmasked.
RUVIA_TEST(ws_connection_delivers_text_message) {
    std::pmr::monotonic_buffer_resource resource;
    std::pmr::string input(&resource);
    WsConnection conn(input);

    const auto f = maskedFrame(&resource, 0x1, "hi");
    const auto status = feedBytes(conn, input, std::string_view(f.data(), f.size()));
    RUVIA_CHECK(status == WsFeedStatus::kOk);

    const auto e = conn.nextEvent();
    RUVIA_CHECK(e.kind == WsEvent::Kind::kMessage);
    RUVIA_CHECK(e.opcode == WebSocketOpcode::kText);
    RUVIA_CHECK(e.payload == std::string_view("hi"));
    RUVIA_CHECK(e.payload.data() == input.data() + 6);  // header + mask, no delivery copy
    RUVIA_CHECK(conn.nextEvent().kind == WsEvent::Kind::kNone);
}

// A Ping is surfaced as kPing AND auto-answered with an unmasked Pong echoing the data.
RUVIA_TEST(ws_connection_auto_pongs_ping) {
    std::pmr::monotonic_buffer_resource resource;
    std::pmr::string input(&resource);
    WsConnection conn(input);

    const auto f = maskedFrame(&resource, 0x9, "pp");
    (void)feedBytes(conn, input, std::string_view(f.data(), f.size()));

    const auto e = conn.nextEvent();
    RUVIA_CHECK(e.kind == WsEvent::Kind::kPing);
    RUVIA_CHECK(e.payload == std::string_view("pp"));

    const auto out = conn.pendingOutput();
    RUVIA_CHECK_EQ(out.size(), static_cast<std::size_t>(4));
    RUVIA_CHECK_EQ(static_cast<unsigned char>(out[0]), static_cast<unsigned char>(0x8A));  // FIN|Pong
    RUVIA_CHECK_EQ(static_cast<unsigned char>(out[1]), static_cast<unsigned char>(2));      // unmasked len
    RUVIA_CHECK(out.substr(2) == std::string_view("pp"));
}

// A peer Close yields kClose (with the parsed code), an echoed Close frame, and marks
// the connection closing.
RUVIA_TEST(ws_connection_echoes_close) {
    std::pmr::monotonic_buffer_resource resource;
    std::pmr::string input(&resource);
    WsConnection conn(input);

    std::pmr::string body(&resource);
    body.push_back(static_cast<char>(0x03));  // 1000 = normal closure
    body.push_back(static_cast<char>(0xE8));
    const auto f = maskedFrame(&resource, 0x8, std::string_view(body.data(), body.size()));
    (void)feedBytes(conn, input, std::string_view(f.data(), f.size()));

    const auto e = conn.nextEvent();
    RUVIA_CHECK(e.kind == WsEvent::Kind::kClose);
    RUVIA_CHECK_EQ(e.closeCode, static_cast<std::uint16_t>(1000));
    RUVIA_CHECK(conn.closing());

    const auto out = conn.pendingOutput();
    RUVIA_CHECK_EQ(static_cast<unsigned char>(out[0]), static_cast<unsigned char>(0x88));  // FIN|Close
}

// A fragmented Text message (first frame FIN=0, continuation FIN=1) reassembles into
// one kMessage.
RUVIA_TEST(ws_connection_reassembles_fragmented_message) {
    std::pmr::monotonic_buffer_resource resource;
    std::pmr::string input(&resource);
    WsConnection conn(input);

    const auto f1 = maskedFrame(&resource, 0x1, "ab", /*fin=*/false);
    const auto f2 = maskedFrame(&resource, 0x0, "cd", /*fin=*/true);  // continuation
    std::pmr::string both(&resource);
    both.append(f1.data(), f1.size());
    both.append(f2.data(), f2.size());
    (void)feedBytes(conn, input, std::string_view(both.data(), both.size()));

    const auto e = conn.nextEvent();
    RUVIA_CHECK(e.kind == WsEvent::Kind::kMessage);
    RUVIA_CHECK(e.payload == std::string_view("abcd"));
    RUVIA_CHECK(conn.nextEvent().kind == WsEvent::Kind::kNone);
}

// An unmasked frame violates RFC 6455 §5.1: the core queues a Close and reports the
// protocol error rather than delivering anything.
RUVIA_TEST(ws_connection_rejects_unmasked_frame) {
    std::pmr::monotonic_buffer_resource resource;
    std::pmr::string input(&resource);
    WsConnection conn(input);

    // Text "hi" with the MASK bit clear.
    std::pmr::string f(&resource);
    f.push_back(static_cast<char>(0x81));  // FIN|Text
    f.push_back(static_cast<char>(2));     // no mask bit, len 2
    f.append("hi", 2);
    (void)feedBytes(conn, input, std::string_view(f.data(), f.size()));

    const auto e = conn.nextEvent();
    RUVIA_CHECK(e.kind == WsEvent::Kind::kProtocolError);
    RUVIA_CHECK(conn.closing());
    const auto out = conn.pendingOutput();
    RUVIA_CHECK_EQ(static_cast<unsigned char>(out[0]), static_cast<unsigned char>(0x88));  // Close
}

// submitMessage encodes an unmasked server Text frame.
RUVIA_TEST(ws_connection_submit_message_encodes_unmasked_frame) {
    std::pmr::monotonic_buffer_resource resource;
    std::pmr::string input(&resource);
    WsConnection conn(input);

    (void)conn.submitMessage(WebSocketOpcode::kText, "hello");
    const auto out = conn.pendingOutput();
    RUVIA_CHECK_EQ(static_cast<unsigned char>(out[0]), static_cast<unsigned char>(0x81));  // FIN|Text
    RUVIA_CHECK_EQ(static_cast<unsigned char>(out[1]), static_cast<unsigned char>(5));      // unmasked len
    RUVIA_CHECK(out.substr(2) == std::string_view("hello"));
}

// A partial frame (only the first byte) is buffered and yields no event until the rest
// arrives on a later feed.
RUVIA_TEST(ws_connection_needs_more_on_partial_frame) {
    std::pmr::monotonic_buffer_resource resource;
    std::pmr::string input(&resource);
    WsConnection conn(input);

    const auto f = maskedFrame(&resource, 0x1, "split");
    (void)feedBytes(conn, input, std::string_view(f.data(), 3));  // header only, no full payload
    RUVIA_CHECK(conn.nextEvent().kind == WsEvent::Kind::kNone);
    RUVIA_CHECK(!conn.closing());

    (void)feedBytes(conn, input, std::string_view(f.data() + 3, f.size() - 3));  // remainder
    const auto e = conn.nextEvent();
    RUVIA_CHECK(e.kind == WsEvent::Kind::kMessage);
    RUVIA_CHECK(e.payload == std::string_view("split"));
}

// With permessage-deflate negotiated, an RSV1 (compressed) message is inflated and
// delivered as its original text.
RUVIA_TEST(ws_connection_inflates_compressed_message) {
    std::pmr::monotonic_buffer_resource resource;
    std::pmr::string input(&resource);
    WsConnection conn(input, 0, /*permessageDeflate=*/true);

    ruvia::detail::WebSocketDeflate encoder;
    RUVIA_CHECK(encoder.ok());
    std::pmr::string compressed(&resource);
    RUVIA_CHECK(encoder.compress("hello hello hello", compressed));

    const auto f = maskedFrame(
        &resource, 0x1, std::string_view(compressed.data(), compressed.size()),
        /*fin=*/true, /*rsv1=*/true);
    (void)feedBytes(conn, input, std::string_view(f.data(), f.size()));

    const auto e = conn.nextEvent();
    RUVIA_CHECK(e.kind == WsEvent::Kind::kMessage);
    RUVIA_CHECK(e.payload == std::string_view("hello hello hello"));
}

// With permessage-deflate on, submitMessage compresses a shrinkable payload and sets
// the RSV1 bit.
RUVIA_TEST(ws_connection_submit_compresses_when_enabled) {
    std::pmr::monotonic_buffer_resource resource;
    std::pmr::string input(&resource);
    WsConnection conn(input, 0, /*permessageDeflate=*/true);

    const std::pmr::string repetitive(200, 'a', &resource);
    (void)conn.submitMessage(WebSocketOpcode::kText, std::string_view(repetitive.data(), repetitive.size()));

    const auto out = conn.pendingOutput();
    RUVIA_CHECK((static_cast<unsigned char>(out[0]) & 0x40U) != 0);  // RSV1 (compressed)
    RUVIA_CHECK((static_cast<unsigned char>(out[0]) & 0x0FU) == 0x1);  // Text opcode
    RUVIA_CHECK(out.size() < repetitive.size());                      // actually smaller
}

// An RSV1 frame when permessage-deflate was NOT negotiated is a protocol error.
RUVIA_TEST(ws_connection_rejects_rsv1_without_deflate) {
    std::pmr::monotonic_buffer_resource resource;
    std::pmr::string input(&resource);
    WsConnection conn(input);  // no deflate

    const auto f = maskedFrame(&resource, 0x1, "x", /*fin=*/true, /*rsv1=*/true);
    (void)feedBytes(conn, input, std::string_view(f.data(), f.size()));

    RUVIA_CHECK(conn.nextEvent().kind == WsEvent::Kind::kProtocolError);
    RUVIA_CHECK(conn.closing());
}
