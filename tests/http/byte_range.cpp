#include <gtest/gtest.h>

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "http_range.h"
#include "http_response.h"

TEST(RuviaHttpPublicApi, strict_byte_ranges_content_range_and_streaming_slicer) {
    // Strict ascending non-overlapping mode rejects out-of-order and overlapping ranges.
    const auto strict_ok = ruvia::resolve_http_byte_range_set("bytes=0-4, 5-9, 20-29", 100, {.max_ranges_ = 32, .strict_order_ = true});
    EXPECT_FALSE(strict_ok.ignored());
    ASSERT_EQ(strict_ok.size(), 3U);
    EXPECT_EQ(strict_ok[0].offset(), 0U);
    EXPECT_EQ(strict_ok[0].length(), 5U);
    EXPECT_EQ(strict_ok[1].offset(), 5U);
    EXPECT_EQ(strict_ok[1].length(), 5U);

    const auto strict_out_of_order = ruvia::resolve_http_byte_range_set("bytes=20-29, 0-4", 100, {.max_ranges_ = 32, .strict_order_ = true});
    EXPECT_TRUE(strict_out_of_order.ignored());

    const auto strict_overlapping = ruvia::resolve_http_byte_range_set("bytes=0-10, 10-20", 100, {.max_ranges_ = 32, .strict_order_ = true});
    EXPECT_TRUE(strict_overlapping.ignored());

    // Configurable max_ranges > 16 works.
    std::string twenty_ranges = "bytes=";
    for (std::size_t i = 0; i < 20; ++i) {
        if (i != 0) {
            twenty_ranges.push_back(',');
        }
        twenty_ranges += std::to_string(i * 2) + "-" + std::to_string(i * 2);
    }
    const auto many = ruvia::resolve_http_byte_range_set(twenty_ranges, 100, {.max_ranges_ = 24, .strict_order_ = true});
    EXPECT_FALSE(many.ignored());
    EXPECT_EQ(many.size(), 20U);

    // Content-Range parser.
    const auto parsed_cr = ruvia::parse_http_content_range("bytes 10-19/100");
    ASSERT_TRUE(parsed_cr.has_value());
    EXPECT_EQ(parsed_cr->first(), 10U);
    EXPECT_EQ(parsed_cr->last(), 19U);
    EXPECT_EQ(parsed_cr->total(), 100U);
    EXPECT_EQ(parsed_cr->length(), 10U);
    EXPECT_FALSE(ruvia::parse_http_content_range("bytes 20-10/100").has_value());
    EXPECT_FALSE(ruvia::parse_http_content_range("bytes 0-100/100").has_value());
    EXPECT_FALSE(ruvia::parse_http_content_range("bytes */100").has_value());

    // Streaming slicer for multipart/byteranges (including offset-based feeding).
    constexpr std::string_view payload = "0123456789abcdefghijklmnopqrstuvwxyz";
    const auto ranges = ruvia::resolve_http_byte_range_set("bytes=2-5, 10-14, 30-35", payload.size());
    const auto plan = ruvia::make_http_multipart_byte_range_plan(ranges, payload.size(), "text/plain", "slice_boundary");
    std::string expected;
    for (const auto& seg : plan.segments()) {
        if (seg.kind_ == ruvia::http_multipart_byte_range_plan::segment_kind::metadata) {
            expected.append(plan.metadata().substr(seg.metadata_offset_, seg.metadata_length_));
        } else {
            expected.append(payload.substr(static_cast<std::size_t>(seg.file_offset_), static_cast<std::size_t>(seg.file_length_)));
        }
    }

    ruvia::http_byte_range_slicer slicer(ranges, payload.size(), "text/plain", "slice_boundary");
    EXPECT_TRUE(slicer.multipart());
    EXPECT_EQ(slicer.content_length(), expected.size());

    std::string sliced;
    // Feed starting from offset 2 in two partial segments, skipping the gap 15..29!
    slicer.write(2, payload.substr(2, 13), sliced);
    EXPECT_FALSE(slicer.complete());
    slicer.write(30, payload.substr(30, 6), sliced);
    EXPECT_TRUE(slicer.complete());
    slicer.finish(sliced);
    EXPECT_EQ(sliced, expected);
}

