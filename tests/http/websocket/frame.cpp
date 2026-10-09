#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory_resource>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "ruvia/http/protocol_byte_limit.h"
#include "ruvia/http/websocket_protocol.h"

#include "test_harness.h"
#include "websocket/http_websocket_frame_codec.h"
#include "websocket/http_websocket_frame_reader.h"
#include "websocket/http_websocket_frame_view.h"

namespace {

using ruvia::protocol_byte_limit;
using ruvia::websocket_opcode;
using ruvia::detail::decode_masked_websocket_payload;
using ruvia::detail::decode_websocket_frame_start;
using ruvia::detail::encode_websocket_frame_header;
using ruvia::detail::is_invalid_websocket_control_frame;
using ruvia::detail::websocket_frame_header_type;
using ruvia::detail::websocket_frame_kind;
using ruvia::detail::websocket_frame_length_exceeds_limit;
using ruvia::detail::websocket_frame_read_result;
using ruvia::detail::websocket_frame_start;
using ruvia::detail::websocket_frame_view;
using ruvia::detail::websocket_protocol_failure;
using ruvia::detail::websocket_try_read_frame;

std::pmr::string masked_frame(unsigned char first, std::string_view payload_value) {
    constexpr std::array<unsigned char, 4> mask{0x12, 0x34, 0x56, 0x78};
    std::pmr::string bytes(std::pmr::get_default_resource());
    bytes.reserve(2 + mask.size() + payload_value.size());
    bytes.push_back(static_cast<char>(first));
    bytes.push_back(static_cast<char>(0x80U | payload_value.size()));
    for (const auto byte : mask) {
        bytes.push_back(static_cast<char>(byte));
    }
    for (std::size_t index = 0; index < payload_value.size(); ++index) {
        const auto byte = static_cast<unsigned char>(payload_value[index]);
        bytes.push_back(static_cast<char>(byte ^ mask[index & 3U]));
    }
    return bytes;
}

}  // namespace

// RFC 6455 §5.2: server-received client frames must be masked (second byte high
// bit set), RSV2/RSV3 must be clear, and reserved opcodes are rejected.
RUVIA_TEST(ws_frame_start_accepts_valid_masked_frames) {
    const auto text = decode_websocket_frame_start(0x81, 0x80, false);
    RUVIA_CHECK(text.has_value());
    RUVIA_CHECK(text->final());
    RUVIA_CHECK(!text->compressed());
    RUVIA_CHECK(text->kind() == websocket_frame_kind::text);
    const auto binary = decode_websocket_frame_start(0x82, 0x80, false);
    RUVIA_CHECK(binary->kind() == websocket_frame_kind::binary);
    const auto close = decode_websocket_frame_start(0x88, 0x80, false);
    RUVIA_CHECK(close->kind() == websocket_frame_kind::close);
    RUVIA_CHECK(decode_websocket_frame_start(0x89, 0x80, false)->kind() == websocket_frame_kind::ping);
    RUVIA_CHECK(decode_websocket_frame_start(0x8A, 0x80, false)->kind() == websocket_frame_kind::pong);
    const auto continuation = decode_websocket_frame_start(0x80, 0x80, false);
    RUVIA_CHECK(continuation->kind() == websocket_frame_kind::continuation);
    const auto fragment_start = decode_websocket_frame_start(0x01, 0x80, false);
    RUVIA_CHECK(!fragment_start->final());
}

RUVIA_TEST(ws_frame_start_rejects_malformed) {
    RUVIA_CHECK(!decode_websocket_frame_start(0x81, 0x00, false));  // not masked
    RUVIA_CHECK(!decode_websocket_frame_start(0x91, 0x80, false));  // RSV2 set
    RUVIA_CHECK(!decode_websocket_frame_start(0xA1, 0x80, false));  // RSV3 set
    RUVIA_CHECK(!decode_websocket_frame_start(0x83, 0x80, false));  // reserved opcode 0x3
    RUVIA_CHECK(!decode_websocket_frame_start(0x87, 0x80, false));  // reserved opcode 0x7
    RUVIA_CHECK(!decode_websocket_frame_start(0x8B, 0x80, false));  // reserved control 0xB
    RUVIA_CHECK(!decode_websocket_frame_start(0x8F, 0x80, false));  // reserved control 0xF
}

