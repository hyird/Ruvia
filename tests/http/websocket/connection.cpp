#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/http/protocol_byte_limit.h"

#include "test_harness.h"
#include "websocket/ws_connection.h"

namespace {

using ruvia::protocol_byte_limit;
using ruvia::websocket_abort_disposition;
using ruvia::websocket_close_event;
using ruvia::websocket_close_submit_status;
using ruvia::websocket_compression;
using ruvia::websocket_connection_role;
using ruvia::websocket_event;
using ruvia::websocket_event_kind;
using ruvia::websocket_frame_submit_status;
using ruvia::websocket_liveness_mode;
using ruvia::websocket_message_event;
using ruvia::websocket_opcode;
using ruvia::websocket_output_consume_status;
using ruvia::websocket_protocol_error_event;
using ruvia::websocket_transport_disposition;
using ruvia::detail::ws_connection;

class toggle_allocation_resource final : public std::pmr::memory_resource {
public:
    void reject(bool value = true) noexcept {
        reject_ = value;
    }

private:
    void* do_allocate(std::size_t bytes_value, std::size_t alignment) override {
        if (reject_) {
            throw std::bad_alloc();
        }
        return std::pmr::new_delete_resource()->allocate(bytes_value, alignment);
    }

    void do_deallocate(void* pointer, std::size_t bytes_value, std::size_t alignment) override {
        std::pmr::new_delete_resource()->deallocate(pointer, bytes_value, alignment);
    }

    [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }

    bool reject_{false};
};

// Build a masked client->server frame (RFC 6455 §5.1): FIN/opcode byte, MASK bit + a
// short length (<=125 for tests), a 4-byte mask, then the masked payload.
std::pmr::string masked_frame(std::pmr::memory_resource* res, std::uint8_t opcode,
    std::string_view payload_value, bool fin = true, bool rsv1 = false) {
    std::pmr::string f(res);
    f.push_back(static_cast<char>((fin ? 0x80U : 0U) | (rsv1 ? 0x40U : 0U) | opcode));
    f.push_back(static_cast<char>(0x80U | static_cast<std::uint8_t>(payload_value.size())));
    const unsigned char mask[4] = {0x11, 0x22, 0x33, 0x44};
    f.append(reinterpret_cast<const char*>(mask), 4);
    for (std::size_t i = 0; i < payload_value.size(); ++i) {
        f.push_back(static_cast<char>(static_cast<unsigned char>(payload_value[i]) ^ mask[i % 4]));
    }
    return f;
}

std::optional<websocket_event> poll_bytes(
    ws_connection& connection, std::pmr::string& input, std::string_view bytes_value) {
    input.append(bytes_value.data(), bytes_value.size());
    return connection.poll();
}

bool fixed_mask(void*, ruvia::websocket_mask_key_type& key) noexcept {
    key = {'\x11', '\x22', '\x33', '\x44'};
    return true;
}

std::pmr::string unmasked_frame(
    std::pmr::memory_resource* resource, std::uint8_t opcode, std::string_view payload_value) {
    std::pmr::string frame(resource);
    frame.push_back(static_cast<char>(0x80U | opcode));
    frame.push_back(static_cast<char>(payload_value.size()));
    frame.append(payload_value);
    return frame;
}

}  // namespace

RUVIA_TEST(ws_client_role_masks_outbound_and_accepts_unmasked_server_frames) {
    std::pmr::monotonic_buffer_resource resource;
    std::pmr::string input(&resource);
    ws_connection client(input, protocol_byte_limit::limited(1024), (websocket_compression{}),
        websocket_connection_role::client, &fixed_mask, nullptr);

    RUVIA_CHECK(client.submit_frame(websocket_opcode::text, "hi") == websocket_frame_submit_status::accepted);
    const auto output = client.output_plan().bytes();
    RUVIA_CHECK_EQ(output.size(), std::size_t{8});
    RUVIA_CHECK_EQ(static_cast<unsigned char>(output[0]), static_cast<unsigned char>(0x81));
    RUVIA_CHECK_EQ(static_cast<unsigned char>(output[1]), static_cast<unsigned char>(0x82));
    RUVIA_CHECK_EQ(output.substr(2, 4), std::string_view("\x11\x22\x33\x44", 4));
    RUVIA_CHECK_EQ(static_cast<unsigned char>(output[6]), static_cast<unsigned char>('h' ^ 0x11));
    RUVIA_CHECK_EQ(static_cast<unsigned char>(output[7]), static_cast<unsigned char>('i' ^ 0x22));

    const auto server_frame = unmasked_frame(&resource, 0x1, "ok");
    const auto event = poll_bytes(client, input, server_frame);
    RUVIA_CHECK(event.has_value());
    RUVIA_CHECK(event->message() != nullptr);
    RUVIA_CHECK_EQ(event->message()->payload(), std::string_view("ok"));
}

