#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <new>
#include <stdexcept>
#include <string>
#include <utility>

#include "ruvia/http/websocket_connection.h"
#include "ruvia/http/websocket_server_protocol.h"

#include "test_harness.h"

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

class counting_resource final : public std::pmr::memory_resource {
public:
    std::size_t live_bytes_{0};
    std::size_t allocations_{0};
    std::size_t deallocations_{0};
    bool reject_{false};

private:
    void* do_allocate(std::size_t bytes_value, std::size_t alignment) override {
        if (reject_) {
            throw std::bad_alloc();
        }
        auto* allocation = std::pmr::new_delete_resource()->allocate(bytes_value, alignment);
        ++allocations_;
        live_bytes_ += bytes_value;
        return allocation;
    }
    void do_deallocate(void* pointer, std::size_t bytes_value, std::size_t alignment) override {
        ++deallocations_;
        live_bytes_ -= bytes_value;
        std::pmr::new_delete_resource()->deallocate(pointer, bytes_value, alignment);
    }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
};

websocket_connection client(mask_source& source_value, protocol_byte_limit limit = protocol_byte_limit::unlimited()) {
    return websocket_connection({.message_limit_ = limit, .role_ = websocket_connection_role::client, .mask_key_generator_ = &mask_source::generate, .mask_key_context_ = &source_value});
}

std::string drain(websocket_connection& connection) {
    const std::string bytes_value(connection.output_plan().bytes());
    if (connection.consume_output(bytes_value.size()) != websocket_output_consume_status::drained) {
        throw std::runtime_error("output did not drain");
    }
    return bytes_value;
}
}  // namespace

RUVIA_TEST(ws_public_context_takeover_mixed_messages_and_connection_lifetime) {
    counting_resource memory;
    {
        mask_source mask;
        websocket_connection sender({.resource_ = &memory, .message_limit_ = protocol_byte_limit::limited(16384), .compression_ = (websocket_compression{.enabled_ = true, .server_no_context_takeover_ = false, .client_no_context_takeover_ = false}), .role_ = websocket_connection_role::client, .mask_key_generator_ = &mask_source::generate, .mask_key_context_ = &mask, .compression_level_ = 9});
        websocket_connection receiver({.resource_ = &memory, .message_limit_ = protocol_byte_limit::limited(16384), .compression_ = (websocket_compression{.enabled_ = true, .server_no_context_takeover_ = false, .client_no_context_takeover_ = false}), .compression_level_ = 9});
        std::string noise(1024, '\0');
        std::uint32_t state_value = 1234567;
        for (auto& byte : noise) {
            state_value ^= state_value << 13;
            state_value ^= state_value >> 17;
            state_value ^= state_value << 5;
            byte = static_cast<char>(state_value);
        }
        std::size_t stable_memory = 0;
        for (int round = 0; round < 256; ++round) {
            const std::string payload_value = round % 4 == 0 ? noise : std::string(8192, 'x');
            const bool compress = round % 4 != 2;
            RUVIA_CHECK(sender.submit_frame(websocket_opcode::binary, payload_value, compress) == websocket_frame_submit_status::accepted);
            const auto wire = drain(sender);
            if (!compress || round % 4 == 0) {
                RUVIA_CHECK((static_cast<unsigned char>(wire[0]) & 0x40U) == 0);
            }
            RUVIA_CHECK(receiver.feed(wire) == websocket_feed_status::accepted);
            const auto event = receiver.next_event();
            RUVIA_CHECK(event && event->message());
            if (!event || !event->message()) {
                return;
            }
            RUVIA_CHECK_EQ(event->message()->payload(), payload_value);
            if (round == 63) {
                stable_memory = memory.live_bytes_;
            }
            if (round > 63) {
                RUVIA_CHECK(memory.live_bytes_ <= stable_memory);
            }
        }
        RUVIA_CHECK(sender.submit_frame(websocket_opcode::ping, "ping") == websocket_frame_submit_status::accepted);
        RUVIA_CHECK((static_cast<unsigned char>(drain(sender)[0]) & 0x40U) == 0);
        RUVIA_CHECK(sender.submit_frame(websocket_opcode::binary, std::string(16385, 'x')) == websocket_frame_submit_status::message_too_large);
    }
    RUVIA_CHECK_EQ(memory.live_bytes_, std::size_t{0});
}

