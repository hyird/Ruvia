#include <gtest/gtest.h>
#include <zlib.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "ruvia/http/http_io_buffer.h"
#include "ruvia/http/websocket_connection.h"

#include "test_exceptions.h"

namespace {
using namespace ruvia;

struct mask_source {
    unsigned calls_{0};
    bool fail_{false};
    static bool generate(void* context_value, websocket_mask_key_type& key) noexcept {
        auto& self = *static_cast<mask_source*>(context_value);
        ++self.calls_;
        key = {static_cast<char>(self.calls_), '\x23', '\x45', '\x67'};
        return !self.fail_;
    }
};

websocket_connection client(http_io_buffer& input, mask_source& source_value, protocol_byte_limit limit = protocol_byte_limit::unlimited()) {
    return websocket_connection(input, {.message_limit_ = limit, .role_ = websocket_connection_role::client, .mask_key_generator_ = &mask_source::generate, .mask_key_context_ = &source_value});
}

std::string drain(websocket_connection& connection) {
    const std::string bytes_value(connection.output_plan().bytes());
    if (connection.consume_output(bytes_value.size()) != websocket_output_consume_status::drained) {
        throw std::runtime_error("output did not drain");
    }
    return bytes_value;
}
}  // namespace

TEST(Ruvia, ws_public_close_code_rejects_wide_and_reserved_values_before_narrowing) {
    const auto rejects = [](auto input) {
        try {
            (void)websocket_close_code::from_value(input);
        } catch (const std::invalid_argument&) {
            return true;
        }
        return false;
    };
    const std::array<std::int64_t, 14> invalid_values{
        std::numeric_limits<std::int64_t>::min(), -64536, -1, 0, 999,
        1004, 1005, 1006, 1015, 1016, 2999, 5000, 66536,
        std::numeric_limits<std::int64_t>::max()};
    for (const auto input : invalid_values) {
        EXPECT_TRUE(!websocket_close_code::try_from_value(input).has_value());
        EXPECT_TRUE(rejects(input));
    }
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    EXPECT_TRUE(!websocket_close_code::try_from_value(maximum).has_value());
    EXPECT_TRUE(rejects(maximum));
}

TEST(Ruvia, ws_public_checked_close_codes_preserve_wire_value_and_role_rules) {
    for (const std::uint64_t input : {1000u, 1003u, 1007u, 1014u, 3000u, 3999u, 4000u, 4999u, 1010u}) {
        mask_source source;
        http_io_buffer client_input;
        auto sender = client(client_input, source);
        http_io_buffer server_input;
        websocket_connection receiver(server_input);
        const auto code = websocket_close_code::from_value(input);
        EXPECT_TRUE(sender.submit_close(code, "checked") == websocket_close_submit_status::accepted);
        server_input.append(drain(sender));
        const auto event = receiver.next_event();
        EXPECT_TRUE(event && event->close() && event->close()->close_code() == input);
        EXPECT_TRUE(event && event->close() && event->close()->reason() == "checked");
    }

    http_io_buffer server_input;
    websocket_connection sender(server_input);
    EXPECT_TRUE(sender.submit_close(websocket_close_code::from_value(1010), {}) == websocket_close_submit_status::invalid_code);
    EXPECT_TRUE(sender.submit_frame(websocket_opcode::text, "still-open") == websocket_frame_submit_status::accepted);
    EXPECT_TRUE(sender.submit_close(websocket_close_status::normal_closure, std::string_view("\xff", 1)) == websocket_close_submit_status::invalid_reason);
    EXPECT_TRUE(sender.submit_close(websocket_close_status::normal_closure, std::string(124, 'x')) == websocket_close_submit_status::reason_too_large);
    const std::string reason(123, 'x');
    EXPECT_TRUE(sender.submit_close(websocket_close_status::normal_closure, reason) == websocket_close_submit_status::accepted);
    mask_source source;
    http_io_buffer client_input;
    auto receiver = client(client_input, source);
    client_input.append(drain(sender));
    const auto message = receiver.next_event();
    EXPECT_TRUE(message && message->message() && message->message()->payload() == "still-open");
    const auto closed = receiver.next_event();
    EXPECT_TRUE(closed && closed->close() && closed->close()->close_code() == 1000);
    EXPECT_TRUE(closed && closed->close() && closed->close()->reason() == reason);
}

TEST(Ruvia, ws_public_compression_accepts_final_blocks_and_preserves_required_history) {
    for (const bool no_takeover : {false, true}) {
        mask_source mask;
        http_io_buffer input;
        websocket_connection receiver(input, {.compression_ = (websocket_compression{.enabled_ = true, .server_no_context_takeover_ = no_takeover}), .role_ = websocket_connection_role::client, .mask_key_generator_ = &mask_source::generate, .mask_key_context_ = &mask});
        // RFC 7692 section 7.2.3.4: BFINAL=1, padding, then an empty block.
        input.append(std::string_view("\xc1\x08\xf3\x48\xcd\xc9\xc9\x07\x00\x00", 10));
        auto event = receiver.next_event();
        EXPECT_TRUE(event && event->message() && event->message()->payload() == "Hello");
        // Two terminated streams within one message share their LZ77 history
        // even when context takeover between messages is disabled.
        input.append(std::string_view("\xc1\x0c\xf3\x48\xcd\xc9\xc9\x07\x00\xf3\x00\x11\x00\x00", 14));
        event = receiver.next_event();
        EXPECT_TRUE(event && event->message() && event->message()->payload() == "HelloHello");
        input.append(std::string_view("\xc1\x01\x00", 3));
        event = receiver.next_event();
        EXPECT_TRUE(event && event->message() && event->message()->payload().empty());
        // This message refers back to the preceding message's dictionary.
        input.append(std::string_view("\xc1\x05\xf2\x00\x11\x00\x00", 7));
        event = receiver.next_event();
        if (no_takeover) {
            EXPECT_TRUE(event && event->protocol_error() && event->protocol_error()->close_code() == 1002);
        } else {
            EXPECT_TRUE(event && event->message() && event->message()->payload() == "Hello");
        }
    }
}

TEST(Ruvia, ws_public_compression_rejects_incomplete_empty_block_headers) {
    for (const auto wire : {std::string_view("\xc1\x00", 2), std::string_view("\xc1\x02\x03\x00", 4)}) {
        mask_source mask;
        http_io_buffer input;
        websocket_connection receiver(input, {.compression_ = (websocket_compression{.enabled_ = true}), .role_ = websocket_connection_role::client, .mask_key_generator_ = &mask_source::generate, .mask_key_context_ = &mask});
        input.append(wire);
        const auto event = receiver.next_event();
        EXPECT_TRUE(event && event->protocol_error() && event->protocol_error()->close_code() == 1002);
        EXPECT_TRUE(receiver.submit_frame(websocket_opcode::text, "after-error") == websocket_frame_submit_status::not_open);
    }
}

