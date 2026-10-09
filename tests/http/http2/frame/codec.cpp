#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "ruvia/http/http2_framing.h"

#include "test_harness.h"

RUVIA_TEST(frame_public_codec_preserves_wire_bytes_and_buffer_boundaries) {
    std::array<char, 11> storage{};
    storage.fill('#');
    auto output = std::span(storage).subspan(1, ruvia::http2_frame_header_bytes);
    RUVIA_CHECK(ruvia::encode_http2_frame_header(output, 0xabcdef,
        static_cast<ruvia::http2_frame_type>(0xfe), 0x81, 0x7f123456));
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
    std::array<char, ruvia::http2_frame_header_bytes> output{};
    output.fill('#');
    const auto unchanged = output;
    for (std::size_t size = 0; size < output.size(); ++size) {
        const auto short_buffer = std::span(output).first(size);
        RUVIA_CHECK(!ruvia::encode_http2_frame_header(short_buffer, 0, ruvia::http2_frame_type::data, 0, 1));
        RUVIA_CHECK(!ruvia::parse_http2_frame_header(short_buffer));
        RUVIA_CHECK_EQ(output, unchanged);
    }
    RUVIA_CHECK(!ruvia::encode_http2_frame_header(output, 0x1000000, ruvia::http2_frame_type::data, 0, 1));
    RUVIA_CHECK_EQ(output, unchanged);
    RUVIA_CHECK(!ruvia::encode_http2_frame_header(output, 0, ruvia::http2_frame_type::data, 0, 0x80000000));
    RUVIA_CHECK_EQ(output, unchanged);
}
