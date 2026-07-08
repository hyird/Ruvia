#include "test_harness.h"

#include <cstdint>
#include <memory_resource>
#include <string_view>

#include "net/ws/WsConnection.h"

namespace {

using ruvia::WebSocketOpcode;
using ruvia::detail::WsConnection;
using ruvia::detail::WsEvent;
using ruvia::detail::WsFeedStatus;

// Build a masked client->server frame (RFC 6455 §5.1): FIN/opcode byte, MASK bit + a
// short length (<=125 for tests), a 4-byte mask, then the masked payload.
std::pmr::string maskedFrame(
    std::pmr::memory_resource* res, std::uint8_t opcode, std::string_view payload, bool fin = true) {
    std::pmr::string f(res);
    f.push_back(static_cast<char>((fin ? 0x80U : 0U) | opcode));
    f.push_back(static_cast<char>(0x80U | static_cast<std::uint8_t>(payload.size())));
    const unsigned char mask[4] = {0x11, 0x22, 0x33, 0x44};
    f.append(reinterpret_cast<const char*>(mask), 4);
    for (std::size_t i = 0; i < payload.size(); ++i) {
        f.push_back(static_cast<char>(static_cast<unsigned char>(payload[i]) ^ mask[i % 4]));
    }
    return f;
}

}  // namespace

// A single masked Text frame is delivered as one complete kMessage event, unmasked.
RUVIA_TEST(ws_connection_delivers_text_message) {
    std::pmr::monotonic_buffer_resource resource;
    WsConnection conn(&resource);

    const auto f = maskedFrame(&resource, 0x1, "hi");
    const auto r = conn.feed(std::string_view(f.data(), f.size()));
    RUVIA_CHECK(r.status == WsFeedStatus::kOk);

    const auto e = conn.nextEvent();
    RUVIA_CHECK(e.kind == WsEvent::Kind::kMessage);
    RUVIA_CHECK(e.opcode == WebSocketOpcode::kText);
    RUVIA_CHECK(e.payload == std::string_view("hi"));
    RUVIA_CHECK(conn.nextEvent().kind == WsEvent::Kind::kNone);
}

// A Ping is surfaced as kPing AND auto-answered with an unmasked Pong echoing the data.
RUVIA_TEST(ws_connection_auto_pongs_ping) {
    std::pmr::monotonic_buffer_resource resource;
    WsConnection conn(&resource);

    const auto f = maskedFrame(&resource, 0x9, "pp");
    conn.feed(std::string_view(f.data(), f.size()));

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
    WsConnection conn(&resource);

    std::pmr::string body(&resource);
    body.push_back(static_cast<char>(0x03));  // 1000 = normal closure
    body.push_back(static_cast<char>(0xE8));
    const auto f = maskedFrame(&resource, 0x8, std::string_view(body.data(), body.size()));
    conn.feed(std::string_view(f.data(), f.size()));

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
    WsConnection conn(&resource);

    const auto f1 = maskedFrame(&resource, 0x1, "ab", /*fin=*/false);
    const auto f2 = maskedFrame(&resource, 0x0, "cd", /*fin=*/true);  // continuation
    std::pmr::string both(&resource);
    both.append(f1.data(), f1.size());
    both.append(f2.data(), f2.size());
    conn.feed(std::string_view(both.data(), both.size()));

    const auto e = conn.nextEvent();
    RUVIA_CHECK(e.kind == WsEvent::Kind::kMessage);
    RUVIA_CHECK(e.payload == std::string_view("abcd"));
    RUVIA_CHECK(conn.nextEvent().kind == WsEvent::Kind::kNone);
}

// An unmasked frame violates RFC 6455 §5.1: the core queues a Close and reports the
// protocol error rather than delivering anything.
RUVIA_TEST(ws_connection_rejects_unmasked_frame) {
    std::pmr::monotonic_buffer_resource resource;
    WsConnection conn(&resource);

    // Text "hi" with the MASK bit clear.
    std::pmr::string f(&resource);
    f.push_back(static_cast<char>(0x81));  // FIN|Text
    f.push_back(static_cast<char>(2));     // no mask bit, len 2
    f.append("hi", 2);
    conn.feed(std::string_view(f.data(), f.size()));

    const auto e = conn.nextEvent();
    RUVIA_CHECK(e.kind == WsEvent::Kind::kProtocolError);
    RUVIA_CHECK(conn.closing());
    const auto out = conn.pendingOutput();
    RUVIA_CHECK_EQ(static_cast<unsigned char>(out[0]), static_cast<unsigned char>(0x88));  // Close
}

// submitMessage encodes an unmasked server Text frame.
RUVIA_TEST(ws_connection_submit_message_encodes_unmasked_frame) {
    std::pmr::monotonic_buffer_resource resource;
    WsConnection conn(&resource);

    conn.submitMessage(WebSocketOpcode::kText, "hello");
    const auto out = conn.pendingOutput();
    RUVIA_CHECK_EQ(static_cast<unsigned char>(out[0]), static_cast<unsigned char>(0x81));  // FIN|Text
    RUVIA_CHECK_EQ(static_cast<unsigned char>(out[1]), static_cast<unsigned char>(5));      // unmasked len
    RUVIA_CHECK(out.substr(2) == std::string_view("hello"));
}

// A partial frame (only the first byte) is buffered and yields no event until the rest
// arrives on a later feed.
RUVIA_TEST(ws_connection_needs_more_on_partial_frame) {
    std::pmr::monotonic_buffer_resource resource;
    WsConnection conn(&resource);

    const auto f = maskedFrame(&resource, 0x1, "split");
    conn.feed(std::string_view(f.data(), 3));  // header only, no full payload
    RUVIA_CHECK(conn.nextEvent().kind == WsEvent::Kind::kNone);
    RUVIA_CHECK(!conn.closing());

    conn.feed(std::string_view(f.data() + 3, f.size() - 3));  // remainder
    const auto e = conn.nextEvent();
    RUVIA_CHECK(e.kind == WsEvent::Kind::kMessage);
    RUVIA_CHECK(e.payload == std::string_view("split"));
}
