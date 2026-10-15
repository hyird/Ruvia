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
#include "ruvia/http/http3_qpack_connection.h"

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
        const auto decoded = decode_http3_message_head(std::get<0>(get).headers_.field_section(), http3_message_head_kind::request);
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
        const auto decoded = decode_http3_message_head(std::get<0>(post).headers_.field_section(), http3_message_head_kind::request);
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
        const auto decoded = decode_http3_message_head(std::get<0>(connect).headers_.field_section(), http3_message_head_kind::request);
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
        const auto decoded = decode_http3_message_head(std::get<0>(bodyless).headers_.field_section(),
            http3_message_head_kind::request);
        RUVIA_CHECK((decoded.index() == 0) && !std::get<0>(decoded).content_length_);
        RUVIA_CHECK(std::get<0>(bodyless).body_plan_.matches(0));
    }

    // Content-Length is generated from body_length_ only, as on HTTP/1 and
    // HTTP/2: a raw field is refused even when it matches the declared length.
    const auto raw_length = encode_http3_client_request_head({.method_ = "GET",
        .scheme_ = "https",
        .authority_ = "example.test:443",
        .path_ = "/",
        .fields_ = std::array{http3_field_section_field_view{"content-length", "0"}},
        .body_length_ = 0,
        .emit_content_length_ = false});
    RUVIA_CHECK((raw_length.index() != 0) && std::get<1>(raw_length).kind_ ==
                                                 http3_client_request_head_error::forbidden_field);
}

RUVIA_TEST(http3_client_request_head_trace_forbids_content_and_known_sensitive_fields) {
    for (const bool connection_owned : {false, true}) {
        http3_qpack_encoder encoder({.max_table_capacity_ = 0, .max_blocked_streams_ = 0});
        const http3_client_request_head_view view{.method_ = "TRACE", .scheme_ = "https", .authority_ = "example.test", .path_ = "/", .body_length_ = 0};
        const auto trace = connection_owned ? encode_http3_client_request_head(encoder, 0, view)
                                            : encode_http3_client_request_head(view);
        RUVIA_CHECK((trace.index() == 0));
        if ((trace.index() != 0)) {
            continue;
        }
        RUVIA_CHECK(std::get<0>(trace).body_plan_.matches(0));
        RUVIA_CHECK(!std::get<0>(trace).body_plan_.matches(1));
        const auto decoded = decode_http3_message_head(std::get<0>(trace).headers_.field_section(), http3_message_head_kind::request);
        RUVIA_CHECK((decoded.index() == 0) && !std::get<0>(decoded).content_length_);
        http3_data_write_plan body_plan(std::get<0>(trace).body_plan_);
        RUVIA_CHECK((body_plan.plan_chunk(std::span<const char>("x", 1), true).index() != 0));
    }
    for (const auto length : {std::optional<std::uint64_t>{}, std::optional<std::uint64_t>{1}}) {
        const auto declared = encode("TRACE", "/", {}, length);
        RUVIA_CHECK(declared.index() != 0 && std::get<1>(declared).kind_ == http3_client_request_head_error::invalid_content_length);
    }
    const auto raw_length = encode("TRACE", "/", std::array{http3_field_section_field_view{"content-length", "0"}});
    RUVIA_CHECK(raw_length.index() != 0 && std::get<1>(raw_length).kind_ == http3_client_request_head_error::forbidden_field);
    for (const std::string_view name : {"Authorization", "PROXY-Authorization", "cOoKiE"}) {
        const std::array fields_value{http3_field_section_field_view{name, "private"}};
        const auto trace = encode("TRACE", "/", fields_value);
        RUVIA_CHECK((trace.index() != 0) && std::get<1>(trace).kind_ == http3_client_request_head_error::forbidden_field);
        RUVIA_CHECK((encode("POST", "/", fields_value).index() == 0));
    }
    RUVIA_CHECK((encode("TRACE", "/", std::array{http3_field_section_field_view{"Max-Forwards", "0"}}, 0).index() == 0));
}

