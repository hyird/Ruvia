#include <algorithm>
#include <array>
#include <memory_resource>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include "ruvia/http/Http3FieldSection.h"
#include "ruvia/http/Http3QpackConnection.h"
#include "ruvia/http/Http3ResponseWriter.h"
#include "ruvia/http/HttpInterimResponse.h"
#include "ruvia/http/HttpLimits.h"
#include "ruvia/http/HttpResponseStream.h"

#include "test_harness.h"

namespace {
struct Fields final {
    std::vector<std::string> names;
    std::vector<std::string> values;
};

bool collect(void* opaque, ruvia::Http3FieldSectionFieldView field) {
    auto& fields = *static_cast<Fields*>(opaque);
    fields.names.emplace_back(field.name);
    fields.values.emplace_back(field.value);
    return true;
}

class CountingResource final : public std::pmr::memory_resource {
public:
    explicit CountingResource(const void* equality_group = nullptr)
        : equality_group_(equality_group ? equality_group : this) {}

    std::size_t allocations{};
    std::size_t deallocations{};

    bool fail_allocations_{};

private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        if (fail_allocations_) {
            throw std::bad_alloc();
        }
        ++allocations;
        return std::pmr::new_delete_resource()->allocate(bytes, alignment);
    }
    void do_deallocate(void* ptr, std::size_t bytes, std::size_t alignment) override {
        ++deallocations;
        std::pmr::new_delete_resource()->deallocate(ptr, bytes, alignment);
    }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        const auto* resource = dynamic_cast<const CountingResource*>(&other);
        return resource && equality_group_ == resource->equality_group_;
    }
    const void* equality_group_;
};
}  // namespace

RUVIA_TEST(http3_interim_and_streaming_heads_project_mixed_case_names_in_order) {
    const std::array headers{
        ruvia::HttpHeaderView{"X-Long-Mixed-Case-Header", "first"},
        ruvia::HttpHeaderView{"x-lowercase-header-name", "lower"},
        ruvia::HttpHeaderView{"X-Long-Mixed-Case-Header", "second"}};
    const ruvia::HttpInterimResponseHead interim(ruvia::http_status::kEarlyHints, headers);
    CountingResource interim_resource;
    {
        const auto result = ruvia::encodeHttp3InterimResponseHead(interim, {}, &interim_resource);
        RUVIA_CHECK(result.has_value());
        if (result) {
            Fields decoded;
            RUVIA_CHECK(ruvia::decodeHttp3FieldSection(result->field_section.fieldSection, collect, &decoded).has_value());
            RUVIA_CHECK_EQ(decoded.names.size(), 4U);
            if (decoded.names.size() == 4) {
                RUVIA_CHECK_EQ(decoded.names[0], ":status");
                RUVIA_CHECK_EQ(decoded.values[0], "103");
                RUVIA_CHECK_EQ(decoded.names[1], "x-long-mixed-case-header");
                RUVIA_CHECK_EQ(decoded.values[1], "first");
                RUVIA_CHECK_EQ(decoded.names[2], "x-lowercase-header-name");
                RUVIA_CHECK_EQ(decoded.values[2], "lower");
                RUVIA_CHECK_EQ(decoded.names[3], "x-long-mixed-case-header");
                RUVIA_CHECK_EQ(decoded.values[3], "second");
            }
        }
    }
    RUVIA_CHECK(interim_resource.allocations > 0);
    RUVIA_CHECK_EQ(interim_resource.allocations, interim_resource.deallocations);

    CountingResource streaming_resource;
    {
        ruvia::HttpResponse response;
        response.header("X-Long-Mixed-Case-Header", "first");
        response.header("x-lowercase-header-name", "lower");
        response.header("X-Long-Mixed-Case-Header", "second",
            {.mode = ruvia::HttpResponseHeaderMode::kAppend});
        const auto result = ruvia::encodeHttp3StreamingResponseHead(std::move(response),
            ruvia::HttpKnownMethod::kGet, ruvia::http_response_stream_kind::generic,
            ruvia::http_response_trailer_intent::none, {}, &streaming_resource);
        RUVIA_CHECK(result.has_value());
        if (result) {
            Fields decoded;
            RUVIA_CHECK(ruvia::decodeHttp3FieldSection(result->head.field_section.fieldSection, collect, &decoded).has_value());
            RUVIA_CHECK_EQ(decoded.names.size(), 5U);
            if (decoded.names.size() == 5) {
                RUVIA_CHECK_EQ(decoded.names[0], ":status");
                RUVIA_CHECK_EQ(decoded.values[0], "200");
                RUVIA_CHECK_EQ(decoded.names[1], "x-long-mixed-case-header");
                RUVIA_CHECK_EQ(decoded.values[1], "first");
                RUVIA_CHECK_EQ(decoded.names[2], "x-lowercase-header-name");
                RUVIA_CHECK_EQ(decoded.values[2], "lower");
                RUVIA_CHECK_EQ(decoded.names[3], "x-long-mixed-case-header");
                RUVIA_CHECK_EQ(decoded.values[3], "second");
                RUVIA_CHECK_EQ(decoded.names[4], "date");
            }
        }
    }
    RUVIA_CHECK(streaming_resource.allocations > 0);
    RUVIA_CHECK_EQ(streaming_resource.allocations, streaming_resource.deallocations);
}

