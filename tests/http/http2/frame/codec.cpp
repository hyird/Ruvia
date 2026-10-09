#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>

#include "http2/http2_flow_control.h"
#include "http2/http2_frame_codec.h"
#include "http2/http2_frame_types.h"
#include "test_harness.h"

namespace {

using ruvia::detail::http2_apply_window_update;
using ruvia::detail::http2_encode_frame_header;
using ruvia::detail::http2_error_code;
using ruvia::detail::http2_frame_header_bytes;
using ruvia::detail::http2_frame_type;
using ruvia::detail::http2_parse_frame_header;
using ruvia::detail::http2_read16;
using ruvia::detail::http2_read24;
using ruvia::detail::http2_read31;
using ruvia::detail::http2_read32;
using ruvia::detail::http2_setting_id;
using ruvia::detail::http2_window_update_increment;
using ruvia::detail::http2_window_update_result;
using ruvia::detail::http2_write16;
using ruvia::detail::http2_write32;
using ruvia::detail::http2_write_frame_header;
using ruvia::detail::http2_write_goaway_payload;
using ruvia::detail::http2_write_settings_entry;
using ruvia::detail::http2_write_window_update;

const unsigned char* bytes(const char* p) noexcept {
    return reinterpret_cast<const unsigned char*>(p);
}

}  // namespace

RUVIA_TEST(frame_public_codec_preserves_wire_bytes_and_buffer_boundaries) {
    std::array<char, 11> storage{};
    storage.fill('#');
    auto output = std::span(storage).subspan(1, http2_frame_header_bytes);
    RUVIA_CHECK(ruvia::encode_http2_frame_header(output, 0xabcdef,
        static_cast<http2_frame_type>(0xfe), 0x81, 0x7f123456));
    const std::array<unsigned char, 9> expected{0xab, 0xcd, 0xef, 0xfe, 0x81, 0x7f, 0x12, 0x34, 0x56};
    for (std::size_t i = 0; i < expected.size(); ++i) {
        RUVIA_CHECK_EQ(static_cast<unsigned char>(output[i]), expected[i]);
    }
    RUVIA_CHECK_EQ(storage.front(), '#');
    RUVIA_CHECK_EQ(storage.back(), '#');
    // A received reserved bit is ignored; unknown frame types remain available
    // to the connection's extension handling rather than being rejected here.
    output[5] = static_cast<char>(0xff);
    const auto decoded = ruvia::parse_http2_frame_header(output);
    RUVIA_CHECK(decoded.has_value());
    if (decoded) {
        RUVIA_CHECK_EQ(decoded->length_, std::uint32_t{0xabcdef});
        RUVIA_CHECK_EQ(decoded->type_, std::uint8_t{0xfe});
        RUVIA_CHECK_EQ(decoded->flags_, std::uint8_t{0x81});
        RUVIA_CHECK_EQ(decoded->stream_id_, std::uint32_t{0x7f123456});
    }
}

RUVIA_TEST(frame_public_codec_rejects_invalid_input_without_writing) {
    std::array<char, http2_frame_header_bytes> output{};
    output.fill('#');
    const auto unchanged = output;
    for (std::size_t size = 0; size < output.size(); ++size) {
        const auto short_buffer = std::span(output).first(size);
        RUVIA_CHECK(!ruvia::encode_http2_frame_header(short_buffer, 0, http2_frame_type::data, 0, 1));
        RUVIA_CHECK(!ruvia::parse_http2_frame_header(short_buffer));
        RUVIA_CHECK_EQ(output, unchanged);
    }
    RUVIA_CHECK(!ruvia::encode_http2_frame_header(output, 0x1000000, http2_frame_type::data, 0, 1));
    RUVIA_CHECK_EQ(output, unchanged);
    RUVIA_CHECK(!ruvia::encode_http2_frame_header(output, 0, http2_frame_type::data, 0, 0x80000000));
    RUVIA_CHECK_EQ(output, unchanged);
}

RUVIA_TEST(frame_header_encode_parse_round_trip) {
    char buf[http2_frame_header_bytes];
    http2_encode_frame_header(buf, 0x123456, http2_frame_type::headers, 0x25, 0x0A);
    const auto header_value = http2_parse_frame_header(std::string_view(buf, http2_frame_header_bytes));
    RUVIA_CHECK_EQ(header_value.length_, std::uint32_t{0x123456});
    RUVIA_CHECK(header_value.type_ == static_cast<std::uint8_t>(http2_frame_type::headers));
    RUVIA_CHECK_EQ(header_value.flags_, std::uint8_t{0x25});
    RUVIA_CHECK_EQ(header_value.stream_id_, std::uint32_t{0x0A});
}

RUVIA_TEST(frame_header_masks_reserved_stream_id_bit) {
    char buf[http2_frame_header_bytes];
    // The top (reserved) bit of the stream id must be cleared on the wire.
    http2_encode_frame_header(buf, 8, http2_frame_type::settings, 0, 0x8000000A);
    const auto header_value = http2_parse_frame_header(std::string_view(buf, http2_frame_header_bytes));
    RUVIA_CHECK_EQ(header_value.stream_id_, std::uint32_t{0x0A});
}