TEST(Ruvia, multipart_range_plan_owns_rfc_framing_and_reports_exact_length) {
    auto ranges = ruvia::resolve_http_byte_range_set("bytes=0-2,7-9", 10);
    std::string media_type = "text/plain";
    std::string boundary = "unit_boundary_42";
    auto plan = ruvia::make_http_multipart_byte_range_plan(ranges, 10, media_type, boundary);
    media_type.assign(media_type.size(), 'x');
    boundary.assign(boundary.size(), 'y');

    EXPECT_EQ(plan.content_type(), "multipart/byteranges; boundary=unit_boundary_42");
    EXPECT_TRUE(plan.metadata().find("Content-Type: text/plain") != std::string_view::npos);
    EXPECT_TRUE(plan.metadata().find("Content-Range: bytes 7-9/10") != std::string_view::npos);
    std::uint64_t content_length = 0;
    for (const auto& segment : plan.segments()) {
        content_length += segment.kind_ == ruvia::http_multipart_byte_range_plan::segment_kind::file ? segment.file_length_ : segment.metadata_length_;
    }
    EXPECT_EQ(plan.content_length(), content_length);
    EXPECT_TRUE(plan.metadata().ends_with("--\r\n"));
}

TEST(Ruvia, multipart_range_plan_quotes_boundary_parameters) {
    const auto ranges = ruvia::resolve_http_byte_range_set("bytes=8-9,0-1", 10);
    auto plan = ruvia::make_http_multipart_byte_range_plan(ranges, 10, "text/plain", "range boundary");
    EXPECT_EQ(plan.content_type(), "multipart/byteranges; boundary=\"range boundary\"");
    EXPECT_TRUE(plan.metadata().find("Content-Range: bytes 8-9/10") < plan.metadata().find("Content-Range: bytes 0-1/10"));
    auto clone = plan.clone();
    EXPECT_EQ(clone.content_type(), plan.content_type());
    EXPECT_EQ(clone.metadata(), plan.metadata());
}