RUVIA_TEST(http3_streaming_head_preserves_length_projects_sse_and_trailer_semantics) {
    CountingResource resource;
    {
        ruvia::HttpResponse response({.resource = &resource});
        response.header("Content-Length", "17");
        auto head = ruvia::encodeHttp3StreamingResponseHead(std::move(response), ruvia::HttpKnownMethod::kGet,
            ruvia::http_response_stream_kind::sse, ruvia::http_response_trailer_intent::present, {}, &resource);
        RUVIA_CHECK(head.has_value());
        if (head) {
            RUVIA_CHECK_EQ(head->head.declaredContentLength.value_or(0), 17U);
            RUVIA_CHECK(head->commit_plan.head_disposition() == ruvia::http_response_stream_head_disposition::body_open);
            RUVIA_CHECK(head->commit_plan.trailer_framing() == ruvia::http_response_stream_trailer_framing::http3_trailing_headers);
            Fields fields;
            RUVIA_CHECK(ruvia::decodeHttp3FieldSection(head->head.field_section.fieldSection, collect, &fields).has_value());
            RUVIA_CHECK(std::ranges::find(fields.values, "text/event-stream") != fields.values.end());
            RUVIA_CHECK(std::ranges::find(fields.values, "no-store") != fields.values.end());
            RUVIA_CHECK(std::ranges::find(fields.names, "date") != fields.names.end());
            RUVIA_CHECK(std::ranges::find(fields.names, "transfer-encoding") == fields.names.end());
        }
    }
    RUVIA_CHECK_EQ(resource.allocations, resource.deallocations);
    for (const auto status : {ruvia::http_status::kOk, ruvia::http_status::kNoContent}) {
        ruvia::HttpResponse response;
        response.status(status);
        const auto head = ruvia::encodeHttp3StreamingResponseHead(std::move(response), ruvia::HttpKnownMethod::kHead,
            ruvia::http_response_stream_kind::generic, ruvia::http_response_trailer_intent::none);
        RUVIA_CHECK(head.has_value());
        if (head) {
            RUVIA_CHECK(head->commit_plan.head_disposition() == ruvia::http_response_stream_head_disposition::message_ended);
        }
    }
    ruvia::HttpResponse forbidden;
    forbidden.status(ruvia::http_status::kNoContent);
    RUVIA_CHECK(!ruvia::encodeHttp3StreamingResponseHead(std::move(forbidden), ruvia::HttpKnownMethod::kGet,
        ruvia::http_response_stream_kind::generic, ruvia::http_response_trailer_intent::present));
}

RUVIA_TEST(http3_response_head_encodes_status_and_fields_for_qpack_decode) {
    const std::array fields{ruvia::Http3FieldSectionFieldView{"content-type", "text/plain"}};
    std::pmr::monotonic_buffer_resource resource;
    const auto result = ruvia::encodeHttp3ResponseHead(
        ruvia::http_status::kOk, ruvia::HttpKnownMethod::kGet, fields, {}, &resource);
    RUVIA_CHECK(result.has_value());
    if (!result) {
        return;
    }
    Fields decoded;
    const auto count = ruvia::decodeHttp3FieldSection(result->field_section.fieldSection, collect, &decoded);
    RUVIA_CHECK(count.has_value());
    RUVIA_CHECK_EQ(decoded.names.size(), 2U);
    if (decoded.names.size() == 2) {
        RUVIA_CHECK_EQ(decoded.names[0], ":status");
        RUVIA_CHECK_EQ(decoded.values[0], "200");
        RUVIA_CHECK_EQ(decoded.names[1], "content-type");
        RUVIA_CHECK_EQ(decoded.values[1], "text/plain");
    }
    RUVIA_CHECK_EQ(result->field_section.decodedFieldSectionSize(), 96U);
    RUVIA_CHECK(result->bodyPlan.statusAllowsBody());
}

RUVIA_TEST(http3_response_writer_emits_canonical_rfc_static_references) {
    const std::array fields{ruvia::Http3FieldSectionFieldView{"content-type", "custom"}};
    const auto result = ruvia::encodeHttp3ResponseHead(
        ruvia::http_status::kContinue, ruvia::HttpKnownMethod::kGet, fields);
    RUVIA_CHECK(result.has_value());
    if (!result) {
        return;
    }

    // :status 100 is static index 63; content-type's static name reference is index 44.
    constexpr std::array<char, 13> canonicalWire{
        '\0', '\0', static_cast<char>(0xff), '\0', static_cast<char>(0x5f),
        static_cast<char>(0x1d), static_cast<char>(0x06), 'c', 'u', 's', 't', 'o', 'm'};
    RUVIA_CHECK_EQ(result->field_section.fieldSection.size(), canonicalWire.size());
    RUVIA_CHECK(std::equal(result->field_section.fieldSection.begin(), result->field_section.fieldSection.end(),
        canonicalWire.begin(), canonicalWire.end()));
}