RUVIA_TEST(http3_client_request_head_options_content_requires_content_type) {
    for (const bool connection_owned : {false, true}) {
        for (const auto length : {std::optional<std::uint64_t>{}, std::optional<std::uint64_t>{0}, std::optional<std::uint64_t>{1}}) {
            http3_qpack_encoder encoder({});
            http3_client_request_head_view view{.method_ = "OPTIONS", .scheme_ = "https", .authority_ = "example.test", .path_ = "*", .body_length_ = length};
            const auto invalid = connection_owned ? encode_http3_client_request_head(encoder, 0, view)
                                                  : encode_http3_client_request_head(view);
            RUVIA_CHECK((invalid.index() != 0));
            if (invalid.index() != 0) {
                RUVIA_CHECK(std::get<1>(invalid).kind_ == http3_client_request_head_error::invalid_field);
            }
            const std::array fields_value{http3_field_section_field_view{"content-type", "text/plain"}};
            view.fields_ = fields_value;
            const auto valid = connection_owned ? encode_http3_client_request_head(encoder, 4, view)
                                                : encode_http3_client_request_head(view);
            RUVIA_CHECK((valid.index() == 0));
        }
    }
    const auto bodyless = encode_http3_client_request_head({.method_ = "OPTIONS", .scheme_ = "https", .authority_ = "example.test", .path_ = "*", .body_length_ = 0, .emit_content_length_ = false});
    RUVIA_CHECK((bodyless.index() == 0));
}

