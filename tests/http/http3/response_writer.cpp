#include <algorithm>
#include <array>
#include <memory_resource>
#include <string>
#include <utility>
#include <vector>

#include "ruvia/http/Http3FieldSection.h"
#include "ruvia/http/Http3ResponseWriter.h"

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
    std::size_t allocations{};
    std::size_t deallocations{};

private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        ++allocations;
        return std::pmr::new_delete_resource()->allocate(bytes, alignment);
    }
    void do_deallocate(void* ptr, std::size_t bytes, std::size_t alignment) override {
        ++deallocations;
        std::pmr::new_delete_resource()->deallocate(ptr, bytes, alignment);
    }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
};
}  // namespace

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
    const auto count = ruvia::decodeHttp3FieldSection(result->fieldSection, collect, &decoded);
    RUVIA_CHECK(count.has_value());
    RUVIA_CHECK_EQ(decoded.names.size(), 2U);
    if (decoded.names.size() == 2) {
        RUVIA_CHECK_EQ(decoded.names[0], ":status");
        RUVIA_CHECK_EQ(decoded.values[0], "200");
        RUVIA_CHECK_EQ(decoded.names[1], "content-type");
        RUVIA_CHECK_EQ(decoded.values[1], "text/plain");
    }
    RUVIA_CHECK_EQ(result->decodedFieldSectionSize(), 96U);
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
    RUVIA_CHECK_EQ(result->fieldSection.size(), canonicalWire.size());
    RUVIA_CHECK(std::equal(result->fieldSection.begin(), result->fieldSection.end(),
        canonicalWire.begin(), canonicalWire.end()));
}

RUVIA_TEST(http3_response_head_body_plan_suppresses_head_and_bodyless_statuses) {
    for (const auto status : {ruvia::http_status::kOk, ruvia::http_status::kNoContent,
             ruvia::http_status::kNotModified}) {
        const auto result = ruvia::encodeHttp3ResponseHead(status, ruvia::HttpKnownMethod::kHead, {});
        RUVIA_CHECK(result.has_value());
        if (result) {
            RUVIA_CHECK_EQ(result->decodedFieldSectionSize(), 42U);
            RUVIA_CHECK(result->bodyPlan.bodySuppressed());
        }
    }
    const auto noContent = ruvia::encodeHttp3ResponseHead(
        ruvia::http_status::kNoContent, ruvia::HttpKnownMethod::kGet, {});
    RUVIA_CHECK(noContent.has_value());
    if (noContent) {
        RUVIA_CHECK_EQ(noContent->decodedFieldSectionSize(), 42U);
        RUVIA_CHECK(noContent->bodyPlan.bodySuppressed());
    }
}

