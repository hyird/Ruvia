#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <variant>

#include "ruvia/http/protocol_byte_limit.h"
#include "ruvia/http/websocket_protocol.h"

#include "test_harness.h"
#include "websocket/http_websocket_frame_codec.h"
#include "websocket/http_websocket_frame_reader.h"

namespace {

using ruvia::protocol_byte_limit;
using ruvia::websocket_opcode;

}  // namespace

RUVIA_TEST(websocket_raw_opcode_validity) {
    using ruvia::detail::is_invalid_websocket_raw_opcode;
    // Valid: 0x0 continuation, 0x1 text, 0x2 binary, 0x8-0xA control (RFC 6455 5.2).
    RUVIA_CHECK(!is_invalid_websocket_raw_opcode(0x0));
    RUVIA_CHECK(!is_invalid_websocket_raw_opcode(0x1));
    RUVIA_CHECK(!is_invalid_websocket_raw_opcode(0x2));
    RUVIA_CHECK(!is_invalid_websocket_raw_opcode(0x8));
    RUVIA_CHECK(!is_invalid_websocket_raw_opcode(0x9));
    RUVIA_CHECK(!is_invalid_websocket_raw_opcode(0xA));
    // Reserved non-control 0x3-0x7 and reserved control 0xB-0xF are invalid.
    for (std::uint8_t op = 0x3; op <= 0x7; ++op) {
        RUVIA_CHECK(is_invalid_websocket_raw_opcode(op));
    }
    for (std::uint8_t op = 0xB; op <= 0xF; ++op) {
        RUVIA_CHECK(is_invalid_websocket_raw_opcode(op));
    }
}

RUVIA_TEST(websocket_control_opcode_classification) {
    using ruvia::detail::is_websocket_control_opcode;
    RUVIA_CHECK(!is_websocket_control_opcode(websocket_opcode::text));
    RUVIA_CHECK(!is_websocket_control_opcode(websocket_opcode::binary));
    RUVIA_CHECK(is_websocket_control_opcode(websocket_opcode::close));
    RUVIA_CHECK(is_websocket_control_opcode(websocket_opcode::ping));
    RUVIA_CHECK(is_websocket_control_opcode(websocket_opcode::pong));
}

RUVIA_TEST(websocket_frame_message_limit_exempts_control_frames) {
    using ruvia::detail::websocket_frame_exceeds_message_limit;
    using ruvia::detail::websocket_frame_kind;
    // Data frames are measured against the per-message size limit.
    const auto limit = protocol_byte_limit::limited(64);
    RUVIA_CHECK(websocket_frame_exceeds_message_limit(websocket_frame_kind::text, 100, limit));
    RUVIA_CHECK(websocket_frame_exceeds_message_limit(websocket_frame_kind::binary, 100, limit));
    RUVIA_CHECK(!websocket_frame_exceeds_message_limit(websocket_frame_kind::text, 50, limit));

    // Control frames (Close/Ping/Pong) are capped at 125 by RFC 6455 5.5 and are
    // NOT subject to the message-size limit: a 100-byte Ping, or a Close carrying a
    // reason phrase, must pass even when max_message_bytes_ is 64.
    RUVIA_CHECK(!websocket_frame_exceeds_message_limit(websocket_frame_kind::ping, 100, limit));
    RUVIA_CHECK(!websocket_frame_exceeds_message_limit(websocket_frame_kind::pong, 100, limit));
    RUVIA_CHECK(!websocket_frame_exceeds_message_limit(websocket_frame_kind::close, 100, limit));
    RUVIA_CHECK(!websocket_frame_exceeds_message_limit(
        websocket_frame_kind::text, 1'000'000, protocol_byte_limit::unlimited()));
}

RUVIA_TEST(websocket_message_size_limits) {
    using ruvia::detail::websocket_append_exceeds_limit;
    using ruvia::detail::websocket_message_exceeds_limit;
    const auto limit = protocol_byte_limit::limited(100);
    RUVIA_CHECK(!websocket_message_exceeds_limit(1'000'000, protocol_byte_limit::unlimited()));
    RUVIA_CHECK(!websocket_message_exceeds_limit(100, limit));  // exact fit is allowed
    RUVIA_CHECK(websocket_message_exceeds_limit(101, limit));

    // Append accounting is overflow-safe (uses subtraction, never current+append).
    RUVIA_CHECK(!websocket_append_exceeds_limit(70, 30, limit));  // 70+30 == 100 ok
    RUVIA_CHECK(websocket_append_exceeds_limit(71, 30, limit));   // 71+30 > 100
    RUVIA_CHECK(websocket_append_exceeds_limit(0, 101, limit));   // single append over limit
    RUVIA_CHECK(!websocket_append_exceeds_limit(1'000'000, 1, protocol_byte_limit::unlimited()));
    constexpr auto max_value = (std::numeric_limits<std::size_t>::max)();
    RUVIA_CHECK(websocket_append_exceeds_limit(max_value - 10, 20, limit));  // no wraparound
}

