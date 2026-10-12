#include <cstddef>
#include <memory_resource>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>

#include "ruvia/http/protocol_byte_limit.h"
#include "ruvia/http/websocket_connection.h"
#include "ruvia/http/websocket_deflate_codecs.h"

#include "failing_memory_resource.h"
#include "test_harness.h"

namespace {
using namespace ruvia;

class counting_resource final : public std::pmr::memory_resource {
public:
    std::size_t allocations_{0};
    std::size_t live_bytes_{0};

private:
    void* do_allocate(std::size_t bytes_value, std::size_t alignment) override {
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

[[nodiscard]] std::string sample_message(std::size_t size, unsigned seed) {
    std::string message(size, '\0');
    unsigned state_value = seed * 2654435761U + 7U;
    for (std::size_t i = 0; i < size; ++i) {
        state_value = state_value * 1103515245U + 12345U;
        message[i] = static_cast<char>('a' + ((state_value >> 16U) % 5U) + ((i / 32U) % 4U));
    }
    return message;
}

[[nodiscard]] std::string deflate(ruvia::testing::test_context& ruvia_ctx, websocket_deflate_codecs& codecs, std::string_view payload, int level, int window) {
    std::pmr::string out;
    RUVIA_CHECK(codecs.deflate_message(payload, out, level, window));
    return std::string(out);
}

[[nodiscard]] std::string inflate(ruvia::testing::test_context& ruvia_ctx, websocket_deflate_codecs& codecs, std::string_view payload) {
    std::pmr::string out;
    RUVIA_CHECK(codecs.inflate_message(payload, out, protocol_byte_limit::unlimited()) ==
                websocket_inflate_result::ok);
    return std::string(out);
}

// The payload of the one unmasked server data frame in `frame`; empty unless
// it carries RSV1 (a compressed message).
[[nodiscard]] std::string compressed_frame_payload(std::string_view frame) {
    if (frame.size() < 2 || (static_cast<unsigned char>(frame[0]) & 0x40U) == 0) {
        return {};
    }
    const auto length_code = static_cast<unsigned char>(frame[1]) & 0x7FU;
    const std::size_t header_bytes = length_code == 126U ? 4 : length_code == 127U ? 10
                                                                                   : 2;
    return std::string(frame.substr(header_bytes));
}

}  // namespace

RUVIA_TEST(websocket_deflate_codecs_match_per_connection_no_context_takeover_bytes) {
    for (int window = 8; window <= 15; ++window) {
        for (int level = 1; level <= 9; ++level) {
            websocket_deflate_codecs codecs(std::pmr::new_delete_resource());
            websocket_connection connection({
                .compression_ = (websocket_compression{.enabled_ = true,
                    .server_max_window_bits_ = window,
                    .client_max_window_bits_ = window}),
                .compression_level_ = level,
            });
            for (unsigned message = 0; message < 3; ++message) {
                const auto payload = sample_message(message == 1 ? 70'000 : 600, message + 1);
                RUVIA_CHECK(connection.submit_frame(websocket_opcode::binary, payload) ==
                            websocket_frame_submit_status::accepted);
                const auto plan = connection.output_plan();
                const auto expected = compressed_frame_payload(plan.bytes());
                RUVIA_CHECK(!expected.empty());
                RUVIA_CHECK(connection.consume_output(plan.size()) == websocket_output_consume_status::drained);

                const auto actual = deflate(ruvia_ctx, codecs, payload, level, window);
                RUVIA_CHECK(actual == expected);
                RUVIA_CHECK(inflate(ruvia_ctx, codecs, actual) == payload);
            }
        }
    }
}

RUVIA_TEST(websocket_deflate_codecs_round_trip_every_level_and_window) {
    websocket_deflate_codecs codecs(std::pmr::new_delete_resource());
    for (int window = 8; window <= 15; ++window) {
        for (int level = 0; level <= 9; ++level) {
            for (const auto size : {std::size_t{0}, std::size_t{1}, std::size_t{5000}}) {
                const auto payload = sample_message(size, static_cast<unsigned>(window * 10 + level));
                const auto compressed = deflate(ruvia_ctx, codecs, payload, level, window);
                RUVIA_CHECK(!compressed.empty());
                RUVIA_CHECK(inflate(ruvia_ctx, codecs, compressed) == payload);
            }
        }
    }
}

RUVIA_TEST(websocket_deflate_codecs_append_to_existing_output) {
    websocket_deflate_codecs codecs(std::pmr::new_delete_resource());
    const auto payload = sample_message(2000, 3);
    const auto compressed = deflate(ruvia_ctx, codecs, payload, 6, 15);

    std::pmr::string out("prefix");
    RUVIA_CHECK(codecs.deflate_message(payload, out, 6, 15));
    RUVIA_CHECK(std::string_view(out) == "prefix" + compressed);

    std::pmr::string inflated("prefix");
    RUVIA_CHECK(codecs.inflate_message(compressed, inflated, protocol_byte_limit::limited(payload.size())) ==
                websocket_inflate_result::ok);
    RUVIA_CHECK(std::string_view(inflated) == "prefix" + payload);
}

RUVIA_TEST(websocket_deflate_codecs_reset_after_failed_messages) {
    websocket_deflate_codecs reference(std::pmr::new_delete_resource());
    websocket_deflate_codecs codecs(std::pmr::new_delete_resource());
    const auto large = sample_message(200'000, 5);
    const auto next = sample_message(3000, 6);
    const auto expected = deflate(ruvia_ctx, reference, next, 6, 15);

    // An output allocation failure abandons the deflater mid-message.
    failing_memory_resource output_resource;
    output_resource.fail_after(0);
    std::pmr::string failed_out(&output_resource);
    bool threw = false;
    try {
        static_cast<void>(codecs.deflate_message(large, failed_out, 6, 15));
    } catch (const std::bad_alloc&) {
        threw = true;
    }
    RUVIA_CHECK(threw);
    RUVIA_CHECK(deflate(ruvia_ctx, codecs, next, 6, 15) == expected);

    // A truncated message leaves the inflater inside a block.
    const auto compressed_large = deflate(ruvia_ctx, codecs, large, 6, 15);
    std::pmr::string out;
    RUVIA_CHECK(codecs.inflate_message(std::string_view(compressed_large).substr(0, compressed_large.size() / 2),
                    out, protocol_byte_limit::unlimited()) == websocket_inflate_result::error);
    RUVIA_CHECK(inflate(ruvia_ctx, codecs, expected) == next);

    out.clear();
    RUVIA_CHECK(codecs.inflate_message("\xff\xff\xff", out, protocol_byte_limit::unlimited()) ==
                websocket_inflate_result::error);
    RUVIA_CHECK(inflate(ruvia_ctx, codecs, expected) == next);

    out.clear();
    RUVIA_CHECK(codecs.inflate_message(compressed_large, out, protocol_byte_limit::limited(large.size() - 1)) ==
                websocket_inflate_result::too_large);
    RUVIA_CHECK(inflate(ruvia_ctx, codecs, expected) == next);
}

RUVIA_TEST(websocket_deflate_codecs_inflate_messages_from_every_sender_window) {
    websocket_deflate_codecs sender(std::pmr::new_delete_resource());
    websocket_deflate_codecs receiver(std::pmr::new_delete_resource());
    for (int window = 8; window <= 15; ++window) {
        const auto payload = sample_message(40'000, static_cast<unsigned>(window));
        RUVIA_CHECK(inflate(ruvia_ctx, receiver, deflate(ruvia_ctx, sender, payload, 6, window)) == payload);
    }
}

RUVIA_TEST(websocket_deflate_codecs_allocate_once_per_codec_from_their_resource) {
    counting_resource resource;
    {
        websocket_deflate_codecs codecs(&resource);
        RUVIA_CHECK_EQ(resource.allocations_, 0U);
        const auto payload = sample_message(10'000, 9);

        const auto compressed = deflate(ruvia_ctx, codecs, payload, 6, 15);
        RUVIA_CHECK(inflate(ruvia_ctx, codecs, compressed) == payload);
        const auto warm_allocations = resource.allocations_;
        RUVIA_CHECK(warm_allocations != 0);
        for (int message = 0; message < 4; ++message) {
            RUVIA_CHECK(deflate(ruvia_ctx, codecs, payload, 6, 15) == compressed);
            RUVIA_CHECK(inflate(ruvia_ctx, codecs, compressed) == payload);
        }
        RUVIA_CHECK_EQ(resource.allocations_, warm_allocations);

        // Another level or window gets its own deflater; the inflater is shared.
        RUVIA_CHECK(inflate(ruvia_ctx, codecs, deflate(ruvia_ctx, codecs, payload, 1, 10)) == payload);
        RUVIA_CHECK(resource.allocations_ > warm_allocations);
    }
    RUVIA_CHECK_EQ(resource.live_bytes_, 0U);
}

RUVIA_TEST(websocket_deflate_codecs_recover_from_codec_allocation_failure) {
    failing_memory_resource resource;
    {
        websocket_deflate_codecs codecs(&resource);
        const auto payload = sample_message(1000, 4);
        resource.fail_after(0);
        bool threw = false;
        try {
            static_cast<void>(deflate(ruvia_ctx, codecs, payload, 6, 15));
        } catch (const std::bad_alloc&) {
            threw = true;
        }
        RUVIA_CHECK(threw);
        RUVIA_CHECK_EQ(resource.live_allocations(), 0U);
        RUVIA_CHECK(inflate(ruvia_ctx, codecs, deflate(ruvia_ctx, codecs, payload, 6, 15)) == payload);
    }
    RUVIA_CHECK_EQ(resource.live_allocations(), 0U);
}

RUVIA_TEST(websocket_deflate_codecs_reject_invalid_parameters) {
    websocket_deflate_codecs codecs(nullptr);
    std::pmr::string out;
    RUVIA_CHECK(ruvia::testing::throws_on([&] { static_cast<void>(codecs.deflate_message("x", out, 6, 7)); }));
    RUVIA_CHECK(ruvia::testing::throws_on([&] { static_cast<void>(codecs.deflate_message("x", out, 6, 16)); }));
    RUVIA_CHECK(ruvia::testing::throws_on([&] { static_cast<void>(codecs.deflate_message("x", out, -1, 15)); }));
    RUVIA_CHECK(ruvia::testing::throws_on([&] { static_cast<void>(codecs.deflate_message("x", out, 10, 15)); }));
    RUVIA_CHECK(out.empty());
}