RUVIA_TEST(ws_frame_start_rsv1_rules) {
    // RSV1 (compression) is valid only on the first data frame when negotiated.
    RUVIA_CHECK(!decode_websocket_frame_start(0xC1, 0x80, false));
    const auto compressed = decode_websocket_frame_start(0xC1, 0x80, true);
    RUVIA_CHECK(compressed.has_value());
    RUVIA_CHECK(compressed->compressed());
    RUVIA_CHECK(!decode_websocket_frame_start(0xC0, 0x80, true));
    RUVIA_CHECK(!decode_websocket_frame_start(0xC8, 0x80, true));
}

RUVIA_TEST(ws_control_frame_rules) {
    const auto close = decode_websocket_frame_start(0x88, 0x80, false);
    RUVIA_CHECK(!is_invalid_websocket_control_frame(*close, 0));
    RUVIA_CHECK(!is_invalid_websocket_control_frame(*close, 125));
    RUVIA_CHECK(is_invalid_websocket_control_frame(*close, 126));
    const auto fragmented_ping = decode_websocket_frame_start(0x09, 0x80, false);
    RUVIA_CHECK(is_invalid_websocket_control_frame(*fragmented_ping, 10));
    const auto data = decode_websocket_frame_start(0x01, 0x80, false);
    RUVIA_CHECK(!is_invalid_websocket_control_frame(*data, 1000000));
}

RUVIA_TEST(ws_frame_view_factories_exclude_invalid_metadata_combinations) {
    const auto continuation = websocket_frame_view::continuation("next", false);
    RUVIA_CHECK(continuation.kind() == websocket_frame_kind::continuation);
    RUVIA_CHECK(!continuation.final());
    RUVIA_CHECK(!continuation.compressed());

    const auto compressed_text = websocket_frame_view::text("data", true, true);
    RUVIA_CHECK(compressed_text.kind() == websocket_frame_kind::text);
    RUVIA_CHECK(compressed_text.compressed());

    RUVIA_CHECK(websocket_frame_view::ping("ok").has_value());
    const std::string oversized_ping(126, 'x');
    RUVIA_CHECK(!websocket_frame_view::ping(oversized_ping).has_value());
    RUVIA_CHECK(websocket_frame_view::close({}).has_value());
    RUVIA_CHECK(!websocket_frame_view::close("x").has_value());
}

RUVIA_TEST(ws_encode_frame_header_length_boundaries) {
    websocket_frame_header_type header_value{};
    const auto u8 = [&header_value](
                        std::size_t index) { return static_cast<unsigned char>(header_value[index]); };
    // <=125: 1-byte length, no mask bit (server frames are unmasked).
    RUVIA_CHECK_EQ(encode_websocket_frame_header(header_value, websocket_opcode::text, 125), std::size_t{2});
    RUVIA_CHECK_EQ(u8(0), 0x81U);
    RUVIA_CHECK_EQ(u8(1), 125U);
    // 126..0xFFFF: 126 marker + 16-bit length.
    RUVIA_CHECK_EQ(
        encode_websocket_frame_header(header_value, websocket_opcode::binary, 126), std::size_t{4});
    RUVIA_CHECK_EQ(u8(0), 0x82U);
    RUVIA_CHECK_EQ(u8(1), 126U);
    RUVIA_CHECK_EQ(u8(2), 0U);
    RUVIA_CHECK_EQ(u8(3), 126U);
    RUVIA_CHECK_EQ(
        encode_websocket_frame_header(header_value, websocket_opcode::text, 65535), std::size_t{4});
    // >0xFFFF: 127 marker + 64-bit length. 65536 = 0x0000000000010000.
    RUVIA_CHECK_EQ(
        encode_websocket_frame_header(header_value, websocket_opcode::text, 65536), std::size_t{10});
    RUVIA_CHECK_EQ(u8(1), 127U);
    RUVIA_CHECK_EQ(u8(2), 0U);
    RUVIA_CHECK_EQ(u8(7), 1U);  // the 0x10000 bit
    RUVIA_CHECK_EQ(u8(8), 0U);
    RUVIA_CHECK_EQ(u8(9), 0U);
}