RUVIA_TEST(ws_public_compression_accepts_final_blocks_and_preserves_required_history) {
    for (const bool no_takeover : {false, true}) {
        mask_source mask;
        websocket_connection receiver({.compression_ = (websocket_compression{.enabled_ = true, .server_no_context_takeover_ = no_takeover}), .role_ = websocket_connection_role::client, .mask_key_generator_ = &mask_source::generate, .mask_key_context_ = &mask});
        // RFC 7692 section 7.2.3.4: BFINAL=1, padding, then an empty block.
        RUVIA_CHECK(receiver.feed(std::string_view("\xc1\x08\xf3\x48\xcd\xc9\xc9\x07\x00\x00", 10)) == websocket_feed_status::accepted);
        auto event = receiver.next_event();
        RUVIA_CHECK(event && event->message() && event->message()->payload() == "Hello");
        // Two terminated streams within one message share their LZ77 history
        // even when context takeover between messages is disabled.
        RUVIA_CHECK(receiver.feed(std::string_view("\xc1\x0c\xf3\x48\xcd\xc9\xc9\x07\x00\xf3\x00\x11\x00\x00", 14)) == websocket_feed_status::accepted);
        event = receiver.next_event();
        RUVIA_CHECK(event && event->message() && event->message()->payload() == "HelloHello");
        RUVIA_CHECK(receiver.feed(std::string_view("\xc1\x01\x00", 3)) == websocket_feed_status::accepted);
        event = receiver.next_event();
        RUVIA_CHECK(event && event->message() && event->message()->payload().empty());
        // This message refers back to the preceding message's dictionary.
        RUVIA_CHECK(receiver.feed(std::string_view("\xc1\x05\xf2\x00\x11\x00\x00", 7)) == websocket_feed_status::accepted);
        event = receiver.next_event();
        if (no_takeover) {
            RUVIA_CHECK(event && event->protocol_error() && event->protocol_error()->close_code() == 1002);
        } else {
            RUVIA_CHECK(event && event->message() && event->message()->payload() == "Hello");
        }
    }
}

RUVIA_TEST(ws_public_compression_rejects_incomplete_empty_block_headers) {
    for (const auto wire : {std::string_view("\xc1\x00", 2), std::string_view("\xc1\x02\x03\x00", 4)}) {
        mask_source mask;
        websocket_connection receiver({.compression_ = (websocket_compression{.enabled_ = true}), .role_ = websocket_connection_role::client, .mask_key_generator_ = &mask_source::generate, .mask_key_context_ = &mask});
        RUVIA_CHECK(receiver.feed(wire) == websocket_feed_status::accepted);
        const auto event = receiver.next_event();
        RUVIA_CHECK(event && event->protocol_error() && event->protocol_error()->close_code() == 1002);
        RUVIA_CHECK(receiver.submit_frame(websocket_opcode::text, "after-error") == websocket_frame_submit_status::not_open);
    }
}

RUVIA_TEST(ws_public_server_events_use_public_payload_types) {
    mask_source mask;
    auto sender = client(mask);
    std::pmr::string input;
    websocket_server_protocol protocol(input);

    RUVIA_CHECK(sender.submit_frame(websocket_opcode::text, "hey") ==
                websocket_frame_submit_status::accepted);
    const std::string message_wire(sender.output_plan().bytes());
    RUVIA_CHECK(sender.consume_output(message_wire.size()) == websocket_output_consume_status::drained);
    input.append(message_wire);
    auto message = protocol.poll();
    RUVIA_CHECK(message && message->kind() == websocket_event_kind::message);
    RUVIA_CHECK(message && message->message());
    if (message && message->message()) {
        RUVIA_CHECK(message->message()->opcode() == websocket_opcode::text);
        RUVIA_CHECK_EQ(message->message()->payload(), "hey");
    }

    RUVIA_CHECK(sender.submit_frame(websocket_opcode::ping, "p") ==
                websocket_frame_submit_status::accepted);
    const std::string ping_wire(sender.output_plan().bytes());
    RUVIA_CHECK(sender.consume_output(ping_wire.size()) == websocket_output_consume_status::drained);
    input.append(ping_wire);
    auto ping = protocol.poll();
    RUVIA_CHECK(ping && ping->kind() == websocket_event_kind::ping);
    RUVIA_CHECK(ping && ping->ping());
    if (ping && ping->ping()) {
        RUVIA_CHECK_EQ(ping->ping()->payload(), "p");
    }
}

