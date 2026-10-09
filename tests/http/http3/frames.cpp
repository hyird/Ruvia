#include <array>
#include <cstdint>
#include <span>
#include <string_view>
#include <variant>

#include "ruvia/http/http3_frames.h"
#include "ruvia/http/http3_var_int.h"

#include "test_harness.h"

namespace {

using ruvia::decode_http3_frame;
using ruvia::decode_http3_frame_header;
using ruvia::decode_http3_var_int;
using ruvia::encode_http3_frame;
using ruvia::encode_http3_var_int;
using ruvia::http3_codec_error;
using ruvia::http3_var_int_max;
using ruvia::is_http3_grease_frame_type;

}  // namespace

RUVIA_TEST(http3_varint_encodes_and_decodes_all_wire_widths) {
    constexpr std::array<std::uint64_t, 8> values{0, 63, 64, 16383, 16384, (1ULL << 30) - 1,
        1ULL << 30, http3_var_int_max};
    constexpr std::array<std::size_t, 8> widths{1, 1, 2, 2, 4, 4, 8, 8};
    for (std::size_t i = 0; i < values.size(); ++i) {
        std::array<char, 8> bytes_value{};
        const auto written = encode_http3_var_int(bytes_value, values[i]);
        RUVIA_CHECK((written.index() == 0));
        if ((written.index() != 0)) {
            continue;
        }
        RUVIA_CHECK_EQ(std::get<0>(written), widths[i]);
        const auto decoded = decode_http3_var_int(std::span<const char>(bytes_value).first(std::get<0>(written)));
        RUVIA_CHECK((decoded.index() == 0));
        if ((decoded.index() == 0)) {
            RUVIA_CHECK_EQ(std::get<0>(decoded).value_, values[i]);
            RUVIA_CHECK_EQ(std::get<0>(decoded).encoded_bytes_, widths[i]);
        }
    }
}

RUVIA_TEST(http3_varint_reports_truncation_and_bounds_without_writing) {
    constexpr std::array<char, 1> two_byte_prefix{static_cast<char>(0x40)};
    RUVIA_CHECK(std::get<1>(decode_http3_var_int(two_byte_prefix)) == http3_codec_error::need_more_data);
    std::array<char, 8> output{};
    output.fill('#');
    const auto before = output;
    RUVIA_CHECK(std::get<1>(encode_http3_var_int(output, http3_var_int_max + 1)) ==
                http3_codec_error::value_out_of_range);
    RUVIA_CHECK(std::get<1>(encode_http3_var_int(std::span<char>(output).first(1), 64)) ==
                http3_codec_error::output_too_small);
    RUVIA_CHECK_EQ(output, before);
}

RUVIA_TEST(http3_frame_codec_reports_invalid_type_before_output_capacity) {
    std::array<char, 1> output{'#'};
    const auto result_value = encode_http3_frame(output, http3_var_int_max + 1, {});
    RUVIA_CHECK(!(result_value.index() == 0));
    if ((result_value.index() != 0)) {
        RUVIA_CHECK(std::get<1>(result_value) == http3_codec_error::value_out_of_range);
    }
    RUVIA_CHECK_EQ(output[0], '#');
}

RUVIA_TEST(http3_frame_codec_round_trips_opaque_payloads) {
    constexpr std::string_view payload_value = "headers-or-data";
    std::array<char, 64> wire{};
    const auto written = encode_http3_frame(wire, 0x1, payload_value);
    RUVIA_CHECK((written.index() == 0));
    if ((written.index() != 0)) {
        return;
    }
    const auto decoded = decode_http3_frame(std::span<const char>(wire).first(std::get<0>(written)));
    RUVIA_CHECK((decoded.index() == 0));
    if ((decoded.index() == 0)) {
        RUVIA_CHECK_EQ(std::get<0>(decoded).type_, std::uint64_t{0x1});
        RUVIA_CHECK_EQ(std::string_view(std::get<0>(decoded).payload_.data(), std::get<0>(decoded).payload_.size()), payload_value);
        RUVIA_CHECK_EQ(std::get<0>(decoded).encoded_bytes_, std::get<0>(written));
    }
}

RUVIA_TEST(http3_frame_codec_distinguishes_truncation_and_keeps_unknown_frames_skippable) {
    constexpr std::array<char, 3> short_header{0x1, 0x4, 0x01};
    RUVIA_CHECK(std::get<1>(decode_http3_frame_header(std::span<const char>(short_header).first(1))) ==
                http3_codec_error::need_more_data);
    RUVIA_CHECK(std::get<1>(decode_http3_frame(short_header)) == http3_codec_error::need_more_data);

    // Unknown extension frame type 0x21 (an HTTP/3 grease type) is surfaced as
    // an opaque frame; consuming encoded_bytes skips it without decoding payload.
    constexpr std::array<char, 5> grease_frame{0x21, 0x01, 'x', 0x01, 0x00};
    const auto frame = decode_http3_frame(grease_frame);
    RUVIA_CHECK((frame.index() == 0));
    RUVIA_CHECK(is_http3_grease_frame_type(0x21));
    RUVIA_CHECK(is_http3_grease_frame_type(0x40));
    RUVIA_CHECK(!is_http3_grease_frame_type(0x22));
    if ((frame.index() == 0)) {
        RUVIA_CHECK_EQ(std::get<0>(frame).type_, std::uint64_t{0x21});
        RUVIA_CHECK_EQ(std::string_view(std::get<0>(frame).payload_.data(), std::get<0>(frame).payload_.size()),
            std::string_view("x"));
        RUVIA_CHECK_EQ(std::get<0>(frame).encoded_bytes_, std::size_t{3});
        const auto next_value = decode_http3_frame(std::span<const char>(grease_frame).subspan(std::get<0>(frame).encoded_bytes_));
        RUVIA_CHECK((next_value.index() == 0));
        if ((next_value.index() == 0)) {
            RUVIA_CHECK_EQ(std::get<0>(next_value).type_, std::uint64_t{1});
        }
    }
}