RUVIA_TEST(ws_client_role_rejects_masked_server_frames) {
    std::pmr::monotonic_buffer_resource resource;
    std::pmr::string input(&resource);
    ws_connection client(input, protocol_byte_limit::limited(1024), (websocket_compression{}),
        websocket_connection_role::client, &fixed_mask, nullptr);
    const auto server_violation = masked_frame(&resource, 0x1, "bad");
    const auto event = poll_bytes(client, input, server_violation);
    RUVIA_CHECK(event.has_value());
    RUVIA_CHECK(event->protocol_error() != nullptr);
    RUVIA_CHECK_EQ(event->protocol_error()->close_code(), std::uint16_t{1002});
    RUVIA_CHECK((static_cast<unsigned char>(client.output_plan().bytes()[1]) & 0x80U) != 0);
}

// A single masked Text frame is delivered as one complete typed event, unmasked.
RUVIA_TEST(ws_connection_event_is_optional_and_discriminated) {
    std::pmr::monotonic_buffer_resource resource;
    std::pmr::string input(&resource);
    ws_connection conn(input);

    const auto f = masked_frame(&resource, 0x1, "hi");
    const auto e = poll_bytes(conn, input, std::string_view(f.data(), f.size()));
    RUVIA_CHECK(e.has_value());
    RUVIA_CHECK(e->kind() == websocket_event_kind::message);
    RUVIA_CHECK(e->ping() == nullptr);
    RUVIA_CHECK(e->close() == nullptr);
    const auto* message = e->message();
    RUVIA_CHECK(message != nullptr);
    RUVIA_CHECK(message->opcode() == websocket_opcode::text);
    RUVIA_CHECK(message->payload() == std::string_view("hi"));
    RUVIA_CHECK(message->payload().data() == input.data() + 6);  // header + mask, no copy
    RUVIA_CHECK(!conn.poll().has_value());
}

// A Ping is surfaced as ping AND auto-answered with an unmasked Pong echoing the data.
RUVIA_TEST(ws_connection_auto_pongs_ping) {
    std::pmr::monotonic_buffer_resource resource;
    std::pmr::string input(&resource);
    ws_connection conn(input);

    const auto f = masked_frame(&resource, 0x9, "pp");
    const auto e = poll_bytes(conn, input, std::string_view(f.data(), f.size()));
    RUVIA_CHECK(e->kind() == websocket_event_kind::ping);
    RUVIA_CHECK(e->ping()->payload() == std::string_view("pp"));
    RUVIA_CHECK(e->message() == nullptr);

    const auto plan = conn.output_plan();
    const auto out = plan.bytes();
    RUVIA_CHECK(plan.disposition() == websocket_transport_disposition::keep_open);
    RUVIA_CHECK_EQ(out.size(), static_cast<std::size_t>(4));
    RUVIA_CHECK_EQ(
        static_cast<unsigned char>(out[0]), static_cast<unsigned char>(0x8A));  // FIN|Pong
    RUVIA_CHECK_EQ(
        static_cast<unsigned char>(out[1]), static_cast<unsigned char>(2));  // unmasked len
    RUVIA_CHECK(out.substr(2) == std::string_view("pp"));
}

// Pong is observational only: its payload is typed separately and it queues no output.
RUVIA_TEST(ws_connection_surfaces_pong_payload) {
    std::pmr::monotonic_buffer_resource resource;
    std::pmr::string input(&resource);
    ws_connection conn(input);

    const auto frame = masked_frame(&resource, 0xA, "ack");
    const auto event = poll_bytes(conn, input, std::string_view(frame.data(), frame.size()));
    RUVIA_CHECK(event->kind() == websocket_event_kind::pong);
    RUVIA_CHECK(event->pong()->payload() == "ack");
    RUVIA_CHECK(event->ping() == nullptr);
    RUVIA_CHECK(conn.output_plan().bytes().empty());
}

