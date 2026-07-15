#include "test_harness.h"

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/http/ProtocolByteLimit.h"
#include "ruvia/http/detail/websocket/WsConnection.h"

namespace {

using ruvia::ProtocolByteLimit;
using ruvia::WebSocketOpcode;
using ruvia::detail::WebSocketDeflateNegotiation;
using ruvia::detail::WsConnection;
using ruvia::detail::WsAbortDisposition;
using ruvia::detail::WsCloseEvent;
using ruvia::detail::WsEvent;
using ruvia::detail::WsEventKind;
using ruvia::detail::WsCloseSubmitStatus;
using ruvia::detail::WsFrameSubmitStatus;
using ruvia::detail::WsLivenessMode;
using ruvia::detail::WsMessageEvent;
using ruvia::detail::WsOutputConsumeStatus;
using ruvia::detail::WsProtocolErrorEvent;
using ruvia::detail::WsTransportDisposition;

template <typename T>
concept HasLooseWsEventFields = requires(T& event) {
    event.kind = WsEventKind::kMessage;
    event.opcode = WebSocketOpcode::kText;
    event.payload = std::string_view{};
    event.closeCode = std::uint16_t{};
};

template <typename T>
concept HasAnyRvalueWsEventBorrow =
    requires(T&& event) { std::move(event).message(); } ||
    requires(T&& event) { std::move(event).ping(); } ||
    requires(T&& event) { std::move(event).pong(); } ||
    requires(T&& event) { std::move(event).close(); } ||
    requires(T&& event) { std::move(event).protocolError(); } ||
    requires(T&& event) { std::move(event).transportEnd(); };

static_assert(!HasAnyRvalueWsEventBorrow<WsEvent>);

template <typename T>
concept HasWsCloseCode = requires(const T& event) {
    { event.closeCode() } -> std::same_as<std::uint16_t>;
};

template <typename T>
concept HasWsPayload = requires(const T& event) {
    { event.payload() } -> std::same_as<std::string_view>;
};

template <typename T>
concept HasWsReason = requires(const T& event) {
    { event.reason() } -> std::same_as<std::string_view>;
};

template <typename T>
concept HasWsSubmitMessageAlias = requires(T& connection) {
    connection.submitMessage(WebSocketOpcode::kText, std::string_view{});
};

template <typename T>
concept HasWsSubmitPingAlias = requires(T& connection) {
    connection.submitPing(std::string_view{});
};

template <typename T>
concept HasWsSubmitPongAlias = requires(T& connection) {
    connection.submitPong(std::string_view{});
};

template <typename T>
concept HasWsApplicationFrameStateSideChannel = requires(
    const T& connection) {
    connection.acceptsApplicationFrames();
};

template <typename T>
concept HasWsEndsTransportAlias = requires(const T& plan) {
    plan.endsTransport();
};

template <typename T>
concept HasWsTransportEndPendingSideChannel = requires(
    const T& connection) {
    connection.transportEndPending();
};

template <typename T>
concept HasWsClosedStateSideChannel = requires(const T& connection) {
    connection.closed();
};

template <typename T>
concept HasWsClosePhaseSideChannel = requires(const T& connection) {
    connection.closePhase();
};

static_assert(std::same_as<
    decltype(std::declval<WsConnection&>().poll()),
    std::optional<WsEvent>>);
static_assert(!std::default_initializable<WsEvent>);
static_assert(!HasLooseWsEventFields<WsEvent>);
static_assert(!std::constructible_from<
    WsConnection,
    std::pmr::string&,
    std::size_t>);
static_assert(std::constructible_from<
    WsConnection,
    std::pmr::string&,
    ProtocolByteLimit>);
static_assert(!HasWsCloseCode<WsMessageEvent>);
static_assert(HasWsCloseCode<WsCloseEvent>);
static_assert(HasWsCloseCode<WsProtocolErrorEvent>);
static_assert(HasWsPayload<WsMessageEvent>);
static_assert(!HasWsPayload<WsCloseEvent>);
static_assert(HasWsReason<WsCloseEvent>);
static_assert(!HasWsReason<WsProtocolErrorEvent>);
static_assert(!std::constructible_from<
    WsConnection,
    std::pmr::string&,
    std::size_t,
    bool>);
static_assert(!HasWsSubmitMessageAlias<WsConnection>);
static_assert(!HasWsSubmitPingAlias<WsConnection>);
static_assert(!HasWsSubmitPongAlias<WsConnection>);
static_assert(!HasWsApplicationFrameStateSideChannel<WsConnection>);
static_assert(!HasWsEndsTransportAlias<ruvia::detail::WsOutputPlan>);
static_assert(!HasWsTransportEndPendingSideChannel<WsConnection>);
static_assert(!HasWsClosedStateSideChannel<WsConnection>);
static_assert(!HasWsClosePhaseSideChannel<WsConnection>);
static_assert(std::same_as<
    decltype(std::declval<const ruvia::detail::WsOutputPlan&>()
        .disposition()),
    WsTransportDisposition>);
static_assert(std::same_as<
    decltype(std::declval<WsConnection&>().submitFrame(
        WebSocketOpcode::kText,
        std::string_view{})),
    WsFrameSubmitStatus>);
static_assert(std::same_as<
    decltype(std::declval<WsConnection&>().submitClose(
        std::uint16_t{},
        std::string_view{})),
    WsCloseSubmitStatus>);
static_assert(std::same_as<
    decltype(std::declval<const WsConnection&>().livenessMode()),
    WsLivenessMode>);
static_assert(std::same_as<
    decltype(std::declval<WsConnection&>().abort()),
    WsAbortDisposition>);

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

std::optional<WsEvent> pollBytes(
    WsConnection& connection,
    std::pmr::string& input,
    std::string_view bytes) {
    input.append(bytes.data(), bytes.size());
    return connection.poll();
}

}  // namespace

