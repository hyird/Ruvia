#include <array>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <span>
#include <string_view>
#include <variant>
#include <vector>

#include "ruvia/http/http3_client_request_head.h"
#include "ruvia/http/http3_data_write_plan.h"
#include "ruvia/http/http3_message_head.h"

#include "test_harness.h"

namespace {
using namespace ruvia;

class counting_resource final : public std::pmr::memory_resource {
public:
    std::size_t allocations_{0};
    std::size_t deallocations_{0};

private:
    void* do_allocate(std::size_t bytes_value, std::size_t alignment) override {
        ++allocations_;
        return std::pmr::new_delete_resource()->allocate(bytes_value, alignment);
    }
    void do_deallocate(void* pointer, std::size_t bytes_value, std::size_t alignment) override {
        ++deallocations_;
        std::pmr::new_delete_resource()->deallocate(pointer, bytes_value, alignment);
    }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
};

std::variant<http3_client_request_head, http3_client_request_head_failure> encode(
    std::string_view method, std::string_view path, std::span<const http3_field_section_field_view> headers = {},
    std::optional<std::uint64_t> length = {}) {
    return encode_http3_client_request_head({.method_ = method, .scheme_ = method == "CONNECT" ? "" : "https", .authority_ = "example.test:443", .path_ = path, .fields_ = headers, .body_length_ = length});
}

}  // namespace

RUVIA_TEST(http3_client_request_head_encodes_get_post_connect_and_body_is_external) {
    auto get = encode("GET", "/a?q=1");
    RUVIA_CHECK((get.index() == 0));
    if ((get.index() == 0)) {
        const auto decoded = decode_http3_message_head(std::get<0>(get).field_section_, http3_message_head_kind::request);
        RUVIA_CHECK((decoded.index() == 0));
        if ((decoded.index() == 0)) {
            RUVIA_CHECK_EQ(std::get<0>(decoded).method_, "GET");
            RUVIA_CHECK_EQ(std::get<0>(decoded).path_, "/a?q=1");
        }
        RUVIA_CHECK(!std::get<0>(get).body_plan_.expected_length_);
    }
    auto post = encode("POST", "/submit", {}, 3);
    RUVIA_CHECK((post.index() == 0));
    if ((post.index() == 0)) {
        const auto decoded = decode_http3_message_head(std::get<0>(post).field_section_, http3_message_head_kind::request);
        RUVIA_CHECK((decoded.index() == 0));
        if ((decoded.index() == 0)) {
            RUVIA_CHECK(std::get<0>(decoded).content_length_ == 3);
        }
        RUVIA_CHECK(std::get<0>(post).body_plan_.matches(3));
        RUVIA_CHECK(!std::get<0>(post).body_plan_.matches(2));
    }
    auto connect = encode("CONNECT", "");
    RUVIA_CHECK((connect.index() == 0));
    if ((connect.index() == 0)) {
        const auto decoded = decode_http3_message_head(std::get<0>(connect).field_section_, http3_message_head_kind::request);
        RUVIA_CHECK((decoded.index() == 0));
        if ((decoded.index() == 0)) {
            RUVIA_CHECK_EQ(std::get<0>(decoded).authority_, "example.test:443");
        }
    }
}

RUVIA_TEST(http3_client_request_head_validates_bodyless_length_without_emitting_a_length) {
    const auto bodyless = encode_http3_client_request_head({.method_ = "GET",
        .scheme_ = "https",
        .authority_ = "example.test:443",
        .path_ = "/",
        .body_length_ = 0,
        .emit_content_length_ = false});
    RUVIA_CHECK((bodyless.index() == 0));
    if ((bodyless.index() == 0)) {
        const auto decoded = decode_http3_message_head(std::get<0>(bodyless).field_section_,
            http3_message_head_kind::request);
        RUVIA_CHECK((decoded.index() == 0) && !std::get<0>(decoded).content_length_);
        RUVIA_CHECK(std::get<0>(bodyless).body_plan_.matches(0));
    }

    const auto mismatch = encode_http3_client_request_head({.method_ = "GET",
        .scheme_ = "https",
        .authority_ = "example.test:443",
        .path_ = "/",
        .fields_ = std::array{http3_field_section_field_view{"content-length", "1"}},
        .body_length_ = 0,
        .emit_content_length_ = false});
    RUVIA_CHECK((mismatch.index() != 0) && std::get<1>(mismatch).kind_ ==
                                               http3_client_request_head_error::invalid_content_length);
}