RUVIA_TEST(ws_mask_unmask_round_trip_all_tail_sizes) {
    const char mask[4] = {0x12, 0x34, 0x56, 0x78};
    // XOR masking is involutive, so masking twice restores the payload; cover every
    // tail remainder (0..3) past the 4-byte-unrolled body.
    for (std::size_t n = 0; n <= 10; ++n) {
        std::string original(n, '\0');
        for (std::size_t i = 0; i < n; ++i) {
            original[i] = static_cast<char>('A' + static_cast<int>(i));
        }
        std::string buffer = original;
        decode_masked_websocket_payload(buffer.data(), buffer.size(), mask);
        if (n > 0) {
            RUVIA_CHECK(buffer != original);
        }
        decode_masked_websocket_payload(buffer.data(), buffer.size(), mask);
        RUVIA_CHECK_EQ(buffer, original);
    }
    // Known value: 'A' (0x41) ^ 0x12 == 0x53.
    std::string one("A");
    decode_masked_websocket_payload(one.data(), one.size(), mask);
    RUVIA_CHECK_EQ(static_cast<unsigned char>(one[0]), 0x53U);
}

RUVIA_TEST(ws_frame_length_limit) {
    const auto limit = protocol_byte_limit::limited(1000);
    RUVIA_CHECK(!websocket_frame_length_exceeds_limit(100, limit));
    RUVIA_CHECK(websocket_frame_length_exceeds_limit(1001, limit));
    RUVIA_CHECK(!websocket_frame_length_exceeds_limit(1000000, protocol_byte_limit::unlimited()));
    RUVIA_CHECK(websocket_frame_length_exceeds_limit(std::numeric_limits<std::uint64_t>::max(), limit));
}

RUVIA_TEST(ws_frame_reader_needs_input_without_sentinel_metadata) {
    std::pmr::string input(std::pmr::get_default_resource());
    std::size_t offset = 0;
    std::size_t pending_compact_until = 0;

    const auto empty = websocket_try_read_frame(
        input, offset, pending_compact_until, protocol_byte_limit::limited(1024), false);
    RUVIA_CHECK(empty.need_input() != nullptr);
    RUVIA_CHECK(empty.frame() == nullptr);
    RUVIA_CHECK(empty.failure() == nullptr);

    input.push_back(static_cast<char>(0x81));
    const auto partial = websocket_try_read_frame(
        input, offset, pending_compact_until, protocol_byte_limit::limited(1024), false);
    RUVIA_CHECK(partial.need_input() != nullptr);
    RUVIA_CHECK(partial.frame() == nullptr);
    RUVIA_CHECK(partial.failure() == nullptr);
    RUVIA_CHECK_EQ(offset, std::size_t{0});
    RUVIA_CHECK_EQ(pending_compact_until, std::size_t{0});
}

RUVIA_TEST(ws_frame_reader_handles_deterministic_arbitrary_bytes) {
    std::uint64_t state_value = 0x9A71'F00D'BADC'0FFEULL;
    const auto next_value = [&state_value]() {
        state_value ^= state_value << 7U;
        state_value ^= state_value >> 9U;
        return state_value;
    };

    for (std::size_t sample = 0; sample < 4096; ++sample) {
        std::pmr::string input(std::pmr::get_default_resource());
        input.resize(static_cast<std::size_t>(next_value() % 1025U));
        for (auto& byte : input) {
            byte = static_cast<char>(next_value());
        }

        std::size_t offset = 0;
        std::size_t pending_compact_until = 0;
        const auto result_value = websocket_try_read_frame(input, offset, pending_compact_until,
            protocol_byte_limit::limited(1024), (next_value() & 1U) != 0, (next_value() & 1U) != 0);
        const auto active_alternatives = static_cast<unsigned int>(result_value.need_input() != nullptr) +
                                         static_cast<unsigned int>(result_value.frame() != nullptr) +
                                         static_cast<unsigned int>(result_value.failure() != nullptr);
        RUVIA_CHECK_EQ(active_alternatives, 1U);
        RUVIA_CHECK(offset <= input.size());
        RUVIA_CHECK(pending_compact_until <= input.size());

        if (result_value.frame() != nullptr) {
            RUVIA_CHECK_EQ(offset, pending_compact_until);
        } else {
            RUVIA_CHECK_EQ(offset, std::size_t{0});
            RUVIA_CHECK_EQ(pending_compact_until, std::size_t{0});
        }
    }
}

