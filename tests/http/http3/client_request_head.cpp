#include <array>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <span>
#include <string_view>
#include <variant>
#include <vector>

#include "ruvia/http/Http3ClientRequestHead.h"
#include "ruvia/http/Http3DataWritePlan.h"
#include "ruvia/http/Http3MessageHead.h"

#include "test_harness.h"

namespace {
using namespace ruvia;

class CountingResource final : public std::pmr::memory_resource {
public:
    std::size_t allocations{0};
    std::size_t deallocations{0};

private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        ++allocations;
        return std::pmr::new_delete_resource()->allocate(bytes, alignment);
    }
    void do_deallocate(void* pointer, std::size_t bytes, std::size_t alignment) override {
        ++deallocations;
        std::pmr::new_delete_resource()->deallocate(pointer, bytes, alignment);
    }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
};

std::variant<Http3ClientRequestHead, Http3ClientRequestHeadFailure> encode(
    std::string_view method, std::string_view path, std::span<const Http3FieldSectionFieldView> headers = {},
    std::optional<std::uint64_t> length = {}) {
    return encodeHttp3ClientRequestHead({.method = method, .scheme = method == "CONNECT" ? "" : "https", .authority = "example.test:443", .path = path, .fields = headers, .bodyLength = length});
}

}  // namespace

RUVIA_TEST(http3_client_request_head_encodes_get_post_connect_and_body_is_external) {
    auto get = encode("GET", "/a?q=1");
    RUVIA_CHECK((get.index() == 0));
    if ((get.index() == 0)) {
        const auto decoded = decodeHttp3MessageHead(std::get<0>(get).fieldSection, Http3MessageHeadKind::kRequest);
        RUVIA_CHECK((decoded.index() == 0));
        if ((decoded.index() == 0)) {
            RUVIA_CHECK_EQ(std::get<0>(decoded).method, "GET");
            RUVIA_CHECK_EQ(std::get<0>(decoded).path, "/a?q=1");
        }
        RUVIA_CHECK(!std::get<0>(get).bodyPlan.expectedLength);
    }
    auto post = encode("POST", "/submit", {}, 3);
    RUVIA_CHECK((post.index() == 0));
    if ((post.index() == 0)) {
        const auto decoded = decodeHttp3MessageHead(std::get<0>(post).fieldSection, Http3MessageHeadKind::kRequest);
        RUVIA_CHECK((decoded.index() == 0));
        if ((decoded.index() == 0)) {
            RUVIA_CHECK(std::get<0>(decoded).contentLength == 3);
        }
        RUVIA_CHECK(std::get<0>(post).bodyPlan.matches(3));
        RUVIA_CHECK(!std::get<0>(post).bodyPlan.matches(2));
    }
    auto connect = encode("CONNECT", "");
    RUVIA_CHECK((connect.index() == 0));
    if ((connect.index() == 0)) {
        const auto decoded = decodeHttp3MessageHead(std::get<0>(connect).fieldSection, Http3MessageHeadKind::kRequest);
        RUVIA_CHECK((decoded.index() == 0));
        if ((decoded.index() == 0)) {
            RUVIA_CHECK_EQ(std::get<0>(decoded).authority, "example.test:443");
        }
    }
}

RUVIA_TEST(http3_client_request_head_validates_bodyless_length_without_emitting_a_length) {
    const auto bodyless = encodeHttp3ClientRequestHead({.method = "GET",
        .scheme = "https",
        .authority = "example.test:443",
        .path = "/",
        .bodyLength = 0,
        .emit_content_length = false});
    RUVIA_CHECK((bodyless.index() == 0));
    if ((bodyless.index() == 0)) {
        const auto decoded = decodeHttp3MessageHead(std::get<0>(bodyless).fieldSection,
            Http3MessageHeadKind::kRequest);
        RUVIA_CHECK((decoded.index() == 0) && !std::get<0>(decoded).contentLength);
        RUVIA_CHECK(std::get<0>(bodyless).bodyPlan.matches(0));
    }

    const auto mismatch = encodeHttp3ClientRequestHead({.method = "GET",
        .scheme = "https",
        .authority = "example.test:443",
        .path = "/",
        .fields = std::array{Http3FieldSectionFieldView{"content-length", "1"}},
        .bodyLength = 0,
        .emit_content_length = false});
    RUVIA_CHECK((mismatch.index() != 0) && std::get<1>(mismatch).kind ==
                                               Http3ClientRequestHeadError::kInvalidContentLength);
}