// A peer Close yields close (with the parsed code), an echoed Close frame, and one
// atomic output plan that ends the underlying transport after those bytes.
RUVIA_TEST(ws_connection_echoes_close) {
    std::pmr::monotonic_buffer_resource resource;
    std::pmr::string input(&resource);
    ws_connection conn(input);

    std::pmr::string body(&resource);
    body.push_back(static_cast<char>(0x03));  // 1000 = normal closure
    body.push_back(static_cast<char>(0xE8));
    body.append("bye");
    const auto f = masked_frame(&resource, 0x8, std::string_view(body.data(), body.size()));
    const auto e = poll_bytes(conn, input, std::string_view(f.data(), f.size()));
    RUVIA_CHECK(e->kind() == websocket_event_kind::close);
    RUVIA_CHECK_EQ(e->close()->close_code(), static_cast<std::uint16_t>(1000));
    RUVIA_CHECK(e->close()->reason() == "bye");
    RUVIA_CHECK(e->protocol_error() == nullptr);
    RUVIA_CHECK(conn.liveness_mode() == websocket_liveness_mode::inactive);

    const auto plan = conn.output_plan();
    const auto out = plan.bytes();
    RUVIA_CHECK(plan.disposition() == websocket_transport_disposition::end_transport);
    RUVIA_CHECK_EQ(
        static_cast<unsigned char>(out[0]), static_cast<unsigned char>(0x88));  // FIN|Close
    const auto original = std::string(out);
    RUVIA_CHECK(conn.consume_output(out.size() + 1) == websocket_output_consume_status::out_of_range);
    RUVIA_CHECK(conn.output_plan().bytes() == original);
    RUVIA_CHECK(conn.output_plan().disposition() == websocket_transport_disposition::end_transport);
    RUVIA_CHECK(conn.consume_output(out.size()) == websocket_output_consume_status::drained);
    conn.commit_transport_end();
    RUVIA_CHECK(conn.abort() == websocket_abort_disposition::no_transport_action);
}

// RFC 6455 uses 1005 locally to report an absent status; it is not present in
// the echoed wire payload and is not confused with a synthetic normal close.
RUVIA_TEST(ws_connection_close_without_status_reports_1005) {
    std::pmr::monotonic_buffer_resource resource;
    std::pmr::string input(&resource);
    ws_connection conn(input);

    const auto frame = masked_frame(&resource, 0x8, {});
    const auto event = poll_bytes(conn, input, std::string_view(frame.data(), frame.size()));
    RUVIA_CHECK(event->close() != nullptr);
    RUVIA_CHECK_EQ(event->close()->close_code(), std::uint16_t{1005});
    RUVIA_CHECK(event->close()->reason().empty());
    const auto output = conn.output_plan().bytes();
    RUVIA_CHECK_EQ(output.size(), std::size_t{2});
    RUVIA_CHECK_EQ(static_cast<unsigned char>(output[0]), 0x88U);
    RUVIA_CHECK_EQ(static_cast<unsigned char>(output[1]), 0U);
}

// A fragmented Text message (first frame FIN=0, continuation FIN=1) reassembles into
// one message.
RUVIA_TEST(ws_connection_reassembles_fragmented_message) {
    std::pmr::monotonic_buffer_resource resource;
    std::pmr::string input(&resource);
    ws_connection conn(input);

    const auto f1 = masked_frame(&resource, 0x1, "ab", /*fin=*/false);
    const auto f2 = masked_frame(&resource, 0x0, "cd", /*fin=*/true);  // continuation
    std::pmr::string both(&resource);
    both.append(f1.data(), f1.size());
    both.append(f2.data(), f2.size());
    const auto e = poll_bytes(conn, input, std::string_view(both.data(), both.size()));
    RUVIA_CHECK(e->message()->payload() == std::string_view("abcd"));
    RUVIA_CHECK(!conn.poll().has_value());
}