TEST(Ruvia, ws_public_server_events_use_public_payload_types) {
    mask_source mask;
    http_io_buffer sender_input;
    auto sender = client(sender_input, mask);
    http_io_buffer input;
    websocket_connection protocol(input);

    EXPECT_TRUE(sender.submit_frame(websocket_opcode::text, "hey") ==
                websocket_frame_submit_status::accepted);
    const std::string message_wire(sender.output_plan().bytes());
    EXPECT_TRUE(sender.consume_output(message_wire.size()) == websocket_output_consume_status::drained);
    input.append(message_wire);
    auto message = protocol.next_event();
    EXPECT_TRUE(message && message->kind() == websocket_event_kind::message);
    EXPECT_TRUE(message && message->message());
    if (message && message->message()) {
        EXPECT_TRUE(message->message()->opcode() == websocket_opcode::text);
        EXPECT_EQ(message->message()->payload(), "hey");
    }

    EXPECT_TRUE(sender.submit_frame(websocket_opcode::ping, "p") ==
                websocket_frame_submit_status::accepted);
    const std::string ping_wire(sender.output_plan().bytes());
    EXPECT_TRUE(sender.consume_output(ping_wire.size()) == websocket_output_consume_status::drained);
    input.append(ping_wire);
    auto ping = protocol.next_event();
    EXPECT_TRUE(ping && ping->kind() == websocket_event_kind::ping);
    EXPECT_TRUE(ping && ping->ping());
    if (ping && ping->ping()) {
        EXPECT_EQ(ping->ping()->payload(), "p");
    }
}

TEST(Ruvia, ws_public_server_protocol_reports_protocol_error_as_public_value) {
    http_io_buffer input;
    websocket_connection protocol(input);
    // A server-side connection must reject an unmasked client frame.
    input.append(std::string_view("\x81\x01x", 3));
    auto error = protocol.next_event();
    EXPECT_TRUE(error && error->kind() == websocket_event_kind::protocol_error);
    EXPECT_TRUE(error && error->protocol_error() && error->protocol_error()->close_code() == 1002);
}

TEST(Ruvia, ws_public_server_protocol_controls_permessage_deflate_per_frame) {
    http_io_buffer input;
    websocket_connection protocol(input, {.message_limit_ = protocol_byte_limit::unlimited(),
                                             .compression_ = websocket_compression{.enabled_ = true},
                                             .deflater_ = {.compression_level_ = 6}});
    const std::string payload_value(200, 'x');

    EXPECT_TRUE(protocol.submit_frame(websocket_opcode::text, payload_value, {.compress_ = false}) ==
                websocket_frame_submit_status::accepted);
    auto output = protocol.output_plan().bytes();
    EXPECT_TRUE(!output.empty());
    if (!output.empty()) {
        EXPECT_TRUE((static_cast<unsigned char>(output[0]) & 0x40U) == 0);
    }
    EXPECT_TRUE(protocol.consume_output(output.size()) ==
                websocket_output_consume_status::drained);

    EXPECT_TRUE(protocol.submit_frame(websocket_opcode::text, payload_value) ==
                websocket_frame_submit_status::accepted);
    output = protocol.output_plan().bytes();
    EXPECT_TRUE(!output.empty());
    if (!output.empty()) {
        EXPECT_TRUE((static_cast<unsigned char>(output[0]) & 0x40U) != 0);
    }
}

TEST(Ruvia, ws_public_server_protocol_preserves_transport_end_semantics) {
    http_io_buffer input;
    websocket_connection protocol(input);
    EXPECT_TRUE(protocol.submit_close(websocket_close_status::normal_closure, "done") == websocket_close_submit_status::accepted);
    EXPECT_TRUE(protocol.output_plan().disposition() ==
                websocket_transport_disposition::keep_open);
    const auto output = protocol.output_plan().bytes();
    EXPECT_TRUE(!output.empty());
    EXPECT_TRUE(protocol.consume_output(output.size()) ==
                websocket_output_consume_status::drained);
    // After our Close frame is sent, the protocol still awaits the peer's
    // Close; transport EOF terminates that wait without fabricating a reply.
    EXPECT_TRUE(protocol.output_plan().disposition() ==
                websocket_transport_disposition::keep_open);
    protocol.notify_transport_eof();
    EXPECT_TRUE(protocol.output_plan().disposition() ==
                websocket_transport_disposition::end_transport);
}

TEST(Ruvia, ws_public_server_eof_discards_pending_output_before_transport_end) {
    http_io_buffer input;
    websocket_connection protocol(input);
    EXPECT_TRUE(protocol.submit_frame(websocket_opcode::text, "unsent") ==
                websocket_frame_submit_status::accepted);
    const auto output = protocol.output_plan().bytes();
    EXPECT_TRUE(output.size() > 1);
    EXPECT_TRUE(protocol.consume_output(1) == websocket_output_consume_status::pending);

    protocol.notify_transport_eof();
    const auto end = protocol.output_plan();
    EXPECT_TRUE(end.bytes().empty());
    EXPECT_TRUE(end.disposition() == websocket_transport_disposition::end_transport);
    // The driver can finish its direction without consuming bytes discarded by EOF.
    EXPECT_TRUE(protocol.consume_output(end.bytes().size()) ==
                websocket_output_consume_status::drained);
    protocol.commit_transport_end();
    EXPECT_TRUE(protocol.abort() == websocket_abort_disposition::no_transport_action);
}

TEST(Ruvia, ws_public_server_keeps_pending_pong_before_peer_close) {
    http_io_buffer input;
    websocket_connection protocol(input);
    EXPECT_TRUE(protocol.submit_close(websocket_close_status::normal_closure, "done") ==
                websocket_close_submit_status::accepted);
    auto output = protocol.output_plan().bytes();
    EXPECT_TRUE(!output.empty());
    EXPECT_TRUE(protocol.consume_output(output.size()) ==
                websocket_output_consume_status::drained);

    // Masked client Ping("p") followed by an empty masked Close.
    input.append(std::string_view(
        "\x89\x81\x01\x02\x03\x04q"
        "\x88\x80\x05\x06\x07\x08",
        13));
    const auto ping = protocol.next_event();
    EXPECT_TRUE(ping && ping->kind() == websocket_event_kind::ping);
    EXPECT_TRUE(ping && ping->ping() && ping->ping()->payload() == "p");
    const auto close = protocol.next_event();
    EXPECT_TRUE(close && close->kind() == websocket_event_kind::close);

    const auto plan = protocol.output_plan();
    EXPECT_EQ(plan.bytes(), std::string_view("\x8a\x01p", 3));
    EXPECT_TRUE(plan.disposition() ==
                websocket_transport_disposition::end_transport);
    EXPECT_TRUE(protocol.consume_output(plan.bytes().size()) ==
                websocket_output_consume_status::drained);
    protocol.commit_transport_end();
    EXPECT_TRUE(protocol.liveness_mode() == websocket_liveness_mode::inactive);
}

