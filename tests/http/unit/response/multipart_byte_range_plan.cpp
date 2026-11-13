#include <gtest/gtest.h>

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/http/http_multipart_byte_range_plan.h"
#include "ruvia/http/http_response.h"

namespace {

std::string materialize_multipart(const ruvia::http_multipart_byte_range_plan& plan, std::string_view payload_value) {
    std::string body;
    for (const auto& segment : plan.segments()) {
        if (segment.kind_ == ruvia::http_multipart_byte_range_plan::segment_kind::metadata) {
            const auto metadata = plan.metadata();
            if (segment.metadata_offset_ > metadata.size() ||
                segment.metadata_length_ > metadata.size() - segment.metadata_offset_) {
                throw std::logic_error("multipart metadata segment is out of bounds");
            }
            body.append(metadata.substr(segment.metadata_offset_, segment.metadata_length_));
        } else {
            if (segment.file_offset_ > payload_value.size() ||
                segment.file_length_ > payload_value.size() - segment.file_offset_) {
                throw std::logic_error("multipart file segment is out of bounds");
            }
            body.append(payload_value.substr(static_cast<std::size_t>(segment.file_offset_),
                static_cast<std::size_t>(segment.file_length_)));
        }
    }
    return body;
}

}  // namespace

TEST(Ruvia, multipart_range_plan_serializes_owned_inputs_and_every_segment_exactly) {
    const auto ranges = ruvia::resolve_http_byte_range_set("bytes=8-9,0-1", 10);
    std::string media_type = "text/plain; charset=utf-8";
    std::string boundary = "range boundary";
    auto plan = ruvia::make_http_multipart_byte_range_plan(
        ranges, 10, media_type, boundary);
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
    const auto plan = ruvia::make_http_multipart_byte_range_plan(
        ranges, payload_value.size(), "application/octet-stream", "all_ranges");
    EXPECT_EQ(materialize_multipart(plan, payload_value), expected);
    EXPECT_EQ(plan.content_length(), expected.size());
}

TEST(Ruvia, multipart_range_plan_moved_from_and_empty_file_path_are_rejected_cleanly) {
    const auto ranges = ruvia::resolve_http_byte_range_set("bytes=0-1,4-5", 10);
    auto plan = ruvia::make_http_multipart_byte_range_plan(
        ranges, 10, "text/plain", "b");
    auto moved = std::move(plan);
    EXPECT_TRUE(plan.content_type().empty());
    EXPECT_TRUE(plan.metadata().empty());
    EXPECT_TRUE(plan.segments().empty());
    EXPECT_EQ(plan.content_length(), std::uint64_t{0});

    ruvia::http_response response;
    bool empty_path_rejected = false;
    try {
        response.multipart_file_body(
            {}, 10, ruvia::http_response_file_identity::unchecked(), std::move(moved));
    } catch (const std::invalid_argument&) {
        empty_path_rejected = true;
    }
    EXPECT_TRUE(empty_path_rejected);

    bool moved_plan_rejected = false;
    try {
        const ruvia::http_native_path_string_type non_empty(1, static_cast<ruvia::http_native_path_char_type>('a'));
        response.multipart_file_body(
            non_empty, 10, ruvia::http_response_file_identity::unchecked(), std::move(plan));
    } catch (const std::invalid_argument&) {
        moved_plan_rejected = true;
    }
    EXPECT_TRUE(moved_plan_rejected);
}