// An unmasked frame violates RFC 6455 §5.1: the core queues a Close and reports the
// protocol error rather than delivering anything.
RUVIA_TEST(ws_connection_rejects_unmasked_frame) {
    std::pmr::monotonic_buffer_resource resource;
    std::pmr::string input(&resource);
    ws_connection conn(input);

    // Text "hi" with the MASK bit clear.
    std::pmr::string f(&resource);
    f.push_back(static_cast<char>(0x81));  // FIN|Text
    f.push_back(static_cast<char>(2));     // no mask bit, len 2
    f.append("hi", 2);
    const auto e = poll_bytes(conn, input, std::string_view(f.data(), f.size()));
    RUVIA_CHECK(e->kind() == websocket_event_kind::protocol_error);
    RUVIA_CHECK_EQ(e->protocol_error()->close_code(), std::uint16_t{1002});
    RUVIA_CHECK(e->message() == nullptr);
    RUVIA_CHECK(conn.liveness_mode() == websocket_liveness_mode::inactive);
    const auto plan = conn.output_plan();
    const auto out = plan.bytes();
    RUVIA_CHECK(plan.disposition() == websocket_transport_disposition::end_transport);
    RUVIA_CHECK_EQ(static_cast<unsigned char>(out[0]), static_cast<unsigned char>(0x88));  // Close
}

// submit_frame is the sole generic outbound-frame entry and encodes an unmasked
// server Text frame.
RUVIA_TEST(ws_connection_submit_frame_encodes_unmasked_frame) {
    std::pmr::monotonic_buffer_resource resource;
    std::pmr::string input(&resource);
    ws_connection conn(input);

    RUVIA_CHECK(
        conn.submit_frame(websocket_opcode::text, "hello") == websocket_frame_submit_status::accepted);
    const auto out = conn.output_plan().bytes();
    RUVIA_CHECK_EQ(
        static_cast<unsigned char>(out[0]), static_cast<unsigned char>(0x81));  // FIN|Text
    RUVIA_CHECK_EQ(
        static_cast<unsigned char>(out[1]), static_cast<unsigned char>(5));  // unmasked len
    RUVIA_CHECK(out.substr(2) == std::string_view("hello"));
}

RUVIA_TEST(ws_connection_submit_frame_accepts_its_current_output_as_input) {
    std::pmr::monotonic_buffer_resource resource;
    std::pmr::string input(&resource);
    ws_connection conn(input);
    const std::string payload_value(128, 'x');

    RUVIA_CHECK(
        conn.submit_frame(websocket_opcode::binary, payload_value) == websocket_frame_submit_status::accepted);
    const auto borrowed = conn.output_plan().bytes();
    const std::string expected(borrowed);
    RUVIA_CHECK(
        conn.submit_frame(websocket_opcode::binary, borrowed) == websocket_frame_submit_status::accepted);

    const auto output = conn.output_plan().bytes();
    const auto second_offset = expected.size();
    RUVIA_CHECK_EQ(static_cast<unsigned char>(output[second_offset]), 0x82U);
    RUVIA_CHECK_EQ(static_cast<unsigned char>(output[second_offset + 1]), 126U);
    RUVIA_CHECK(output.substr(second_offset + 4) == expected);
}

// A partial frame (only the first byte) is buffered and yields no event until the rest
// arrives on a later feed.
RUVIA_TEST(ws_connection_needs_more_on_partial_frame) {
    std::pmr::monotonic_buffer_resource resource;
    std::pmr::string input(&resource);
    ws_connection conn(input);

    const auto f = masked_frame(&resource, 0x1, "split");
    RUVIA_CHECK(!poll_bytes(conn, input, std::string_view(f.data(), 3)).has_value());
    RUVIA_CHECK(conn.liveness_mode() == websocket_liveness_mode::open);

    const auto e =
        poll_bytes(conn, input, std::string_view(f.data() + 3, f.size() - 3));  // remainder
    RUVIA_CHECK(e->message()->payload() == std::string_view("split"));
}