TEST(Ruvia, ws_public_client_server_exchange_and_partial_output) {
    mask_source source;
    http_io_buffer sender_input;
    auto sender = client(sender_input, source);
    http_io_buffer receiver_input;
    websocket_connection receiver(receiver_input);
    const std::string payload_value(65536, '\xff');
    for (unsigned round = 0; round < 3; ++round) {
        EXPECT_TRUE(sender.submit_frame(websocket_opcode::binary, payload_value) == websocket_frame_submit_status::accepted);
        const std::string wire(sender.output_plan().bytes());
        EXPECT_TRUE((static_cast<unsigned char>(wire[1]) & 0x80U) != 0);
        EXPECT_TRUE(sender.consume_output(wire.size() + 1) == websocket_output_consume_status::out_of_range);
        EXPECT_TRUE(sender.consume_output(1) == websocket_output_consume_status::pending);
        EXPECT_EQ(sender.output_plan().bytes(), std::string_view(wire).substr(1));
        EXPECT_TRUE(sender.consume_output(wire.size() - 1) == websocket_output_consume_status::drained);
        receiver_input.append(std::string_view(wire).substr(0, 5));
        EXPECT_TRUE(!receiver.next_event());
        receiver_input.append(std::string_view(wire).substr(5));
        const auto event = receiver.next_event();
        EXPECT_TRUE(event && event->message());
        if (!event || !event->message()) {
            return;
        }
        EXPECT_EQ(event->message()->payload(), payload_value);
        EXPECT_TRUE(receiver.submit_frame(websocket_opcode::binary, event->message()->payload()) == websocket_frame_submit_status::accepted);
        const auto reply = drain(receiver);
        EXPECT_TRUE((static_cast<unsigned char>(reply[1]) & 0x80U) == 0);
        sender_input.append(reply);
        const auto echo = sender.next_event();
        EXPECT_TRUE(echo && echo->message());
        if (!echo || !echo->message()) {
            return;
        }
        EXPECT_EQ(echo->message()->payload(), payload_value);
    }
    EXPECT_EQ(source.calls_, 3U);
}

TEST(Ruvia, ws_public_client_fragmented_message_with_ping_and_close) {
    mask_source source;
    http_io_buffer input;
    auto connection = client(input, source);
    http_io_buffer server_input;
    websocket_connection server(server_input);
    // A fragmented binary message with a control frame between fragments.
    const std::string wire(
        "\x02\x02"
        "ab"
        "\x89\x01"
        "p"
        "\x80\x02"
        "cd",
        11);
    input.append(wire);
    const auto ping = connection.next_event();
    EXPECT_TRUE(ping && ping->ping());
    const auto pong = drain(connection);
    server_input.append(pong);
    const auto pong_event = server.next_event();
    EXPECT_TRUE(pong_event && pong_event->pong() && pong_event->pong()->payload() == "p");
    const auto message = connection.next_event();
    EXPECT_TRUE(message && message->message() && message->message()->payload() == "abcd");
    EXPECT_TRUE(server.submit_close(websocket_close_status::normal_closure, "done") == websocket_close_submit_status::accepted);
    const auto close = drain(server);
    input.append(close);
    const auto closed = connection.next_event();
    EXPECT_TRUE(closed && closed->close() && closed->close()->close_code() == 1000);
    // RFC 6455 section 7.1.1: the server closes the transport first, so the
    // client flushes its echo and keeps the transport open until server EOF.
    EXPECT_TRUE(connection.output_plan().disposition() == websocket_transport_disposition::keep_open);
    const auto response = drain(connection);
    EXPECT_TRUE(connection.output_plan().disposition() == websocket_transport_disposition::keep_open);
    EXPECT_TRUE(connection.liveness_mode() == websocket_liveness_mode::awaiting_peer_eof);
    server_input.append(response);
    const auto acknowledged = server.next_event();
    EXPECT_TRUE(acknowledged && acknowledged->close());
    // Bytes after the close handshake are discarded while waiting.
    EXPECT_TRUE(connection.feed("x") == websocket_feed_status::accepted);
    input.append("x");
    EXPECT_TRUE(!connection.next_event());
    EXPECT_TRUE(connection.submit_frame(websocket_opcode::text, "late") == websocket_frame_submit_status::not_open);
    connection.notify_transport_eof();
    const auto end = connection.next_event();
    EXPECT_TRUE(end && end->transport_end());
    EXPECT_TRUE(connection.output_plan().disposition() == websocket_transport_disposition::end_transport);
    connection.commit_transport_end();
    EXPECT_TRUE(connection.feed("x") == websocket_feed_status::inactive);
    EXPECT_EQ(source.calls_, 2U);
    EXPECT_TRUE(pong.substr(2, 4) != response.substr(2, 4));
}

TEST(Ruvia, ws_public_client_waits_for_server_eof_after_initiating_close) {
    mask_source source;
    http_io_buffer input;
    websocket_connection connection(input, {.role_ = websocket_connection_role::client,
                                               .mask_key_generator_ = &mask_source::generate,
                                               .mask_key_context_ = &source});
    http_io_buffer server_input;
    websocket_connection server(server_input);
    EXPECT_TRUE(connection.submit_close(websocket_close_status::normal_closure, "done") == websocket_close_submit_status::accepted);
    const auto request_bytes = drain(connection);
    server_input.append(request_bytes);
    const auto request = server.next_event();
    EXPECT_TRUE(request && request->close());
    const auto reply = drain(server);
    EXPECT_TRUE(server.output_plan().disposition() == websocket_transport_disposition::end_transport);

    // The server's Close reply, then bytes the client must ignore.
    input.append(reply);
    input.append(std::string_view("\x81\x01x", 3));
    const auto closed = connection.next_event();
    EXPECT_TRUE(closed && closed->close() && closed->close()->close_code() == 1000);
    const auto waiting = connection.output_plan();
    EXPECT_TRUE(waiting.bytes().empty());
    EXPECT_TRUE(waiting.disposition() == websocket_transport_disposition::keep_open);
    EXPECT_TRUE(connection.liveness_mode() == websocket_liveness_mode::awaiting_peer_eof);
    EXPECT_TRUE(!connection.next_event());
    EXPECT_EQ(input.size(), std::size_t{0});
    connection.notify_transport_eof();
    const auto end = connection.next_event();
    EXPECT_TRUE(end && end->transport_end());
    EXPECT_TRUE(connection.output_plan().disposition() == websocket_transport_disposition::end_transport);
    connection.commit_transport_end();
    EXPECT_TRUE(connection.liveness_mode() == websocket_liveness_mode::inactive);
}