RUVIA_TEST(http3_response_head_decoded_size_counts_status_and_multiple_fields) {
    const auto statusOnly = ruvia::encodeHttp3ResponseHead(ruvia::http_status::kOk,
        ruvia::HttpKnownMethod::kGet, {},
        {.maxEncodedBytes = 1024, .maxDecodedBytes = 42, .maxFields = 1});
    RUVIA_CHECK(statusOnly.has_value());
    if (statusOnly) {
        RUVIA_CHECK_EQ(statusOnly->decodedFieldSectionSize(), 42U);
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
        RUVIA_CHECK_EQ(exact->decodedFieldSectionSize(), 122U);
    }
    const auto overLimit = ruvia::encodeHttp3ResponseHead(ruvia::http_status::kOk,
        ruvia::HttpKnownMethod::kGet, fields,
        {.maxEncodedBytes = 1024, .maxDecodedBytes = 121, .maxFields = 3});
    RUVIA_CHECK(!overLimit);
    if (!overLimit) {
        RUVIA_CHECK(overLimit.error().fieldSectionError ==
                    ruvia::Http3FieldSectionError::kFieldListTooLarge);
    }

    auto source = ruvia::encodeHttp3ResponseHead(
        ruvia::http_status::kOk, ruvia::HttpKnownMethod::kGet, {});
    RUVIA_CHECK(source.has_value());
    if (source) {
        auto moved = std::move(*source);
        RUVIA_CHECK_EQ(moved.decodedFieldSectionSize(), 42U);
        RUVIA_CHECK_EQ(source->decodedFieldSectionSize(), 0U);
        RUVIA_CHECK(source->fieldSection.empty());
        auto destination = ruvia::encodeHttp3ResponseHead(
            ruvia::http_status::kOk, ruvia::HttpKnownMethod::kGet, {});
        RUVIA_CHECK(destination.has_value());
        if (destination) {
            *destination = std::move(moved);
            RUVIA_CHECK_EQ(destination->decodedFieldSectionSize(), 42U);
            RUVIA_CHECK_EQ(moved.decodedFieldSectionSize(), 0U);
            RUVIA_CHECK(moved.fieldSection.empty());
        }
    }
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
    const auto count = ruvia::decodeHttp3FieldSection(encoded->fieldSection, collect, &decoded);
    RUVIA_CHECK(count.has_value());
    RUVIA_CHECK(std::find(decoded.names.begin(), decoded.names.end(), "x-mixed") != decoded.names.end());
    RUVIA_CHECK(std::count(decoded.names.begin(), decoded.names.end(), "set-cookie") == 2);
    RUVIA_CHECK(std::find(decoded.values.begin(), decoded.values.end(), "7") != decoded.values.end());
    RUVIA_CHECK_EQ(encoded->decodedFieldSectionSize(), 220U);

    response.body("change");
    RUVIA_CHECK(!ruvia::encodeHttp3ResponseHead(response, plan));
}

RUVIA_TEST(http3_response_writer_reports_final_automatic_and_explicit_content_length_size) {
    ruvia::HttpResponse automaticResponse;
    automaticResponse.body("payload");
    const auto automaticPlan = ruvia::planBufferedHttpResponseWrite(ruvia::HttpKnownMethod::kGet,
        automaticResponse);
    const auto automatic = ruvia::encodeHttp3ResponseHead(automaticResponse, automaticPlan,
        {.maxEncodedBytes = 1024, .maxDecodedBytes = 89, .maxFields = 2});
    RUVIA_CHECK(automatic.has_value());
    if (automatic) {
        RUVIA_CHECK_EQ(automatic->decodedFieldSectionSize(), 89U);
    }
    const auto automaticOverLimit = ruvia::encodeHttp3ResponseHead(automaticResponse, automaticPlan,
        {.maxEncodedBytes = 1024, .maxDecodedBytes = 88, .maxFields = 2});
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
        {.maxEncodedBytes = 1024, .maxDecodedBytes = 89, .maxFields = 2});
    RUVIA_CHECK(explicitLength.has_value());
    if (explicitLength) {
        RUVIA_CHECK_EQ(explicitLength->decodedFieldSectionSize(), 89U);
        Fields decoded;
        RUVIA_CHECK(ruvia::decodeHttp3FieldSection(explicitLength->fieldSection, collect, &decoded).has_value());
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
    headResponse.fileBody("unused.bin", 123, 0, 123, {}, false);
    const auto headPlan = ruvia::planBufferedHttpResponseWrite(ruvia::HttpKnownMethod::kHead, headResponse);
    const auto head = ruvia::encodeHttp3ResponseHead(headResponse, headPlan);
    RUVIA_CHECK(head.has_value());
    if (head) {
        Fields decoded;
        RUVIA_CHECK(ruvia::decodeHttp3FieldSection(head->fieldSection, collect, &decoded).has_value());
        RUVIA_CHECK(std::find(decoded.values.begin(), decoded.values.end(), "123") != decoded.values.end());
        RUVIA_CHECK_EQ(head->decodedFieldSectionSize(), 91U);
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
            RUVIA_CHECK(ruvia::decodeHttp3FieldSection(result->fieldSection, collect, &decoded).has_value());
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
        {.maxEncodedBytes = 4, .maxDecodedBytes = 1024, .maxFields = 2});
    RUVIA_CHECK(encoded.has_value());
    if (encoded) {
        RUVIA_CHECK(encoded->fieldSection.size() <= 4);
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
