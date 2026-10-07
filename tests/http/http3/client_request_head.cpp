#include <array>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <span>
#include <string_view>
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

std::expected<Http3ClientRequestHead, Http3ClientRequestHeadFailure> encode(
    std::string_view method, std::string_view path, std::span<const Http3FieldSectionFieldView> headers = {},
    std::optional<std::uint64_t> length = {}) {
    return encodeHttp3ClientRequestHead({.method = method, .scheme = method == "CONNECT" ? "" : "https", .authority = "example.test:443", .path = path, .fields = headers, .bodyLength = length});
}

}  // namespace

RUVIA_TEST(http3_client_request_head_encodes_get_post_connect_and_body_is_external) {
    auto get = encode("GET", "/a?q=1");
    RUVIA_CHECK(get.has_value());
    if (get) {
        const auto decoded = decodeHttp3MessageHead(get->fieldSection, Http3MessageHeadKind::kRequest);
        RUVIA_CHECK(decoded.has_value());
        if (decoded) {
            RUVIA_CHECK_EQ(decoded->method, "GET");
            RUVIA_CHECK_EQ(decoded->path, "/a?q=1");
        }
        RUVIA_CHECK(!get->bodyPlan.expectedLength);
    }
    auto post = encode("POST", "/submit", {}, 3);
    RUVIA_CHECK(post.has_value());
    if (post) {
        const auto decoded = decodeHttp3MessageHead(post->fieldSection, Http3MessageHeadKind::kRequest);
        RUVIA_CHECK(decoded.has_value());
        if (decoded) {
            RUVIA_CHECK(decoded->contentLength == 3);
        }
        RUVIA_CHECK(post->bodyPlan.matches(3));
        RUVIA_CHECK(!post->bodyPlan.matches(2));
    }
    auto connect = encode("CONNECT", "");
    RUVIA_CHECK(connect.has_value());
    if (connect) {
        const auto decoded = decodeHttp3MessageHead(connect->fieldSection, Http3MessageHeadKind::kRequest);
        RUVIA_CHECK(decoded.has_value());
        if (decoded) {
            RUVIA_CHECK_EQ(decoded->authority, "example.test:443");
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
    RUVIA_CHECK(bodyless.has_value());
    if (bodyless) {
        const auto decoded = decodeHttp3MessageHead(bodyless->fieldSection,
            Http3MessageHeadKind::kRequest);
        RUVIA_CHECK(decoded && !decoded->contentLength);
        RUVIA_CHECK(bodyless->bodyPlan.matches(0));
    }

    const auto mismatch = encodeHttp3ClientRequestHead({.method = "GET",
        .scheme = "https",
        .authority = "example.test:443",
        .path = "/",
        .fields = std::array{Http3FieldSectionFieldView{"content-length", "1"}},
        .bodyLength = 0,
        .emit_content_length = false});
    RUVIA_CHECK(!mismatch && mismatch.error().kind ==
                                 Http3ClientRequestHeadError::kInvalidContentLength);
}

RUVIA_TEST(http3_client_request_head_trace_forbids_content_and_known_sensitive_fields) {
    for (const auto length : {std::optional<std::uint64_t>{}, std::optional<std::uint64_t>{0}}) {
        const auto trace = encode("TRACE", "/", {}, length);
        RUVIA_CHECK(trace.has_value());
        if (!trace) {
            continue;
        }
        RUVIA_CHECK(trace->bodyPlan.matches(0));
        RUVIA_CHECK(!trace->bodyPlan.matches(1));
        const auto decoded = decodeHttp3MessageHead(trace->fieldSection, Http3MessageHeadKind::kRequest);
        RUVIA_CHECK(decoded && decoded->contentLength == length);
        Http3DataWritePlan bodyPlan(trace->bodyPlan);
        RUVIA_CHECK(!bodyPlan.planChunk(std::span<const char>("x", 1), true));
    }
    for (const auto& invalid : {encode("TRACE", "/", {}, 1),
             encode("TRACE", "/", std::array{Http3FieldSectionFieldView{"content-length", "1"}})}) {
        RUVIA_CHECK(!invalid && invalid.error().kind == Http3ClientRequestHeadError::kInvalidContentLength);
    }
    for (const std::string_view name : {"Authorization", "PROXY-Authorization", "cOoKiE"}) {
        const std::array fields{Http3FieldSectionFieldView{name, "private"}};
        const auto trace = encode("TRACE", "/", fields);
        RUVIA_CHECK(!trace && trace.error().kind == Http3ClientRequestHeadError::kForbiddenField);
        RUVIA_CHECK(encode("POST", "/", fields).has_value());
    }
    RUVIA_CHECK(encode("TRACE", "/", std::array{Http3FieldSectionFieldView{"Max-Forwards", "0"}}).has_value());
}

RUVIA_TEST(http3_client_request_head_normalizes_headers_and_rejects_invalid_inputs) {
    const std::array headers{Http3FieldSectionFieldView{"X-Test", "yes"},
        Http3FieldSectionFieldView{"x-ready", "ok"}};
    auto normalized = encode("GET", "/", headers);
    RUVIA_CHECK(normalized.has_value());
    if (normalized) {
        const auto decoded = decodeHttp3MessageHead(normalized->fieldSection, Http3MessageHeadKind::kRequest);
        RUVIA_CHECK(decoded.has_value());
        if (decoded) {
            RUVIA_CHECK_EQ(decoded->headers[0].name, "x-test");
            RUVIA_CHECK_EQ(decoded->headers[1].name, "x-ready");
        }
    }
    RUVIA_CHECK(!encode("CONNECT", "/"));
    RUVIA_CHECK(!encode("CONNECT", "", {}, 1));
    RUVIA_CHECK(!encode("CONNECT", "",
        std::array{Http3FieldSectionFieldView{"content-length", "1"}}));
    RUVIA_CHECK(!encode("GET", "relative"));
    RUVIA_CHECK(!encode("GET", "/", std::array{Http3FieldSectionFieldView{"host", "wrong.test"}}));
    RUVIA_CHECK(!encode("GET", "/", std::array{Http3FieldSectionFieldView{"te", "gzip"}}));
    const std::array duplicate{Http3FieldSectionFieldView{"content-length", "1"},
        Http3FieldSectionFieldView{"Content-Length", "1"}};
    RUVIA_CHECK(!encode("POST", "/", duplicate, 1));
    RUVIA_CHECK(!encode("POST", "/", std::array{Http3FieldSectionFieldView{"content-length", "3"}}, 4));
    RUVIA_CHECK(!encode("GET", "/", std::array{Http3FieldSectionFieldView{":path", "/"}}));
    RUVIA_CHECK(!encode("GET", "/", std::array{Http3FieldSectionFieldView{"x@bad", "value"}}));
    for (const std::string_view value : {"", " 1", "1 ", "1, 1", "+1", "-1", "18446744073709551616"}) {
        const auto invalid = encode("POST", "/", std::array{Http3FieldSectionFieldView{"content-length", value}});
        RUVIA_CHECK(!invalid && invalid.error().kind == Http3ClientRequestHeadError::kInvalidContentLength);
    }
    const auto wide = encode("POST", "/",
        std::array{Http3FieldSectionFieldView{"content-length", "18446744073709551615"}});
    RUVIA_CHECK(wide.has_value());
    if (wide) {
        RUVIA_CHECK(wide->bodyPlan.expectedLength == UINT64_MAX);
    }
    RUVIA_CHECK(encode("GET", "/", std::array{Http3FieldSectionFieldView{"x-ows", " value\t"}}).has_value());
}

RUVIA_TEST(http3_client_request_head_rejects_invalid_cors_preflight_fields) {
    for (const auto field : {Http3FieldSectionFieldView{"Origin", "https://app.example/path"},
             Http3FieldSectionFieldView{"Access-Control-Request-Method", "POST GET"},
             Http3FieldSectionFieldView{"Access-Control-Request-Headers", "x bad"},
             Http3FieldSectionFieldView{"access-control-request-headers", ""}}) {
        const std::array fields{field};
        const auto request = encode("OPTIONS", "/", fields);
        RUVIA_CHECK(!request && request.error().kind == Http3ClientRequestHeadError::kInvalidField);
    }
    const std::array valid{Http3FieldSectionFieldView{"Origin", "https://app.example"},
        Http3FieldSectionFieldView{"Access-Control-Request-Method", "POST"},
        Http3FieldSectionFieldView{"Access-Control-Request-Headers", "x-trace, content-type"}};
    const auto encoded = encode("OPTIONS", "/", valid);
    RUVIA_CHECK(encoded.has_value());
    if (encoded) {
        RUVIA_CHECK(decodeHttp3MessageHead(encoded->fieldSection, Http3MessageHeadKind::kRequest));
    }
}

RUVIA_TEST(http3_client_request_head_rejects_malformed_representation_fields) {
    const std::array valid{Http3FieldSectionFieldView{"Content-Type", "application/json"},
        Http3FieldSectionFieldView{"Content-Encoding", "gzip, br"}};
    RUVIA_CHECK(encode("POST", "/", valid).has_value());
    for (const auto field : {Http3FieldSectionFieldView{"Content-Type", "text plain"},
             Http3FieldSectionFieldView{"Content-Encoding", "gzip;q=1"}}) {
        const std::array fields{field};
        const auto request = encode("POST", "/", fields);
        RUVIA_CHECK(!request && request.error().kind == Http3ClientRequestHeadError::kInvalidField);
    }
    const std::array repeated{Http3FieldSectionFieldView{"Content-Type", "text/plain"},
        Http3FieldSectionFieldView{"content-type", "application/json"}};
    const auto duplicate = encode("POST", "/", repeated);
    RUVIA_CHECK(!duplicate && duplicate.error().kind == Http3ClientRequestHeadError::kInvalidField);
}

RUVIA_TEST(http3_client_request_head_rejects_forbidden_declared_trailer_names) {
    const std::array forbidden{Http3FieldSectionFieldView{"Trailer", "Content-Length"}};
    const auto request = encode("POST", "/", forbidden);
    RUVIA_CHECK(!request && request.error().kind == Http3ClientRequestHeadError::kInvalidField);
    const std::array valid{Http3FieldSectionFieldView{"Trailer", "x-checksum"}};
    RUVIA_CHECK(encode("POST", "/", valid).has_value());
}

RUVIA_TEST(http3_client_request_head_rejects_malformed_expectation) {
    const std::array malformed{Http3FieldSectionFieldView{"Expect", "foo?bar"}};
    const auto request = encode("POST", "/", malformed);
    RUVIA_CHECK(!request && request.error().kind == Http3ClientRequestHeadError::kInvalidField);
    const std::array valid{Http3FieldSectionFieldView{"Expect", "foo=bar"}};
    RUVIA_CHECK(encode("POST", "/", valid).has_value());
}

RUVIA_TEST(http3_client_request_head_enforces_limits_and_releases_resource_allocations) {
    CountingResource resource;
    auto failed = encodeHttp3ClientRequestHead({.method = "GET", .scheme = "https", .authority = "example.test:443", .path = "/"},
        {.maxEncodedBytes = 1}, &resource);
    RUVIA_CHECK(!failed);
    auto limited = encodeHttp3ClientRequestHead({.method = "GET", .scheme = "https", .authority = "example.test:443", .path = "/"},
        {.maxFields = 3});
    RUVIA_CHECK(!limited);
    auto pseudoLimit = encodeHttp3ClientRequestHead({.method = "GET", .scheme = "https", .authority = "example.test:443", .path = "/"}, {.maxDecodedBytes = 180});
    RUVIA_CHECK(!pseudoLimit);
    if (!pseudoLimit) {
        RUVIA_CHECK(pseudoLimit.error().fieldSectionError ==
                    Http3FieldSectionError::kFieldListTooLarge);
    }
    const auto exact = encodeHttp3ClientRequestHead(
        {.method = "GET", .scheme = "https", .authority = "example.test:443", .path = "/"},
        {.maxDecodedBytes = 182, .maxFields = 4});
    RUVIA_CHECK(exact.has_value());
    const auto bothLimited = encodeHttp3ClientRequestHead(
        {.method = "GET", .scheme = "https", .authority = "example.test:443", .path = "/", .fields = std::array{Http3FieldSectionFieldView{"x", "y"}}},
        {.maxDecodedBytes = 1, .maxFields = 4});
    RUVIA_CHECK(!bothLimited && bothLimited.error().fieldSectionError == Http3FieldSectionError::kTooManyFields);
    {
        auto result = encodeHttp3ClientRequestHead({.method = "GET", .scheme = "https", .authority = "example.test:443", .path = "/"}, {}, &resource);
        RUVIA_CHECK(result.has_value());
    }
    RUVIA_CHECK_EQ(resource.allocations, resource.deallocations);
}