RUVIA_TEST(http3_client_request_head_trace_forbids_content_and_known_sensitive_fields) {
    for (const auto length : {std::optional<std::uint64_t>{}, std::optional<std::uint64_t>{0}}) {
        const auto trace = encode("TRACE", "/", {}, length);
        RUVIA_CHECK((trace.index() == 0));
        if ((trace.index() != 0)) {
            continue;
        }
        RUVIA_CHECK(std::get<0>(trace).bodyPlan.matches(0));
        RUVIA_CHECK(!std::get<0>(trace).bodyPlan.matches(1));
        const auto decoded = decodeHttp3MessageHead(std::get<0>(trace).fieldSection, Http3MessageHeadKind::kRequest);
        RUVIA_CHECK((decoded.index() == 0) && std::get<0>(decoded).contentLength == length);
        Http3DataWritePlan bodyPlan(std::get<0>(trace).bodyPlan);
        RUVIA_CHECK((bodyPlan.planChunk(std::span<const char>("x", 1), true).index() != 0));
    }
    for (const auto& invalid : {encode("TRACE", "/", {}, 1),
             encode("TRACE", "/", std::array{Http3FieldSectionFieldView{"content-length", "1"}})}) {
        RUVIA_CHECK(invalid.index() != 0 && std::get<1>(invalid).kind == Http3ClientRequestHeadError::kInvalidContentLength);
    }
    for (const std::string_view name : {"Authorization", "PROXY-Authorization", "cOoKiE"}) {
        const std::array fields{Http3FieldSectionFieldView{name, "private"}};
        const auto trace = encode("TRACE", "/", fields);
        RUVIA_CHECK((trace.index() != 0) && std::get<1>(trace).kind == Http3ClientRequestHeadError::kForbiddenField);
        RUVIA_CHECK((encode("POST", "/", fields).index() == 0));
    }
    RUVIA_CHECK((encode("TRACE", "/", std::array{Http3FieldSectionFieldView{"Max-Forwards", "0"}}).index() == 0));
}

RUVIA_TEST(http3_client_request_head_normalizes_headers_and_rejects_invalid_inputs) {
    const std::array headers{Http3FieldSectionFieldView{"X-Test", "yes"},
        Http3FieldSectionFieldView{"x-ready", "ok"}};
    auto normalized = encode("GET", "/", headers);
    RUVIA_CHECK((normalized.index() == 0));
    if ((normalized.index() == 0)) {
        const auto decoded = decodeHttp3MessageHead(std::get<0>(normalized).fieldSection, Http3MessageHeadKind::kRequest);
        RUVIA_CHECK((decoded.index() == 0));
        if ((decoded.index() == 0)) {
            RUVIA_CHECK_EQ(std::get<0>(decoded).headers[0].name, "x-test");
            RUVIA_CHECK_EQ(std::get<0>(decoded).headers[1].name, "x-ready");
        }
    }
    RUVIA_CHECK((encode("CONNECT", "/").index() != 0));
    RUVIA_CHECK((encode("CONNECT", "", {}, 1).index() != 0));
    RUVIA_CHECK((encode("CONNECT", "",
                     std::array{Http3FieldSectionFieldView{"content-length", "1"}})
                     .index() != 0));
    RUVIA_CHECK((encode("GET", "relative").index() != 0));
    RUVIA_CHECK((encode("GET", "/", std::array{Http3FieldSectionFieldView{"host", "wrong.test"}}).index() != 0));
    RUVIA_CHECK((encode("GET", "/", std::array{Http3FieldSectionFieldView{"te", "gzip"}}).index() != 0));
    const std::array duplicate{Http3FieldSectionFieldView{"content-length", "1"},
        Http3FieldSectionFieldView{"Content-Length", "1"}};
    RUVIA_CHECK((encode("POST", "/", duplicate, 1).index() != 0));
    RUVIA_CHECK((encode("POST", "/", std::array{Http3FieldSectionFieldView{"content-length", "3"}}, 4).index() != 0));
    RUVIA_CHECK((encode("GET", "/", std::array{Http3FieldSectionFieldView{":path", "/"}}).index() != 0));
    RUVIA_CHECK((encode("GET", "/", std::array{Http3FieldSectionFieldView{"x@bad", "value"}}).index() != 0));
    for (const std::string_view value : {"", " 1", "1 ", "1, 1", "+1", "-1", "18446744073709551616"}) {
        const auto invalid = encode("POST", "/", std::array{Http3FieldSectionFieldView{"content-length", value}});
        RUVIA_CHECK((invalid.index() != 0) && std::get<1>(invalid).kind == Http3ClientRequestHeadError::kInvalidContentLength);
    }
    const auto wide = encode("POST", "/",
        std::array{Http3FieldSectionFieldView{"content-length", "18446744073709551615"}});
    RUVIA_CHECK((wide.index() == 0));
    if ((wide.index() == 0)) {
        RUVIA_CHECK(std::get<0>(wide).bodyPlan.expectedLength == UINT64_MAX);
    }
    RUVIA_CHECK((encode("GET", "/", std::array{Http3FieldSectionFieldView{"x-ows", " value\t"}}).index() == 0));
}

