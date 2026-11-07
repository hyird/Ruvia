#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "ruvia/http/http2_framing.h"

TEST(Ruvia, frame_public_codec_preserves_wire_bytes_and_buffer_boundaries) {
    std::array<char, 11> storage{};
    storage.fill('#');
    auto output = std::span(storage).subspan(1, ruvia::http2_frame_header_bytes);
    EXPECT_TRUE(ruvia::encode_http2_frame_header(output, 0xabcdef,
        static_cast<ruvia::http2_frame_type>(0xfe), 0x81, 0x7f123456));
    const std::array<unsigned char, 9> expected{0xab, 0xcd, 0xef, 0xfe, 0x81, 0x7f, 0x12, 0x34, 0x56};
    for (std::size_t i = 0; i < expected.size(); ++i) {
        EXPECT_EQ(static_cast<unsigned char>(output[i]), expected[i]);
    }
    EXPECT_EQ(storage.front(), '#');
    EXPECT_EQ(storage.back(), '#');
    // A received reserved bit is ignored; unknown frame types remain available
    // to the connection's extension handling rather than being rejected here.
    output[5] = static_cast<char>(0xff);
    const auto decoded = ruvia::parse_http2_frame_header(output);
    EXPECT_TRUE(decoded.has_value());
    if (decoded) {
        EXPECT_EQ(decoded->length_, std::uint32_t{0xabcdef});
        EXPECT_EQ(decoded->type_, std::uint8_t{0xfe});
        EXPECT_EQ(decoded->flags_, std::uint8_t{0x81});
        EXPECT_EQ(decoded->stream_id_, std::uint32_t{0x7f123456});
    }
}

TEST(Ruvia, frame_public_codec_rejects_invalid_input_without_writing) {
    std::array<char, ruvia::http2_frame_header_bytes> output{};
    output.fill('#');
    const auto unchanged = output;
    for (std::size_t size = 0; size < output.size(); ++size) {
        const auto short_buffer = std::span(output).first(size);
        EXPECT_TRUE(!ruvia::encode_http2_frame_header(short_buffer, 0, ruvia::http2_frame_type::data, 0, 1));
        EXPECT_TRUE(!ruvia::parse_http2_frame_header(short_buffer));
        EXPECT_EQ(output, unchanged);
    }
    EXPECT_TRUE(!ruvia::encode_http2_frame_header(output, 0x1000000, ruvia::http2_frame_type::data, 0, 1));
    EXPECT_EQ(output, unchanged);
    EXPECT_TRUE(!ruvia::encode_http2_frame_header(output, 0, ruvia::http2_frame_type::data, 0, 0x80000000));
    EXPECT_EQ(output, unchanged);
}