// A borrowed payload is a distinct parameter form: it never binds an owning
// temporary, and a plain string never borrows implicitly.
static_assert(!std::is_constructible_v<websocket_borrowed_payload, std::string>);
static_assert(!std::is_constructible_v<websocket_borrowed_payload, std::string>);
static_assert(std::is_constructible_v<websocket_borrowed_payload, const std::string&>);
static_assert(!std::is_convertible_v<std::string_view, websocket_borrowed_payload>);

// RFC 6455 section 5.4: one entry sends whole messages and fragments; a frame
// is the final fragment of its message.
TEST(Ruvia, ws_public_submit_frame_sends_fragments_and_whole_messages) {
    http_io_buffer input;
    websocket_connection sender(input);
    EXPECT_EQ(sender.submit_frame(websocket_opcode::text, "ab", {.final_ = false}), websocket_frame_submit_status::accepted);
    EXPECT_EQ(sender.submit_frame(websocket_opcode::binary, "x"), websocket_frame_submit_status::message_in_progress);
    // Control frames are never fragmented (section 5.5) and may interleave.
    EXPECT_EQ(sender.submit_frame(websocket_opcode::ping, "p", {.final_ = false}), websocket_frame_submit_status::invalid_opcode);
    EXPECT_EQ(sender.submit_frame(websocket_opcode::ping, "p"), websocket_frame_submit_status::accepted);
    EXPECT_EQ(sender.submit_frame(websocket_opcode::text, "\xC3", {.final_ = false}), websocket_frame_submit_status::accepted);
    const std::string tail(
        "\xA9"
        "cd");
    EXPECT_EQ(sender.submit_frame(websocket_opcode::text, websocket_borrowed_payload(tail)), websocket_frame_submit_status::accepted);
    EXPECT_EQ(sender.output_plan().borrowed_bytes(), tail);
    // The next frame copies the borrowed payload ahead of itself.
    EXPECT_EQ(sender.submit_frame(websocket_opcode::binary, "z"), websocket_frame_submit_status::accepted);
    EXPECT_TRUE(sender.output_plan().borrowed_bytes().empty());
    const std::string_view expected(
        "\x01\x02"
        "ab"
        "\x89\x01"
        "p"
        "\x00\x01"
        "\xC3"
        "\x80\x03"
        "\xA9"
        "cd"
        "\x82\x01"
        "z",
        18);
    const auto wire = drain(sender);
    EXPECT_EQ(std::string_view(wire), expected);

    mask_source source;
    http_io_buffer client_input;
    auto receiver = client(client_input, source);
    client_input.append(wire);
    const auto ping = receiver.next_event();
    EXPECT_TRUE(ping && ping->ping() && ping->ping()->payload() == "p");
    const auto text = receiver.next_event();
    EXPECT_TRUE(text && text->message() && text->message()->payload() ==
                                               "ab\xC3\xA9"
                                               "cd");
    const auto binary = receiver.next_event();
    EXPECT_TRUE(binary && binary->message() && binary->message()->payload() == "z");
}

TEST(Ruvia, ws_public_client_rejects_masked_server_and_oversized_message) {
    mask_source source;
    http_io_buffer invalid_server_input;
    auto invalid_server = client(invalid_server_input, source);
    EXPECT_TRUE(invalid_server.submit_frame(websocket_opcode::text, "bad") == websocket_frame_submit_status::accepted);
    const auto masked = drain(invalid_server);
    http_io_buffer receiver_input;
    auto receiver = client(receiver_input, source);
    receiver_input.append(masked);
    const auto error = receiver.next_event();
    EXPECT_TRUE(error && error->protocol_error() && error->protocol_error()->close_code() == 1002);
    http_io_buffer limited_input;
    auto limited = client(limited_input, source, protocol_byte_limit::limited(2));
    limited_input.append(std::string_view(
        "\x82\x03"
        "abc",
        5));
    const auto large = limited.next_event();
    EXPECT_TRUE(large && large->protocol_error() && large->protocol_error()->close_code() == 1009);
    EXPECT_TRUE(limited.submit_frame(websocket_opcode::binary, "x") == websocket_frame_submit_status::not_open);
}

TEST(Ruvia, ws_public_client_requires_entropy_and_handles_transport_end) {
    EXPECT_TRUE(ruvia::test::throws_on([] {
        http_io_buffer input;
        websocket_connection connection(input, {.role_ = websocket_connection_role::client});
    }));
    EXPECT_TRUE(ruvia::test::throws_on([] {
        http_io_buffer input;
        websocket_connection connection(input, {.role_ = static_cast<websocket_connection_role>(255)});
    }));
    mask_source source;
    http_io_buffer input;
    auto connection = client(input, source);
    source.fail_ = true;
    EXPECT_TRUE(ruvia::test::throws_on([&] {
        (void)connection.submit_frame(websocket_opcode::binary, "x");
    }));
    EXPECT_TRUE(connection.output_plan().bytes().empty());
    EXPECT_TRUE(connection.abort() == websocket_abort_disposition::abort_transport);
    EXPECT_TRUE(connection.abort() == websocket_abort_disposition::no_transport_action);
    http_io_buffer eof_input;
    auto eof = client(eof_input, source);
    eof.notify_transport_eof();
    const auto event = eof.next_event();
    EXPECT_TRUE(event && event->transport_end());
    eof.commit_transport_end();
    EXPECT_TRUE(eof.liveness_mode() == websocket_liveness_mode::inactive);
}