RUVIA_TEST(http3_interim_response_writer_normalizes_null_resource_and_retains_owned_results) {
    const std::array headers{ruvia::HttpHeaderView{"Link", "</style.css>; rel=preload"}};
    const ruvia::HttpInterimResponseHead response(ruvia::http_status::kEarlyHints, headers);
    const auto static_null = ruvia::encodeHttp3InterimResponseHead(response, {}, nullptr);
    RUVIA_CHECK(static_null.has_value());
    if (static_null) {
        Fields decoded;
        const auto count = ruvia::decodeHttp3FieldSection(static_null->field_section.fieldSection, collect, &decoded);
        RUVIA_CHECK(count.has_value());
        RUVIA_CHECK_EQ(decoded.names.size(), 2U);
        if (decoded.names.size() == 2) {
            RUVIA_CHECK_EQ(decoded.names[0], ":status");
            RUVIA_CHECK_EQ(decoded.values[0], "103");
            RUVIA_CHECK_EQ(decoded.names[1], "link");
            RUVIA_CHECK_EQ(decoded.values[1], "</style.css>; rel=preload");
        }
    }

    ruvia::Http3QpackEncoder encoder({.maxTableCapacity = 0, .maxBlockedStreams = 0});
    const auto dynamic_null = ruvia::encodeHttp3InterimResponseHead(encoder, 0, response, {}, nullptr);
    RUVIA_CHECK(dynamic_null.has_value());
    if (dynamic_null) {
        Fields decoded;
        const auto count = ruvia::decodeHttp3FieldSection(dynamic_null->field_section.fieldSection, collect, &decoded);
        RUVIA_CHECK(count.has_value());
        RUVIA_CHECK_EQ(decoded.names.size(), 2U);
        if (decoded.names.size() == 2) {
            RUVIA_CHECK_EQ(decoded.names[0], ":status");
            RUVIA_CHECK_EQ(decoded.values[0], "103");
            RUVIA_CHECK_EQ(decoded.names[1], "link");
            RUVIA_CHECK_EQ(decoded.values[1], "</style.css>; rel=preload");
        }
    }

    CountingResource static_resource;
    {
        const auto first = ruvia::encodeHttp3InterimResponseHead(response, {}, &static_resource);
        RUVIA_CHECK(first.has_value());
        if (!first) {
            return;
        }
        RUVIA_CHECK(first->field_section.fieldSection.get_allocator().resource() == &static_resource);
        const std::vector<char> retained(first->field_section.fieldSection.begin(), first->field_section.fieldSection.end());
        const auto second = ruvia::encodeHttp3InterimResponseHead(response, {}, &static_resource);
        RUVIA_CHECK(second.has_value());
        RUVIA_CHECK(std::ranges::equal(first->field_section.fieldSection, retained));
        RUVIA_CHECK(static_resource.allocations > static_resource.deallocations);
    }
    RUVIA_CHECK_EQ(static_resource.allocations, static_resource.deallocations);

    CountingResource dynamic_resource;
    {
        ruvia::Http3QpackEncoder resource_encoder({.maxTableCapacity = 0, .maxBlockedStreams = 0});
        const auto result = ruvia::encodeHttp3InterimResponseHead(resource_encoder, 0, response, {}, &dynamic_resource);
        RUVIA_CHECK(result.has_value());
        if (result) {
            RUVIA_CHECK(result->field_section.fieldSection.get_allocator().resource() == &dynamic_resource);
            RUVIA_CHECK(dynamic_resource.allocations > dynamic_resource.deallocations);
        }
    }
    RUVIA_CHECK_EQ(dynamic_resource.allocations, dynamic_resource.deallocations);

    const std::array invalid_headers{ruvia::HttpHeaderView{"Connection", "close"}};
    const ruvia::HttpInterimResponseHead invalid_response(ruvia::http_status::kEarlyHints, invalid_headers);
    const auto invalid = ruvia::encodeHttp3InterimResponseHead(invalid_response, {}, nullptr);
    RUVIA_CHECK(!invalid);
    if (!invalid) {
        RUVIA_CHECK(invalid.error().kind == ruvia::Http3ResponseHeadError::kForbiddenField);
    }
}

RUVIA_TEST(http3_response_head_body_plan_suppresses_head_and_bodyless_statuses) {
    for (const auto status : {ruvia::http_status::kOk, ruvia::http_status::kNoContent,
             ruvia::http_status::kNotModified}) {
        const auto result = ruvia::encodeHttp3ResponseHead(status, ruvia::HttpKnownMethod::kHead, {});
        RUVIA_CHECK(result.has_value());
        if (result) {
            RUVIA_CHECK_EQ(result->field_section.decodedFieldSectionSize(), 42U);
            RUVIA_CHECK(result->bodyPlan.bodySuppressed());
        }
    }
    const auto noContent = ruvia::encodeHttp3ResponseHead(
        ruvia::http_status::kNoContent, ruvia::HttpKnownMethod::kGet, {});
    RUVIA_CHECK(noContent.has_value());
    if (noContent) {
        RUVIA_CHECK_EQ(noContent->field_section.decodedFieldSectionSize(), 42U);
        RUVIA_CHECK(noContent->bodyPlan.bodySuppressed());
    }
}

RUVIA_TEST(http3_response_head_decoded_size_counts_status_and_multiple_fields) {
    const auto statusOnly = ruvia::encodeHttp3ResponseHead(ruvia::http_status::kOk,
        ruvia::HttpKnownMethod::kGet, {},
        {.maxEncodedBytes = 1024, .maxDecodedBytes = 42, .maxFields = 1});
    RUVIA_CHECK(statusOnly.has_value());
    if (statusOnly) {
        RUVIA_CHECK_EQ(statusOnly->field_section.decodedFieldSectionSize(), 42U);
    }
    const auto statusOverLimit = ruvia::encodeHttp3ResponseHead(ruvia::http_status::kOk,
        ruvia::HttpKnownMethod::kGet, {},
        {.maxEncodedBytes = 1024, .maxDecodedBytes = 41, .maxFields = 1});
    RUVIA_CHECK(!statusOverLimit);
    if (!statusOverLimit) {
        RUVIA_CHECK(statusOverLimit.error().fieldSectionError ==
                    ruvia::Http3FieldSectionError::kFieldListTooLarge);
    }

    const std::array fields{ruvia::Http3FieldSectionFieldView{"x", "y"},
        ruvia::Http3FieldSectionFieldView{"long-name", "value"}};
    const auto exact = ruvia::encodeHttp3ResponseHead(ruvia::http_status::kOk,
        ruvia::HttpKnownMethod::kGet, fields,
        {.maxEncodedBytes = 1024, .maxDecodedBytes = 122, .maxFields = 3});
    RUVIA_CHECK(exact.has_value());
    if (exact) {
        RUVIA_CHECK_EQ(exact->field_section.decodedFieldSectionSize(), 122U);
    }
    const auto overLimit = ruvia::encodeHttp3ResponseHead(ruvia::http_status::kOk,
        ruvia::HttpKnownMethod::kGet, fields,
        {.maxEncodedBytes = 1024, .maxDecodedBytes = 121, .maxFields = 3});
    RUVIA_CHECK(!overLimit);
    if (!overLimit) {
        RUVIA_CHECK(overLimit.error().fieldSectionError ==
                    ruvia::Http3FieldSectionError::kFieldListTooLarge);
    }

    const std::string large_value(ruvia::kMaxHttpHeaderBytes + 1, 'v');
    const std::array large_fields{ruvia::Http3FieldSectionFieldView{"x", large_value}};
    const auto large_size = std::size_t{42 + 32 + 1} + large_value.size();
    const auto large = ruvia::encodeHttp3ResponseHead(ruvia::http_status::kOk,
        ruvia::HttpKnownMethod::kGet, large_fields,
        {.maxEncodedBytes = large_size * 2, .maxDecodedBytes = large_size, .maxFields = 2});
    RUVIA_CHECK(large.has_value());
    if (large) {
        RUVIA_CHECK_EQ(large->field_section.decodedFieldSectionSize(), large_size);
    }

    auto source = ruvia::encodeHttp3ResponseHead(
        ruvia::http_status::kOk, ruvia::HttpKnownMethod::kGet, {});
    RUVIA_CHECK(source.has_value());
    if (source) {
        auto moved = std::move(*source);
        RUVIA_CHECK_EQ(moved.field_section.decodedFieldSectionSize(), 42U);
        RUVIA_CHECK_EQ(source->field_section.decodedFieldSectionSize(), 0U);
        RUVIA_CHECK(source->field_section.fieldSection.empty());
        auto destination = ruvia::encodeHttp3ResponseHead(
            ruvia::http_status::kOk, ruvia::HttpKnownMethod::kGet, {});
        RUVIA_CHECK(destination.has_value());
        if (destination) {
            *destination = std::move(moved);
            RUVIA_CHECK_EQ(destination->field_section.decodedFieldSectionSize(), 42U);
            RUVIA_CHECK_EQ(moved.field_section.decodedFieldSectionSize(), 0U);
            RUVIA_CHECK(moved.field_section.fieldSection.empty());
        }
    }
}

