#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <new>
#include <optional>
#include <string>
#include <string_view>

#include "ruvia/http/http_io_buffer.h"
#include "ruvia/http/http_pmr_string.h"
#include "ruvia/http/protocol_byte_limit.h"
#include "ruvia/http/websocket_connection.h"
#include "ruvia/http/websocket_deflate_codecs.h"

#include "stl_allocation_fixture.h"
#include "test_harness.h"

namespace {
using namespace ruvia;

class counting_resource final : public std::pmr::memory_resource {
public:
    std::size_t allocations_{0};
    std::size_t live_bytes_{0};
    bool reject_{false};

private:
    void* do_allocate(std::size_t bytes_value, std::size_t alignment) override {
        if (reject_) {
            throw std::bad_alloc();
        }
        ++allocations_;
        live_bytes_ += bytes_value;
        return std::pmr::new_delete_resource()->allocate(bytes_value, alignment);
    }

    void do_deallocate(void* pointer, std::size_t bytes_value, std::size_t alignment) override {
        live_bytes_ -= bytes_value;
        std::pmr::new_delete_resource()->deallocate(pointer, bytes_value, alignment);
    }

    [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
};

bool zero_mask(void*, websocket_mask_key_type& key) noexcept {
    key = {};
    return true;
}

// Compressible text that is not a single repeated byte.
[[nodiscard]] std::string sample_message(std::size_t size, unsigned seed) {
    std::string message(size, '\0');
    unsigned state_value = seed * 2654435761U + 7U;
    for (std::size_t i = 0; i < size; ++i) {
        state_value = state_value * 1103515245U + 12345U;
        message[i] = static_cast<char>('a' + ((state_value >> 16U) % 5U) + ((i / 32U) % 4U));
    }
    return message;
}

// Bytes DEFLATE cannot shrink.
[[nodiscard]] std::string noise_message(std::size_t size) {
    std::string message(size, '\0');
    std::uint32_t state_value = 1234567;
    for (auto& byte : message) {
        state_value ^= state_value << 13;
        state_value ^= state_value >> 17;
        state_value ^= state_value << 5;
        byte = static_cast<char>(state_value);
    }
    return message;
}

[[nodiscard]] websocket_connection client(websocket_compression compression) {
    return websocket_connection({.message_limit_ = protocol_byte_limit::unlimited(),
        .compression_ = compression,
        .role_ = websocket_connection_role::client,
        .mask_key_generator_ = &zero_mask});
}

// Takes every pending output byte of a sender.
template <typename sender_type>
[[nodiscard]] std::string drain(ruvia::testing::test_context& ruvia_ctx, sender_type& sender) {
    const auto plan = sender.output_plan();
    std::string bytes_value(plan.bytes());
    bytes_value.append(plan.borrowed_bytes());
    RUVIA_CHECK(sender.consume_output(bytes_value.size()) == websocket_output_consume_status::drained);
    return bytes_value;
}

[[nodiscard]] bool compressed(std::string_view frame) noexcept {
    return !frame.empty() && (static_cast<unsigned char>(frame[0]) & 0x40U) != 0;
}

// Delivers one client frame to a server protocol and returns its message.
[[nodiscard]] std::optional<std::string> receive(
    ruvia::testing::test_context& ruvia_ctx, http_io_buffer& input, websocket_connection& protocol, std::string_view wire) {
    input.append(wire);
    const auto event = protocol.next_event();
    RUVIA_CHECK(event && event->message());
    if (!event || !event->message()) {
        return std::nullopt;
    }
    return std::string(event->message()->payload());
}

struct server_endpoint final {
    explicit server_endpoint(websocket_connection_options options)
        : input_(&memory_),
          protocol_(input_, [&] {
              options.message_limit_ = protocol_byte_limit::unlimited();
              return options;
          }()) {}