TEST(Ruvia, ws_public_wire_bytes_count_whole_frames_and_close_roles_fix_termination) {
    mask_source source;
    http_io_buffer client_input;
    auto client_side = client(client_input, source);
    http_io_buffer server_input;
    websocket_connection server(server_input);
    EXPECT_TRUE(client_side.termination() == websocket_termination::none);
    EXPECT_TRUE(server.termination() == websocket_termination::none);

    // Masked text "hello": 2-byte header, 4-byte mask key and 5 payload bytes.
    EXPECT_TRUE(client_side.submit_frame(websocket_opcode::text, "hello") == websocket_frame_submit_status::accepted);
    EXPECT_EQ(client_side.bytes_sent(), 0U);
    const auto text = drain(client_side);
    EXPECT_EQ(text.size(), 11U);
    EXPECT_EQ(client_side.bytes_sent(), 11U);
    // A frame counts once it is parsed whole.
    server_input.append(std::string_view(text).substr(0, 4));
    EXPECT_TRUE(!server.next_event());
    EXPECT_EQ(server.bytes_received(), 0U);
    server_input.append(std::string_view(text).substr(4));
    const auto message = server.next_event();
    EXPECT_TRUE(message && message->message() && message->message()->payload() == "hello");
    EXPECT_EQ(server.bytes_received(), 11U);

    // Ping "abc" (5 bytes) and the client's automatic masked Pong (9 bytes).
    EXPECT_TRUE(server.submit_frame(websocket_opcode::ping, "abc") == websocket_frame_submit_status::accepted);
    client_input.append(drain(server));
    const auto ping = client_side.next_event();
    EXPECT_TRUE(ping && ping->ping());
    server_input.append(drain(client_side));
    const auto pong = server.next_event();
    EXPECT_TRUE(pong && pong->pong() && pong->pong()->payload() == "abc");

    // Client-initiated Close 1000 (8 bytes), then the server echo (4 bytes).
    EXPECT_TRUE(client_side.submit_close(websocket_close_status::normal_closure, {}) == websocket_close_submit_status::accepted);
    server_input.append(drain(client_side));
    const auto close = server.next_event();
    EXPECT_TRUE(close && close->close() && close->close()->close_code() == 1000);
    EXPECT_TRUE(server.termination() == websocket_termination::peer_close);
    EXPECT_TRUE(client_side.termination() == websocket_termination::none);
    client_input.append(drain(server));
    const auto echoed = client_side.next_event();
    EXPECT_TRUE(echoed && echoed->close());
    EXPECT_TRUE(client_side.termination() == websocket_termination::local_close);

    EXPECT_EQ(client_side.bytes_sent(), 11U + 9U + 8U);
    EXPECT_EQ(server.bytes_received(), client_side.bytes_sent());
    EXPECT_EQ(server.bytes_sent(), 5U + 4U);
    EXPECT_EQ(client_side.bytes_received(), server.bytes_sent());

    // The first cause survives the transport end and any later abort.
    server.commit_transport_end();
    EXPECT_TRUE(server.abort(websocket_termination::io_error) == websocket_abort_disposition::no_transport_action);
    EXPECT_TRUE(server.termination() == websocket_termination::peer_close);
    client_side.notify_transport_eof();
    EXPECT_TRUE(client_side.abort(websocket_termination::timeout) == websocket_abort_disposition::abort_transport);
    EXPECT_TRUE(client_side.termination() == websocket_termination::local_close);
}

TEST(Ruvia, ws_public_termination_reports_the_first_failure_cause) {
    // An unmasked client frame fails as 1002 and is not counted as received.
    http_io_buffer unmasked_input;
    websocket_connection unmasked(unmasked_input);
    unmasked_input.append(std::string_view("\x81\x01x", 3));
    const auto unmasked_error = unmasked.next_event();
    EXPECT_TRUE(unmasked_error && unmasked_error->protocol_error());
    EXPECT_TRUE(unmasked.termination() == websocket_termination::protocol_error);
    EXPECT_EQ(unmasked.bytes_received(), 0U);
    unmasked.notify_transport_eof();
    EXPECT_TRUE(unmasked.termination() == websocket_termination::protocol_error);

    mask_source source;
    http_io_buffer utf8_input;
    auto utf8 = client(utf8_input, source);
    utf8_input.append(std::string_view("\x81\x01\xff", 3));
    const auto utf8_error = utf8.next_event();
    EXPECT_TRUE(utf8_error && utf8_error->protocol_error() && utf8_error->protocol_error()->close_code() == 1007);
    EXPECT_TRUE(utf8.termination() == websocket_termination::invalid_payload_data);

    http_io_buffer limited_input;
    auto limited = client(limited_input, source, protocol_byte_limit::limited(2));
    limited_input.append(std::string_view(
        "\x82\x03"
        "abc",
        5));
    const auto large = limited.next_event();
    EXPECT_TRUE(large && large->protocol_error() && large->protocol_error()->close_code() == 1009);
    EXPECT_TRUE(limited.termination() == websocket_termination::message_too_large);

    http_io_buffer eof_input;
    auto eof = client(eof_input, source);
    eof.notify_transport_eof();
    EXPECT_TRUE(eof.termination() == websocket_termination::eof);
    (void)eof.abort(websocket_termination::timeout);
    EXPECT_TRUE(eof.termination() == websocket_termination::eof);

    for (const auto cause : {websocket_termination::none, websocket_termination::aborted,
             websocket_termination::timeout, websocket_termination::io_error}) {
        http_io_buffer input;
        websocket_connection connection(input);
        EXPECT_TRUE(connection.abort(cause) == websocket_abort_disposition::abort_transport);
        connection.notify_transport_eof();
        EXPECT_TRUE(connection.termination() == (cause == websocket_termination::none ? websocket_termination::aborted : cause));
    }
}

TEST(Ruvia, ws_public_wire_bytes_count_deflated_payloads) {
    mask_source source;
    const websocket_compression compression{.enabled_ = true};
    http_io_buffer server_input;
    websocket_connection server(server_input, {.compression_ = compression});
    http_io_buffer client_input;
    websocket_connection client_side(client_input, {.compression_ = compression, .role_ = websocket_connection_role::client, .mask_key_generator_ = &mask_source::generate, .mask_key_context_ = &source});
    const std::string payload_value(4096, 'x');
    EXPECT_TRUE(server.submit_frame(websocket_opcode::text, payload_value) == websocket_frame_submit_status::accepted);
    const auto wire = drain(server);
    EXPECT_TRUE(wire.size() < payload_value.size());
    EXPECT_EQ(server.bytes_sent(), wire.size());
    client_input.append(wire);
    const auto message = client_side.next_event();
    EXPECT_TRUE(message && message->message() && message->message()->payload() == payload_value);
    EXPECT_EQ(client_side.bytes_received(), wire.size());
}