RUVIA_TEST(http3_client_request_head_rejects_invalid_cors_preflight_fields) {
    for (const auto field : {Http3FieldSectionFieldView{"Origin", "https://app.example/path"},
             Http3FieldSectionFieldView{"Access-Control-Request-Method", "POST GET"},
             Http3FieldSectionFieldView{"Access-Control-Request-Headers", "x bad"},
             Http3FieldSectionFieldView{"access-control-request-headers", ""}}) {
        const std::array fields{field};
        const auto request = encode("OPTIONS", "/", fields);
        RUVIA_CHECK((request.index() != 0) && std::get<1>(request).kind == Http3ClientRequestHeadError::kInvalidField);
    }
    const std::array valid{Http3FieldSectionFieldView{"Origin", "https://app.example"},
        Http3FieldSectionFieldView{"Access-Control-Request-Method", "POST"},
        Http3FieldSectionFieldView{"Access-Control-Request-Headers", "x-trace, content-type"}};
    const auto encoded = encode("OPTIONS", "/", valid);
    RUVIA_CHECK((encoded.index() == 0));
    if ((encoded.index() == 0)) {
        RUVIA_CHECK((decodeHttp3MessageHead(std::get<0>(encoded).fieldSection, Http3MessageHeadKind::kRequest)).index() == 0);
    }
}

RUVIA_TEST(http3_client_request_head_rejects_malformed_representation_fields) {
    const std::array valid{Http3FieldSectionFieldView{"Content-Type", "application/json"},
        Http3FieldSectionFieldView{"Content-Encoding", "gzip, br"}};
    RUVIA_CHECK((encode("POST", "/", valid).index() == 0));
    for (const auto field : {Http3FieldSectionFieldView{"Content-Type", "text plain"},
             Http3FieldSectionFieldView{"Content-Encoding", "gzip;q=1"}}) {
        const std::array fields{field};
        const auto request = encode("POST", "/", fields);
        RUVIA_CHECK((request.index() != 0) && std::get<1>(request).kind == Http3ClientRequestHeadError::kInvalidField);
    }
    const std::array repeated{Http3FieldSectionFieldView{"Content-Type", "text/plain"},
        Http3FieldSectionFieldView{"content-type", "application/json"}};
    const auto duplicate = encode("POST", "/", repeated);
    RUVIA_CHECK((duplicate.index() != 0) && std::get<1>(duplicate).kind == Http3ClientRequestHeadError::kInvalidField);
}

RUVIA_TEST(http3_client_request_head_rejects_forbidden_declared_trailer_names) {
    const std::array forbidden{Http3FieldSectionFieldView{"Trailer", "Content-Length"}};
    const auto request = encode("POST", "/", forbidden);
    RUVIA_CHECK((request.index() != 0) && std::get<1>(request).kind == Http3ClientRequestHeadError::kInvalidField);
    const std::array valid{Http3FieldSectionFieldView{"Trailer", "x-checksum"}};
    RUVIA_CHECK((encode("POST", "/", valid).index() == 0));
}

RUVIA_TEST(http3_client_request_head_rejects_malformed_expectation) {
    const std::array malformed{Http3FieldSectionFieldView{"Expect", "foo?bar"}};
    const auto request = encode("POST", "/", malformed);
    RUVIA_CHECK((request.index() != 0) && std::get<1>(request).kind == Http3ClientRequestHeadError::kInvalidField);
    const std::array valid{Http3FieldSectionFieldView{"Expect", "foo=bar"}};
    RUVIA_CHECK((encode("POST", "/", valid).index() == 0));
}

RUVIA_TEST(http3_client_request_head_enforces_limits_and_releases_resource_allocations) {
    CountingResource resource;
    auto failed = encodeHttp3ClientRequestHead({.method = "GET", .scheme = "https", .authority = "example.test:443", .path = "/"},
        {.maxEncodedBytes = 1}, &resource);
    RUVIA_CHECK((failed.index() != 0));
    auto limited = encodeHttp3ClientRequestHead({.method = "GET", .scheme = "https", .authority = "example.test:443", .path = "/"},
        {.maxFields = 3});
    RUVIA_CHECK((limited.index() != 0));
    auto pseudoLimit = encodeHttp3ClientRequestHead({.method = "GET", .scheme = "https", .authority = "example.test:443", .path = "/"}, {.maxDecodedBytes = 180});
    RUVIA_CHECK((pseudoLimit.index() != 0));
    if ((pseudoLimit.index() != 0)) {
        RUVIA_CHECK(std::get<1>(pseudoLimit).fieldSectionError ==
                    Http3FieldSectionError::kFieldListTooLarge);
    }
    const auto exact = encodeHttp3ClientRequestHead(
        {.method = "GET", .scheme = "https", .authority = "example.test:443", .path = "/"},
        {.maxDecodedBytes = 182, .maxFields = 4});
    RUVIA_CHECK((exact.index() == 0));
    const auto bothLimited = encodeHttp3ClientRequestHead(
        {.method = "GET", .scheme = "https", .authority = "example.test:443", .path = "/", .fields = std::array{Http3FieldSectionFieldView{"x", "y"}}},
        {.maxDecodedBytes = 1, .maxFields = 4});
    RUVIA_CHECK((bothLimited.index() != 0) && std::get<1>(bothLimited).fieldSectionError == Http3FieldSectionError::kTooManyFields);
    {
        auto result = encodeHttp3ClientRequestHead({.method = "GET", .scheme = "https", .authority = "example.test:443", .path = "/"}, {}, &resource);
        RUVIA_CHECK((result.index() == 0));
    }
    RUVIA_CHECK_EQ(resource.allocations, resource.deallocations);
}