RUVIA_TEST(ws_public_protocol_reuses_borrowed_input_and_releases_memory) {
    counting_resource memory;
    std::pmr::string input(&memory);
    input.reserve(1024);
    const auto bytes_before_protocol = memory.live_bytes_;
    {
        websocket_server_protocol protocol(input);
        mask_source mask;
        auto sender = client(mask);
        for (int i = 0; i < 32; ++i) {
            RUVIA_CHECK(sender.submit_frame(websocket_opcode::text, "payload") ==
                        websocket_frame_submit_status::accepted);
            const std::string wire(sender.output_plan().bytes());
            RUVIA_CHECK(sender.consume_output(wire.size()) == websocket_output_consume_status::drained);
            input.append(wire);
            const auto live_before_poll = memory.live_bytes_;
            {
                auto event = protocol.poll();
                RUVIA_CHECK(event && event->message());
                if (event && event->message()) {
                    RUVIA_CHECK_EQ(event->message()->payload(), "payload");
                    const auto payload_address = reinterpret_cast<std::uintptr_t>(
                        event->message()->payload().data());
                    const auto input_address = reinterpret_cast<std::uintptr_t>(input.data());
                    RUVIA_CHECK(payload_address >= input_address);
                    RUVIA_CHECK(payload_address + event->message()->payload().size() <=
                                input_address + input.size());
                }
            }
            // Borrowed payloads stay in input; transient debug iterator proxies
            // must be released when the event is destroyed.
            RUVIA_CHECK_EQ(memory.live_bytes_, live_before_poll);
        }
        RUVIA_CHECK(protocol.submit_frame(websocket_opcode::text, "out") ==
                    websocket_frame_submit_status::accepted);
        auto pending = protocol.output_plan().bytes();
        RUVIA_CHECK(protocol.consume_output(pending.size()) ==
                    websocket_output_consume_status::drained);
        const auto live_before_submit = memory.live_bytes_;
        for (int i = 0; i < 32; ++i) {
            RUVIA_CHECK(protocol.submit_frame(websocket_opcode::text, "out") ==
                        websocket_frame_submit_status::accepted);
            pending = protocol.output_plan().bytes();
            RUVIA_CHECK(protocol.consume_output(pending.size()) ==
                        websocket_output_consume_status::drained);
        }
        // PMR implementations may allocate a transient container proxy on each
        // submit; those allocations must not remain live after output drains.
        RUVIA_CHECK_EQ(memory.live_bytes_, live_before_submit);
        protocol.notify_transport_eof();
        auto end = protocol.poll();
        RUVIA_CHECK(end && end->kind() == websocket_event_kind::transport_end);
        protocol.commit_transport_end();
        RUVIA_CHECK(protocol.abort() == websocket_abort_disposition::no_transport_action);
    }
    RUVIA_CHECK_EQ(memory.live_bytes_, bytes_before_protocol);
}

RUVIA_TEST(ws_public_server_protocol_reports_protocol_error_as_public_value) {
    std::pmr::string input;
    websocket_server_protocol protocol(input);
    // A server-side connection must reject an unmasked client frame.
    input.append("\x81\x01x", 3);
    auto error = protocol.poll();
    RUVIA_CHECK(error && error->kind() == websocket_event_kind::protocol_error);
    RUVIA_CHECK(error && error->protocol_error() && error->protocol_error()->close_code() == 1002);
}