// With permessage-deflate negotiated, an RSV1 (compressed) message is inflated and
// delivered as its original text.
RUVIA_TEST(ws_connection_inflates_compressed_message) {
    std::pmr::monotonic_buffer_resource resource;
    std::pmr::string input(&resource);
    ws_connection conn(
        input, protocol_byte_limit::unlimited(), (websocket_compression{.enabled_ = true}));

    ruvia::detail::websocket_deflate encoder;
    std::pmr::string compressed(&resource);
    RUVIA_CHECK(encoder.compress("hello hello hello", compressed));

    const auto f =
        masked_frame(&resource, 0x1, std::string_view(compressed.data(), compressed.size()),
            /*fin=*/true,
            /*rsv1=*/true);
    const auto e = poll_bytes(conn, input, std::string_view(f.data(), f.size()));
    RUVIA_CHECK(e->message()->payload() == std::string_view("hello hello hello"));
}

// Messages suppressed during the closing handshake must not leave their inflate
// output behind for the next message. A Binary message inflating to a non-UTF-8 byte
// is legal and ignored; a following Text message must be judged on its own bytes, not
// on the concatenation, and the peer Close must still complete the handshake.
RUVIA_TEST(ws_connection_suppressed_compressed_message_does_not_taint_next_utf8) {
    std::pmr::monotonic_buffer_resource resource;
    std::pmr::string input(&resource);
    ws_connection conn(
        input, protocol_byte_limit::unlimited(), (websocket_compression{.enabled_ = true}));

    RUVIA_CHECK(conn.submit_close(1000, "") == websocket_close_submit_status::accepted);
    const auto plan = conn.output_plan();
    RUVIA_CHECK(conn.consume_output(plan.bytes().size()) == websocket_output_consume_status::drained);
    RUVIA_CHECK(conn.liveness_mode() == websocket_liveness_mode::awaiting_peer_close);

    ruvia::detail::websocket_deflate encoder;
    std::pmr::string binary_block(&resource);
    RUVIA_CHECK(encoder.compress("\xFF", binary_block));
    std::pmr::string text_block(&resource);
    RUVIA_CHECK(encoder.compress("hi", text_block));

    std::pmr::string close_payload(&resource);
    close_payload.push_back(static_cast<char>(0x03));
    close_payload.push_back(static_cast<char>(0xE8));

    const auto binary_frame =
        masked_frame(&resource, 0x2, std::string_view(binary_block.data(), binary_block.size()),
            /*fin=*/true,
            /*rsv1=*/true);
    const auto text_frame =
        masked_frame(&resource, 0x1, std::string_view(text_block.data(), text_block.size()),
            /*fin=*/true,
            /*rsv1=*/true);
    const auto peer_close =
        masked_frame(&resource, 0x8, std::string_view(close_payload.data(), close_payload.size()));

    std::pmr::string inbound(&resource);
    inbound.append(binary_frame.data(), binary_frame.size());
    inbound.append(text_frame.data(), text_frame.size());
    inbound.append(peer_close.data(), peer_close.size());

    const auto event = poll_bytes(conn, input, std::string_view(inbound.data(), inbound.size()));
    RUVIA_CHECK(event.has_value());
    RUVIA_CHECK(event->close() != nullptr);  // not a spurious 1007 protocol error
    if (event->close() != nullptr) {
        RUVIA_CHECK_EQ(event->close()->close_code(), std::uint16_t{1000});
    }
}