// A single masked Text frame is delivered as one complete typed event, unmasked.
RUVIA_TEST(ws_connection_event_is_optional_and_discriminated) {
    std::pmr::monotonic_buffer_resource resource;
    std::pmr::string input(&resource);
    WsConnection conn(input);

    const auto f = maskedFrame(&resource, 0x1, "hi");
    const auto e = pollBytes(conn, input, std::string_view(f.data(), f.size()));
    RUVIA_CHECK(e.has_value());
    RUVIA_CHECK(e->kind() == WsEventKind::kMessage);
    RUVIA_CHECK(e->ping() == nullptr);
    RUVIA_CHECK(e->close() == nullptr);
    const auto* message = e->message();
    RUVIA_CHECK(message != nullptr);
    RUVIA_CHECK(message->opcode() == WebSocketOpcode::kText);
    RUVIA_CHECK(message->payload() == std::string_view("hi"));
    RUVIA_CHECK(
        message->payload().data() == input.data() + 6);  // header + mask, no copy
    RUVIA_CHECK(!conn.poll().has_value());
}

// A Ping is surfaced as kPing AND auto-answered with an unmasked Pong echoing the data.
RUVIA_TEST(ws_connection_auto_pongs_ping) {
    std::pmr::monotonic_buffer_resource resource;
    std::pmr::string input(&resource);
    WsConnection conn(input);

    const auto f = maskedFrame(&resource, 0x9, "pp");
    const auto e = pollBytes(conn, input, std::string_view(f.data(), f.size()));
    RUVIA_CHECK(e->kind() == WsEventKind::kPing);
    RUVIA_CHECK(e->ping()->payload() == std::string_view("pp"));
    RUVIA_CHECK(e->message() == nullptr);

    const auto plan = conn.outputPlan();
    const auto out = plan.bytes();
    RUVIA_CHECK(plan.disposition() == WsTransportDisposition::kKeepOpen);
    RUVIA_CHECK_EQ(out.size(), static_cast<std::size_t>(4));
    RUVIA_CHECK_EQ(static_cast<unsigned char>(out[0]), static_cast<unsigned char>(0x8A));  // FIN|Pong
    RUVIA_CHECK_EQ(static_cast<unsigned char>(out[1]), static_cast<unsigned char>(2));      // unmasked len
    RUVIA_CHECK(out.substr(2) == std::string_view("pp"));
}

// Pong is observational only: its payload is typed separately and it queues no output.
RUVIA_TEST(ws_connection_surfaces_pong_payload) {
    std::pmr::monotonic_buffer_resource resource;
    std::pmr::string input(&resource);
    WsConnection conn(input);

    const auto frame = maskedFrame(&resource, 0xA, "ack");
    const auto event = pollBytes(
        conn, input, std::string_view(frame.data(), frame.size()));
    RUVIA_CHECK(event->kind() == WsEventKind::kPong);
    RUVIA_CHECK(event->pong()->payload() == "ack");
    RUVIA_CHECK(event->ping() == nullptr);
    RUVIA_CHECK(conn.outputPlan().bytes().empty());
}