RUVIA_TEST(http3_client_request_head_trace_forbids_content_and_known_sensitive_fields) {
    for (const auto length : {std::optional<std::uint64_t>{}, std::optional<std::uint64_t>{0}}) {
        const auto trace = encode("TRACE", "/", {}, length);
        RUVIA_CHECK((trace.index() == 0));
        if ((trace.index() != 0)) {
            continue;
        }
        RUVIA_CHECK(std::get<0>(trace).body_plan_.matches(0));
        RUVIA_CHECK(!std::get<0>(trace).body_plan_.matches(1));
        const auto decoded = decode_http3_message_head(std::get<0>(trace).field_section_, http3_message_head_kind::request);
        RUVIA_CHECK((decoded.index() == 0) && std::get<0>(decoded).content_length_ == length);
        http3_data_write_plan body_plan(std::get<0>(trace).body_plan_);
        RUVIA_CHECK((body_plan.plan_chunk(std::span<const char>("x", 1), true).index() != 0));
    }
    for (const auto& invalid : {encode("TRACE", "/", {}, 1),
             encode("TRACE", "/", std::array{http3_field_section_field_view{"content-length", "1"}})}) {
        RUVIA_CHECK(invalid.index() != 0 && std::get<1>(invalid).kind_ == http3_client_request_head_error::invalid_content_length);
    }
    for (const std::string_view name : {"Authorization", "PROXY-Authorization", "cOoKiE"}) {
        const std::array fields_value{http3_field_section_field_view{name, "private"}};
        const auto trace = encode("TRACE", "/", fields_value);
        RUVIA_CHECK((trace.index() != 0) && std::get<1>(trace).kind_ == http3_client_request_head_error::forbidden_field);
        RUVIA_CHECK((encode("POST", "/", fields_value).index() == 0));
    }
    RUVIA_CHECK((encode("TRACE", "/", std::array{http3_field_section_field_view{"Max-Forwards", "0"}}).index() == 0));
}

RUVIA_TEST(http3_client_request_head_normalizes_headers_and_rejects_invalid_inputs) {
    const std::array headers{http3_field_section_field_view{"X-Test", "yes"},
        http3_field_section_field_view{"x-ready", "ok"}};
    auto normalized = encode("GET", "/", headers);
    RUVIA_CHECK((normalized.index() == 0));
    if ((normalized.index() == 0)) {
        const auto decoded = decode_http3_message_head(std::get<0>(normalized).field_section_, http3_message_head_kind::request);
        RUVIA_CHECK((decoded.index() == 0));
        if ((decoded.index() == 0)) {
            RUVIA_CHECK_EQ(std::get<0>(decoded).headers_[0].name_, "x-test");
            RUVIA_CHECK_EQ(std::get<0>(decoded).headers_[1].name_, "x-ready");
        }
    }
    RUVIA_CHECK((encode("CONNECT", "/").index() != 0));
    RUVIA_CHECK((encode("CONNECT", "", {}, 1).index() != 0));
    RUVIA_CHECK((encode("CONNECT", "",
                     std::array{http3_field_section_field_view{"content-length", "1"}})
                     .index() != 0));
    RUVIA_CHECK((encode("GET", "relative").index() != 0));
    RUVIA_CHECK((encode("GET", "/", std::array{http3_field_section_field_view{"host", "wrong.test"}}).index() != 0));
    RUVIA_CHECK((encode("GET", "/", std::array{http3_field_section_field_view{"te", "gzip"}}).index() != 0));
    const std::array duplicate{http3_field_section_field_view{"content-length", "1"},
        http3_field_section_field_view{"Content-Length", "1"}};
    RUVIA_CHECK((encode("POST", "/", duplicate, 1).index() != 0));
    RUVIA_CHECK((encode("POST", "/", std::array{http3_field_section_field_view{"content-length", "3"}}, 4).index() != 0));
    RUVIA_CHECK((encode("GET", "/", std::array{http3_field_section_field_view{":path", "/"}}).index() != 0));
    RUVIA_CHECK((encode("GET", "/", std::array{http3_field_section_field_view{"x@bad", "value"}}).index() != 0));
    for (const std::string_view value : {"", " 1", "1 ", "1, 1", "+1", "-1", "18446744073709551616"}) {
        const auto invalid = encode("POST", "/", std::array{http3_field_section_field_view{"content-length", value}});
        RUVIA_CHECK((invalid.index() != 0) && std::get<1>(invalid).kind_ == http3_client_request_head_error::invalid_content_length);
    }
    const auto wide = encode("POST", "/",
        std::array{http3_field_section_field_view{"content-length", "18446744073709551615"}});
    RUVIA_CHECK((wide.index() == 0));
    if ((wide.index() == 0)) {
        RUVIA_CHECK(std::get<0>(wide).body_plan_.expected_length_ == UINT64_MAX);
    }
    RUVIA_CHECK((encode("GET", "/", std::array{http3_field_section_field_view{"x-ows", " value\t"}}).index() == 0));
}