// The decompression-bomb limit is per message. Two suppressed compressed messages that
// each fit must not be charged as one accumulated total.
RUVIA_TEST(ws_connection_suppressed_compressed_message_does_not_charge_next_limit) {
    std::pmr::monotonic_buffer_resource resource;
    std::pmr::string input(&resource);
    ws_connection conn(
        input, protocol_byte_limit::limited(1000), (websocket_compression{.enabled_ = true}));

    RUVIA_CHECK(conn.submit_close(1000, "") == websocket_close_submit_status::accepted);
    const auto plan = conn.output_plan();
    RUVIA_CHECK(conn.consume_output(plan.bytes().size()) == websocket_output_consume_status::drained);

    const std::string payload_value(600, 'a');  // 600 < 1000; two of them would exceed it
    ruvia::detail::websocket_deflate encoder;
    std::pmr::string first_block(&resource);
    RUVIA_CHECK(encoder.compress(payload_value, first_block));
    std::pmr::string second_block(&resource);
    RUVIA_CHECK(encoder.compress(payload_value, second_block));

    std::pmr::string close_payload(&resource);
    close_payload.push_back(static_cast<char>(0x03));
    close_payload.push_back(static_cast<char>(0xE8));

    const auto first =
        masked_frame(&resource, 0x1, std::string_view(first_block.data(), first_block.size()),
            /*fin=*/true,
            /*rsv1=*/true);
    const auto second =
        masked_frame(&resource, 0x1, std::string_view(second_block.data(), second_block.size()),
            /*fin=*/true,
            /*rsv1=*/true);
    const auto peer_close =
        masked_frame(&resource, 0x8, std::string_view(close_payload.data(), close_payload.size()));

    std::pmr::string inbound(&resource);
    inbound.append(first.data(), first.size());
    inbound.append(second.data(), second.size());
    inbound.append(peer_close.data(), peer_close.size());

    const auto event = poll_bytes(conn, input, std::string_view(inbound.data(), inbound.size()));
    RUVIA_CHECK(event.has_value());
    RUVIA_CHECK(event->close() != nullptr);  // not a spurious 1009 message-too-large
    if (event->close() != nullptr) {
        RUVIA_CHECK_EQ(event->close()->close_code(), std::uint16_t{1000});
    }
}

// With permessage-deflate on, submit_frame compresses a shrinkable payload and sets
// the RSV1 bit.
RUVIA_TEST(ws_connection_submit_compresses_when_enabled) {
    std::pmr::monotonic_buffer_resource resource;
    std::pmr::string input(&resource);
    ws_connection conn(
        input, protocol_byte_limit::unlimited(), (websocket_compression{.enabled_ = true}));

    const std::pmr::string repetitive(200, 'a', &resource);
    RUVIA_CHECK(conn.submit_frame(websocket_opcode::text,
                    std::string_view(repetitive.data(), repetitive.size())) ==
                websocket_frame_submit_status::accepted);

    const auto out = conn.output_plan().bytes();
    RUVIA_CHECK((static_cast<unsigned char>(out[0]) & 0x40U) != 0);    // RSV1 (compressed)
    RUVIA_CHECK((static_cast<unsigned char>(out[0]) & 0x0FU) == 0x1);  // Text opcode
    RUVIA_CHECK(out.size() < repetitive.size());                       // actually smaller
}

// An RSV1 frame when permessage-deflate was NOT negotiated is a protocol error.
RUVIA_TEST(ws_connection_rejects_rsv1_without_deflate) {
    std::pmr::monotonic_buffer_resource resource;
    std::pmr::string input(&resource);
    ws_connection conn(input);  // no deflate

    const auto f = masked_frame(&resource, 0x1, "x", /*fin=*/true, /*rsv1=*/true);
    const auto event = poll_bytes(conn, input, std::string_view(f.data(), f.size()));
    RUVIA_CHECK(event->protocol_error() != nullptr);
    RUVIA_CHECK(conn.output_plan().disposition() == websocket_transport_disposition::end_transport);
}

RUVIA_TEST(ws_connection_outbound_frame_rejections_are_typed_and_transactional) {
    std::pmr::monotonic_buffer_resource resource;
    std::pmr::string input(&resource);
    ws_connection conn(input, protocol_byte_limit::limited(4));

    const std::string invalid_text("\xc0\x80", 2);
    RUVIA_CHECK(conn.submit_frame(websocket_opcode::text, invalid_text) ==
                websocket_frame_submit_status::invalid_text_payload);
    RUVIA_CHECK(
        conn.submit_frame(websocket_opcode::text, "12345") == websocket_frame_submit_status::message_too_large);
    RUVIA_CHECK(conn.submit_frame(websocket_opcode::ping, std::string(126, 'p')) ==
                websocket_frame_submit_status::control_frame_too_large);
    RUVIA_CHECK(
        conn.submit_frame(websocket_opcode::close, {}) == websocket_frame_submit_status::invalid_opcode);
    RUVIA_CHECK(conn.submit_frame(static_cast<websocket_opcode>(0x7), {}) ==
                websocket_frame_submit_status::invalid_opcode);
    RUVIA_CHECK(conn.output_plan().bytes().empty());
    RUVIA_CHECK(conn.liveness_mode() == websocket_liveness_mode::open);
}