RUVIA_TEST(ws_frame_reader_keeps_next_frame_intact_across_compaction) {
    std::uint64_t state_value = 0xBA5E'F00D'1234'ABCDULL;
    const auto next_value = [&state_value]() {
        state_value ^= state_value << 7U;
        state_value ^= state_value >> 9U;
        return state_value;
    };

    std::pmr::string input(std::pmr::get_default_resource());
    std::vector<std::string> payloads;
    payloads.reserve(512);
    for (std::size_t frame_index = 0; frame_index < 512; ++frame_index) {
        std::string payload_value(static_cast<std::size_t>(next_value() % 126U), '\0');
        for (auto& byte : payload_value) {
            byte = static_cast<char>(next_value());
        }
        input.append(masked_frame((next_value() & 1U) != 0 ? 0x81U : 0x82U, payload_value));
        payloads.push_back(std::move(payload_value));
    }

    std::size_t offset = 0;
    std::size_t pending_compact_until = 0;
    for (const auto& expected : payloads) {
        const auto result_value = websocket_try_read_frame(
            input, offset, pending_compact_until, protocol_byte_limit::limited(125), false);
        RUVIA_CHECK(result_value.need_input() == nullptr);
        RUVIA_CHECK(result_value.failure() == nullptr);
        RUVIA_CHECK(result_value.frame() != nullptr);
        RUVIA_CHECK_EQ(result_value.frame()->payload(), std::string_view(expected));
        RUVIA_CHECK(offset <= input.size());
        RUVIA_CHECK(pending_compact_until <= input.size());
    }

    const auto drained = websocket_try_read_frame(
        input, offset, pending_compact_until, protocol_byte_limit::limited(125), false);
    RUVIA_CHECK(drained.need_input() != nullptr);
    RUVIA_CHECK(input.empty());
    RUVIA_CHECK_EQ(offset, std::size_t{0});
    RUVIA_CHECK_EQ(pending_compact_until, std::size_t{0});
}

RUVIA_TEST(ws_frame_reader_returns_one_unmasked_borrowed_frame) {
    auto input = masked_frame(0x81, "hi");
    std::size_t offset = 0;
    std::size_t pending_compact_until = 0;

    const auto result_value = websocket_try_read_frame(
        input, offset, pending_compact_until, protocol_byte_limit::limited(1024), false);
    RUVIA_CHECK(result_value.need_input() == nullptr);
    RUVIA_CHECK(result_value.failure() == nullptr);
    RUVIA_CHECK(result_value.frame() != nullptr);
    RUVIA_CHECK(result_value.frame()->kind() == websocket_frame_kind::text);
    RUVIA_CHECK(result_value.frame()->final());
    RUVIA_CHECK(!result_value.frame()->compressed());
    RUVIA_CHECK_EQ(result_value.frame()->payload(), std::string_view("hi"));
    RUVIA_CHECK_EQ(offset, input.size());
    RUVIA_CHECK_EQ(pending_compact_until, input.size());
}

