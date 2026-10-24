#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory_resource>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/http/http_multipart_byte_range_plan.h"
#include "ruvia/http/http_response.h"

#include "failing_memory_resource.h"
#include "test_harness.h"

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

RUVIA_TEST(multipart_range_plan_serializes_owned_inputs_and_every_segment_exactly) {
    const auto ranges = ruvia::resolve_http_byte_range_set("bytes=8-9,0-1", 10);
    std::string media_type = "text/plain; charset=utf-8";
    std::string boundary = "range boundary";
    auto plan = ruvia::make_http_multipart_byte_range_plan(
        ranges, 10, media_type, boundary, std::pmr::new_delete_resource());
    media_type.assign(media_type.size(), 'x');
    boundary.assign(boundary.size(), 'x');
    const std::string_view expected =
        "--range boundary\r\nContent-Type: text/plain; charset=utf-8\r\n"
        "Content-Range: bytes 8-9/10\r\n\r\n89\r\n"
        "--range boundary\r\nContent-Type: text/plain; charset=utf-8\r\n"
        "Content-Range: bytes 0-1/10\r\n\r\n01\r\n"
        "--range boundary--\r\n";
    RUVIA_CHECK_EQ(plan.content_type(), "multipart/byteranges; boundary=\"range boundary\"");
    RUVIA_CHECK_EQ(materialize_multipart(plan, "0123456789"), expected);
    RUVIA_CHECK_EQ(plan.content_length(), expected.size());
}

RUVIA_TEST(multipart_range_plan_preserves_all_ranges_across_metadata_growth) {
    std::string field = "bytes=";
    std::string expected;
    constexpr std::string_view payload_value = "0123456789abcdefghijklmnopqrstuv";
    for (std::size_t index = 0; index < ruvia::http_byte_range_set::capacity; ++index) {
        const auto offset = std::to_string(index * 2);
        if (index != 0) {
            field.push_back(',');
        }
        field.append(offset + "-" + offset);
        expected.append("--all_ranges\r\nContent-Type: application/octet-stream\r\nContent-Range: bytes ");
        expected.append(offset + "-" + offset + "/32\r\n\r\n");
        expected.push_back(payload_value[index * 2]);
        expected.append("\r\n");
    }
    expected.append("--all_ranges--\r\n");
    const auto ranges = ruvia::resolve_http_byte_range_set(field, payload_value.size());
    RUVIA_CHECK_EQ(ranges.size(), ruvia::http_byte_range_set::capacity);
    const auto plan = ruvia::make_http_multipart_byte_range_plan(
        ranges, payload_value.size(), "application/octet-stream", "all_ranges", std::pmr::new_delete_resource());
    RUVIA_CHECK_EQ(materialize_multipart(plan, payload_value), expected);
    RUVIA_CHECK_EQ(plan.content_length(), expected.size());
}

RUVIA_TEST(multipart_range_plan_retains_cloned_storage_until_owner_destruction) {
    failing_memory_resource source_resource;
    failing_memory_resource retained_resource;
    const auto ranges = ruvia::resolve_http_byte_range_set("bytes=0-1,8-9", 10);
    {
        auto retained = [&] {
            auto source_value = ruvia::make_http_multipart_byte_range_plan(
                ranges, 10, "text/plain", "retained_boundary", &source_resource);
            return source_value.clone(&retained_resource);
        }();
        RUVIA_CHECK_EQ(source_resource.live_allocations(), std::size_t{0});
        const auto baseline = retained_resource.live_allocations();
        RUVIA_CHECK(baseline != 0);
        const auto metadata = retained.metadata().data();
        const auto expected = materialize_multipart(retained, "0123456789");
        for (std::size_t index = 0; index < 32; ++index) {
            {
                auto temporary = ruvia::make_http_multipart_byte_range_plan(
                    ranges, 10, "application/octet-stream", "temporary_boundary", &source_resource);
                RUVIA_CHECK(temporary.content_length() != 0);
            }
            RUVIA_CHECK_EQ(source_resource.live_allocations(), std::size_t{0});
            RUVIA_CHECK_EQ(retained_resource.live_allocations(), baseline);
            RUVIA_CHECK_EQ(materialize_multipart(retained, "0123456789"), expected);
        }
        retained_resource.fail_after(0);
        auto moved = std::move(retained);
        retained_resource.allow_allocations();
        RUVIA_CHECK(moved.resource() == &retained_resource);
        RUVIA_CHECK(moved.metadata().data() == metadata);
        RUVIA_CHECK_EQ(materialize_multipart(moved, "0123456789"), expected);
    }
    RUVIA_CHECK_EQ(retained_resource.live_allocations(), std::size_t{0});
}