RUVIA_TEST(ws_public_server_protocol_controls_permessage_deflate_per_frame) {
    std::pmr::string input;
    websocket_server_protocol protocol(input, protocol_byte_limit::unlimited(),
        websocket_server_protocol_options{(websocket_compression{.enabled_ = true}), 6});
    const std::string payload_value(200, 'x');

    RUVIA_CHECK(protocol.submit_frame(websocket_opcode::text, payload_value, false) ==
                websocket_frame_submit_status::accepted);
    auto output = protocol.output_plan().bytes();
    RUVIA_CHECK(!output.empty());
    if (!output.empty()) {
        RUVIA_CHECK((static_cast<unsigned char>(output[0]) & 0x40U) == 0);
    }
    RUVIA_CHECK(protocol.consume_output(output.size()) ==
                websocket_output_consume_status::drained);

    RUVIA_CHECK(protocol.submit_frame(websocket_opcode::text, payload_value) ==
                websocket_frame_submit_status::accepted);
    output = protocol.output_plan().bytes();
    RUVIA_CHECK(!output.empty());
    if (!output.empty()) {
        RUVIA_CHECK((static_cast<unsigned char>(output[0]) & 0x40U) != 0);
    }
}

RUVIA_TEST(ws_public_server_protocol_preserves_transport_end_semantics) {
    std::pmr::string input;
    websocket_server_protocol protocol(input);
    RUVIA_CHECK(protocol.submit_close(1000, "done") == websocket_close_submit_status::accepted);
    RUVIA_CHECK(protocol.output_plan().disposition() ==
                websocket_transport_disposition::keep_open);
    const auto output = protocol.output_plan().bytes();
    RUVIA_CHECK(!output.empty());
    RUVIA_CHECK(protocol.consume_output(output.size()) ==
                websocket_output_consume_status::drained);
    // After our Close frame is sent, the protocol still awaits the peer's
    // Close; transport EOF terminates that wait without fabricating a reply.
    RUVIA_CHECK(protocol.output_plan().disposition() ==
                websocket_transport_disposition::keep_open);
    protocol.notify_transport_eof();
    RUVIA_CHECK(protocol.output_plan().disposition() ==
                websocket_transport_disposition::end_transport);
}

RUVIA_TEST(ws_public_server_eof_discards_pending_output_before_transport_end) {
    std::pmr::string input;
    websocket_server_protocol protocol(input);
    RUVIA_CHECK(protocol.submit_frame(websocket_opcode::text, "unsent") ==
                websocket_frame_submit_status::accepted);
    const auto output = protocol.output_plan().bytes();
    RUVIA_CHECK(output.size() > 1);
    RUVIA_CHECK(protocol.consume_output(1) == websocket_output_consume_status::pending);

    protocol.notify_transport_eof();
    const auto end = protocol.output_plan();
    RUVIA_CHECK(end.bytes().empty());
    RUVIA_CHECK(end.disposition() == websocket_transport_disposition::end_transport);
    // The driver can finish its direction without consuming bytes discarded by EOF.
    RUVIA_CHECK(protocol.consume_output(end.bytes().size()) ==
                websocket_output_consume_status::drained);
    protocol.commit_transport_end();
    RUVIA_CHECK(protocol.abort() == websocket_abort_disposition::no_transport_action);
}

RUVIA_TEST(ws_public_server_keeps_pending_pong_before_peer_close) {
    std::pmr::string input;
    websocket_server_protocol protocol(input);
    RUVIA_CHECK(protocol.submit_close(1000, "done") ==
                websocket_close_submit_status::accepted);
    auto output = protocol.output_plan().bytes();
    RUVIA_CHECK(!output.empty());
    RUVIA_CHECK(protocol.consume_output(output.size()) ==
                websocket_output_consume_status::drained);

    // Masked client Ping("p") followed by an empty masked Close.
    input.append(
        "\x89\x81\x01\x02\x03\x04q"
        "\x88\x80\x05\x06\x07\x08",
        13);
    const auto ping = protocol.poll();
    RUVIA_CHECK(ping && ping->kind() == websocket_event_kind::ping);
    RUVIA_CHECK(ping && ping->ping() && ping->ping()->payload() == "p");
    const auto close = protocol.poll();
    RUVIA_CHECK(close && close->kind() == websocket_event_kind::close);

    const auto plan = protocol.output_plan();
    RUVIA_CHECK_EQ(plan.bytes(), std::string_view("\x8a\x01p", 3));
    RUVIA_CHECK(plan.disposition() ==
                websocket_transport_disposition::end_transport);
    RUVIA_CHECK(protocol.consume_output(plan.bytes().size()) ==
                websocket_output_consume_status::drained);
    protocol.commit_transport_end();
    RUVIA_CHECK(protocol.liveness_mode() == websocket_liveness_mode::inactive);
}