RUVIA_TEST(ws_frame_reader_reports_typed_wire_failures) {
    std::size_t offset = 0;
    std::size_t pending_compact_until = 0;
    std::pmr::string unmasked(std::string_view("\x81\x02hi", 4), std::pmr::get_default_resource());
    const auto mask_failure = websocket_try_read_frame(
        unmasked, offset, pending_compact_until, protocol_byte_limit::limited(1024), false);
    RUVIA_CHECK(mask_failure.failure() != nullptr);
    RUVIA_CHECK(mask_failure.failure()->error() == websocket_protocol_failure::protocol_error);
    RUVIA_CHECK(mask_failure.frame() == nullptr);

    auto too_large = masked_frame(0x82, "123456");
    offset = 0;
    pending_compact_until = 0;
    const auto size_failure = websocket_try_read_frame(
        too_large, offset, pending_compact_until, protocol_byte_limit::limited(5), false);
    RUVIA_CHECK(size_failure.failure() != nullptr);
    RUVIA_CHECK(size_failure.failure()->error() == websocket_protocol_failure::message_too_large);

    const std::string invalid_close("\x03\xe8\xc0\x80", 4);  // 1000 plus invalid UTF-8 reason
    auto close = masked_frame(0x88, invalid_close);
    offset = 0;
    pending_compact_until = 0;
    const auto close_failure = websocket_try_read_frame(
        close, offset, pending_compact_until, protocol_byte_limit::limited(1024), false);
    RUVIA_CHECK(close_failure.failure() != nullptr);
    RUVIA_CHECK(close_failure.failure()->error() == websocket_protocol_failure::invalid_payload_data);
}

// RFC 6455 §5.2: the length MUST be encoded in the minimal number of bytes. A frame
// using the 16-bit form for a length <126, or the 64-bit form for a length <=65535,
// is a protocol error. A conformant peer never emits these; reject them.
RUVIA_TEST(ws_frame_reader_rejects_non_minimal_length_encoding) {
    constexpr std::array<unsigned char, 4> mask{0x12, 0x34, 0x56, 0x78};
    const auto masked_body = [&mask](std::pmr::string& out, std::string_view body) {
        for (const auto byte : mask) {
            out.push_back(static_cast<char>(byte));
        }
        for (std::size_t i = 0; i < body.size(); ++i) {
            out.push_back(static_cast<char>(static_cast<unsigned char>(body[i]) ^ mask[i & 3U]));
        }
    };
    const auto reads_as_protocol_error = [](std::pmr::string& frame) {
        std::size_t offset = 0;
        std::size_t pending_compact_until = 0;
        const auto result_value = websocket_try_read_frame(
            frame, offset, pending_compact_until, protocol_byte_limit::limited(1U << 20), false);
        return result_value.failure() != nullptr &&
               result_value.failure()->error() == websocket_protocol_failure::protocol_error;
    };

    // 16-bit form (126) carrying a 2-byte payload -- should have used the 7-bit form.
    std::pmr::string wide16(std::pmr::get_default_resource());
    wide16.push_back(static_cast<char>(0x82));          // FIN + binary
    wide16.push_back(static_cast<char>(0x80U | 126U));  // masked, 16-bit length marker
    wide16.push_back(0x00);
    wide16.push_back(0x02);  // length = 2 (non-minimal)
    masked_body(wide16, "hi");
    RUVIA_CHECK(reads_as_protocol_error(wide16));

    // 64-bit form (127) carrying a 2-byte payload -- should have used the 7-bit form.
    std::pmr::string wide64(std::pmr::get_default_resource());
    wide64.push_back(static_cast<char>(0x82));
    wide64.push_back(static_cast<char>(0x80U | 127U));  // masked, 64-bit length marker
    for (int i = 0; i < 7; ++i) {
        wide64.push_back(0x00);
    }
    wide64.push_back(0x02);  // length = 2 (non-minimal)
    masked_body(wide64, "hi");
    RUVIA_CHECK(reads_as_protocol_error(wide64));

    // Boundary: a genuinely 126-byte payload legitimately uses the 16-bit form.
    std::pmr::string minimal16(std::pmr::get_default_resource());
    minimal16.push_back(static_cast<char>(0x82));
    minimal16.push_back(static_cast<char>(0x80U | 126U));
    minimal16.push_back(0x00);
    minimal16.push_back(0x7E);  // length = 126 (minimal)
    masked_body(minimal16, std::string(126, 'x'));
    std::size_t offset = 0;
    std::size_t pending_compact_until = 0;
    const auto ok = websocket_try_read_frame(
        minimal16, offset, pending_compact_until, protocol_byte_limit::limited(1U << 20), false);
    RUVIA_CHECK(ok.frame() != nullptr);
    RUVIA_CHECK_EQ(ok.frame()->payload().size(), std::size_t{126});
}