// A peer Close yields kClose (with the parsed code), an echoed Close frame, and one
// atomic output plan that ends the underlying transport after those bytes.
RUVIA_TEST(ws_connection_echoes_close) {
    std::pmr::monotonic_buffer_resource resource;
    std::pmr::string input(&resource);
    WsConnection conn(input);

    std::pmr::string body(&resource);
    body.push_back(static_cast<char>(0x03));  // 1000 = normal closure
    body.push_back(static_cast<char>(0xE8));
    body.append("bye");
    const auto f = maskedFrame(&resource, 0x8, std::string_view(body.data(), body.size()));
    const auto e = pollBytes(conn, input, std::string_view(f.data(), f.size()));
    RUVIA_CHECK(e->kind() == WsEventKind::kClose);
    RUVIA_CHECK_EQ(e->close()->closeCode(), static_cast<std::uint16_t>(1000));
    RUVIA_CHECK(e->close()->reason() == "bye");
    RUVIA_CHECK(e->protocolError() == nullptr);
    RUVIA_CHECK(conn.livenessMode() == WsLivenessMode::kInactive);

    const auto plan = conn.outputPlan();
    const auto out = plan.bytes();
    RUVIA_CHECK(plan.disposition() ==
        WsTransportDisposition::kEndTransport);
    RUVIA_CHECK_EQ(static_cast<unsigned char>(out[0]), static_cast<unsigned char>(0x88));  // FIN|Close
    const auto original = std::string(out);
    RUVIA_CHECK(conn.consumeOutput(out.size() + 1) ==
        WsOutputConsumeStatus::kOutOfRange);
    RUVIA_CHECK(conn.outputPlan().bytes() == original);
    RUVIA_CHECK(conn.outputPlan().disposition() ==
        WsTransportDisposition::kEndTransport);
    RUVIA_CHECK(conn.consumeOutput(out.size()) ==
        WsOutputConsumeStatus::kDrained);
    conn.commitTransportEnd();
    RUVIA_CHECK(conn.abort() == WsAbortDisposition::kNoTransportAction);
}

// RFC 6455 uses 1005 locally to report an absent status; it is not present in
// the echoed wire payload and is not confused with a synthetic normal close.
RUVIA_TEST(ws_connection_close_without_status_reports_1005) {
    std::pmr::monotonic_buffer_resource resource;
    std::pmr::string input(&resource);
    WsConnection conn(input);

    const auto frame = maskedFrame(&resource, 0x8, {});
    const auto event = pollBytes(
        conn, input, std::string_view(frame.data(), frame.size()));
    RUVIA_CHECK(event->close() != nullptr);
    RUVIA_CHECK_EQ(event->close()->closeCode(), std::uint16_t{1005});
    RUVIA_CHECK(event->close()->reason().empty());
    const auto output = conn.outputPlan().bytes();
    RUVIA_CHECK_EQ(output.size(), std::size_t{2});
    RUVIA_CHECK_EQ(static_cast<unsigned char>(output[0]), 0x88U);
    RUVIA_CHECK_EQ(static_cast<unsigned char>(output[1]), 0U);
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
    const auto e = pollBytes(conn, input, std::string_view(both.data(), both.size()));
    RUVIA_CHECK(e->message()->payload() == std::string_view("abcd"));
    RUVIA_CHECK(!conn.poll().has_value());
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
    const auto e = pollBytes(conn, input, std::string_view(f.data(), f.size()));
    RUVIA_CHECK(e->kind() == WsEventKind::kProtocolError);
    RUVIA_CHECK_EQ(e->protocolError()->closeCode(), std::uint16_t{1002});
    RUVIA_CHECK(e->message() == nullptr);
    RUVIA_CHECK(conn.livenessMode() == WsLivenessMode::kInactive);
    const auto plan = conn.outputPlan();
    const auto out = plan.bytes();
    RUVIA_CHECK(plan.disposition() ==
        WsTransportDisposition::kEndTransport);
    RUVIA_CHECK_EQ(static_cast<unsigned char>(out[0]), static_cast<unsigned char>(0x88));  // Close
}

// submitFrame is the sole generic outbound-frame entry and encodes an unmasked
// server Text frame.
RUVIA_TEST(ws_connection_submit_frame_encodes_unmasked_frame) {
    std::pmr::monotonic_buffer_resource resource;
    std::pmr::string input(&resource);
    WsConnection conn(input);

    RUVIA_CHECK(conn.submitFrame(WebSocketOpcode::kText, "hello") ==
        WsFrameSubmitStatus::kAccepted);
    const auto out = conn.outputPlan().bytes();
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
    RUVIA_CHECK(
        !pollBytes(conn, input, std::string_view(f.data(), 3)).has_value());
    RUVIA_CHECK(conn.livenessMode() == WsLivenessMode::kOpen);

    const auto e = pollBytes(
        conn, input, std::string_view(f.data() + 3, f.size() - 3));  // remainder
    RUVIA_CHECK(e->message()->payload() == std::string_view("split"));
}