RUVIA_TEST(ws_public_client_server_exchange_and_partial_output) {
    mask_source source;
    auto sender = client(source);
    websocket_connection receiver;
    const std::string payload_value(65536, '\xff');
    for (unsigned round = 0; round < 3; ++round) {
        RUVIA_CHECK(sender.submit_frame(websocket_opcode::binary, payload_value) == websocket_frame_submit_status::accepted);
        const std::string wire(sender.output_plan().bytes());
        RUVIA_CHECK((static_cast<unsigned char>(wire[1]) & 0x80U) != 0);
        RUVIA_CHECK(sender.consume_output(wire.size() + 1) == websocket_output_consume_status::out_of_range);
        RUVIA_CHECK(sender.consume_output(1) == websocket_output_consume_status::pending);
        RUVIA_CHECK_EQ(sender.output_plan().bytes(), std::string_view(wire).substr(1));
        RUVIA_CHECK(sender.consume_output(wire.size() - 1) == websocket_output_consume_status::drained);
        RUVIA_CHECK(receiver.feed(std::string_view(wire).substr(0, 5)) == websocket_feed_status::accepted);
        RUVIA_CHECK(!receiver.next_event());
        RUVIA_CHECK(receiver.feed(std::string_view(wire).substr(5)) == websocket_feed_status::accepted);
        const auto event = receiver.next_event();
        RUVIA_CHECK(event && event->message());
        if (!event || !event->message()) {
            return;
        }
        RUVIA_CHECK_EQ(event->message()->payload(), payload_value);
        RUVIA_CHECK(receiver.submit_frame(websocket_opcode::binary, event->message()->payload()) == websocket_frame_submit_status::accepted);
        const auto reply = drain(receiver);
        RUVIA_CHECK((static_cast<unsigned char>(reply[1]) & 0x80U) == 0);
        RUVIA_CHECK(sender.feed(reply) == websocket_feed_status::accepted);
        const auto echo = sender.next_event();
        RUVIA_CHECK(echo && echo->message());
        if (!echo || !echo->message()) {
            return;
        }
        RUVIA_CHECK_EQ(echo->message()->payload(), payload_value);
    }
    RUVIA_CHECK_EQ(source.calls_, 3U);
}

RUVIA_TEST(ws_public_client_fragmented_message_with_ping_and_close) {
    mask_source source;
    auto connection = client(source);
    websocket_connection server;
    // A fragmented binary message with a control frame between fragments.
    const std::string wire(
        "\x02\x02"
        "ab"
        "\x89\x01"
        "p"
        "\x80\x02"
        "cd",
        11);
    RUVIA_CHECK(connection.feed(wire) == websocket_feed_status::accepted);
    const auto ping = connection.next_event();
    RUVIA_CHECK(ping && ping->ping());
    const auto pong = drain(connection);
    RUVIA_CHECK(server.feed(pong) == websocket_feed_status::accepted);
    const auto pong_event = server.next_event();
    RUVIA_CHECK(pong_event && pong_event->pong() && pong_event->pong()->payload() == "p");
    const auto message = connection.next_event();
    RUVIA_CHECK(message && message->message() && message->message()->payload() == "abcd");
    RUVIA_CHECK(server.submit_close(1000, "done") == websocket_close_submit_status::accepted);
    const auto close = drain(server);
    RUVIA_CHECK(connection.feed(close) == websocket_feed_status::accepted);
    const auto closed = connection.next_event();
    RUVIA_CHECK(closed && closed->close() && closed->close()->close_code() == 1000);
    RUVIA_CHECK(connection.output_plan().disposition() == websocket_transport_disposition::end_transport);
    const auto response = drain(connection);
    RUVIA_CHECK(server.feed(response) == websocket_feed_status::accepted);
    const auto acknowledged = server.next_event();
    RUVIA_CHECK(acknowledged && acknowledged->close());
    connection.commit_transport_end();
    RUVIA_CHECK(connection.feed("x") == websocket_feed_status::inactive);
    RUVIA_CHECK_EQ(source.calls_, 2U);
    RUVIA_CHECK(pong.substr(2, 4) != response.substr(2, 4));
}