RUVIA_TEST(http3_response_head_moves_and_extracts_owned_section_without_allocating) {
    CountingResource resource;
    CountingResource equivalent_resource(&resource);
    for (auto* destination_resource : {&resource, &equivalent_resource}) {
        {
            ruvia::Http3ResponseHead source(std::pmr::vector<char>(64, 's', &resource),
                ruvia::planHttpResponseBody(ruvia::HttpKnownMethod::kHead, ruvia::http_status::kOk), 91, 17);
            ruvia::Http3ResponseHead destination(std::pmr::vector<char>(32, 'd', destination_resource),
                ruvia::planHttpResponseBody(ruvia::HttpKnownMethod::kGet, ruvia::http_status::kOk), 42, 3);
            const auto* bytes = source.field_section.fieldSection.data();
            const auto allocations = resource.allocations + equivalent_resource.allocations;
            auto moved = std::move(source);
            RUVIA_CHECK(moved.field_section.fieldSection.data() == bytes);
            RUVIA_CHECK_EQ(source.field_section.decodedFieldSectionSize(), 0U);
            RUVIA_CHECK(source.field_section.fieldSection.empty());
            destination = std::move(moved);
            RUVIA_CHECK(destination.field_section.fieldSection.data() == bytes);
            RUVIA_CHECK(destination.field_section.fieldSection.get_allocator().resource() == destination_resource);
            RUVIA_CHECK_EQ(destination.field_section.decodedFieldSectionSize(), 91U);
            RUVIA_CHECK(destination.bodyPlan.bodySuppressed());
            RUVIA_CHECK_EQ(destination.declaredContentLength.value_or(0), 17U);
            RUVIA_CHECK_EQ(moved.field_section.decodedFieldSectionSize(), 0U);
            RUVIA_CHECK(moved.field_section.fieldSection.empty());
            auto section = std::move(destination.field_section);
            RUVIA_CHECK(section.fieldSection.data() == bytes);
            RUVIA_CHECK_EQ(section.decodedFieldSectionSize(), 91U);
            RUVIA_CHECK_EQ(destination.field_section.decodedFieldSectionSize(), 0U);
            RUVIA_CHECK(destination.field_section.fieldSection.empty());
            RUVIA_CHECK(destination.bodyPlan.bodySuppressed());
            RUVIA_CHECK_EQ(destination.declaredContentLength.value_or(0), 17U);
            RUVIA_CHECK_EQ(resource.allocations + equivalent_resource.allocations, allocations);
        }
        RUVIA_CHECK_EQ(resource.allocations + equivalent_resource.allocations,
            resource.deallocations + equivalent_resource.deallocations);
    }
}

RUVIA_TEST(http3_response_head_cross_resource_assignment_preserves_metadata_on_allocation_failure) {
    CountingResource source_resource;
    CountingResource destination_resource;
    {
        ruvia::Http3ResponseHead source(std::pmr::vector<char>(64, 's', &source_resource),
            ruvia::planHttpResponseBody(ruvia::HttpKnownMethod::kHead, ruvia::http_status::kOk), 91, 17);
        ruvia::Http3ResponseHead destination(std::pmr::vector<char>(32, 'd', &destination_resource),
            ruvia::planHttpResponseBody(ruvia::HttpKnownMethod::kGet, ruvia::http_status::kOk), 42, 3);
        const auto* source_bytes = source.field_section.fieldSection.data();
        const auto* destination_bytes = destination.field_section.fieldSection.data();
        destination_resource.fail_allocations_ = true;
        bool threw = false;
        try {
            destination = std::move(source);
        } catch (const std::bad_alloc&) {
            threw = true;
        }
        RUVIA_CHECK(threw);
        RUVIA_CHECK(destination.field_section.fieldSection.data() == destination_bytes);
        RUVIA_CHECK_EQ(destination.field_section.fieldSection.size(), 32U);
        RUVIA_CHECK(std::ranges::all_of(destination.field_section.fieldSection, [](char byte) { return byte == 'd'; }));
        RUVIA_CHECK_EQ(destination.field_section.decodedFieldSectionSize(), 42U);
        RUVIA_CHECK(!destination.bodyPlan.bodySuppressed());
        RUVIA_CHECK_EQ(destination.declaredContentLength.value_or(0), 3U);
        RUVIA_CHECK(source.field_section.fieldSection.data() == source_bytes);
        RUVIA_CHECK_EQ(source.field_section.fieldSection.size(), 64U);
        RUVIA_CHECK(std::ranges::all_of(source.field_section.fieldSection, [](char byte) { return byte == 's'; }));
        RUVIA_CHECK_EQ(source.field_section.decodedFieldSectionSize(), 91U);
        RUVIA_CHECK(source.bodyPlan.bodySuppressed());
        RUVIA_CHECK_EQ(source.declaredContentLength.value_or(0), 17U);

        destination_resource.fail_allocations_ = false;
        const auto allocations = destination_resource.allocations;
        destination = std::move(source);
        RUVIA_CHECK_EQ(destination_resource.allocations, allocations + 1);
        RUVIA_CHECK(destination.field_section.fieldSection.get_allocator().resource() == &destination_resource);
        RUVIA_CHECK_EQ(destination.field_section.fieldSection.size(), 64U);
        RUVIA_CHECK(std::ranges::all_of(destination.field_section.fieldSection, [](char byte) { return byte == 's'; }));
        RUVIA_CHECK_EQ(destination.field_section.decodedFieldSectionSize(), 91U);
        RUVIA_CHECK(destination.bodyPlan.bodySuppressed());
        RUVIA_CHECK_EQ(destination.declaredContentLength.value_or(0), 17U);
        RUVIA_CHECK_EQ(source.field_section.decodedFieldSectionSize(), 0U);
        RUVIA_CHECK(source.field_section.fieldSection.empty());
    }
    RUVIA_CHECK_EQ(source_resource.allocations, source_resource.deallocations);
    RUVIA_CHECK_EQ(destination_resource.allocations, destination_resource.deallocations);
}