// With permessage-deflate negotiated, an RSV1 (compressed) message is inflated and
// delivered as its original text.
RUVIA_TEST(ws_connection_inflates_compressed_message) {
    std::pmr::monotonic_buffer_resource resource;
    std::pmr::string input(&resource);
    WsConnection conn(
        input,
        ProtocolByteLimit::unlimited(),
        WebSocketDeflateNegotiation::kAccepted);

    ruvia::detail::WebSocketDeflate encoder;
    std::pmr::string compressed(&resource);
    RUVIA_CHECK(encoder.compress("hello hello hello", compressed));

    const auto f = maskedFrame(
        &resource, 0x1, std::string_view(compressed.data(), compressed.size()),
        /*fin=*/true, /*rsv1=*/true);
    const auto e = pollBytes(conn, input, std::string_view(f.data(), f.size()));
    RUVIA_CHECK(e->message()->payload() == std::string_view("hello hello hello"));
}

// With permessage-deflate on, submitFrame compresses a shrinkable payload and sets
// the RSV1 bit.
RUVIA_TEST(ws_connection_submit_compresses_when_enabled) {
    std::pmr::monotonic_buffer_resource resource;
    std::pmr::string input(&resource);
    WsConnection conn(
        input,
        ProtocolByteLimit::unlimited(),
        WebSocketDeflateNegotiation::kAccepted);

    const std::pmr::string repetitive(200, 'a', &resource);
    RUVIA_CHECK(conn.submitFrame(
        WebSocketOpcode::kText,
        std::string_view(repetitive.data(), repetitive.size())) ==
        WsFrameSubmitStatus::kAccepted);

    const auto out = conn.outputPlan().bytes();
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
    const auto event = pollBytes(conn, input, std::string_view(f.data(), f.size()));
    RUVIA_CHECK(event->protocolError() != nullptr);
    RUVIA_CHECK(conn.outputPlan().disposition() ==
        WsTransportDisposition::kEndTransport);
}

RUVIA_TEST(ws_connection_outbound_frame_rejections_are_typed_and_transactional) {
    std::pmr::monotonic_buffer_resource resource;
    std::pmr::string input(&resource);
    WsConnection conn(input, ProtocolByteLimit::limited(4));

    RUVIA_CHECK(conn.submitFrame(WebSocketOpcode::kText, "12345") ==
        WsFrameSubmitStatus::kMessageTooLarge);
    RUVIA_CHECK(conn.submitFrame(
        WebSocketOpcode::kPing,
        std::string(126, 'p')) ==
        WsFrameSubmitStatus::kControlFrameTooLarge);
    RUVIA_CHECK(conn.submitFrame(WebSocketOpcode::kClose, {}) ==
        WsFrameSubmitStatus::kInvalidOpcode);
    RUVIA_CHECK(conn.submitFrame(
        static_cast<WebSocketOpcode>(0x7), {}) ==
        WsFrameSubmitStatus::kInvalidOpcode);
    RUVIA_CHECK(conn.outputPlan().bytes().empty());
    RUVIA_CHECK(conn.livenessMode() == WsLivenessMode::kOpen);
}

RUVIA_TEST(ws_connection_outbound_close_rejections_are_typed_and_transactional) {
    std::pmr::monotonic_buffer_resource resource;
    std::pmr::string input(&resource);
    WsConnection conn(input);

    RUVIA_CHECK(conn.submitClose(1005, {}) ==
        WsCloseSubmitStatus::kInvalidCode);
    RUVIA_CHECK(conn.submitClose(1000, std::string("\xc0\x80", 2)) ==
        WsCloseSubmitStatus::kInvalidReason);
    RUVIA_CHECK(conn.submitClose(1000, std::string(124, 'x')) ==
        WsCloseSubmitStatus::kReasonTooLarge);
    RUVIA_CHECK(conn.outputPlan().bytes().empty());
    RUVIA_CHECK(conn.livenessMode() == WsLivenessMode::kOpen);
}