RUVIA_TEST(ws_public_client_rejects_masked_server_and_oversized_message) {
    mask_source source;
    auto invalid_server = client(source);
    RUVIA_CHECK(invalid_server.submit_frame(websocket_opcode::text, "bad") == websocket_frame_submit_status::accepted);
    const auto masked = drain(invalid_server);
    auto receiver = client(source);
    RUVIA_CHECK(receiver.feed(masked) == websocket_feed_status::accepted);
    const auto error = receiver.next_event();
    RUVIA_CHECK(error && error->protocol_error() && error->protocol_error()->close_code() == 1002);
    auto limited = client(source, protocol_byte_limit::limited(2));
    RUVIA_CHECK(limited.feed(std::string_view("\x82\x03"
                                              "abc",
                    5)) == websocket_feed_status::accepted);
    const auto large = limited.next_event();
    RUVIA_CHECK(large && large->protocol_error() && large->protocol_error()->close_code() == 1009);
    RUVIA_CHECK(limited.submit_frame(websocket_opcode::binary, "x") == websocket_frame_submit_status::not_open);
}

RUVIA_TEST(ws_public_client_requires_entropy_and_handles_transport_end) {
    RUVIA_CHECK(ruvia::testing::throws_on([] {
        websocket_connection connection({.role_ = websocket_connection_role::client});
    }));
    RUVIA_CHECK(ruvia::testing::throws_on([] {
        websocket_connection connection({.role_ = static_cast<websocket_connection_role>(255)});
    }));
    mask_source source;
    auto connection = client(source);
    source.fail_ = true;
    RUVIA_CHECK(ruvia::testing::throws_on([&] {
        (void)connection.submit_frame(websocket_opcode::binary, "x");
    }));
    RUVIA_CHECK(connection.output_plan().bytes().empty());
    RUVIA_CHECK(connection.abort() == websocket_abort_disposition::abort_transport);
    RUVIA_CHECK(connection.abort() == websocket_abort_disposition::no_transport_action);
    auto eof = client(source);
    eof.notify_transport_eof();
    const auto event = eof.next_event();
    RUVIA_CHECK(event && event->transport_end());
    eof.commit_transport_end();
    RUVIA_CHECK(eof.liveness_mode() == websocket_liveness_mode::inactive);
}

RUVIA_TEST(ws_public_connection_reuses_buffers_and_releases_owned_memory) {
    counting_resource resource;
    mask_source source;
    {
        websocket_connection connection({.resource_ = &resource,
            .role_ = websocket_connection_role::client,
            .mask_key_generator_ = &mask_source::generate,
            .mask_key_context_ = &source});
        websocket_connection server;
        const std::string payload_value(8192, 'x');
        RUVIA_CHECK(server.submit_frame(websocket_opcode::binary, payload_value) == websocket_frame_submit_status::accepted);
        const auto wire = drain(server);
        std::size_t warmed_bytes = 0;
        for (unsigned i = 0; i < 100; ++i) {
            RUVIA_CHECK(connection.feed(wire) == websocket_feed_status::accepted);
            const auto event = connection.next_event();
            RUVIA_CHECK(event && event->message());
            if (!event || !event->message()) {
                return;
            }
            RUVIA_CHECK(connection.submit_frame(websocket_opcode::binary, event->message()->payload()) == websocket_frame_submit_status::accepted);
            (void)drain(connection);
            RUVIA_CHECK(!connection.next_event());
            if (i == 2) {
                warmed_bytes = resource.live_bytes_;
            }
            if (i > 2) {
                RUVIA_CHECK_EQ(resource.live_bytes_, warmed_bytes);
            }
        }
        RUVIA_CHECK(resource.live_bytes_ > 0);
        (void)connection.abort();
    }
    RUVIA_CHECK_EQ(resource.live_bytes_, std::size_t{0});
}