#if !defined(_MSC_VER)
RUVIA_TEST(ws_connection_outbound_allocation_failure_publishes_no_partial_frame) {
    toggle_allocation_resource resource;
    std::pmr::string input(&resource);
    ws_connection conn(input);
    const std::string payload_value(128, 'x');

    resource.reject();
    bool threw = false;
    try {
        (void)conn.submit_frame(websocket_opcode::binary, payload_value);
    } catch (const std::bad_alloc&) {
        threw = true;
    }
    RUVIA_CHECK(threw);
    RUVIA_CHECK(conn.output_plan().bytes().empty());
    RUVIA_CHECK(conn.liveness_mode() == websocket_liveness_mode::open);

    resource.reject(false);
    RUVIA_CHECK(
        conn.submit_frame(websocket_opcode::binary, payload_value) == websocket_frame_submit_status::accepted);
    RUVIA_CHECK_EQ(conn.output_plan().bytes().size(), payload_value.size() + 4);
}

RUVIA_TEST(ws_connection_inbound_allocation_failure_is_explicitly_terminal) {
    toggle_allocation_resource resource;
    std::pmr::string input(&resource);
    ws_connection conn(input);
    const std::string payload_value(100, 'x');
    const auto first_fragment = masked_frame(std::pmr::get_default_resource(), 0x1, payload_value, false);
    input.append(first_fragment);

    resource.reject();
    bool threw = false;
    try {
        (void)conn.poll();
    } catch (const std::bad_alloc&) {
        threw = true;
    }
    RUVIA_CHECK(threw);
    RUVIA_CHECK(conn.liveness_mode() == websocket_liveness_mode::inactive);
    RUVIA_CHECK(conn.output_plan().bytes().empty());

    resource.reject(false);
    const auto terminal = conn.poll();
    RUVIA_CHECK(terminal && terminal->transport_end() != nullptr);
}
#endif

RUVIA_TEST(ws_connection_outbound_close_rejections_are_typed_and_transactional) {
    std::pmr::monotonic_buffer_resource resource;
    std::pmr::string input(&resource);
    ws_connection conn(input);

    RUVIA_CHECK(conn.submit_close(1005, {}) == websocket_close_submit_status::invalid_code);
    RUVIA_CHECK(
        conn.submit_close(1000, std::string("\xc0\x80", 2)) == websocket_close_submit_status::invalid_reason);
    RUVIA_CHECK(
        conn.submit_close(1000, std::string(124, 'x')) == websocket_close_submit_status::reason_too_large);
    RUVIA_CHECK(conn.output_plan().bytes().empty());
    RUVIA_CHECK(conn.liveness_mode() == websocket_liveness_mode::open);
}

// RFC 6455 assigns 1010 to clients that expected an extension the server did
// not negotiate. A server must fail that mismatch during the opening handshake
// instead of initiating a Close frame with the client-only status code.
RUVIA_TEST(ws_connection_server_rejects_client_only_1010_close) {
    std::pmr::monotonic_buffer_resource resource;
    std::pmr::string input(&resource);
    ws_connection conn(input);

    RUVIA_CHECK(conn.submit_close(1010, "permessage-deflate") == websocket_close_submit_status::invalid_code);
    RUVIA_CHECK(conn.output_plan().bytes().empty());
    RUVIA_CHECK(conn.liveness_mode() == websocket_liveness_mode::open);

    RUVIA_CHECK(conn.submit_close(1000, "normal") == websocket_close_submit_status::accepted);
}