TEST(Ruvia, ws_default_message_limit_rejects_oversized_declared_frame_before_payload) {
    // A masked binary frame declaring 16 MiB + 1 bytes requires no payload to
    // establish that it cannot fit the finite default message policy.
    const std::string header_value("\x82\xff\x00\x00\x00\x00\x01\x00\x00\x01\x00\x00\x00\x00", 14);
    websocket_connection connection;
    EXPECT_TRUE(connection.feed(header_value) == websocket_feed_status::accepted);
    const auto event = connection.next_event();
    EXPECT_TRUE(event && event->protocol_error());
    EXPECT_EQ(event->protocol_error()->close_code(), std::uint16_t{1009});
    http_io_buffer input;
    input.append(header_value);
    websocket_connection protocol(input);
    const auto borrowed_event = protocol.next_event();
    EXPECT_TRUE(borrowed_event && borrowed_event->protocol_error());
    EXPECT_EQ(borrowed_event->protocol_error()->close_code(), std::uint16_t{1009});
}

TEST(Ruvia, ws_owned_input_bound_excludes_parsed_prefix) {
    websocket_connection connection({.max_buffered_input_bytes_ = 32});
    const std::string ping("\x89\x81\x00\x00\x00\x00p", 7);
    std::string binary("\x82\x94\x00\x00\x00\x00", 6);
    binary.append(20, 'b');
    const auto first_chunk = ping + binary.substr(0, 10);
    EXPECT_TRUE(connection.feed(first_chunk) == websocket_feed_status::accepted);
    const auto ping_event = connection.next_event();
    EXPECT_TRUE(ping_event && ping_event->ping());
    (void)connection.consume_output(connection.output_plan().bytes().size());
    EXPECT_TRUE(!connection.next_event());
    // 16 pending + 10 buffered frame bytes fit the 32-byte bound once the
    // already-delivered Ping no longer occupies the buffer.
    EXPECT_TRUE(connection.feed(std::string_view(binary).substr(10)) == websocket_feed_status::accepted);
    const auto message_event = connection.next_event();
    EXPECT_TRUE(message_event && message_event->message());
    if (message_event && message_event->message()) {
        EXPECT_EQ(message_event->message()->payload(), std::string(20, 'b'));
    }
}

TEST(Ruvia, ws_owned_input_accepts_message_payload_during_compaction) {
    for (const bool following_frame : {false, true}) {
        const std::string nested_frame("\x82\x01x", 3);
        std::string wire("\x82\x03", 2);
        wire += nested_frame;
        if (following_frame) {
            wire.append(
                "\x82\x08"
                "abcdefgh",
                10);
        }
        mask_source mask;
        websocket_connection connection({.role_ = websocket_connection_role::client,
            .mask_key_generator_ = &mask_source::generate,
            .mask_key_context_ = &mask,
            .max_buffered_input_bytes_ = wire.size()});
        EXPECT_TRUE(connection.feed(wire) == websocket_feed_status::accepted);
        const auto wrapper = connection.next_event();
        EXPECT_TRUE(wrapper && wrapper->message());
        if (!wrapper || !wrapper->message()) {
            return;
        }
        EXPECT_EQ(wrapper->message()->payload(), nested_frame);

        // Feed bytes borrowed from the completed message while any following
        // frame is still buffered. The input bound requires reclaiming the
        // consumed wrapper before accepting the nested frame.
        EXPECT_TRUE(connection.feed(wrapper->message()->payload()) == websocket_feed_status::accepted);
        if (following_frame) {
            const auto following = connection.next_event();
            EXPECT_TRUE(following && following->message());
            if (!following || !following->message()) {
                return;
            }
            EXPECT_EQ(following->message()->payload(), "abcdefgh");
        }
        const auto nested = connection.next_event();
        EXPECT_TRUE(nested && nested->message());
        if (nested && nested->message()) {
            EXPECT_EQ(nested->message()->payload(), "x");
        }
        EXPECT_TRUE(!connection.next_event());
        EXPECT_TRUE(connection.liveness_mode() == websocket_liveness_mode::open);
    }
}

TEST(Ruvia, ws_input_accepts_borrowed_message_payload_during_compaction) {
    for (const bool following_frame : {false, true}) {
        const std::string nested_frame("\x82\x01x", 3);
        std::string wire("\x82\x03", 2);
        wire += nested_frame;
        if (following_frame) {
            wire.append(
                "\x82\x08"
                "abcdefgh",
                10);
        }
        mask_source mask;
        http_io_buffer input;
        websocket_connection connection(input,
            {.role_ = websocket_connection_role::client,
                .mask_key_generator_ = &mask_source::generate,
                .mask_key_context_ = &mask});
        input.append(wire);
        const auto wrapper = connection.next_event();
        EXPECT_TRUE(wrapper && wrapper->message());
        if (!wrapper || !wrapper->message()) {
            return;
        }
        EXPECT_EQ(wrapper->message()->payload(), nested_frame);

        // Append bytes borrowed from the completed message while any following
        // frame is still buffered. Making room must copy them before the
        // consumed wrapper is reclaimed.
        input.append(wrapper->message()->payload());
        if (following_frame) {
            const auto following = connection.next_event();
            EXPECT_TRUE(following && following->message());
            if (!following || !following->message()) {
                return;
            }
            EXPECT_EQ(following->message()->payload(), "abcdefgh");
        }
        const auto nested = connection.next_event();
        EXPECT_TRUE(nested && nested->message());
        if (nested && nested->message()) {
            EXPECT_EQ(nested->message()->payload(), "x");
        }
        EXPECT_TRUE(!connection.next_event());
        EXPECT_TRUE(connection.liveness_mode() == websocket_liveness_mode::open);
    }
}