RUVIA_TEST(ws_public_connection_moves_keep_event_and_output_storage_stable) {
    counting_resource source_memory;
    counting_resource replaced_memory;
    mask_source mask;
    const std::string payload_value(1024, 'x');
    {
        websocket_connection source_value({.resource_ = &source_memory,
            .role_ = websocket_connection_role::client,
            .mask_key_generator_ = &mask_source::generate,
            .mask_key_context_ = &mask});
        websocket_connection server;
        RUVIA_CHECK(server.submit_frame(websocket_opcode::binary, payload_value) == websocket_frame_submit_status::accepted);
        const auto wire = drain(server);
        RUVIA_CHECK(source_value.feed(wire) == websocket_feed_status::accepted);
        const auto event = source_value.next_event();
        RUVIA_CHECK(event && event->message());
        if (!event || !event->message()) {
            return;
        }
        const auto retained_payload = event->message()->payload();
        RUVIA_CHECK(source_value.submit_frame(websocket_opcode::binary, retained_payload) == websocket_frame_submit_status::accepted);
        const auto retained_output = source_value.output_plan().bytes();
        const auto live_before_move = source_memory.live_bytes_;
        const auto allocations_before_move = source_memory.allocations_;
        websocket_connection moved(std::move(source_value));
        websocket_connection replacement({.resource_ = &replaced_memory});
        RUVIA_CHECK(replaced_memory.live_bytes_ > 0);
        replacement = std::move(moved);
        RUVIA_CHECK_EQ(replaced_memory.live_bytes_, std::size_t{0});
        RUVIA_CHECK_EQ(source_memory.live_bytes_, live_before_move);
        RUVIA_CHECK_EQ(source_memory.allocations_, allocations_before_move);
        RUVIA_CHECK_EQ(retained_payload, payload_value);
        RUVIA_CHECK(replacement.output_plan().bytes().data() == retained_output.data());
        RUVIA_CHECK_EQ(replacement.output_plan().bytes(), retained_output);
        RUVIA_CHECK(server.feed(retained_output) == websocket_feed_status::accepted);
        const auto echoed = server.next_event();
        RUVIA_CHECK(echoed && echoed->message() && echoed->message()->payload() == payload_value);
        // EOF/abort discard logical output without invalidating a transport's
        // outstanding write borrow. Only destruction releases the backing bytes.
        const std::string saved_output(retained_output);
        replacement.notify_transport_eof();
        RUVIA_CHECK(replacement.abort() == websocket_abort_disposition::abort_transport);
        RUVIA_CHECK_EQ(retained_output, saved_output);
        RUVIA_CHECK_EQ(source_memory.live_bytes_, live_before_move);
    }
    RUVIA_CHECK_EQ(source_memory.live_bytes_, std::size_t{0});
    RUVIA_CHECK_EQ(source_memory.allocations_, source_memory.deallocations_);
    RUVIA_CHECK_EQ(replaced_memory.allocations_, replaced_memory.deallocations_);
}

RUVIA_TEST(ws_public_driver_releases_owned_memory_after_construction_failures) {
    counting_resource memory;
    {
        websocket_connection unused({.resource_ = &memory});
        RUVIA_CHECK(memory.live_bytes_ > 0);
    }
    RUVIA_CHECK_EQ(memory.live_bytes_, std::size_t{0});
    RUVIA_CHECK(ruvia::testing::throws_on([&] {
        websocket_connection invalid({.resource_ = &memory, .role_ = websocket_connection_role::client});
    }));
    RUVIA_CHECK_EQ(memory.live_bytes_, std::size_t{0});
    RUVIA_CHECK(ruvia::testing::throws_on([&] {
        websocket_connection invalid({.resource_ = &memory, .compression_ = websocket_compression{.enabled_ = true, .server_max_window_bits_ = 7}});
    }));
    RUVIA_CHECK_EQ(memory.live_bytes_, std::size_t{0});
    {
        std::pmr::string input(&memory);
        const auto input_bytes = memory.live_bytes_;
        {
            websocket_server_protocol unused(input);
            RUVIA_CHECK(memory.live_bytes_ > input_bytes);
        }
        RUVIA_CHECK_EQ(memory.live_bytes_, input_bytes);
        RUVIA_CHECK(ruvia::testing::throws_on([&] {
            websocket_server_protocol invalid(input, protocol_byte_limit::unlimited(),
                websocket_server_protocol_options{.compression_ = websocket_compression{.enabled_ = true, .server_max_window_bits_ = 7}});
        }));
        RUVIA_CHECK_EQ(memory.live_bytes_, input_bytes);
        RUVIA_CHECK(ruvia::testing::throws_on([&] {
            websocket_server_protocol invalid(input, protocol_byte_limit::unlimited(),
                websocket_server_protocol_options{.compression_level_ = 10});
        }));
        RUVIA_CHECK_EQ(memory.live_bytes_, input_bytes);
    }
    memory.reject_ = true;
    RUVIA_CHECK(ruvia::testing::throws_on([&] {
        websocket_connection rejected({.resource_ = &memory});
    }));
    RUVIA_CHECK_EQ(memory.live_bytes_, std::size_t{0});
    RUVIA_CHECK_EQ(memory.allocations_, memory.deallocations_);
}