RUVIA_TEST(websocket_frame_length_and_read_overflow_guards) {
    using ruvia::detail::websocket_frame_length_exceeds_limit;
    using ruvia::detail::websocket_masked_frame_read_size_overflows;
    constexpr auto u64_max = (std::numeric_limits<std::uint64_t>::max)();

    const auto limit = protocol_byte_limit::limited(1000);
    RUVIA_CHECK(!websocket_frame_length_exceeds_limit(100, limit));
    RUVIA_CHECK(websocket_frame_length_exceeds_limit(2000, limit));
    RUVIA_CHECK(!websocket_frame_length_exceeds_limit(2000, protocol_byte_limit::unlimited()));
    // A declared 64-bit length beyond any addressable size is over the limit.
    RUVIA_CHECK(websocket_frame_length_exceeds_limit(u64_max, limit));

    // header + 4-byte mask + payload must not overflow size_t.
    RUVIA_CHECK(!websocket_masked_frame_read_size_overflows(100, 14));
    RUVIA_CHECK(websocket_masked_frame_read_size_overflows(u64_max, 14));
}

RUVIA_TEST(websocket_integer_readers) {
    using ruvia::detail::read_websocket_uint16;
    using ruvia::detail::read_websocket_uint64;

    const char be16[] = {static_cast<char>(0x12), static_cast<char>(0x34)};
    RUVIA_CHECK_EQ(read_websocket_uint16(be16), std::uint16_t{0x1234});

    // 64-bit big-endian, most-significant bit clear -> 0x0100 = 256.
    constexpr char be64[] = {0, 0, 0, 0, 0, 0, static_cast<char>(0x01), 0};
    constexpr auto value = read_websocket_uint64(be64);
    RUVIA_CHECK((value.index() == 0));
    RUVIA_CHECK_EQ(std::get<0>(value), std::uint64_t{256});

    // The MSB of a 64-bit length must be 0 (RFC 6455 5.2); otherwise rejected.
    const char msb_set[] = {static_cast<char>(0x80), 0, 0, 0, 0, 0, 0, 0};
    const auto rejected = read_websocket_uint64(msb_set);
    RUVIA_CHECK(!(rejected.index() == 0));
    RUVIA_CHECK_EQ(std::get<1>(rejected), ruvia::detail::websocket_protocol_failure::protocol_error);

    const char maximum[] = {0x7f, '\xff', '\xff', '\xff', '\xff', '\xff', '\xff', '\xff'};
    RUVIA_CHECK_EQ(std::get<0>(read_websocket_uint64(maximum)), (std::uint64_t{1} << 63) - 1);
    const char unaligned[] = {0, 0x01, 0x23, 0x45, 0x67, '\x89', '\xab', '\xcd', '\xef', 0};
    RUVIA_CHECK_EQ(std::get<0>(read_websocket_uint64(std::span<const char, 8>(unaligned + 1, 8))), std::uint64_t{0x0123456789abcdef});
}

RUVIA_TEST(websocket_read_buffer_compaction) {
    using ruvia::detail::compact_websocket_read_buffer;
    const auto make = [](std::string_view text) {
        std::pmr::string buffer(std::pmr::get_default_resource());
        buffer.assign(text.data(), text.size());
        return buffer;
    };

    // pending_compact_until == 0 -> no-op, offset untouched.
    {
        auto buffer = make("hello");
        std::size_t offset = 2;
        std::size_t pending = 0;
        compact_websocket_read_buffer(buffer, offset, pending);
        RUVIA_CHECK_EQ(std::string_view(buffer), std::string_view("hello"));
        RUVIA_CHECK_EQ(offset, std::size_t{2});
    }
    // Fully consumed -> clear the buffer.
    {
        auto buffer = make("hello");
        std::size_t offset = 0;
        std::size_t pending = 5;
        compact_websocket_read_buffer(buffer, offset, pending);
        RUVIA_CHECK(buffer.empty());
        RUVIA_CHECK_EQ(offset, std::size_t{0});
        RUVIA_CHECK_EQ(pending, std::size_t{0});
    }
    // Small prefix below half -> lazy: offset advances, bytes stay in place.
    {
        auto buffer = make("0123456789");
        std::size_t offset = 0;
        std::size_t pending = 3;
        compact_websocket_read_buffer(buffer, offset, pending);
        RUVIA_CHECK_EQ(std::string_view(buffer), std::string_view("0123456789"));
        RUVIA_CHECK_EQ(offset, std::size_t{3});
        RUVIA_CHECK_EQ(pending, std::size_t{0});
    }
    // Consumed prefix >= remaining -> compact: tail moves to the front.
    {
        auto buffer = make("PREFIXtail");
        std::size_t offset = 0;
        std::size_t pending = 6;
        compact_websocket_read_buffer(buffer, offset, pending);
        RUVIA_CHECK_EQ(std::string_view(buffer), std::string_view("tail"));
        RUVIA_CHECK_EQ(offset, std::size_t{0});
    }
}