RUVIA_TEST(multipart_range_plan_allocation_failures_release_partial_storage_and_allow_retry) {
    const auto ranges = ruvia::resolve_http_byte_range_set("bytes=0-1,8-9", 10);
    const std::string boundary(70, 'b');
    failing_memory_resource resource;
    bool succeeded = false;
    for (std::size_t failure = 0; failure < 64 && !succeeded; ++failure) {
        resource.fail_after(failure);
        try {
            const auto plan = ruvia::make_http_multipart_byte_range_plan(
                ranges, 10, "application/octet-stream; charset=utf-8", boundary, &resource);
            RUVIA_CHECK(plan.content_length() != 0);
            succeeded = true;
        } catch (const std::bad_alloc&) {
        }
        resource.allow_allocations();
        RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
    }
    RUVIA_CHECK(succeeded);
    {
        const auto source_value = ruvia::make_http_multipart_byte_range_plan(
            ranges, 10, "text/plain", "clone_boundary", &resource);
        const auto baseline = resource.live_allocations();
        failing_memory_resource clone_resource;
        succeeded = false;
        for (std::size_t failure = 0; failure < 64 && !succeeded; ++failure) {
            clone_resource.fail_after(failure);
            try {
                const auto clone = source_value.clone(&clone_resource);
                RUVIA_CHECK_EQ(clone.metadata(), source_value.metadata());
                RUVIA_CHECK_EQ(clone.content_length(), source_value.content_length());
                succeeded = true;
            } catch (const std::bad_alloc&) {
            }
            clone_resource.allow_allocations();
            RUVIA_CHECK_EQ(clone_resource.live_allocations(), std::size_t{0});
            RUVIA_CHECK_EQ(resource.live_allocations(), baseline);
        }
        RUVIA_CHECK(succeeded);
    }
    RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
}

RUVIA_TEST(multipart_range_plan_formats_uint64_edges_and_reclaims_error_storage) {
    failing_memory_resource resource;
    const auto maximum = (std::numeric_limits<std::uint64_t>::max)();
    const auto ranges = ruvia::resolve_http_byte_range_set("bytes=18446744073709551613-18446744073709551613,0-0", maximum);
    const std::string boundary(70, 'b');
    {
        const auto plan = ruvia::make_http_multipart_byte_range_plan(
            ranges, maximum, "text/plain", boundary, &resource);
        RUVIA_CHECK(plan.metadata().find("Content-Range: bytes 18446744073709551613-18446744073709551613/18446744073709551615\r\n") != std::string_view::npos);
        RUVIA_CHECK(plan.metadata().find("Content-Range: bytes 0-0/18446744073709551615\r\n") != std::string_view::npos);
        RUVIA_CHECK_EQ(plan.content_length(), plan.metadata().size() + std::uint64_t{2});
    }
    RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
    bool rejected = false;
    try {
        (void)ruvia::make_http_multipart_byte_range_plan(ranges, 10, "text/plain", boundary, &resource);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    RUVIA_CHECK(rejected);
    RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
    const auto overflowing = ruvia::resolve_http_byte_range_set(
        "bytes=0-9223372036854775806,9223372036854775808-18446744073709551614", maximum);
    rejected = false;
    try {
        (void)ruvia::make_http_multipart_byte_range_plan(overflowing, maximum, "text/plain", boundary, &resource);
    } catch (const std::length_error&) {
        rejected = true;
    }
    RUVIA_CHECK(rejected);
    RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
}

RUVIA_TEST(multipart_range_plan_moved_from_and_empty_file_path_are_rejected_cleanly) {
    const auto ranges = ruvia::resolve_http_byte_range_set("bytes=0-1,4-5", 10);
    auto plan = ruvia::make_http_multipart_byte_range_plan(
        ranges, 10, "text/plain", "b", std::pmr::get_default_resource());
    auto moved = std::move(plan);
    RUVIA_CHECK(plan.content_type().empty());
    RUVIA_CHECK(plan.metadata().empty());
    RUVIA_CHECK(plan.segments().empty());
    RUVIA_CHECK_EQ(plan.content_length(), std::uint64_t{0});

    ruvia::http_response response;
    bool empty_path_rejected = false;
    try {
        response.multipart_file_body(
            {}, 10, ruvia::http_response_file_identity::unchecked(), std::move(moved));
    } catch (const std::invalid_argument&) {
        empty_path_rejected = true;
    }
    RUVIA_CHECK(empty_path_rejected);

    bool moved_plan_rejected = false;
    try {
        const ruvia::http_native_path_string_type non_empty(1, static_cast<ruvia::http_native_path_char_type>('a'));
        response.multipart_file_body(
            non_empty, 10, ruvia::http_response_file_identity::unchecked(), std::move(plan));
    } catch (const std::invalid_argument&) {
        moved_plan_rejected = true;
    }
    RUVIA_CHECK(moved_plan_rejected);
}