RUVIA_TEST(http3_response_head_rejects_http1_fields_and_response_te) {
    for (const auto field : {ruvia::Http3FieldSectionFieldView{"connection", "close"},
             ruvia::Http3FieldSectionFieldView{"transfer-encoding", "chunked"},
             ruvia::Http3FieldSectionFieldView{"te", "gzip"},
             ruvia::Http3FieldSectionFieldView{"te", "trailers"}}) {
        const std::array fields{field};
        const auto result = ruvia::encodeHttp3ResponseHead(
            ruvia::http_status::kOk, ruvia::HttpKnownMethod::kGet, fields);
        RUVIA_CHECK(!result);
        if (!result) {
            RUVIA_CHECK(result.error().kind == ruvia::Http3ResponseHeadError::kForbiddenField);
        }
    }
}

RUVIA_TEST(http3_response_head_validates_content_length_and_status_rules) {
    const std::array validLength{ruvia::Http3FieldSectionFieldView{"content-length", "18446744073709551615"}};
    const auto valid = ruvia::encodeHttp3ResponseHead(
        ruvia::http_status::kOk, ruvia::HttpKnownMethod::kGet, validLength);
    RUVIA_CHECK(valid.has_value());

    const std::array duplicateEqual{
        ruvia::Http3FieldSectionFieldView{"content-length", "00042"},
        ruvia::Http3FieldSectionFieldView{"content-length", "42"}};
    RUVIA_CHECK(ruvia::encodeHttp3ResponseHead(
        ruvia::http_status::kOk, ruvia::HttpKnownMethod::kGet, duplicateEqual)
            .has_value());

    for (const auto value : {"", "+1", "-1", " 1", "1 ", "1, 1", "18446744073709551616"}) {
        const std::array fields{ruvia::Http3FieldSectionFieldView{"content-length", value}};
        const auto result = ruvia::encodeHttp3ResponseHead(
            ruvia::http_status::kOk, ruvia::HttpKnownMethod::kGet, fields);
        RUVIA_CHECK(!result);
        if (!result) {
            RUVIA_CHECK(result.error().kind == ruvia::Http3ResponseHeadError::kInvalidField);
        }
    }

    const std::array conflicting{
        ruvia::Http3FieldSectionFieldView{"content-length", "42"},
        ruvia::Http3FieldSectionFieldView{"content-length", "43"}};
    const auto mismatch = ruvia::encodeHttp3ResponseHead(
        ruvia::http_status::kOk, ruvia::HttpKnownMethod::kGet, conflicting);
    RUVIA_CHECK(!mismatch);
    if (!mismatch) {
        RUVIA_CHECK(mismatch.error().kind == ruvia::Http3ResponseHeadError::kInvalidField);
    }

    for (const auto status : {ruvia::http_status::kContinue, ruvia::http_status::kNoContent}) {
        const std::array fields{ruvia::Http3FieldSectionFieldView{"content-length", "0"}};
        const auto result = ruvia::encodeHttp3ResponseHead(status, ruvia::HttpKnownMethod::kGet, fields);
        RUVIA_CHECK(!result);
        if (!result) {
            RUVIA_CHECK(result.error().kind == ruvia::Http3ResponseHeadError::kInvalidField);
        }
    }
    const std::array lengthFor304{ruvia::Http3FieldSectionFieldView{"content-length", "12"}};
    RUVIA_CHECK(ruvia::encodeHttp3ResponseHead(
        ruvia::http_status::kNotModified, ruvia::HttpKnownMethod::kGet, lengthFor304)
            .has_value());

    const auto switching = ruvia::encodeHttp3ResponseHead(
        ruvia::http_status::kSwitchingProtocols, ruvia::HttpKnownMethod::kGet, {});
    RUVIA_CHECK(!switching);
    if (!switching) {
        RUVIA_CHECK(switching.error().kind == ruvia::Http3ResponseHeadError::kUnsupportedStatus);
    }
}

RUVIA_TEST(http3_response_writer_projects_buffered_response_and_canonical_length) {
    ruvia::HttpResponse response;
    response.body("payload");
    response.header("X-MiXeD", "ok");
    response.header("Set-Cookie", "a=1", {.mode = ruvia::HttpResponseHeaderMode::kAppend});
    response.header("Set-Cookie", "b=2", {.mode = ruvia::HttpResponseHeaderMode::kAppend});
    const auto plan = ruvia::planBufferedHttpResponseWrite(ruvia::HttpKnownMethod::kGet, response);
    const auto encoded = ruvia::encodeHttp3ResponseHead(response, plan);
    RUVIA_CHECK(encoded.has_value());
    if (!encoded) {
        return;
    }
    Fields decoded;
    const auto count = ruvia::decodeHttp3FieldSection(encoded->field_section.fieldSection, collect, &decoded);
    RUVIA_CHECK(count.has_value());
    RUVIA_CHECK(std::find(decoded.names.begin(), decoded.names.end(), "x-mixed") != decoded.names.end());
    RUVIA_CHECK(std::count(decoded.names.begin(), decoded.names.end(), "set-cookie") == 2);
    RUVIA_CHECK(std::find(decoded.values.begin(), decoded.values.end(), "7") != decoded.values.end());
    RUVIA_CHECK(std::find(decoded.names.begin(), decoded.names.end(), "date") != decoded.names.end());
    RUVIA_CHECK_EQ(encoded->field_section.decodedFieldSectionSize(), 285U);

    response.body("change");
    RUVIA_CHECK(!ruvia::encodeHttp3ResponseHead(response, plan));
}

