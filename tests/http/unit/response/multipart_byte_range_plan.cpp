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

#include "failing_memory_resource.h"
#include "test_harness.h"

namespace {

std::string materialize_multipart(const ruvia::http_multipart_byte_range_plan& plan, std::string_view payload) {
    std::string body;
    for (const auto& segment : plan.segments()) {
        if (segment.kind == ruvia::http_multipart_byte_range_plan::segment_kind::metadata) {
            const auto metadata = plan.metadata();
            if (segment.metadata_offset > metadata.size() ||
                segment.metadata_length > metadata.size() - segment.metadata_offset) {
                throw std::logic_error("multipart metadata segment is out of bounds");
            }
            body.append(metadata.substr(segment.metadata_offset, segment.metadata_length));
        } else {
            if (segment.file_offset > payload.size() ||
                segment.file_length > payload.size() - segment.file_offset) {
                throw std::logic_error("multipart file segment is out of bounds");
            }
            body.append(payload.substr(static_cast<std::size_t>(segment.file_offset),
                static_cast<std::size_t>(segment.file_length)));
        }
    }
    return body;
}

}  // namespace

RUVIA_TEST(multipart_range_plan_serializes_owned_inputs_and_every_segment_exactly) {
    const auto ranges = ruvia::resolve_http_byte_range_set("bytes=8-9,0-1", 10);
    std::string media_type = "text/plain; charset=utf-8";
    std::string boundary = "range boundary";
    std::string encoding = "gzip";
    auto plan = ruvia::make_http_multipart_byte_range_plan(
        ranges, 10, media_type, boundary, encoding, std::pmr::new_delete_resource());
    media_type.assign(media_type.size(), 'x');
    boundary.assign(boundary.size(), 'x');
    encoding.assign(encoding.size(), 'x');
    const std::string_view expected =
        "--range boundary\r\nContent-Type: text/plain; charset=utf-8\r\n"
        "Content-Encoding: gzip\r\nContent-Range: bytes 8-9/10\r\n\r\n89\r\n"
        "--range boundary\r\nContent-Type: text/plain; charset=utf-8\r\n"
        "Content-Encoding: gzip\r\nContent-Range: bytes 0-1/10\r\n\r\n01\r\n"
        "--range boundary--\r\n";
    RUVIA_CHECK_EQ(plan.content_type(), "multipart/byteranges; boundary=\"range boundary\"");
    RUVIA_CHECK_EQ(materialize_multipart(plan, "0123456789"), expected);
    RUVIA_CHECK_EQ(plan.content_length(), expected.size());
}

RUVIA_TEST(multipart_range_plan_preserves_all_ranges_across_metadata_growth) {
    std::string field = "bytes=";
    std::string expected;
    constexpr std::string_view payload = "0123456789abcdefghijklmnopqrstuv";
    for (std::size_t index = 0; index < ruvia::http_byte_range_set::capacity; ++index) {
        const auto offset = std::to_string(index * 2);
        if (index != 0) {
            field.push_back(',');
        }
        field.append(offset + "-" + offset);
        expected.append("--all_ranges\r\nContent-Type: application/octet-stream\r\nContent-Range: bytes ");
        expected.append(offset + "-" + offset + "/32\r\n\r\n");
        expected.push_back(payload[index * 2]);
        expected.append("\r\n");
    }
    expected.append("--all_ranges--\r\n");
    const auto ranges = ruvia::resolve_http_byte_range_set(field, payload.size());
    RUVIA_CHECK_EQ(ranges.size(), ruvia::http_byte_range_set::capacity);
    const auto plan = ruvia::make_http_multipart_byte_range_plan(
        ranges, payload.size(), "application/octet-stream", "all_ranges", {}, std::pmr::new_delete_resource());
    RUVIA_CHECK_EQ(materialize_multipart(plan, payload), expected);
    RUVIA_CHECK_EQ(plan.content_length(), expected.size());
}

RUVIA_TEST(multipart_range_plan_retains_cloned_storage_until_owner_destruction) {
    failing_memory_resource source_resource;
    failing_memory_resource retained_resource;
    const auto ranges = ruvia::resolve_http_byte_range_set("bytes=0-1,8-9", 10);
    {
        auto retained = [&] {
            auto source = ruvia::make_http_multipart_byte_range_plan(
                ranges, 10, "text/plain", "retained_boundary", {}, &source_resource);
            return source.clone(&retained_resource);
        }();
        RUVIA_CHECK_EQ(source_resource.live_allocations(), std::size_t{0});
        const auto baseline = retained_resource.live_allocations();
        RUVIA_CHECK(baseline != 0);
        const auto metadata = retained.metadata().data();
        const auto expected = materialize_multipart(retained, "0123456789");
        for (std::size_t index = 0; index < 32; ++index) {
            {
                auto temporary = ruvia::make_http_multipart_byte_range_plan(
                    ranges, 10, "application/octet-stream", "temporary_boundary", {}, &source_resource);
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
                ranges, 10, "application/octet-stream; charset=utf-8", boundary, "gzip", &resource);
            RUVIA_CHECK(plan.content_length() != 0);
            succeeded = true;
        } catch (const std::bad_alloc&) {
        }
        resource.allow_allocations();
        RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
    }
    RUVIA_CHECK(succeeded);
    {
        const auto source = ruvia::make_http_multipart_byte_range_plan(
            ranges, 10, "text/plain", "clone_boundary", {}, &resource);
        const auto baseline = resource.live_allocations();
        failing_memory_resource clone_resource;
        succeeded = false;
        for (std::size_t failure = 0; failure < 64 && !succeeded; ++failure) {
            clone_resource.fail_after(failure);
            try {
                const auto clone = source.clone(&clone_resource);
                RUVIA_CHECK_EQ(clone.metadata(), source.metadata());
                RUVIA_CHECK_EQ(clone.content_length(), source.content_length());
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
            ranges, maximum, "text/plain", boundary, {}, &resource);
        RUVIA_CHECK(plan.metadata().find("Content-Range: bytes 18446744073709551613-18446744073709551613/18446744073709551615\r\n") != std::string_view::npos);
        RUVIA_CHECK(plan.metadata().find("Content-Range: bytes 0-0/18446744073709551615\r\n") != std::string_view::npos);
        RUVIA_CHECK_EQ(plan.content_length(), plan.metadata().size() + std::uint64_t{2});
    }
    RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
    bool rejected = false;
    try {
        (void)ruvia::make_http_multipart_byte_range_plan(ranges, 10, "text/plain", boundary, {}, &resource);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    RUVIA_CHECK(rejected);
    RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
    const auto overflowing = ruvia::resolve_http_byte_range_set(
        "bytes=0-9223372036854775806,9223372036854775808-18446744073709551614", maximum);
    rejected = false;
    try {
        (void)ruvia::make_http_multipart_byte_range_plan(overflowing, maximum, "text/plain", boundary, {}, &resource);
    } catch (const std::length_error&) {
        rejected = true;
    }
    RUVIA_CHECK(rejected);
    RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
}