// A locally initiated Close is not transport EOF. Its bytes are flushed while the
// transport remains open; application data is then ignored until the peer Close
// completes the handshake and produces the terminal transport plan.
RUVIA_TEST(ws_connection_local_close_waits_for_peer_close) {
    std::pmr::monotonic_buffer_resource resource;
    std::pmr::string input(&resource);
    WsConnection conn(input);

    RUVIA_CHECK(conn.submitClose(1000, "bye") ==
        WsCloseSubmitStatus::kAccepted);
    RUVIA_CHECK(conn.livenessMode() == WsLivenessMode::kAwaitingPeerClose);
    auto plan = conn.outputPlan();
    RUVIA_CHECK(plan.disposition() ==
        WsTransportDisposition::kKeepOpen);
    RUVIA_CHECK_EQ(static_cast<unsigned char>(plan.bytes()[0]), 0x88U);
    const auto original = std::string(plan.bytes());
    RUVIA_CHECK(conn.consumeOutput(plan.bytes().size() + 1) ==
        WsOutputConsumeStatus::kOutOfRange);
    RUVIA_CHECK(conn.outputPlan().bytes() == original);
    RUVIA_CHECK(conn.outputPlan().disposition() ==
        WsTransportDisposition::kKeepOpen);
    RUVIA_CHECK(conn.consumeOutput(1) ==
        WsOutputConsumeStatus::kPending);
    RUVIA_CHECK(conn.outputPlan().bytes() ==
        std::string_view(original).substr(1));
    RUVIA_CHECK(conn.consumeOutput(original.size() - 1) ==
        WsOutputConsumeStatus::kDrained);
    RUVIA_CHECK(conn.livenessMode() == WsLivenessMode::kAwaitingPeerClose);

    std::pmr::string closePayload(&resource);
    closePayload.push_back(static_cast<char>(0x03));
    closePayload.push_back(static_cast<char>(0xE8));
    const auto ignoredText = maskedFrame(&resource, 0x1, "late");
    const auto peerClose = maskedFrame(
        &resource, 0x8, std::string_view(closePayload.data(), closePayload.size()));
    std::pmr::string inbound(&resource);
    inbound.append(ignoredText.data(), ignoredText.size());
    inbound.append(peerClose.data(), peerClose.size());

    const auto event = pollBytes(conn, input, std::string_view(inbound.data(), inbound.size()));
    RUVIA_CHECK(event->close() != nullptr);
    RUVIA_CHECK_EQ(event->close()->closeCode(), std::uint16_t{1000});
    plan = conn.outputPlan();
    RUVIA_CHECK(plan.bytes().empty());  // local Close was already sent; no duplicate
    RUVIA_CHECK(plan.disposition() ==
        WsTransportDisposition::kEndTransport);
    conn.commitTransportEnd();
    RUVIA_CHECK(conn.abort() == WsAbortDisposition::kNoTransportAction);
}

// A transport EOF is not rewritten into a synthetic normal WebSocket Close. It
// discards any queued WS bytes and asks only for transport termination.
RUVIA_TEST(ws_connection_transport_eof_discards_unsent_close) {
    std::pmr::monotonic_buffer_resource resource;
    std::pmr::string input(&resource);
    WsConnection conn(input);

    RUVIA_CHECK(conn.submitClose(1000, {}) ==
        WsCloseSubmitStatus::kAccepted);
    RUVIA_CHECK(!conn.outputPlan().bytes().empty());
    conn.notifyTransportEof();
    const auto plan = conn.outputPlan();
    RUVIA_CHECK(plan.bytes().empty());
    RUVIA_CHECK(plan.disposition() ==
        WsTransportDisposition::kEndTransport);
    const auto event = conn.poll();
    RUVIA_CHECK(event->kind() == WsEventKind::kTransportEnd);
    RUVIA_CHECK(event->transportEnd() != nullptr);
    RUVIA_CHECK(event->close() == nullptr);
}

RUVIA_TEST(ws_connection_reports_not_open_for_outbound_submissions_after_close) {
    std::pmr::monotonic_buffer_resource resource;
    std::pmr::string input(&resource);
    WsConnection conn(input);
    RUVIA_CHECK(conn.submitClose(1000, {}) ==
        WsCloseSubmitStatus::kAccepted);
    RUVIA_CHECK(conn.submitFrame(WebSocketOpcode::kText, "late") ==
        WsFrameSubmitStatus::kNotOpen);
    RUVIA_CHECK(conn.submitClose(1000, {}) ==
        WsCloseSubmitStatus::kAlreadyClosing);
}

RUVIA_TEST(ws_connection_abort_returns_transport_action_once) {
    std::pmr::monotonic_buffer_resource resource;
    std::pmr::string input(&resource);
    WsConnection conn(input);

    RUVIA_CHECK(conn.abort() == WsAbortDisposition::kAbortTransport);
    RUVIA_CHECK(conn.abort() == WsAbortDisposition::kNoTransportAction);
    RUVIA_CHECK(conn.submitFrame(WebSocketOpcode::kText, "late") ==
        WsFrameSubmitStatus::kNotOpen);
    RUVIA_CHECK(conn.submitClose(1000, {}) ==
        WsCloseSubmitStatus::kClosed);
}