TEST(Ruvia, ws_public_masking_round_trips_over_odd_lengths_and_offsets) {
    mask_source mask;
    http_io_buffer sender_input;
    auto sender = client(sender_input, mask, protocol_byte_limit::unlimited());
    http_io_buffer receiver_input;
    websocket_connection receiver(receiver_input, {.message_limit_ = protocol_byte_limit::unlimited()});
    for (const std::size_t length : {std::size_t{0}, std::size_t{1}, std::size_t{2}, std::size_t{3}, std::size_t{4},
             std::size_t{5}, std::size_t{7}, std::size_t{8}, std::size_t{9}, std::size_t{15}, std::size_t{16},
             std::size_t{17}, std::size_t{31}, std::size_t{33}, std::size_t{125}, std::size_t{126},
             std::size_t{127}, std::size_t{1021}, std::size_t{65535}, std::size_t{65537}}) {
        std::string payload_value(length, '\0');
        for (std::size_t i = 0; i < length; ++i) {
            payload_value[i] = static_cast<char>((i * 31U + length) & 0xFFU);
        }
        // A leading frame of `offset` bytes moves where the payload lands in
        // the receiver's input, so every alignment is unmasked in place.
        for (std::size_t offset = 0; offset < 8; ++offset) {
            const std::string leading(offset, 'L');
            EXPECT_TRUE(sender.submit_frame(websocket_opcode::binary, leading) == websocket_frame_submit_status::accepted);
            const auto leading_wire = drain(sender);
            EXPECT_TRUE(sender.submit_frame(websocket_opcode::binary, payload_value) == websocket_frame_submit_status::accepted);
            const auto wire = drain(sender);

            // Check the wire against RFC 6455 section 5.3 directly: a mask that
            // is only self-inverse would still round-trip.
            const std::size_t header_size = 2 + (length <= 125 ? 0 : (length <= 0xFFFF ? 2 : 8));
            EXPECT_EQ(wire.size(), header_size + 4 + length);
            if (wire.size() != header_size + 4 + length) {
                return;
            }
            const auto key = std::string_view(wire).substr(header_size, 4);
            bool masked_on_wire = true;
            for (std::size_t i = 0; i < length; ++i) {
                masked_on_wire = masked_on_wire && wire[header_size + 4 + i] == static_cast<char>(payload_value[i] ^ key[i % 4]);
            }
            EXPECT_TRUE(masked_on_wire);

            const auto frames = leading_wire + wire;
            receiver_input.append(frames);
            const auto leading_event = receiver.next_event();
            EXPECT_TRUE(leading_event && leading_event->message());
            if (leading_event && leading_event->message()) {
                EXPECT_EQ(leading_event->message()->payload(), leading);
            }
            const auto event = receiver.next_event();
            EXPECT_TRUE(event && event->message());
            if (event && event->message()) {
                EXPECT_TRUE(event->message()->payload() == payload_value);
            }
        }
    }
}

TEST(Ruvia, ws_server_borrowed_frame_follows_pending_output_until_drained) {
    http_io_buffer input;
    websocket_connection protocol(input, {.message_limit_ = protocol_byte_limit::unlimited()});
    const std::string payload_value(300, 'b');
    EXPECT_TRUE(protocol.submit_frame(websocket_opcode::binary, websocket_borrowed_payload(payload_value)) == websocket_frame_submit_status::accepted);
    auto plan = protocol.output_plan();
    EXPECT_EQ(plan.bytes().size(), std::size_t{4});
    EXPECT_TRUE(plan.borrowed_bytes().data() == payload_value.data());
    EXPECT_EQ(plan.size(), std::size_t{304});

    // A partial write may end inside the borrowed payload.
    EXPECT_TRUE(protocol.consume_output(104) == websocket_output_consume_status::pending);
    plan = protocol.output_plan();
    EXPECT_TRUE(plan.bytes().empty());
    EXPECT_TRUE(plan.borrowed_bytes().data() == payload_value.data() + 100);
    EXPECT_EQ(plan.borrowed_bytes().size(), std::size_t{200});
    EXPECT_TRUE(protocol.consume_output(201) == websocket_output_consume_status::out_of_range);
    EXPECT_TRUE(protocol.consume_output(200) == websocket_output_consume_status::drained);
    EXPECT_EQ(protocol.output_plan().size(), std::size_t{0});

    // Output queued behind a borrowed payload copies it first, so the wire
    // keeps submission order and no longer depends on the caller's bytes.
    std::string message_value("hello");
    EXPECT_TRUE(protocol.submit_frame(websocket_opcode::text, websocket_borrowed_payload(message_value)) == websocket_frame_submit_status::accepted);
    mask_source mask;
    http_io_buffer peer_input;
    auto peer = client(peer_input, mask);
    EXPECT_TRUE(peer.submit_frame(websocket_opcode::ping, "p") == websocket_frame_submit_status::accepted);
    input.append(drain(peer));
    const auto ping = protocol.next_event();
    EXPECT_TRUE(ping && ping->ping());
    message_value[0] = 'X';
    plan = protocol.output_plan();
    EXPECT_TRUE(plan.borrowed_bytes().empty());
    const std::string wire(plan.bytes());
    EXPECT_TRUE(protocol.consume_output(wire.size()) == websocket_output_consume_status::drained);
    peer_input.append(wire);
    const auto message = peer.next_event();
    EXPECT_TRUE(message && message->message());
    if (message && message->message()) {
        EXPECT_EQ(message->message()->payload(), "hello");
    }
    const auto pong = peer.next_event();
    EXPECT_TRUE(pong && pong->pong());
    if (pong && pong->pong()) {
        EXPECT_EQ(pong->pong()->payload(), "p");
    }
}

TEST(Ruvia, ws_server_borrowed_frame_owns_compressed_output) {
    http_io_buffer input;
    websocket_connection protocol(input, {.message_limit_ = protocol_byte_limit::unlimited(),
                                             .compression_ = websocket_compression{.enabled_ = true}});
    const std::string payload_value(4096, 'x');
    EXPECT_TRUE(protocol.submit_frame(websocket_opcode::binary, websocket_borrowed_payload(payload_value)) == websocket_frame_submit_status::accepted);
    const auto plan = protocol.output_plan();
    EXPECT_TRUE(plan.borrowed_bytes().empty());
    EXPECT_TRUE(!plan.bytes().empty() && (static_cast<unsigned char>(plan.bytes()[0]) & 0x40U) != 0);
    EXPECT_TRUE(protocol.submit_frame(websocket_opcode::binary, websocket_borrowed_payload(payload_value), {.compress_ = false}) == websocket_frame_submit_status::accepted);
    EXPECT_TRUE(protocol.output_plan().borrowed_bytes().data() == payload_value.data());
}