// A locally initiated Close is not transport EOF. Its bytes are flushed while the
// transport remains open; application data is then ignored until the peer Close
// completes the handshake and produces the terminal transport plan.
RUVIA_TEST(ws_connection_local_close_waits_for_peer_close) {
    std::pmr::monotonic_buffer_resource resource;
    std::pmr::string input(&resource);
    ws_connection conn(input);

    RUVIA_CHECK(conn.submit_close(1000, "bye") == websocket_close_submit_status::accepted);
    RUVIA_CHECK(conn.liveness_mode() == websocket_liveness_mode::awaiting_peer_close);
    auto plan = conn.output_plan();
    RUVIA_CHECK(plan.disposition() == websocket_transport_disposition::keep_open);
    RUVIA_CHECK_EQ(static_cast<unsigned char>(plan.bytes()[0]), 0x88U);
    const auto original = std::string(plan.bytes());
    RUVIA_CHECK(conn.consume_output(plan.bytes().size() + 1) == websocket_output_consume_status::out_of_range);
    RUVIA_CHECK(conn.output_plan().bytes() == original);
    RUVIA_CHECK(conn.output_plan().disposition() == websocket_transport_disposition::keep_open);
    RUVIA_CHECK(conn.consume_output(1) == websocket_output_consume_status::pending);
    RUVIA_CHECK(conn.output_plan().bytes() == std::string_view(original).substr(1));
    RUVIA_CHECK(conn.consume_output(original.size() - 1) == websocket_output_consume_status::drained);
    RUVIA_CHECK(conn.liveness_mode() == websocket_liveness_mode::awaiting_peer_close);

    std::pmr::string close_payload(&resource);
    close_payload.push_back(static_cast<char>(0x03));
    close_payload.push_back(static_cast<char>(0xE8));
    const auto ignored_text = masked_frame(&resource, 0x1, "late");
    const auto peer_close =
        masked_frame(&resource, 0x8, std::string_view(close_payload.data(), close_payload.size()));
    std::pmr::string inbound(&resource);
    inbound.append(ignored_text.data(), ignored_text.size());
    inbound.append(peer_close.data(), peer_close.size());

    const auto event = poll_bytes(conn, input, std::string_view(inbound.data(), inbound.size()));
    RUVIA_CHECK(event->close() != nullptr);
    RUVIA_CHECK_EQ(event->close()->close_code(), std::uint16_t{1000});
    plan = conn.output_plan();
    RUVIA_CHECK(plan.bytes().empty());  // local Close was already sent; no duplicate
    RUVIA_CHECK(plan.disposition() == websocket_transport_disposition::end_transport);
    conn.commit_transport_end();
    RUVIA_CHECK(conn.abort() == websocket_abort_disposition::no_transport_action);
}

// A transport EOF is not rewritten into a synthetic normal websocket Close. It
// discards any queued WS bytes and asks only for transport termination.
RUVIA_TEST(ws_connection_transport_eof_discards_unsent_close) {
    std::pmr::monotonic_buffer_resource resource;
    std::pmr::string input(&resource);
    ws_connection conn(input);

    RUVIA_CHECK(conn.submit_close(1000, {}) == websocket_close_submit_status::accepted);
    RUVIA_CHECK(!conn.output_plan().bytes().empty());
    conn.notify_transport_eof();
    const auto plan = conn.output_plan();
    RUVIA_CHECK(plan.bytes().empty());
    RUVIA_CHECK(plan.disposition() == websocket_transport_disposition::end_transport);
    const auto event = conn.poll();
    RUVIA_CHECK(event->kind() == websocket_event_kind::transport_end);
    RUVIA_CHECK(event->transport_end() != nullptr);
    RUVIA_CHECK(event->close() == nullptr);
}

RUVIA_TEST(ws_connection_reports_not_open_for_outbound_submissions_after_close) {
    std::pmr::monotonic_buffer_resource resource;
    std::pmr::string input(&resource);
    ws_connection conn(input);
    RUVIA_CHECK(conn.submit_close(1000, {}) == websocket_close_submit_status::accepted);
    RUVIA_CHECK(conn.submit_frame(websocket_opcode::text, "late") == websocket_frame_submit_status::not_open);
    RUVIA_CHECK(conn.submit_close(1000, {}) == websocket_close_submit_status::already_closing);
}

RUVIA_TEST(ws_connection_abort_returns_transport_action_once) {
    std::pmr::monotonic_buffer_resource resource;
    std::pmr::string input(&resource);
    ws_connection conn(input);

    RUVIA_CHECK(conn.abort() == websocket_abort_disposition::abort_transport);
    RUVIA_CHECK(conn.abort() == websocket_abort_disposition::no_transport_action);
    RUVIA_CHECK(conn.submit_frame(websocket_opcode::text, "late") == websocket_frame_submit_status::not_open);
    RUVIA_CHECK(conn.submit_close(1000, {}) == websocket_close_submit_status::closed);
}