    counting_resource memory_;
    http_io_buffer input_;
    websocket_connection protocol_;
};

}  // namespace

// A direction without context takeover produces the same wire bytes whether
// it borrows the shared codecs or owns a per-connection codec, and with the
// shared codecs the connection's resource never holds codec state.
RUVIA_TEST(ws_shared_deflate_matches_private_no_context_takeover_bytes) {
    const std::string messages[]{sample_message(600, 1), sample_message(70'000, 2), std::string(20, 'a'),
        std::string(4096, 'x'), sample_message(3000, 3)};
    for (const int window : {8, 11, 15}) {
        for (const int level : {1, 6, 9}) {
            const websocket_compression compression{.enabled_ = true, .server_max_window_bits_ = window};
            counting_resource codec_memory;
            websocket_deflate_codecs codecs(&codec_memory);
            server_endpoint shared({.compression_ = compression, .compression_level_ = level, .shared_deflate_ = &codecs});
            server_endpoint owned({.compression_ = compression, .compression_level_ = level});
            auto peer = client(compression);
            for (const auto& message : messages) {
                RUVIA_CHECK(shared.protocol_.submit_frame(websocket_opcode::text, message) == websocket_frame_submit_status::accepted);
                RUVIA_CHECK(owned.protocol_.submit_frame(websocket_opcode::text, message) == websocket_frame_submit_status::accepted);
                const auto shared_wire = drain(ruvia_ctx, shared.protocol_);
                RUVIA_CHECK_EQ(shared_wire, drain(ruvia_ctx, owned.protocol_));
                RUVIA_CHECK(compressed(shared_wire));
                RUVIA_CHECK(peer.feed(shared_wire) == websocket_feed_status::accepted);
                const auto event = peer.next_event();
                RUVIA_CHECK(event && event->message() && event->message()->payload() == message);

                RUVIA_CHECK(peer.submit_frame(websocket_opcode::text, message) == websocket_frame_submit_status::accepted);
                const auto wire = drain(ruvia_ctx, peer);
                RUVIA_CHECK(receive(ruvia_ctx, shared.input_, shared.protocol_, wire) == message);
                RUVIA_CHECK(receive(ruvia_ctx, owned.input_, owned.protocol_, wire) == message);
            }
            // Both connections hold the same buffers; only the private one
            // also holds codec state, at least the hash heads and pending
            // buffer of 64 KiB each.
            RUVIA_CHECK(owned.memory_.live_bytes_ >= shared.memory_.live_bytes_ + std::size_t{64} * 1024);
            RUVIA_CHECK(codec_memory.allocations_ > 0);
        }
    }
}

// A message that fails inside the shared codecs leaves them reset: the
// connection that failed and every other borrower keep producing the bytes of
// a fresh codec.
RUVIA_TEST(ws_shared_deflate_resets_after_a_failed_connection_message) {
    const websocket_compression compression{.enabled_ = true};
    websocket_deflate_codecs codecs(std::pmr::new_delete_resource());
    server_endpoint failing({.compression_ = compression, .shared_deflate_ = &codecs});
    server_endpoint other({.compression_ = compression, .shared_deflate_ = &codecs});
    const auto message = sample_message(200'000, 4);

    // The compressed output outgrows its buffer mid-message.
    failing.memory_.reject_ = true;
    RUVIA_CHECK(ruvia::testing::throws_on([&] {
        static_cast<void>(failing.protocol_.submit_frame(websocket_opcode::binary, message));
    }));
    failing.memory_.reject_ = false;
    RUVIA_CHECK(failing.protocol_.liveness_mode() == websocket_liveness_mode::open);
    RUVIA_CHECK(failing.protocol_.output_plan().bytes().empty());

    server_endpoint reference({.compression_ = compression});
    RUVIA_CHECK(reference.protocol_.submit_frame(websocket_opcode::binary, message) == websocket_frame_submit_status::accepted);
    const auto expected = drain(ruvia_ctx, reference.protocol_);
    RUVIA_CHECK(other.protocol_.submit_frame(websocket_opcode::binary, message) == websocket_frame_submit_status::accepted);
    RUVIA_CHECK_EQ(drain(ruvia_ctx, other.protocol_), expected);
    RUVIA_CHECK(failing.protocol_.submit_frame(websocket_opcode::binary, message) == websocket_frame_submit_status::accepted);
    RUVIA_CHECK_EQ(drain(ruvia_ctx, failing.protocol_), expected);

    // A corrupt compressed message fails only its own connection.
    const std::string garbage("\xc2\x84\x00\x00\x00\x00\xff\xff\xff\xff", 10);
    failing.input_.append(garbage);
    const auto failure = failing.protocol_.next_event();
    RUVIA_CHECK(failure && failure->protocol_error() && failure->protocol_error()->close_code() == 1002);
    auto peer = client(compression);
    RUVIA_CHECK(peer.submit_frame(websocket_opcode::binary, message) == websocket_frame_submit_status::accepted);
    RUVIA_CHECK(receive(ruvia_ctx, other.input_, other.protocol_, drain(ruvia_ctx, peer)) == message);
}

// After one large message in each direction, every connection buffer goes
// back to at most websocket_retained_buffer_bytes once it is drained.
RUVIA_TEST(ws_connection_buffers_keep_bounded_storage_after_a_large_message) {
    const websocket_compression compression{.enabled_ = true};
    websocket_deflate_codecs codecs(std::pmr::new_delete_resource());
    server_endpoint server({.compression_ = compression, .shared_deflate_ = &codecs});
    auto peer = client(compression);
    const auto exchange = [&](const std::string& inbound, bool compress_inbound, const std::string& outbound) {
        RUVIA_CHECK(peer.submit_frame(websocket_opcode::binary, inbound, compress_inbound) == websocket_frame_submit_status::accepted);
        RUVIA_CHECK(receive(ruvia_ctx, server.input_, server.protocol_, drain(ruvia_ctx, peer)) == inbound);
        RUVIA_CHECK(server.protocol_.submit_frame(websocket_opcode::binary, outbound) == websocket_frame_submit_status::accepted);
        const auto wire = drain(ruvia_ctx, server.protocol_);
        RUVIA_CHECK(peer.feed(wire) == websocket_feed_status::accepted);
        const auto event = peer.next_event();
        RUVIA_CHECK(event && event->message() && event->message()->payload() == outbound);
        // Releases the delivered message and its input.
        RUVIA_CHECK(!server.protocol_.next_event().has_value());
    };

    const auto small = sample_message(32, 5);
    exchange(small, true, small);
    const auto baseline = server.memory_.live_bytes_;
    // 64 KiB through each buffer: raw input, inflated input, an output frame
    // and an outbound trial compression that did not shrink the payload.
    const auto large = std::size_t{64} * 1024;
    exchange(noise_message(large), false, noise_message(large));
    exchange(sample_message(large, 6), true, sample_message(large, 7));
    RUVIA_CHECK(server.memory_.live_bytes_ <= baseline + 4 * websocket_retained_buffer_bytes);

    // Messages within the bound reuse the retained storage.
    const auto medium = sample_message(8 * 1024, 8);
    exchange(medium, false, noise_message(8 * 1024));
    const auto allocations = server.memory_.allocations_;
    for (int round = 0; round < 4; ++round) {
        exchange(medium, false, noise_message(8 * 1024));
    }
    if constexpr (!ruvia::test::stl_moves_allocate_debug_metadata) {
        RUVIA_CHECK_EQ(server.memory_.allocations_, allocations);
    }
}

// Context takeover keeps a private dictionary per direction even when shared
// codecs are available; a direction without takeover still uses the shared
// ones. Private codecs exist only once a compressed message needs them.
RUVIA_TEST(ws_takeover_directions_keep_private_codecs_beside_shared_ones) {
    const auto message = sample_message(2000, 9);
    for (const bool server_takeover : {true, false}) {
        const websocket_compression compression{.enabled_ = true,
            .server_no_context_takeover_ = !server_takeover,
            .client_no_context_takeover_ = false};
        counting_resource codec_memory;
        websocket_deflate_codecs codecs(&codec_memory);
        server_endpoint server({.compression_ = compression, .shared_deflate_ = &codecs});
        auto peer = client(compression);
        const auto idle_bytes = server.memory_.live_bytes_;

        std::size_t first_size = 0;
        for (int round = 0; round < 3; ++round) {
            RUVIA_CHECK(server.protocol_.submit_frame(websocket_opcode::text, message) == websocket_frame_submit_status::accepted);
            const auto wire = drain(ruvia_ctx, server.protocol_);
            RUVIA_CHECK(compressed(wire));
            if (round == 0) {
                first_size = wire.size();
            } else {
                // Only a kept dictionary shrinks a repeated message.
                RUVIA_CHECK(server_takeover ? wire.size() < first_size : wire.size() == first_size);
            }
            RUVIA_CHECK(peer.feed(wire) == websocket_feed_status::accepted);
            const auto event = peer.next_event();
            RUVIA_CHECK(event && event->message() && event->message()->payload() == message);

            RUVIA_CHECK(peer.submit_frame(websocket_opcode::text, message) == websocket_frame_submit_status::accepted);
            RUVIA_CHECK(receive(ruvia_ctx, server.input_, server.protocol_, drain(ruvia_ctx, peer)) == message);
        }
        // The private inflater (and deflater with server takeover) came from
        // the connection; the shared codecs serve only a no-takeover sender.
        RUVIA_CHECK(server.memory_.live_bytes_ > idle_bytes + std::size_t{32} * 1024);
        RUVIA_CHECK((codec_memory.allocations_ != 0) == !server_takeover);
    }
}

// memLevel sizes only a private takeover deflater: 4 takes 120 KiB less of
// zlib state than 8, and both stay interoperable.
RUVIA_TEST(ws_takeover_deflater_uses_the_configured_mem_level) {
    const websocket_compression compression{.enabled_ = true,
        .server_no_context_takeover_ = false,
        .client_no_context_takeover_ = false};
    const auto message = sample_message(5000, 10);
    std::size_t codec_bytes[2]{};
    for (const int mem_level : {8, 4}) {
        server_endpoint server({.compression_ = compression, .mem_level_ = mem_level});
        const auto idle_bytes = server.memory_.live_bytes_;
        auto peer = client(compression);
        for (int round = 0; round < 2; ++round) {
            RUVIA_CHECK(server.protocol_.submit_frame(websocket_opcode::text, message) == websocket_frame_submit_status::accepted);
            const auto wire = drain(ruvia_ctx, server.protocol_);
            RUVIA_CHECK(peer.feed(wire) == websocket_feed_status::accepted);
            const auto event = peer.next_event();
            RUVIA_CHECK(event && event->message() && event->message()->payload() == message);
        }
        codec_bytes[mem_level == 8 ? 0 : 1] = server.memory_.live_bytes_ - idle_bytes;
    }
    RUVIA_CHECK(codec_bytes[0] >= codec_bytes[1] + std::size_t{96} * 1024);
}

RUVIA_TEST(ws_deflate_mem_level_is_validated) {
    for (const int mem_level : {0, 10}) {
        RUVIA_CHECK(ruvia::testing::throws_on([&] {
            websocket_connection invalid({.mem_level_ = mem_level});
        }));
        http_io_buffer input;
        RUVIA_CHECK(ruvia::testing::throws_on([&] {
            websocket_connection invalid(input, {.message_limit_ = protocol_byte_limit::unlimited(), .mem_level_ = mem_level});
        }));
    }
    for (const int mem_level : {1, 9}) {
        websocket_connection valid({.compression_ = {.enabled_ = true}, .mem_level_ = mem_level});
        RUVIA_CHECK(valid.submit_frame(websocket_opcode::text, std::string(1000, 'v')) == websocket_frame_submit_status::accepted);
    }
}

RUVIA_TEST(ws_server_protocol_reports_bytes_missing_from_a_partial_frame) {
    auto peer = client({});
    const auto message = sample_message(70'000, 11);
    RUVIA_CHECK(peer.submit_frame(websocket_opcode::binary, message) == websocket_frame_submit_status::accepted);
    const auto wire = drain(ruvia_ctx, peer);
    http_io_buffer input;
    websocket_connection protocol(input, {.message_limit_ = protocol_byte_limit::unlimited()});
    RUVIA_CHECK_EQ(protocol.needed_input_bytes(), std::size_t{0});
    // 2 + 8 length + 4 mask header bytes; the length is not complete yet.
    input.append(std::string_view(wire).substr(0, 5));
    RUVIA_CHECK(!protocol.next_event().has_value());
    RUVIA_CHECK_EQ(protocol.needed_input_bytes(), std::size_t{0});
    input.append(std::string_view(wire).substr(5, 995));
    RUVIA_CHECK(!protocol.next_event().has_value());
    RUVIA_CHECK_EQ(protocol.needed_input_bytes(), wire.size() - 1000);
    input.append(std::string_view(wire).substr(1000));
    const auto event = protocol.next_event();
    RUVIA_CHECK(event && event->message() && event->message()->payload() == message);
    RUVIA_CHECK_EQ(protocol.needed_input_bytes(), std::size_t{0});
}