RUVIA_TEST(http3_client_request_head_rejects_invalid_cors_preflight_fields) {
    for (const auto field : {http3_field_section_field_view{"Origin", "https://app.example/path"},
             http3_field_section_field_view{"Access-Control-Request-Method", "POST GET"},
             http3_field_section_field_view{"Access-Control-Request-Headers", "x bad"},
             http3_field_section_field_view{"access-control-request-headers", ""}}) {
        const std::array fields_value{field};
        const auto request = encode("OPTIONS", "/", fields_value);
        RUVIA_CHECK((request.index() != 0) && std::get<1>(request).kind_ == http3_client_request_head_error::invalid_field);
    }
    const std::array valid{http3_field_section_field_view{"Origin", "https://app.example"},
        http3_field_section_field_view{"Access-Control-Request-Method", "POST"},
        http3_field_section_field_view{"Access-Control-Request-Headers", "x-trace, content-type"}};
    const auto encoded = encode("OPTIONS", "/", valid);
    RUVIA_CHECK((encoded.index() == 0));
    if ((encoded.index() == 0)) {
        RUVIA_CHECK((decode_http3_message_head(std::get<0>(encoded).field_section_, http3_message_head_kind::request)).index() == 0);
    }
}

RUVIA_TEST(http3_client_request_head_rejects_malformed_representation_fields) {
    const std::array valid{http3_field_section_field_view{"Content-Type", "application/json"},
        http3_field_section_field_view{"Content-Encoding", "gzip, br"}};
    RUVIA_CHECK((encode("POST", "/", valid).index() == 0));
    for (const auto field : {http3_field_section_field_view{"Content-Type", "text plain"},
             http3_field_section_field_view{"Content-Encoding", "gzip;q=1"}}) {
        const std::array fields_value{field};
        const auto request = encode("POST", "/", fields_value);
        RUVIA_CHECK((request.index() != 0) && std::get<1>(request).kind_ == http3_client_request_head_error::invalid_field);
    }
    const std::array repeated{http3_field_section_field_view{"Content-Type", "text/plain"},
        http3_field_section_field_view{"content-type", "application/json"}};
    const auto duplicate = encode("POST", "/", repeated);
    RUVIA_CHECK((duplicate.index() != 0) && std::get<1>(duplicate).kind_ == http3_client_request_head_error::invalid_field);
}

RUVIA_TEST(http3_client_request_head_rejects_forbidden_declared_trailer_names) {
    const std::array forbidden{http3_field_section_field_view{"Trailer", "Content-Length"}};
    const auto request = encode("POST", "/", forbidden);
    RUVIA_CHECK((request.index() != 0) && std::get<1>(request).kind_ == http3_client_request_head_error::invalid_field);
    const std::array valid{http3_field_section_field_view{"Trailer", "x-checksum"}};
    RUVIA_CHECK((encode("POST", "/", valid).index() == 0));
}

RUVIA_TEST(http3_client_request_head_rejects_malformed_expectation) {
    const std::array malformed{http3_field_section_field_view{"Expect", "foo?bar"}};
    const auto request = encode("POST", "/", malformed);
    RUVIA_CHECK((request.index() != 0) && std::get<1>(request).kind_ == http3_client_request_head_error::invalid_field);
    const std::array valid{http3_field_section_field_view{"Expect", "foo=bar"}};
    RUVIA_CHECK((encode("POST", "/", valid).index() == 0));
}

RUVIA_TEST(http3_client_request_head_enforces_limits_and_releases_resource_allocations) {
    counting_resource resource;
    auto failed = encode_http3_client_request_head({.method_ = "GET", .scheme_ = "https", .authority_ = "example.test:443", .path_ = "/"},
        {.max_encoded_bytes_ = 1}, &resource);
    RUVIA_CHECK((failed.index() != 0));
    auto limited = encode_http3_client_request_head({.method_ = "GET", .scheme_ = "https", .authority_ = "example.test:443", .path_ = "/"},
        {.max_fields_ = 3});
    RUVIA_CHECK((limited.index() != 0));
    auto pseudo_limit = encode_http3_client_request_head({.method_ = "GET", .scheme_ = "https", .authority_ = "example.test:443", .path_ = "/"}, {.max_decoded_bytes_ = 180});
    RUVIA_CHECK((pseudo_limit.index() != 0));
    if ((pseudo_limit.index() != 0)) {
        RUVIA_CHECK(std::get<1>(pseudo_limit).field_section_error_ ==
                    http3_field_section_error::field_list_too_large);
    }
    const auto exact = encode_http3_client_request_head(
        {.method_ = "GET", .scheme_ = "https", .authority_ = "example.test:443", .path_ = "/"},
        {.max_decoded_bytes_ = 182, .max_fields_ = 4});
    RUVIA_CHECK((exact.index() == 0));
    const auto both_limited = encode_http3_client_request_head(
        {.method_ = "GET", .scheme_ = "https", .authority_ = "example.test:443", .path_ = "/", .fields_ = std::array{http3_field_section_field_view{"x", "y"}}},
        {.max_decoded_bytes_ = 1, .max_fields_ = 4});
    RUVIA_CHECK((both_limited.index() != 0) && std::get<1>(both_limited).field_section_error_ == http3_field_section_error::too_many_fields);
    {
        auto result_value = encode_http3_client_request_head({.method_ = "GET", .scheme_ = "https", .authority_ = "example.test:443", .path_ = "/"}, {}, &resource);
        RUVIA_CHECK((result_value.index() == 0));
    }
    RUVIA_CHECK_EQ(resource.allocations_, resource.deallocations_);
}