RUVIA_TEST(frame_write_helpers_advance_the_cursor) {
    char buf[http2_frame_header_bytes];
    char* end = http2_write_frame_header(buf, 0, http2_frame_type::data, 0, 1);
    RUVIA_CHECK(static_cast<std::size_t>(end - buf) == http2_frame_header_bytes);

    char two[2];
    char* end_two = http2_write16(two, 0x1234);
    RUVIA_CHECK(end_two - two == 2);
    RUVIA_CHECK_EQ(static_cast<unsigned>(static_cast<unsigned char>(two[0])), 0x12u);  // big-endian
    RUVIA_CHECK_EQ(static_cast<unsigned>(static_cast<unsigned char>(two[1])), 0x34u);
    RUVIA_CHECK_EQ(http2_read16(bytes(two)), std::uint16_t{0x1234});

    char four[4];
    char* end_four = http2_write32(four, 0x12345678);
    RUVIA_CHECK(end_four - four == 4);
    RUVIA_CHECK_EQ(static_cast<unsigned>(static_cast<unsigned char>(four[0])), 0x12u);
    RUVIA_CHECK_EQ(http2_read32(bytes(four)), std::uint32_t{0x12345678});
}

RUVIA_TEST(frame_settings_entry_serialization) {
    char buf[6];
    char* end = http2_write_settings_entry(buf, http2_setting_id::max_concurrent_streams, 100);
    RUVIA_CHECK(end - buf == 6);
    RUVIA_CHECK_EQ(
        http2_read16(bytes(buf)), static_cast<std::uint16_t>(http2_setting_id::max_concurrent_streams));
    RUVIA_CHECK_EQ(http2_read32(bytes(buf) + 2), std::uint32_t{100});
}

RUVIA_TEST(frame_window_update_serialization) {
    char buf[http2_frame_header_bytes + 4];
    // A high bit set on the increment must be masked off (reserved bit).
    char* end = http2_write_window_update(buf, 7, 0x80000005);
    RUVIA_CHECK(static_cast<std::size_t>(end - buf) == http2_frame_header_bytes + 4);
    const auto header_value = http2_parse_frame_header(std::string_view(buf, http2_frame_header_bytes));
    RUVIA_CHECK(header_value.type_ == static_cast<std::uint8_t>(http2_frame_type::window_update));
    RUVIA_CHECK_EQ(header_value.length_, std::uint32_t{4});
    RUVIA_CHECK_EQ(header_value.stream_id_, std::uint32_t{7});
    RUVIA_CHECK_EQ(http2_read32(bytes(buf) + http2_frame_header_bytes), std::uint32_t{5});
}

RUVIA_TEST(frame_goaway_payload_serialization) {
    char buf[8];
    // The last-stream-id reserved bit is masked; the error code follows.
    char* end = http2_write_goaway_payload(buf, 0x8000000F, http2_error_code::protocol_error);
    RUVIA_CHECK(end - buf == 8);
    RUVIA_CHECK_EQ(http2_read32(bytes(buf)), std::uint32_t{0x0F});
    RUVIA_CHECK_EQ(
        http2_read32(bytes(buf) + 4), static_cast<std::uint32_t>(http2_error_code::protocol_error));
}

RUVIA_TEST(http2_big_endian_readers) {
    const unsigned char data[] = {0x12, 0x34, 0x56, 0x78};
    RUVIA_CHECK_EQ(http2_read16(data), std::uint16_t{0x1234});
    RUVIA_CHECK_EQ(http2_read24(data), std::uint32_t{0x123456});
    RUVIA_CHECK_EQ(http2_read32(data), std::uint32_t{0x12345678});
    RUVIA_CHECK_EQ(http2_read31(data), std::uint32_t{0x12345678});  // high bit already 0
    // http2_read31 masks the reserved top bit; http2_read32 keeps it.
    const unsigned char high[] = {0xFF, 0xFF, 0xFF, 0xFF};
    RUVIA_CHECK_EQ(http2_read31(high), std::uint32_t{0x7fffffff});
    RUVIA_CHECK_EQ(http2_read32(high), std::uint32_t{0xffffffff});
}

RUVIA_TEST(http2_window_update_increment_masks_reserved_bit) {
    const std::string payload_value = std::string("\xff\xff\xff\xff", 4);
    RUVIA_CHECK_EQ(http2_window_update_increment(payload_value), std::uint32_t{0x7fffffff});
}

RUVIA_TEST(http2_apply_window_update) {
    std::int32_t window = 100;
    RUVIA_CHECK(http2_apply_window_update(window, 50) == http2_window_update_result::ok);
    RUVIA_CHECK_EQ(window, 150);
    // A zero increment is a distinct (protocol-error) result.
    RUVIA_CHECK(http2_apply_window_update(window, 0) == http2_window_update_result::zero_increment);
    RUVIA_CHECK_EQ(window, 150);
}

RUVIA_TEST(http2_window_update_overflow_is_flow_control_error) {
    constexpr auto max_value = std::numeric_limits<std::int32_t>::max();  // 2^31 - 1
    // A WINDOW_UPDATE pushing the window past 2^31-1 must be rejected (RFC 7540 6.9.1)...
    std::int32_t near_max = max_value - 10;
    RUVIA_CHECK(http2_apply_window_update(near_max, 20) == http2_window_update_result::overflow);
    RUVIA_CHECK_EQ(near_max, max_value - 10);  // window is unchanged on overflow
    // ...but reaching exactly 2^31-1 is allowed.
    std::int32_t exact = max_value - 10;
    RUVIA_CHECK(http2_apply_window_update(exact, 10) == http2_window_update_result::ok);
    RUVIA_CHECK_EQ(exact, max_value);
}
#include <array>