RUVIA_TEST(http3_client_request_head_normalizes_headers_and_rejects_invalid_inputs) {
    const std::array headers{http3_field_section_field_view{"X-Test", "yes"},
        http3_field_section_field_view{"x-ready", "ok"}};
    auto normalized = encode("GET", "/", headers);
    RUVIA_CHECK((normalized.index() == 0));
    if ((normalized.index() == 0)) {
        const auto decoded = decode_http3_message_head(std::get<0>(normalized).headers_.field_section(), http3_message_head_kind::request);
        RUVIA_CHECK((decoded.index() == 0));
        if ((decoded.index() == 0)) {
            RUVIA_CHECK_EQ(std::get<0>(decoded).headers_[0].name(), "x-test");
            RUVIA_CHECK_EQ(std::get<0>(decoded).headers_[1].name(), "x-ready");
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
    for (const std::string_view value : {"0", "1", "18446744073709551615", "", "+1"}) {
        const auto raw_length = encode("POST", "/", std::array{http3_field_section_field_view{"Content-Length", value}}, 1);
        RUVIA_CHECK((raw_length.index() != 0) && std::get<1>(raw_length).kind_ == http3_client_request_head_error::forbidden_field);
    }
    const auto wide = encode("POST", "/", {}, UINT64_MAX);
    RUVIA_CHECK((wide.index() == 0));
    if ((wide.index() == 0)) {
        RUVIA_CHECK(std::get<0>(wide).body_plan_.expected_length_ == UINT64_MAX);
    }
    RUVIA_CHECK((encode("GET", "/", std::array{http3_field_section_field_view{"x-ows", " value\t"}}).index() == 0));
}

RUVIA_TEST(http3_client_request_head_validates_non_http_authority_before_encoding) {
    for (const bool connection_owned : {false, true}) {
        http3_qpack_encoder encoder({.max_table_capacity_ = 0, .max_blocked_streams_ = 0});
        const auto encode_authority = [&](std::string_view authority) {
            const http3_client_request_head_view request{
                .method_ = "GET", .scheme_ = "custom", .authority_ = authority, .path_ = "/resource"};
            return connection_owned ? encode_http3_client_request_head(encoder, 0, request)
                                    : encode_http3_client_request_head(request);
        };
        for (const std::string_view authority : {"[broken", "[::1]suffix", "[v1.]", "example.test:8o",
                 "user@host@example.test", "exa%zzmple.test", "example.test\\suffix"}) {
            const auto rejected = encode_authority(authority);
            RUVIA_CHECK(rejected.index() != 0);
            if (rejected.index() != 0) {
                RUVIA_CHECK(std::get<1>(rejected).kind_ == http3_client_request_head_error::invalid_target);
            }
            RUVIA_CHECK(encoder.pending_encoder_output().empty());
        }
        for (const std::string_view authority : {"example.test", "exa%6dple.test", "user:pass@example.test:8080",
                 "[2001:db8::1]:8443", "[v1.example]:80", "service!name:1234"}) {
            const auto encoded = encode_authority(authority);
            RUVIA_CHECK(encoded.index() == 0);
            if (encoded.index() != 0) {
                continue;
            }
            const auto decoded = decode_http3_message_head(std::get<0>(encoded).headers_.field_section(), http3_message_head_kind::request);
            RUVIA_CHECK(decoded.index() == 0);
            if (decoded.index() == 0) {
                RUVIA_CHECK_EQ(std::get<0>(decoded).scheme_, "custom");
                RUVIA_CHECK_EQ(std::get<0>(decoded).authority_, authority);
                RUVIA_CHECK_EQ(std::get<0>(decoded).path_, "/resource");
            }
        }
    }
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
    const auto encoded = encode_http3_client_request_head({.method_ = "OPTIONS", .scheme_ = "https", .authority_ = "example.test:443", .path_ = "/", .fields_ = valid, .body_length_ = 0, .emit_content_length_ = false});
    RUVIA_CHECK((encoded.index() == 0));
    if ((encoded.index() == 0)) {
        RUVIA_CHECK((decode_http3_message_head(std::get<0>(encoded).headers_.field_section(), http3_message_head_kind::request)).index() == 0);
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

RUVIA_TEST(http3_client_request_head_generates_expect_only_from_the_expectation) {
    for (const std::string_view value : {"100-continue", "foo=bar", "foo?bar"}) {
        const auto raw = encode("POST", "/", std::array{http3_field_section_field_view{"Expect", value}}, 1);
        RUVIA_CHECK((raw.index() != 0) && std::get<1>(raw).kind_ == http3_client_request_head_error::forbidden_field);
    }
    const auto with_expectation = [](std::optional<std::uint64_t> length, http_client_request_expectation expectation) {
        return encode_http3_client_request_head({.method_ = "POST",
            .scheme_ = "https",
            .authority_ = "example.test:443",
            .path_ = "/",
            .body_length_ = length,
            .expectation_ = expectation});
    };
    for (const auto length : {std::optional<std::uint64_t>{3}, std::optional<std::uint64_t>{}}) {
        const auto encoded = with_expectation(length, http_client_request_expectation::continue_value);
        RUVIA_CHECK((encoded.index() == 0));
        if ((encoded.index() != 0)) {
            continue;
        }
        const auto decoded = decode_http3_message_head(std::get<0>(encoded).headers_.field_section(),
            http3_message_head_kind::request);
        RUVIA_CHECK((decoded.index() == 0));
        if ((decoded.index() == 0)) {
            std::size_t expect_fields = 0;
            for (const auto& field : std::get<0>(decoded).headers_) {
                if (field.name() == "expect") {
                    ++expect_fields;
                    RUVIA_CHECK_EQ(field.value(), "100-continue");
                }
            }
            RUVIA_CHECK_EQ(expect_fields, std::size_t{1});
        }
    }
    // RFC 9110 section 10.1.1: 100-continue requires content to follow.
    const auto bodyless = with_expectation(0, http_client_request_expectation::continue_value);
    RUVIA_CHECK((bodyless.index() != 0) && std::get<1>(bodyless).kind_ == http3_client_request_head_error::invalid_field);
    const auto unknown = with_expectation(3, static_cast<http_client_request_expectation>(0x7f));
    RUVIA_CHECK((unknown.index() != 0) && std::get<1>(unknown).kind_ == http3_client_request_head_error::invalid_field);
}

RUVIA_TEST(http3_client_request_head_allows_only_trailers_in_te) {
    for (const std::string_view value : {"trailers", "  trailers ", "TRAILERS"}) {
        RUVIA_CHECK((encode("GET", "/", std::array{http3_field_section_field_view{"TE", value}}).index() == 0));
    }
    for (const std::string_view value : {"gzip", "trailers, gzip", "trailers,,trailers", ""}) {
        const auto request = encode("GET", "/", std::array{http3_field_section_field_view{"te", value}});
        RUVIA_CHECK((request.index() != 0) && std::get<1>(request).kind_ == http3_client_request_head_error::forbidden_field);
    }
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