RUVIA_TEST(http3_response_writer_preserves_one_application_date) {
    ruvia::HttpResponse response;
    response.header("Date", "Wed, 21 Oct 2015 07:28:00 GMT");
    response.body("ok");
    const auto plan =
        ruvia::planBufferedHttpResponseWrite(ruvia::HttpKnownMethod::kGet, response);
    const auto encoded = ruvia::encodeHttp3ResponseHead(response, plan);
    RUVIA_CHECK(encoded.has_value());
    if (!encoded) {
        return;
    }
    Fields decoded;
    RUVIA_CHECK(
        ruvia::decodeHttp3FieldSection(encoded->field_section.fieldSection, collect, &decoded).has_value());
    RUVIA_CHECK_EQ(std::count(decoded.names.begin(), decoded.names.end(), "date"), 1);
    const auto date = std::find(decoded.names.begin(), decoded.names.end(), "date");
    RUVIA_CHECK(date != decoded.names.end());
    if (date != decoded.names.end()) {
        const auto index = static_cast<std::size_t>(date - decoded.names.begin());
        RUVIA_CHECK_EQ(decoded.values[index], "Wed, 21 Oct 2015 07:28:00 GMT");
    }
}

RUVIA_TEST(http3_response_writer_reports_final_automatic_and_explicit_content_length_size) {
    ruvia::HttpResponse automaticResponse;
    automaticResponse.body("payload");
    const auto automaticPlan = ruvia::planBufferedHttpResponseWrite(ruvia::HttpKnownMethod::kGet,
        automaticResponse);
    const auto automatic = ruvia::encodeHttp3ResponseHead(automaticResponse, automaticPlan,
        {.maxEncodedBytes = 1024, .maxDecodedBytes = 154, .maxFields = 3});
    RUVIA_CHECK(automatic.has_value());
    if (automatic) {
        RUVIA_CHECK_EQ(automatic->field_section.decodedFieldSectionSize(), 154U);
    }
    const auto automaticOverLimit = ruvia::encodeHttp3ResponseHead(automaticResponse, automaticPlan,
        {.maxEncodedBytes = 1024, .maxDecodedBytes = 153, .maxFields = 3});
    RUVIA_CHECK(!automaticOverLimit);
    if (!automaticOverLimit) {
        RUVIA_CHECK(automaticOverLimit.error().fieldSectionError ==
                    ruvia::Http3FieldSectionError::kFieldListTooLarge);
    }

    ruvia::HttpResponse explicitResponse;
    explicitResponse.body("payload");
    explicitResponse.header("Content-Length", "0007");
    const auto explicitPlan = ruvia::planBufferedHttpResponseWrite(ruvia::HttpKnownMethod::kGet,
        explicitResponse);
    const auto explicitLength = ruvia::encodeHttp3ResponseHead(explicitResponse, explicitPlan,
        {.maxEncodedBytes = 1024, .maxDecodedBytes = 154, .maxFields = 3});
    RUVIA_CHECK(explicitLength.has_value());
    if (explicitLength) {
        RUVIA_CHECK_EQ(explicitLength->field_section.decodedFieldSectionSize(), 154U);
        Fields decoded;
        RUVIA_CHECK(ruvia::decodeHttp3FieldSection(explicitLength->field_section.fieldSection, collect, &decoded).has_value());
        const auto length = std::find(decoded.names.begin(), decoded.names.end(), "content-length");
        RUVIA_CHECK(length != decoded.names.end());
        if (length != decoded.names.end()) {
            const auto index = static_cast<std::size_t>(length - decoded.names.begin());
            RUVIA_CHECK_EQ(decoded.values[index], "7");
        }
    }
}

RUVIA_TEST(http3_response_writer_head_and_status_content_length_projection) {
    ruvia::HttpResponse headResponse;
    headResponse.fileBody("unused.bin", 123, 0, 123, ruvia::HttpResponseFileIdentity::unchecked());
    const auto headPlan = ruvia::planBufferedHttpResponseWrite(ruvia::HttpKnownMethod::kHead, headResponse);
    const auto head = ruvia::encodeHttp3ResponseHead(headResponse, headPlan);
    RUVIA_CHECK(head.has_value());
    if (head) {
        Fields decoded;
        RUVIA_CHECK(ruvia::decodeHttp3FieldSection(head->field_section.fieldSection, collect, &decoded).has_value());
        RUVIA_CHECK(std::find(decoded.values.begin(), decoded.values.end(), "123") != decoded.values.end());
        RUVIA_CHECK_EQ(head->field_section.decodedFieldSectionSize(), 156U);
        RUVIA_CHECK(head->bodyPlan.bodySuppressed());
    }

    for (const auto status : {ruvia::http_status::kNoContent, ruvia::http_status::kResetContent}) {
        ruvia::HttpResponse response;
        response.status(status);
        const auto plan = ruvia::planBufferedHttpResponseWrite(ruvia::HttpKnownMethod::kGet, response);
        const auto result = ruvia::encodeHttp3ResponseHead(response, plan);
        RUVIA_CHECK(result.has_value());
        if (result) {
            Fields decoded;
            RUVIA_CHECK(ruvia::decodeHttp3FieldSection(result->field_section.fieldSection, collect, &decoded).has_value());
            const auto expected = status == ruvia::http_status::kResetContent ? "0" : "";
            if (expected[0] != '\0') {
                RUVIA_CHECK(std::find(decoded.values.begin(), decoded.values.end(), expected) != decoded.values.end());
            } else {
                RUVIA_CHECK(std::find(decoded.names.begin(), decoded.names.end(), "content-length") == decoded.names.end());
            }
        }
    }
    ruvia::HttpResponse notModified;
    notModified.status(ruvia::http_status::kNotModified);
    notModified.header("Content-Length", "12");
    const auto notModifiedPlan = ruvia::planBufferedHttpResponseWrite(ruvia::HttpKnownMethod::kGet, notModified);
    const auto notModifiedHead = ruvia::encodeHttp3ResponseHead(notModified, notModifiedPlan);
    RUVIA_CHECK(notModifiedHead.has_value());
}