TEST(Ruvia, ws_public_eight_bit_receive_window_accepts_zlib_nine_bit_deflater_streams) {
    // zlib has no 8-bit raw deflate window, so a zlib peer that negotiated
    // window 8 compresses with window 9. Its deflater still never refers back
    // more than 250 bytes, which the 256-byte inflate window covers. The
    // inflater consults that window only for history produced by earlier
    // output passes, so the incompressible prefix ends just past the 8 KiB
    // inflate pass and the 300-byte repeat refers back across that boundary.
    std::string message(8192 + 10, '\0');
    std::uint64_t random = 0x9e3779b97f4a7c15ULL;
    for (auto& byte : message) {
        random ^= random << 13;
        random ^= random >> 7;
        random ^= random << 17;
        byte = static_cast<char>(random);
    }
    message += message.substr(message.size() - 300, 300);
    z_stream stream{};
    EXPECT_TRUE(deflateInit2(&stream, 6, Z_DEFLATED, -9, 8, Z_DEFAULT_STRATEGY) == Z_OK);
    std::string compressed(message.size() + 256, '\0');
    stream.next_in = reinterpret_cast<Bytef*>(message.data());
    stream.avail_in = static_cast<uInt>(message.size());
    stream.next_out = reinterpret_cast<Bytef*>(compressed.data());
    stream.avail_out = static_cast<uInt>(compressed.size());
    EXPECT_TRUE(deflate(&stream, Z_SYNC_FLUSH) == Z_OK);
    compressed.resize(compressed.size() - stream.avail_out);
    deflateEnd(&stream);
    EXPECT_TRUE(compressed.size() >= 4 && compressed.size() < 65536);
    compressed.resize(compressed.size() - 4);

    for (const bool no_takeover : {false, true}) {
        mask_source mask;
        http_io_buffer input;
        websocket_connection receiver(input, {.compression_ = (websocket_compression{.enabled_ = true, .server_no_context_takeover_ = no_takeover, .server_max_window_bits_ = 8}), .role_ = websocket_connection_role::client, .mask_key_generator_ = &mask_source::generate, .mask_key_context_ = &mask});
        std::string wire("\xc2\x7e", 2);
        wire.push_back(static_cast<char>(compressed.size() >> 8));
        wire.push_back(static_cast<char>(compressed.size() & 0xff));
        wire += compressed;
        input.append(wire);
        const auto event = receiver.next_event();
        EXPECT_TRUE(event && event->message() && event->message()->payload() == message);
    }
}

TEST(Ruvia, ws_public_receiver_accepts_non_minimal_payload_length_forms) {
    // RFC 6455 section 5.2 makes the minimal length form a sender obligation;
    // the longer forms still delimit the frame exactly, so a receiver reads
    // them instead of failing the connection.
    const std::string sixteen_bit("\x81\xfe\x00\x05\x00\x00\x00\x00hello", 13);
    const std::string sixty_four_bit("\x81\xff\x00\x00\x00\x00\x00\x00\x00\x05\x00\x00\x00\x00hello", 19);
    const std::string control_sixteen_bit("\x89\xfe\x00\x02\x00\x00\x00\x00hi", 10);
    for (const auto& wire : {sixteen_bit, sixty_four_bit}) {
        http_io_buffer input;
        websocket_connection server(input);
        input.append(wire);
        const auto event = server.next_event();
        EXPECT_TRUE(event && event->message() && event->message()->payload() == "hello");
    }
    http_io_buffer input;
    websocket_connection server(input);
    input.append(control_sixteen_bit);
    const auto event = server.next_event();
    EXPECT_TRUE(event && event->ping() && event->ping()->payload() == "hi");
}

TEST(Ruvia, ws_public_feed_reports_backpressure_when_initial_buffer_exceeds_limit) {
    http_io_buffer input;
    input.append("123456789");
    websocket_connection connection(input, {.max_buffered_input_bytes_ = 8});
    EXPECT_TRUE(connection.feed("x") == websocket_feed_status::backpressured);
    EXPECT_EQ(input.size(), std::size_t{9});
}

TEST(Ruvia, ws_receive_ceiling_is_early_directional_and_only_tightens) {
    for (const std::size_t receive_limit : {std::size_t{256}, std::size_t{2048}}) {
        const auto effective = (std::min)(receive_limit, std::size_t{1024});
        websocket_connection receiver({.message_limit_ = protocol_byte_limit::limited(1024),
            .receive_message_limit_ = protocol_byte_limit::limited(receive_limit)});
        EXPECT_EQ(receiver.submit_frame(websocket_opcode::binary, std::string(1024, 'r'), {.compress_ = false}),
            websocket_frame_submit_status::accepted);
        (void)drain(receiver);
        EXPECT_EQ(receiver.submit_frame(websocket_opcode::binary, std::string(1025, 'r'), {.compress_ = false}),
            websocket_frame_submit_status::message_too_large);
        // 仅发送帧头，无正文：在分配或等待完整消息前拒绝声明的超限长度。
        std::string header("\x82\xfe", 2);
        header.push_back(static_cast<char>((effective + 1) >> 8));
        header.push_back(static_cast<char>((effective + 1) & 0xff));
        header.append(4, '\0');
        EXPECT_EQ(receiver.feed(header), websocket_feed_status::accepted);
        const auto failure = receiver.next_event();
        ASSERT_TRUE(failure && failure->protocol_error());
        EXPECT_EQ(failure->protocol_error()->close_code(), 1009);
    }
    // 各分片单独合法，累计消息长度仍受同一个接收上限约束。
    websocket_connection receiver({.receive_message_limit_ = protocol_byte_limit::limited(4)});
    const std::string first("\x02\x83\0\0\0\0abc", 9);
    const std::string last("\x80\x82\0\0\0\0de", 8);
    EXPECT_EQ(receiver.feed(first), websocket_feed_status::accepted);
    EXPECT_FALSE(receiver.next_event());
    EXPECT_EQ(receiver.feed(last), websocket_feed_status::accepted);
    const auto failure = receiver.next_event();
    ASSERT_TRUE(failure && failure->protocol_error());
    EXPECT_EQ(failure->protocol_error()->close_code(), 1009);
}

TEST(Ruvia, ws_fragmented_deflate_enforces_uncompressed_message_limit) {
    {
        websocket_connection sender({.message_limit_ = protocol_byte_limit::limited(1000),
            .compression_ = websocket_compression{.enabled_ = true}});
        const std::string first_chunk(600, 'a');
        const std::string second_chunk(500, 'a');
        EXPECT_EQ(sender.submit_frame(websocket_opcode::binary, first_chunk, {.final_ = false, .compress_ = true}),
            websocket_frame_submit_status::accepted);
        EXPECT_EQ(sender.submit_frame(websocket_opcode::binary, second_chunk, {.final_ = true, .compress_ = true}),
            websocket_frame_submit_status::message_too_large);
    }
    {
        websocket_connection sender({.message_limit_ = protocol_byte_limit::limited(64),
            .compression_ = websocket_compression{.enabled_ = true}});
        std::string incompressible(64, '\0');
        std::uint64_t rng = 0x517cc1b727220a95ULL;
        for (char& ch : incompressible) {
            rng ^= rng << 13U;
            rng ^= rng >> 7U;
            rng ^= rng << 17U;
            ch = static_cast<char>(rng & 0xffU);
        }
        const std::string_view view(incompressible);
        EXPECT_EQ(sender.submit_frame(websocket_opcode::binary, view.substr(0, 32), {.final_ = false, .compress_ = true}),
            websocket_frame_submit_status::accepted);
        EXPECT_EQ(sender.submit_frame(websocket_opcode::binary, view.substr(32, 32), {.final_ = true, .compress_ = true}),
            websocket_frame_submit_status::accepted);
    }
}