TEST(Ruvia, multipart_range_plan_rejects_invalid_media_type) {
    const auto ranges = ruvia::resolve_http_byte_range_set("bytes=0-1,8-9", 10);
    bool threw = false;
    try {
        static_cast<void>(ruvia::make_http_multipart_byte_range_plan(ranges, 10, "text/plain\r\nX-Evil: yes", "valid_boundary"));
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    EXPECT_TRUE(threw);
}

TEST(Ruvia, multipart_range_plan_rejects_content_length_overflow) {
    auto ranges = ruvia::resolve_http_byte_range_set("bytes=0-9223372036854775806,9223372036854775808-18446744073709551614", (std::numeric_limits<std::uint64_t>::max)());
    EXPECT_EQ(ranges.size(), std::size_t{2});
    bool threw = false;
    try {
        static_cast<void>(ruvia::make_http_multipart_byte_range_plan(ranges, (std::numeric_limits<std::uint64_t>::max)(), "application/octet-stream", "overflow_check"));
    } catch (const std::length_error&) {
        threw = true;
    }
    EXPECT_TRUE(threw);
}

namespace {

std::string materialize_multipart(const ruvia::http_multipart_byte_range_plan& plan, std::string_view payload_value) {
    std::string body;
    for (const auto& segment : plan.segments()) {
        if (segment.kind_ == ruvia::http_multipart_byte_range_plan::segment_kind::metadata) {
            const auto metadata = plan.metadata();
            if (segment.metadata_offset_ > metadata.size() || segment.metadata_length_ > metadata.size() - segment.metadata_offset_) {
                throw std::logic_error("multipart metadata segment is out of bounds");
            }
            body.append(metadata.substr(segment.metadata_offset_, segment.metadata_length_));
        } else {
            if (segment.file_offset_ > payload_value.size() || segment.file_length_ > payload_value.size() - segment.file_offset_) {
                throw std::logic_error("multipart file segment is out of bounds");
            }
            body.append(payload_value.substr(static_cast<std::size_t>(segment.file_offset_), static_cast<std::size_t>(segment.file_length_)));
        }
    }
    return body;
}

}  // namespace

TEST(Ruvia, multipart_range_plan_serializes_owned_inputs_and_every_segment_exactly) {
    const auto ranges = ruvia::resolve_http_byte_range_set("bytes=8-9,0-1", 10);
    std::string media_type = "text/plain; charset=utf-8";
    std::string boundary = "range boundary";
    auto plan = ruvia::make_http_multipart_byte_range_plan(ranges, 10, media_type, boundary);
    media_type.assign(media_type.size(), 'x');
    boundary.assign(boundary.size(), 'x');
    const std::string_view expected =
        "--range boundary\r\nContent-Type: text/plain; charset=utf-8\r\n"
        "Content-Range: bytes 8-9/10\r\n\r\n89\r\n"
        "--range boundary\r\nContent-Type: text/plain; charset=utf-8\r\n"
        "Content-Range: bytes 0-1/10\r\n\r\n01\r\n"
        "--range boundary--\r\n";
    EXPECT_EQ(plan.content_type(), "multipart/byteranges; boundary=\"range boundary\"");
    EXPECT_EQ(materialize_multipart(plan, "0123456789"), expected);
    EXPECT_EQ(plan.content_length(), expected.size());
}

TEST(Ruvia, multipart_range_plan_preserves_all_ranges_across_metadata_growth) {
    std::string field = "bytes=";
    std::string expected;
    constexpr std::string_view payload_value = "0123456789abcdefghijklmnopqrstuv";
    for (std::size_t index = 0; index < ruvia::http_byte_range_set::capacity; ++index) {
        char offset_buffer[20];
        const auto offset_end = std::to_chars(offset_buffer, offset_buffer + sizeof(offset_buffer), index * 2).ptr;
        const std::string_view offset(offset_buffer, offset_end);
        if (index != 0) {
            field.push_back(',');
        }
        field.append(offset).append("-").append(offset);
        expected.append("--all_ranges\r\nContent-Type: application/octet-stream\r\nContent-Range: bytes ");
        expected.append(offset).append("-").append(offset).append("/32\r\n\r\n");
        expected.push_back(payload_value[index * 2]);
        expected.append("\r\n");
    }
    expected.append("--all_ranges--\r\n");
    const auto ranges = ruvia::resolve_http_byte_range_set(field, payload_value.size());
    EXPECT_EQ(ranges.size(), ruvia::http_byte_range_set::capacity);
    const auto plan = ruvia::make_http_multipart_byte_range_plan(ranges, payload_value.size(), "application/octet-stream", "all_ranges");
    EXPECT_EQ(materialize_multipart(plan, payload_value), expected);
    EXPECT_EQ(plan.content_length(), expected.size());
}

TEST(Ruvia, multipart_range_plan_moved_from_and_empty_file_path_are_rejected_cleanly) {
    const auto ranges = ruvia::resolve_http_byte_range_set("bytes=0-1,4-5", 10);
    auto plan = ruvia::make_http_multipart_byte_range_plan(ranges, 10, "text/plain", "b");
    auto moved = std::move(plan);
    EXPECT_TRUE(plan.content_type().empty());
    EXPECT_TRUE(plan.metadata().empty());
    EXPECT_TRUE(plan.segments().empty());
    EXPECT_EQ(plan.content_length(), std::uint64_t{0});

    ruvia::http_response response;
    bool empty_path_rejected = false;
    try {
        response.multipart_file_body({}, 10, ruvia::http_response_file_identity::unchecked(), std::move(moved));
    } catch (const std::invalid_argument&) {
        empty_path_rejected = true;
    }
    EXPECT_TRUE(empty_path_rejected);

    bool moved_plan_rejected = false;
    try {
        const ruvia::http_native_path_string_type non_empty(1, static_cast<ruvia::http_native_path_char_type>('a'));
        response.multipart_file_body(non_empty, 10, ruvia::http_response_file_identity::unchecked(), std::move(plan));
    } catch (const std::invalid_argument&) {
        moved_plan_rejected = true;
    }
    EXPECT_TRUE(moved_plan_rejected);
}