RUVIA_TEST(ws_default_message_limit_rejects_oversized_declared_frame_before_payload) {
    // A masked binary frame declaring 16 MiB + 1 bytes requires no payload to
    // establish that it cannot fit the finite default message policy.
    const std::string header_value("\x82\xff\x00\x00\x00\x00\x01\x00\x00\x01\x00\x00\x00\x00", 14);
    websocket_connection connection;
    RUVIA_CHECK(connection.feed(header_value) == websocket_feed_status::accepted);
    const auto event = connection.next_event();
    RUVIA_CHECK(event && event->protocol_error());
    RUVIA_CHECK_EQ(event->protocol_error()->close_code(), std::uint16_t{1009});
    std::pmr::string input(header_value);
    websocket_server_protocol protocol(input);
    const auto borrowed_event = protocol.poll();
    RUVIA_CHECK(borrowed_event && borrowed_event->protocol_error());
    RUVIA_CHECK_EQ(borrowed_event->protocol_error()->close_code(), std::uint16_t{1009});
}

RUVIA_TEST(ws_owned_input_backpressure_does_not_copy_rejected_bytes) {
    counting_resource resource;
    websocket_connection connection({.resource_ = &resource, .max_buffered_input_bytes_ = 16});
    const auto baseline = resource.live_bytes_;
    const std::string oversized(17, 'x');
    RUVIA_CHECK(connection.feed(oversized) == websocket_feed_status::backpressured);
    RUVIA_CHECK_EQ(resource.live_bytes_, baseline);
    const std::string ping("\x89\x81\x00\x00\x00\x00p", 7);
    RUVIA_CHECK(connection.feed(ping) == websocket_feed_status::accepted);
    const auto event = connection.next_event();
    RUVIA_CHECK(event && event->ping());
    RUVIA_CHECK_EQ(event->ping()->payload(), "p");
    (void)connection.consume_output(connection.output_plan().bytes().size());
    RUVIA_CHECK(!connection.next_event());
    RUVIA_CHECK(connection.feed(ping) == websocket_feed_status::accepted);
    RUVIA_CHECK(connection.next_event()->ping() != nullptr);
}

RUVIA_TEST(ws_owned_input_bound_excludes_parsed_prefix) {
    websocket_connection connection({.max_buffered_input_bytes_ = 32});
    const std::string ping("\x89\x81\x00\x00\x00\x00p", 7);
    std::string binary("\x82\x94\x00\x00\x00\x00", 6);
    binary.append(20, 'b');
    const auto first_chunk = ping + binary.substr(0, 10);
    RUVIA_CHECK(connection.feed(first_chunk) == websocket_feed_status::accepted);
    const auto ping_event = connection.next_event();
    RUVIA_CHECK(ping_event && ping_event->ping());
    (void)connection.consume_output(connection.output_plan().bytes().size());
    RUVIA_CHECK(!connection.next_event());
    // 16 pending + 10 buffered frame bytes fit the 32-byte bound once the
    // already-delivered Ping no longer occupies the buffer.
    RUVIA_CHECK(connection.feed(std::string_view(binary).substr(10)) == websocket_feed_status::accepted);
    const auto message_event = connection.next_event();
    RUVIA_CHECK(message_event && message_event->message());
    if (message_event && message_event->message()) {
        RUVIA_CHECK_EQ(message_event->message()->payload(), std::string(20, 'b'));
    }
}