RUVIA_TEST(http3_response_writer_accepts_compact_static_qpack_under_encoded_limit) {
    ruvia::HttpResponse response;
    response.status(ruvia::http_status::kNotModified);
    response.header("Content-Type", "application/json");
    const auto plan = ruvia::planBufferedHttpResponseWrite(ruvia::HttpKnownMethod::kGet, response);
    const auto encoded = ruvia::encodeHttp3ResponseHead(response, plan,
        {.maxEncodedBytes = 64, .maxDecodedBytes = 1024, .maxFields = 3});
    RUVIA_CHECK(encoded.has_value());
    if (encoded) {
        RUVIA_CHECK(encoded->field_section.fieldSection.size() <= 64);
    }
}

RUVIA_TEST(http3_response_writer_rejects_invalid_projected_headers_and_limits) {
    for (const auto& [name, value] : {std::pair{"Connection", "close"}}) {
        ruvia::HttpResponse response;
        response.header(name, value);
        const auto plan = ruvia::planBufferedHttpResponseWrite(ruvia::HttpKnownMethod::kGet, response);
        RUVIA_CHECK(!ruvia::encodeHttp3ResponseHead(response, plan));
    }
    const std::array injected{ruvia::Http3FieldSectionFieldView{"x-test", "bad\r\ninjected: yes"}};
    RUVIA_CHECK(!ruvia::encodeHttp3ResponseHead(
        ruvia::http_status::kOk, ruvia::HttpKnownMethod::kGet, injected));
    ruvia::HttpResponse response;
    response.body("x");
    response.header("Content-Length", "2");
    const auto plan = ruvia::planBufferedHttpResponseWrite(ruvia::HttpKnownMethod::kGet, response);
    RUVIA_CHECK(!ruvia::encodeHttp3ResponseHead(response, plan));
    response.removeHeader("Content-Length");
    const auto validPlan = ruvia::planBufferedHttpResponseWrite(ruvia::HttpKnownMethod::kGet, response);
    const auto limited = ruvia::encodeHttp3ResponseHead(response, validPlan,
        {.maxEncodedBytes = 64, .maxDecodedBytes = 64, .maxFields = 1});
    RUVIA_CHECK(!limited);
}

RUVIA_TEST(http3_response_trailers_encode_allowed_fields_in_order_and_lowercase) {
    const std::array fields{
        ruvia::Http3FieldSectionFieldView{"ETag", "\"v1\""},
        ruvia::Http3FieldSectionFieldView{"Accept-Ranges", "bytes"},
        ruvia::Http3FieldSectionFieldView{"X-Custom", "one"},
        ruvia::Http3FieldSectionFieldView{"x-custom", "two"}};
    const auto encoded = ruvia::encodeHttp3ResponseTrailers(fields);
    RUVIA_CHECK(encoded.has_value());
    if (!encoded) {
        return;
    }
    Fields decoded;
    const auto count = ruvia::decodeHttp3FieldSection(encoded->fieldSection, collect, &decoded);
    RUVIA_CHECK(count.has_value());
    RUVIA_CHECK_EQ(decoded.names.size(), fields.size());
    RUVIA_CHECK_EQ(encoded->decodedFieldSectionSize(), 176U);
    if (decoded.names.size() == fields.size()) {
        RUVIA_CHECK_EQ(decoded.names[0], "etag");
        RUVIA_CHECK_EQ(decoded.values[0], "\"v1\"");
        RUVIA_CHECK_EQ(decoded.names[1], "accept-ranges");
        RUVIA_CHECK_EQ(decoded.names[2], "x-custom");
        RUVIA_CHECK_EQ(decoded.values[2], "one");
        RUVIA_CHECK_EQ(decoded.values[3], "two");
    }
}

RUVIA_TEST(http3_response_trailers_reject_forbidden_and_malformed_fields) {
    for (const auto field : {ruvia::Http3FieldSectionFieldView{"Content-Length", "1"},
             ruvia::Http3FieldSectionFieldView{"Date", "today"},
             ruvia::Http3FieldSectionFieldView{"Location", "/"},
             ruvia::Http3FieldSectionFieldView{":status", "200"}}) {
        const std::array fields{field};
        RUVIA_CHECK(!ruvia::encodeHttp3ResponseTrailers(fields));
    }
    const std::array crlf{ruvia::Http3FieldSectionFieldView{"x-test", "bad\r\ninjected"}};
    const auto invalid = ruvia::encodeHttp3ResponseTrailers(crlf);
    RUVIA_CHECK(!invalid);
    if (!invalid) {
        RUVIA_CHECK(invalid.error().kind == ruvia::Http3ResponseHeadError::kInvalidField);
    }
    const std::array contentLength{ruvia::Http3FieldSectionFieldView{"content-length", "1"}};
    const auto forbidden = ruvia::encodeHttp3ResponseTrailers(contentLength);
    RUVIA_CHECK(!forbidden);
    if (!forbidden) {
        RUVIA_CHECK(forbidden.error().kind == ruvia::Http3ResponseHeadError::kForbiddenField);
    }
}

RUVIA_TEST(http3_response_trailers_enforce_field_section_limits_and_empty_section) {
    const auto empty = ruvia::encodeHttp3ResponseTrailers({});
    RUVIA_CHECK(empty.has_value());
    if (empty) {
        RUVIA_CHECK_EQ(empty->decodedFieldSectionSize(), 0U);
        Fields decoded;
        const auto count = ruvia::decodeHttp3FieldSection(empty->fieldSection, collect, &decoded);
        RUVIA_CHECK(count.has_value());
        if (count) {
            RUVIA_CHECK_EQ(*count, 0U);
        }
    }
    const std::array fields{ruvia::Http3FieldSectionFieldView{"x", "y"}};
    const auto tooMany = ruvia::encodeHttp3ResponseTrailers(fields,
        {.maxEncodedBytes = 1024, .maxDecodedBytes = 1024, .maxFields = 0});
    RUVIA_CHECK(!tooMany);
    if (!tooMany) {
        RUVIA_CHECK(tooMany.error().fieldSectionError == ruvia::Http3FieldSectionError::kTooManyFields);
    }
    const auto exactDecoded = ruvia::encodeHttp3ResponseTrailers(fields,
        {.maxEncodedBytes = 1024, .maxDecodedBytes = 34, .maxFields = 1});
    RUVIA_CHECK(exactDecoded.has_value());
    if (exactDecoded) {
        RUVIA_CHECK_EQ(exactDecoded->decodedFieldSectionSize(), 34U);
    }
    const auto tooLargeDecoded = ruvia::encodeHttp3ResponseTrailers(fields,
        {.maxEncodedBytes = 1024, .maxDecodedBytes = 33, .maxFields = 1});
    RUVIA_CHECK(!tooLargeDecoded);
    if (!tooLargeDecoded) {
        RUVIA_CHECK(tooLargeDecoded.error().fieldSectionError ==
                    ruvia::Http3FieldSectionError::kFieldListTooLarge);
    }
    const auto tooLargeEncoded = ruvia::encodeHttp3ResponseTrailers(fields,
        {.maxEncodedBytes = 1, .maxDecodedBytes = 1024, .maxFields = 1});
    RUVIA_CHECK(!tooLargeEncoded);
    if (!tooLargeEncoded) {
        RUVIA_CHECK(tooLargeEncoded.error().fieldSectionError ==
                    ruvia::Http3FieldSectionError::kFieldSectionTooLarge);
    }
}

RUVIA_TEST(http3_response_trailers_payload_releases_pmr_allocations_on_destruction) {
    CountingResource resource;
    CountingResource destinationResource;
    {
        const std::array fields{ruvia::Http3FieldSectionFieldView{"X-Test", "value"}};
        auto encoded = ruvia::encodeHttp3ResponseTrailers(fields, {}, &resource);
        RUVIA_CHECK(encoded.has_value());
        if (!encoded) {
            return;
        }
        RUVIA_CHECK_EQ(encoded->decodedFieldSectionSize(), 43U);
        auto moved = std::move(*encoded);
        RUVIA_CHECK_EQ(moved.decodedFieldSectionSize(), 43U);
        RUVIA_CHECK_EQ(encoded->decodedFieldSectionSize(), 0U);
        RUVIA_CHECK(encoded->fieldSection.empty());
        auto destination = ruvia::encodeHttp3ResponseTrailers({}, {}, &destinationResource);
        RUVIA_CHECK(destination.has_value());
        if (destination) {
            *destination = std::move(moved);
            RUVIA_CHECK_EQ(destination->decodedFieldSectionSize(), 43U);
            RUVIA_CHECK_EQ(moved.decodedFieldSectionSize(), 0U);
            RUVIA_CHECK(moved.fieldSection.empty());
        }
        RUVIA_CHECK(resource.allocations > resource.deallocations);
    }
    RUVIA_CHECK(resource.allocations > 0);
    RUVIA_CHECK_EQ(resource.allocations, resource.deallocations);
    RUVIA_CHECK(destinationResource.allocations > 0);
    RUVIA_CHECK_EQ(destinationResource.allocations, destinationResource.deallocations);
}

RUVIA_TEST(http3_response_head_enforces_section_limits_including_status) {
    const auto tooFewFields = ruvia::encodeHttp3ResponseHead(ruvia::http_status::kOk,
        ruvia::HttpKnownMethod::kGet, {}, {.maxEncodedBytes = 64, .maxDecodedBytes = 1024, .maxFields = 0});
    RUVIA_CHECK(!tooFewFields);
    if (!tooFewFields) {
        RUVIA_CHECK(tooFewFields.error().fieldSectionError ==
                    ruvia::Http3FieldSectionError::kTooManyFields);
    }

    const auto statusOnlyLimit = ruvia::encodeHttp3ResponseHead(ruvia::http_status::kOk,
        ruvia::HttpKnownMethod::kGet, {}, {.maxEncodedBytes = 64, .maxDecodedBytes = 1024, .maxFields = 1});
    RUVIA_CHECK(statusOnlyLimit.has_value());

    const std::array oneField{ruvia::Http3FieldSectionFieldView{"x", "y"}};
    const auto oneOverLimit = ruvia::encodeHttp3ResponseHead(ruvia::http_status::kOk,
        ruvia::HttpKnownMethod::kGet, oneField, {.maxEncodedBytes = 64, .maxDecodedBytes = 1024, .maxFields = 1});
    RUVIA_CHECK(!oneOverLimit);
    if (!oneOverLimit) {
        RUVIA_CHECK(oneOverLimit.error().fieldSectionError ==
                    ruvia::Http3FieldSectionError::kTooManyFields);
    }

    const auto tooSmall = ruvia::encodeHttp3ResponseHead(ruvia::http_status::kOk,
        ruvia::HttpKnownMethod::kGet, {}, {.maxEncodedBytes = 2, .maxDecodedBytes = 1024, .maxFields = 8});
    RUVIA_CHECK(!tooSmall);
    if (!tooSmall) {
        RUVIA_CHECK(tooSmall.error().fieldSectionError ==
                    ruvia::Http3FieldSectionError::kFieldSectionTooLarge);
    }
}
