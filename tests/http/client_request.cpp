#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "content_decoding_fixture.h"
#include "http1_chunked_body.h"
#include "http1_client_request.h"
#include "http1_client_response.h"
#include "http1_server_response.h"
#include "http_client_redirect.h"
#include "http_header.h"
#include "http_request.h"

namespace {

using ruvia::http1_client_request_buffer_too_small;
using ruvia::http1_client_request_prepare_error;
using ruvia::http1_client_request_wire_policy;
using ruvia::http1_client_request_writer;
using ruvia::http1_close_policy;
using ruvia::http_client_request_content_view;
using ruvia::http_client_request_view;
using ruvia::http_origin_view;
using ruvia::is_valid_http_origin_form_target;
using ruvia::prepared_http1_client_request;

TEST(Ruvia, http_client_origin_target_validation) {
    EXPECT_TRUE(is_valid_http_origin_form_target("/ok%2F?q=%7B%7D"));
    EXPECT_TRUE(!is_valid_http_origin_form_target("*"));
    EXPECT_TRUE(!is_valid_http_origin_form_target(""));
    EXPECT_TRUE(!is_valid_http_origin_form_target("relative"));
    EXPECT_TRUE(!is_valid_http_origin_form_target("/bad#fragment"));
    EXPECT_TRUE(!is_valid_http_origin_form_target("/bad\\path"));
    EXPECT_TRUE(!is_valid_http_origin_form_target("/bad%zz"));
    EXPECT_TRUE(!is_valid_http_origin_form_target("/bad%"));
    EXPECT_TRUE(!is_valid_http_origin_form_target("/bad%2"));
}

template <std::size_t n = 2048>
struct prepared_fixture final {
    std::array<char, n> buffer_{};
    std::variant<prepared_http1_client_request, http1_client_request_buffer_too_small, http1_client_request_prepare_error> result_;

    prepared_fixture(const http_origin_view& origin, const http_client_request_view& request, http1_client_request_wire_policy policy = http1_client_request_wire_policy{}) :
        result_(http1_client_request_writer().prepare(origin, request, buffer_, policy)) {}
};

[[nodiscard]] http1_client_request_prepare_error prepare_error(const http_client_request_view& request, http1_client_request_wire_policy policy = http1_client_request_wire_policy{}) {
    std::array<char, 512> buffer;
    const auto result_value = http1_client_request_writer().prepare(http_origin_view::https({.host_ = "example.test"}), request, buffer, policy);
    const auto* failure = std::get_if<http1_client_request_prepare_error>(&result_value);
    if (failure == nullptr) {
        throw std::runtime_error("test expected request preparation to fail");
    }
    return *failure;
}

auto prepare_field_request(unsigned entry, std::span<const ruvia::http_header_view> fields, std::span<char> output, ruvia::http_request_field_source source) {
    const auto origin = http_origin_view::https({.host_ = "example.test"});
    const http1_client_request_wire_policy policy{.field_source_ = source};
    const http1_client_request_writer writer;
    switch (entry) {
        case 0:
            return writer.prepare(origin, http_client_request_view{.headers_ = fields}, output, policy);
        case 1:
            return writer.prepare_streaming(origin, ruvia::http1_client_request_head_view{.method_ = "POST", .headers_ = fields, .content_length_ = 0}, output, policy);
        case 2:
            return writer.prepare_connect(origin, fields, output, policy);
        default:
            return writer.prepare_connect("example.test:443", fields, output, policy);
    }
}
}  // namespace

TEST(Ruvia, http1_client_request_writer_emits_one_canonical_scatter_gather_plan) {
    const ruvia::http_header_view headers[] = {
        {"X-Test", "one"},
    };
    http_client_request_view request;
    request.method_ = "POST";
    request.target_ = "/items?q=1";
    request.headers_ = headers;
    request.content_ = http_client_request_content_view::bytes("payload");

    prepared_fixture fixture_value(http_origin_view::https({.host_ = "example.test"}), request, http1_client_request_wire_policy{.expectation_ = ruvia::http_client_request_expectation::continue_value});
    const auto* prepared = std::get_if<prepared_http1_client_request>(&fixture_value.result_);
    EXPECT_TRUE(prepared != nullptr);
    if (prepared == nullptr) {
        return;
    }
    EXPECT_TRUE(prepared->head() ==
        "POST /items?q=1 HTTP/1.1\r\n"
        "Host: example.test\r\n"
        "X-Test: one\r\n"
        "Content-Length: 7\r\n"
        "Expect: 100-continue\r\n\r\n");
    const auto* gated = prepared->content_plan().continue_gated();
    EXPECT_TRUE(gated != nullptr);
    EXPECT_TRUE(prepared->content_plan().without_content() == nullptr);
    EXPECT_TRUE(prepared->content_plan().immediate() == nullptr);
    if (gated != nullptr) {
        EXPECT_TRUE(gated->bytes() == "payload");
    }
    const std::array<std::byte, 7> raw_bytes{std::byte{'p'}, std::byte{'a'}, std::byte{'y'}, std::byte{'l'}, std::byte{'o'}, std::byte{'a'}, std::byte{'d'}};
    const auto byte_content = http_client_request_content_view::bytes(std::span<const std::byte>(raw_bytes));
    EXPECT_TRUE(byte_content.borrowed_bytes() != nullptr);
    if (const auto* borrowed = byte_content.borrowed_bytes()) {
        EXPECT_EQ(borrowed->value(), std::string_view("payload"));
    }
}

TEST(Ruvia, http1_client_request_content_distinguishes_absent_from_explicit_empty) {
    http_client_request_view absent;
    absent.method_ = "GET";
    EXPECT_TRUE(absent.content_.without_content() != nullptr);
    EXPECT_TRUE(absent.content_.borrowed_bytes() == nullptr);
    prepared_fixture absent_fixture(http_origin_view::https({.host_ = "example.test"}), absent);
    const auto* absent_prepared = std::get_if<prepared_http1_client_request>(&absent_fixture.result_);
    EXPECT_TRUE(absent_prepared != nullptr);
    if (absent_prepared != nullptr) {
        EXPECT_TRUE(absent_prepared->content_plan().without_content() != nullptr);
        EXPECT_TRUE(absent_prepared->content_plan().immediate() == nullptr);
        EXPECT_TRUE(absent_prepared->content_plan().continue_gated() == nullptr);
        EXPECT_TRUE(!(absent_prepared->head().find("Content-Length") != std::string_view::npos));
    }

    http_client_request_view empty;
    empty.method_ = "POST";
    empty.content_ = http_client_request_content_view::bytes("");
    EXPECT_TRUE(empty.content_.without_content() == nullptr);
    EXPECT_TRUE(empty.content_.borrowed_bytes() != nullptr);
    if (const auto* bytes = empty.content_.borrowed_bytes()) {
        EXPECT_TRUE(bytes->value().empty());
    }
    prepared_fixture empty_fixture(http_origin_view::https({.host_ = "example.test"}), empty);
    const auto* empty_prepared = std::get_if<prepared_http1_client_request>(&empty_fixture.result_);
    EXPECT_TRUE(empty_prepared != nullptr);
    if (empty_prepared != nullptr) {
        const auto* immediate = empty_prepared->content_plan().immediate();
        EXPECT_TRUE(immediate != nullptr);
        EXPECT_TRUE(empty_prepared->content_plan().without_content() == nullptr);
        EXPECT_TRUE(empty_prepared->content_plan().continue_gated() == nullptr);
        if (immediate != nullptr) {
            EXPECT_TRUE(immediate->bytes().empty());
        }
        EXPECT_TRUE((empty_prepared->head().find("Content-Length: 0\r\n") != std::string_view::npos));
    }
}

TEST(Ruvia, http1_client_request_writer_owns_request_target_forms_and_host) {
    http_client_request_view extension;
    extension.method_ = "PROPFIND";
    extension.target_ = "/dav";
    prepared_fixture extension_fixture(http_origin_view::https({.host_ = "example.test", .port_ = 8443}), extension);
    const auto* extension_prepared = std::get_if<prepared_http1_client_request>(&extension_fixture.result_);
    EXPECT_TRUE(extension_prepared != nullptr);
    if (extension_prepared != nullptr) {
        EXPECT_TRUE(extension_prepared->head().starts_with("PROPFIND /dav HTTP/1.1\r\nHost: example.test:8443\r\n"));
    }

    http_client_request_view options;
    options.method_ = "OPTIONS";
    options.target_ = "*";
    prepared_fixture options_fixture(http_origin_view::http({.host_ = "example.test"}), options);
    EXPECT_TRUE(std::holds_alternative<prepared_http1_client_request>(options_fixture.result_));

    http_client_request_view invalid_asterisk;
    invalid_asterisk.method_ = "GET";
    invalid_asterisk.target_ = "*";
    EXPECT_TRUE(prepare_error(invalid_asterisk) == http1_client_request_prepare_error::invalid_target);

    http_client_request_view absolute;
    absolute.target_ = "https://example.test/path";
    EXPECT_TRUE(prepare_error(absolute) == http1_client_request_prepare_error::invalid_target);

    http_client_request_view connect;
    connect.method_ = "CONNECT";
    connect.target_ = "example.test:443";
    EXPECT_TRUE(prepare_error(connect) == http1_client_request_prepare_error::connect_requires_dedicated_entry);
}

TEST(Ruvia, http1_client_connect_entry_generates_authority_form_atomically) {
    std::array<char, 512> buffer;
    const auto result_value = http1_client_request_writer().prepare_connect(http_origin_view::https({.host_ = "example.test"}), {}, buffer);
    const auto* prepared = std::get_if<prepared_http1_client_request>(&result_value);
    EXPECT_TRUE(prepared != nullptr);
    if (prepared != nullptr) {
        EXPECT_TRUE(prepared->head() ==
            "CONNECT example.test:443 HTTP/1.1\r\n"
            "Host: example.test:443\r\n\r\n");
        EXPECT_TRUE(prepared->content_plan().without_content() != nullptr);
    }

    std::array<char, 512> ipv6_buffer;
    const auto ipv6 = http1_client_request_writer().prepare_connect(http_origin_view::https({.host_ = "[::1]"}), {}, ipv6_buffer);
    const auto* ipv6_prepared = std::get_if<prepared_http1_client_request>(&ipv6);
    EXPECT_TRUE(ipv6_prepared != nullptr);
    if (ipv6_prepared != nullptr) {
        EXPECT_TRUE(ipv6_prepared->head().starts_with("CONNECT [::1]:443 HTTP/1.1\r\nHost: [::1]:443\r\n"));
    }

    std::array<char, 128> invalid_buffer;
    const auto invalid = http1_client_request_writer().prepare_connect(http_origin_view::https({.host_ = "example.test", .port_ = 0}), {}, invalid_buffer);
    const auto* invalid_failure = std::get_if<http1_client_request_prepare_error>(&invalid);
    EXPECT_TRUE(invalid_failure != nullptr);
    EXPECT_TRUE(*invalid_failure == http1_client_request_prepare_error::invalid_connect_origin);

    const auto invalid_authority = http1_client_request_writer().prepare_connect("example.test:0", {}, invalid_buffer);
    const auto* invalid_authority_failure = std::get_if<http1_client_request_prepare_error>(&invalid_authority);
    EXPECT_TRUE(invalid_authority_failure != nullptr && *invalid_authority_failure == http1_client_request_prepare_error::invalid_connect_origin);
}

TEST(Ruvia, http1_client_request_writer_is_the_only_host_and_framing_owner) {
    struct case_value final {
        std::string_view name_;
        std::string_view value_;
        http1_client_request_prepare_error error_;
    };
    const case_value cases[] = {
        {"Host", "other.test", http1_client_request_prepare_error::host_header_managed_by_writer},
        {"Content-Length", "7", http1_client_request_prepare_error::content_length_managed_by_writer},
        {"Transfer-Encoding", "chunked", http1_client_request_prepare_error::transfer_encoding_unsupported},
        {"Trailer", "Digest", http1_client_request_prepare_error::trailer_requires_chunked_content},
        {"Expect", "100-continue", http1_client_request_prepare_error::expect_header_managed_by_writer},
    };
    for (const auto& test : cases) {
        const ruvia::http_header_view header_value(test.name_, test.value_);
        http_client_request_view request;
        request.method_ = "POST";
        request.headers_ = std::span<const ruvia::http_header_view>(&header_value, 1);
        request.content_ = http_client_request_content_view::bytes("payload");
        EXPECT_TRUE(prepare_error(request) == test.error_);
    }
}

TEST(Ruvia, http1_client_request_writer_rejects_repeated_singleton_fields) {
    struct case_value final {
        std::string_view name_;
        std::string_view first_;
        std::string_view second_;
    };
    const case_value cases[] = {
        {"Access-Control-Request-Method", "GET", "POST"},
        {"Authorization", "Bearer first", "Bearer second"},
        {"Content-Type", "text/plain", "application/json"},
        {"If-Modified-Since", "Sun, 06 Nov 1994 08:49:37 GMT", "Mon, 07 Nov 1994 08:49:37 GMT"},
        {"If-Range", "\"first\"", "\"second\""},
        {"If-Unmodified-Since", "Sun, 06 Nov 1994 08:49:37 GMT", "Mon, 07 Nov 1994 08:49:37 GMT"},
        {"Origin", "https://first.test", "https://second.test"},
        {"Range", "bytes=0-1", "bytes=2-3"},
        {"Sec-WebSocket-Key", "first", "second"},
        {"Sec-WebSocket-Version", "13", "12"},
        {"User-Agent", "first/1", "second/2"},
    };
    for (const auto& test : cases) {
        const ruvia::http_header_view headers[] = {
            {test.name_, test.first_},
            {test.name_, test.second_},
        };
        http_client_request_view request;
        request.headers_ = headers;

        EXPECT_TRUE(prepare_error(request) == http1_client_request_prepare_error::invalid_header);
    }
}

TEST(Ruvia, http1_client_request_writer_validates_cors_fields) {
    const ruvia::http_header_view invalid_headers[] = {
        {"Origin", "https://example.test/path"},
        {"Access-Control-Request-Method", "GET, POST"},
        {"Access-Control-Request-Headers", "X-Good, Bad Header"},
    };
    for (const auto& header : invalid_headers) {
        http_client_request_view request;
        request.headers_ = std::span<const ruvia::http_header_view>(&header, 1);
        EXPECT_TRUE(prepare_error(request) == http1_client_request_prepare_error::invalid_header);
    }

    const ruvia::http_header_view valid_headers[] = {
        {"Origin", "https://first.test https://second.test"},
        {"Access-Control-Request-Method", "GET"},
        {"Access-Control-Request-Headers", "X-First"},
        {"Access-Control-Request-Headers", "X-Second, X-Third"},
    };
    http_client_request_view valid;
    valid.headers_ = valid_headers;
    prepared_fixture fixture_value(http_origin_view::https({.host_ = "example.test"}), valid);
    EXPECT_TRUE(std::holds_alternative<prepared_http1_client_request>(fixture_value.result_));
}

TEST(Ruvia, http1_client_request_writer_owns_hop_by_hop_field_contracts) {
    struct failure_case final {
        std::array<ruvia::http_header_view, 2> headers_;
        std::size_t count_;
        http1_client_request_prepare_error error_;
    };
    const failure_case failures[] = {
        {.headers_ = {ruvia::http_header_view("Connection", "close,"), {}}, .count_ = 1, .error_ = http1_client_request_prepare_error::invalid_connection},
        {.headers_ = {ruvia::http_header_view("Connection", "close;parameter"), {}}, .count_ = 1, .error_ = http1_client_request_prepare_error::invalid_connection},
        {.headers_ = {ruvia::http_header_view("Upgrade", "websocket"), {}}, .count_ = 1, .error_ = http1_client_request_prepare_error::upgrade_connection_option_required},
        {.headers_ = {ruvia::http_header_view("Connection", "Upgrade"), ruvia::http_header_view("Upgrade", "websocket/")}, .count_ = 2, .error_ = http1_client_request_prepare_error::invalid_upgrade},
        {.headers_ = {ruvia::http_header_view("TE", "trailers"), {}}, .count_ = 1, .error_ = http1_client_request_prepare_error::te_connection_option_required},
    };
    for (const auto& test : failures) {
        http_client_request_view request;
        request.headers_ = std::span<const ruvia::http_header_view>(test.headers_.data(), test.count_);
        EXPECT_TRUE(prepare_error(request) == test.error_);
    }

    for (const std::string_view connection_options : {"Host", "close, content-length", "EXPECT, keep-alive", "Cache-Control", "Trailer", "Authorization", "Cookie", "Range", "Transfer-Encoding", "close, transfer-encoding", "Forwarded", "close, X-Forwarded-For", "x-forwarded-proto", "keep-alive, X-FORWARDED-HOST"}) {
        const ruvia::http_header_view connection("Connection", connection_options);
        http_client_request_view request;
        request.method_ = "POST";
        request.headers_ = std::span<const ruvia::http_header_view>(&connection, 1);
        request.content_ = http_client_request_content_view::bytes("payload");
        EXPECT_TRUE(prepare_error(request) == http1_client_request_prepare_error::invalid_connection);
    }

    const ruvia::http_header_view valid_headers[] = {
        {"Connection", "keep-alive"},
        {"Connection", "Upgrade, TE, X-Hop, Proxy-Authorization"},
        {"Upgrade", "custom/1, websocket"},
        {"TE", "trailers"},
        {"X-Hop", "value"},
        {"X-Forwarded-For", "192.0.2.1"},
    };
    http_client_request_view valid;
    valid.headers_ = valid_headers;
    prepared_fixture fixture_value(http_origin_view::https({.host_ = "example.test"}), valid);
    EXPECT_TRUE(std::holds_alternative<prepared_http1_client_request>(fixture_value.result_));
}

TEST(Ruvia, http1_client_request_writer_validates_te_capabilities_and_weights) {
    const auto prepare_with_te = [](std::string_view value) -> std::optional<http1_client_request_prepare_error> {
        const std::array headers{
            ruvia::http_header_view("Connection", "TE"),
            ruvia::http_header_view("TE", value),
        };
        http_client_request_view request;
        request.headers_ = headers;
        std::array<char, 512> buffer;
        const auto result_value = http1_client_request_writer().prepare(http_origin_view::https({.host_ = "example.test"}), request, buffer);
        if (const auto* failure = std::get_if<http1_client_request_prepare_error>(&result_value)) {
            return *failure;
        }
        EXPECT_TRUE(std::holds_alternative<prepared_http1_client_request>(result_value));
        return std::nullopt;
    };

    for (const std::string_view valid : {"", "trailers", "gzip", "deflate;q=0.5", "deflate;Q=0.5", "x-gzip ; q=1.000", "gzip;q=0, trailers", "trailers , TRAILERS"}) {
        EXPECT_TRUE(!prepare_with_te(valid).has_value());
    }

    for (const std::string_view invalid : {",trailers", "trailers,", "trailers,,gzip", "chunked", "br", "trailers;q=0.5", "gzip;q=1.001", "gzip;q=\"0.5\"", "gzip;q =0.5", "gzip;q= 0.5", "gzip; q = 0.5", "gzip;level=1", "gzip;q=0.5;level=1", "gzip; q", "gzip;q="}) {
        EXPECT_TRUE(prepare_with_te(invalid) == http1_client_request_prepare_error::invalid_header);
    }
}

TEST(Ruvia, http1_client_request_writer_enforces_expect_content_semantics) {
    const ruvia::http_header_view expect("Expect", "100-Continue");
    http_client_request_view raw_expectation;
    raw_expectation.method_ = "POST";
    raw_expectation.headers_ = std::span<const ruvia::http_header_view>(&expect, 1);
    raw_expectation.content_ = http_client_request_content_view::bytes("x");
    EXPECT_TRUE(prepare_error(raw_expectation) == http1_client_request_prepare_error::expect_header_managed_by_writer);

    http_client_request_view absent;
    absent.method_ = "POST";
    prepared_fixture absent_fixture(http_origin_view::https({.host_ = "example.test"}), absent, http1_client_request_wire_policy{.expectation_ = ruvia::http_client_request_expectation::continue_value});
    const auto* absent_failure = std::get_if<http1_client_request_prepare_error>(&absent_fixture.result_);
    EXPECT_TRUE(absent_failure != nullptr);
    if (absent_failure != nullptr) {
        EXPECT_TRUE(*absent_failure == http1_client_request_prepare_error::expectation_without_content);
    }

    http_client_request_view empty = absent;
    empty.content_ = http_client_request_content_view::bytes("");
    prepared_fixture empty_fixture(http_origin_view::https({.host_ = "example.test"}), empty, http1_client_request_wire_policy{.expectation_ = ruvia::http_client_request_expectation::continue_value});
    const auto* empty_failure = std::get_if<http1_client_request_prepare_error>(&empty_fixture.result_);
    EXPECT_TRUE(empty_failure != nullptr);
    if (empty_failure != nullptr) {
        EXPECT_TRUE(*empty_failure == http1_client_request_prepare_error::expectation_without_content);
    }

    http_client_request_view nonempty;
    nonempty.method_ = "POST";
    nonempty.content_ = http_client_request_content_view::bytes("x");
    prepared_fixture fixture_value(http_origin_view::https({.host_ = "example.test"}), nonempty, http1_client_request_wire_policy{.expectation_ = ruvia::http_client_request_expectation::continue_value});
    const auto* prepared = std::get_if<prepared_http1_client_request>(&fixture_value.result_);
    EXPECT_TRUE(prepared != nullptr);
    if (prepared != nullptr) {
        EXPECT_TRUE(prepared->content_plan().continue_gated() != nullptr);
        EXPECT_TRUE(prepared->head().find("Expect: 100-continue\r\n") != std::string_view::npos);
    }
}

TEST(Ruvia, http1_client_request_writer_rejects_invalid_wire_policy) {
    http_client_request_view request;
    const auto invalid_without_expectation = http1_client_request_wire_policy{.close_policy_ = static_cast<http1_close_policy>(0xFF)};
    const auto invalid_expect_continue = http1_client_request_wire_policy{.close_policy_ = static_cast<http1_close_policy>(0xFF), .expectation_ = ruvia::http_client_request_expectation::continue_value};
    const auto invalid_expectation = http1_client_request_wire_policy{.expectation_ = static_cast<ruvia::http_client_request_expectation>(0xFF)};
    EXPECT_TRUE(prepare_error(request, invalid_without_expectation) == http1_client_request_prepare_error::invalid_close_policy);
    EXPECT_TRUE(prepare_error(request, invalid_expect_continue) == http1_client_request_prepare_error::invalid_close_policy);
    EXPECT_TRUE(prepare_error(request, invalid_expectation) == http1_client_request_prepare_error::invalid_expectation);

    std::array<char, 512> buffer{};
    const auto connect = http1_client_request_writer().prepare_connect(http_origin_view::https({.host_ = "example.test"}), std::span<const ruvia::http_header_view>{}, buffer, invalid_without_expectation);
    const auto* connect_failure = std::get_if<http1_client_request_prepare_error>(&connect);
    EXPECT_TRUE(connect_failure != nullptr);
    if (connect_failure != nullptr) {
        EXPECT_TRUE(*connect_failure == http1_client_request_prepare_error::invalid_close_policy);
    }

    const auto invalid_connect_expectation = http1_client_request_writer().prepare_connect(http_origin_view::https({.host_ = "example.test"}), std::span<const ruvia::http_header_view>{}, buffer, invalid_expectation);
    const auto* invalid_connect_expectation_failure = std::get_if<http1_client_request_prepare_error>(&invalid_connect_expectation);
    EXPECT_TRUE(invalid_connect_expectation_failure != nullptr);
    if (invalid_connect_expectation_failure != nullptr) {
        EXPECT_TRUE(*invalid_connect_expectation_failure == http1_client_request_prepare_error::invalid_expectation);
    }
}

// RFC 9112 section 3.2.2: a request through a forward proxy names its origin
// in absolute-form, and the generated Host carries the same authority
// (section 3.2). OPTIONS * keeps only the authority (section 3.2.4).
TEST(Ruvia, http1_client_request_writer_emits_absolute_form_for_a_forward_proxy) {
    const http1_client_request_wire_policy absolute{.target_form_ = ruvia::http1_request_target_form::absolute};
    const auto head_of = [](const auto& fixture_value) {
        const auto* prepared = std::get_if<prepared_http1_client_request>(&fixture_value.result_);
        return prepared != nullptr ? std::string(prepared->head()) : std::string("<failed>");
    };

    http_client_request_view request;
    request.target_ = "/items?q=1";
    const prepared_fixture default_port(http_origin_view::http({.host_ = "example.test"}), request, absolute);
    EXPECT_EQ(head_of(default_port), "GET http://example.test/items?q=1 HTTP/1.1\r\nHost: example.test\r\n\r\n");
    const prepared_fixture origin_form(http_origin_view::http({.host_ = "example.test"}), request);
    EXPECT_EQ(head_of(origin_form), "GET /items?q=1 HTTP/1.1\r\nHost: example.test\r\n\r\n");

    const prepared_fixture explicit_port(http_origin_view::https({.host_ = "[::1]", .port_ = 8443}), request, absolute);
    const auto explicit_head = head_of(explicit_port);
    EXPECT_EQ(explicit_head, "GET https://[::1]:8443/items?q=1 HTTP/1.1\r\nHost: [::1]:8443\r\n\r\n");
    // The proxy reads the same authority from the target and Host.
    const auto received = ruvia::test::parse_http1_request_message(explicit_head);
    ASSERT_TRUE(received.complete());
    EXPECT_EQ(received.state_.request().target_form(), ruvia::http_request_target_form::absolute);
    EXPECT_EQ(received.state_.request().authority(), "[::1]:8443");
    EXPECT_EQ(received.state_.request().path(), "/items");

    http_client_request_view options;
    options.method_ = "OPTIONS";
    options.target_ = "*";
    const prepared_fixture asterisk(http_origin_view::http({.host_ = "example.test", .port_ = 8001}), options, absolute);
    EXPECT_EQ(head_of(asterisk), "OPTIONS http://example.test:8001 HTTP/1.1\r\nHost: example.test:8001\r\n\r\n");

    std::array<char, 512> buffer{};
    const auto streaming = http1_client_request_writer().prepare_streaming(http_origin_view::https({.host_ = "example.test"}), ruvia::http1_client_request_head_view{.method_ = "POST", .target_ = "/upload", .content_length_ = 3}, buffer, absolute);
    const auto* streamed = std::get_if<prepared_http1_client_request>(&streaming);
    ASSERT_TRUE(streamed != nullptr);
    EXPECT_EQ(streamed->head(), "POST https://example.test/upload HTTP/1.1\r\nHost: example.test\r\nContent-Length: 3\r\n\r\n");

    // The head budget counts the scheme and authority of the target.
    std::array<char, 40> small{};
    const auto too_small = http1_client_request_writer().prepare(http_origin_view::http({.host_ = "example.test"}), request, small, absolute);
    const auto* required = std::get_if<http1_client_request_buffer_too_small>(&too_small);
    ASSERT_TRUE(required != nullptr);
    EXPECT_EQ(required->required_head_bytes(), head_of(default_port).size());
}

TEST(Ruvia, http1_client_request_writer_rejects_invalid_target_forms) {
    EXPECT_EQ(prepare_error(http_client_request_view{}, http1_client_request_wire_policy{.target_form_ = static_cast<ruvia::http1_request_target_form>(0xFF)}), http1_client_request_prepare_error::invalid_target_form);
    // CONNECT is authority-form only (RFC 9112 section 3.2.3).
    const http1_client_request_wire_policy absolute{.target_form_ = ruvia::http1_request_target_form::absolute};
    std::array<char, 512> buffer{};
    const auto by_origin = http1_client_request_writer().prepare_connect(http_origin_view::https({.host_ = "example.test"}), std::span<const ruvia::http_header_view>{}, buffer, absolute);
    const auto by_authority = http1_client_request_writer().prepare_connect("example.test:443", std::span<const ruvia::http_header_view>{}, buffer, absolute);
    for (const auto* result_value : {&by_origin, &by_authority}) {
        const auto* failure = std::get_if<http1_client_request_prepare_error>(result_value);
        ASSERT_TRUE(failure != nullptr);
        EXPECT_EQ(*failure, http1_client_request_prepare_error::invalid_target_form);
    }
}

TEST(Ruvia, http1_client_request_writer_enforces_method_content_semantics) {
    http_client_request_view trace;
    trace.method_ = "TRACE";
    trace.content_ = http_client_request_content_view::bytes("trace body");
    EXPECT_TRUE(prepare_error(trace) == http1_client_request_prepare_error::content_forbidden_for_method);

    http_client_request_view options;
    options.method_ = "OPTIONS";
    options.content_ = http_client_request_content_view::bytes("options body");
    EXPECT_TRUE(prepare_error(options) == http1_client_request_prepare_error::options_content_type_required);

    const ruvia::http_header_view content_type_value("Content-Type", "application/json");
    options.headers_ = std::span<const ruvia::http_header_view>(&content_type_value, 1);
    prepared_fixture fixture_value(http_origin_view::https({.host_ = "example.test"}), options);
    const auto* prepared = std::get_if<prepared_http1_client_request>(&fixture_value.result_);
    EXPECT_TRUE(prepared != nullptr);
    if (prepared != nullptr) {
        EXPECT_TRUE(prepared->content_plan().immediate() != nullptr);
        if (const auto* content = prepared->content_plan().immediate()) {
            EXPECT_EQ(content->bytes(), std::string_view("options body"));
        }
    }

    const ruvia::http_header_view invalid_content_type("Content-Type", "invalid");
    options.headers_ = std::span<const ruvia::http_header_view>(&invalid_content_type, 1);
    EXPECT_TRUE(prepare_error(options) == http1_client_request_prepare_error::invalid_header);
}

TEST(Ruvia, http1_client_request_writer_allows_empty_method_content) {
    for (const char* method : {"OPTIONS", "TRACE"}) {
        http_client_request_view request;
        request.method_ = method;
        request.content_ = http_client_request_content_view::bytes("");
        prepared_fixture fixture(http_origin_view::http({.host_ = "example.test"}), request);
        const auto* prepared = std::get_if<prepared_http1_client_request>(&fixture.result_);
        EXPECT_TRUE(prepared != nullptr);
        if (prepared == nullptr) {
            continue;
        }
        EXPECT_TRUE(prepared->head().find("Content-Length: 0\r\n") != std::string_view::npos);
        const auto* content = prepared->content_plan().immediate();
        EXPECT_TRUE(content != nullptr);
        if (content != nullptr) {
            EXPECT_TRUE(content->bytes().empty());
        }

        std::array<char, 1024> head_buffer;
        ruvia::http1_client_request_head_view streaming;
        streaming.method_ = method;
        streaming.content_length_ = 0;
        const auto result = http1_client_request_writer().prepare_streaming(http_origin_view::http({.host_ = "example.test"}), streaming, head_buffer);
        const auto* streaming_prepared = std::get_if<prepared_http1_client_request>(&result);
        EXPECT_TRUE(streaming_prepared != nullptr);
        if (streaming_prepared != nullptr) {
            EXPECT_TRUE(streaming_prepared->head().find("Content-Length: 0\r\n") != std::string_view::npos);
            const auto* streaming_content = streaming_prepared->content_plan().streaming();
            EXPECT_TRUE(streaming_content != nullptr);
            if (streaming_content != nullptr) {
                EXPECT_EQ(streaming_content->content_length(), std::optional<std::uint64_t>{0});
            }
        }
    }
}

TEST(Ruvia, http1_client_request_writer_rejects_invalid_content_type_parameters) {
    for (const std::string_view value : {"text/plain; charset", "text/plain; charset=", "text/plain; charset =utf-8", "text/plain; charset=utf-8; CHARSET=latin1", "text/plain; charset=\"unterminated"}) {
        const ruvia::http_header_view content_type_value("Content-Type", value);
        http_client_request_view request;
        request.method_ = "POST";
        request.headers_ = std::span<const ruvia::http_header_view>(&content_type_value, 1);
        request.content_ = http_client_request_content_view::bytes("body");
        EXPECT_TRUE(prepare_error(request) == http1_client_request_prepare_error::invalid_header);
    }

    const ruvia::http_header_view valid_content_type("Content-Type", "text/plain; charset=\"utf-8\"");
    http_client_request_view valid;
    valid.method_ = "POST";
    valid.headers_ = std::span<const ruvia::http_header_view>(&valid_content_type, 1);
    valid.content_ = http_client_request_content_view::bytes("body");
    prepared_fixture fixture_value(http_origin_view::https({.host_ = "example.test"}), valid);
    EXPECT_TRUE(std::holds_alternative<prepared_http1_client_request>(fixture_value.result_));
}

TEST(Ruvia, http1_client_request_writer_rejects_invalid_content_encoding_syntax) {
    for (const std::string_view value : {"gzip;level=9", "bad coding", "", ",gzip", "gzip,"}) {
        const ruvia::http_header_view content_encoding("Content-Encoding", value);
        http_client_request_view request;
        request.method_ = "POST";
        request.headers_ = std::span<const ruvia::http_header_view>(&content_encoding, 1);
        request.content_ = http_client_request_content_view::bytes("body");
        EXPECT_TRUE(prepare_error(request) == http1_client_request_prepare_error::invalid_header);
    }

    for (const std::string_view value : {"deflate", "gzip, br"}) {
        const ruvia::http_header_view content_encoding("Content-Encoding", value);
        http_client_request_view request;
        request.method_ = "POST";
        request.headers_ = std::span<const ruvia::http_header_view>(&content_encoding, 1);
        request.content_ = http_client_request_content_view::bytes("body");
        prepared_fixture fixture_value(http_origin_view::https({.host_ = "example.test"}), request);
        EXPECT_TRUE(std::holds_alternative<prepared_http1_client_request>(fixture_value.result_));
    }
}

TEST(Ruvia, http1_client_request_writer_returns_exact_buffer_requirement_without_partial_output) {
    http_client_request_view request;
    request.method_ = "POST";
    request.target_ = "/upload";
    request.content_ = http_client_request_content_view::bytes("body");

    std::array<char, 8> small;
    small.fill('z');
    const auto too_small = http1_client_request_writer().prepare(http_origin_view::https({.host_ = "example.test"}), request, small);
    const auto* too_small_val = std::get_if<http1_client_request_buffer_too_small>(&too_small);
    EXPECT_TRUE(too_small_val != nullptr);
    EXPECT_TRUE(std::ranges::all_of(small, [](char value) {
        return value == 'z';
    }));

    std::array<char, 512> enough;
    const auto prepared = http1_client_request_writer().prepare(http_origin_view::https({.host_ = "example.test"}), request, enough);
    const auto* prepared_val = std::get_if<prepared_http1_client_request>(&prepared);
    EXPECT_TRUE(prepared_val != nullptr);
    if (prepared_val != nullptr && too_small_val != nullptr) {
        EXPECT_EQ(too_small_val->required_head_bytes(), prepared_val->head().size());
    }
}

TEST(Ruvia, http1_client_request_writer_enforces_header_size_transactionally) {
    std::array<ruvia::http_header_view, 80> headers;
    headers.fill(ruvia::http_header_view("X-Test", "x"));
    http_client_request_view many;
    many.headers_ = headers;
    std::array<char, 2048> many_output{};
    const auto many_prepared = http1_client_request_writer().prepare(http_origin_view::https({.host_ = "example.test"}), many, many_output);
    EXPECT_TRUE(std::holds_alternative<prepared_http1_client_request>(many_prepared));
    http_client_request_view oversized;
    std::string target(ruvia::max_http_header_bytes, 'a');
    target.front() = '/';
    oversized.target_ = target;
    EXPECT_TRUE(prepare_error(oversized) == http1_client_request_prepare_error::header_too_large);

    http_client_request_view invalid_method;
    invalid_method.method_ = "GET\r";
    std::array<char, 128> untouched;
    untouched.fill('q');
    const auto invalid = http1_client_request_writer().prepare(http_origin_view::https({.host_ = "example.test"}), invalid_method, untouched);
    const auto* invalid_failure = std::get_if<http1_client_request_prepare_error>(&invalid);
    EXPECT_TRUE(invalid_failure != nullptr);
    EXPECT_TRUE(*invalid_failure == http1_client_request_prepare_error::invalid_method);
    EXPECT_TRUE(std::ranges::all_of(untouched, [](char value) {
        return value == 'q';
    }));
    EXPECT_TRUE(!ruvia::http1_client_request_prepare_error_message(http1_client_request_prepare_error::invalid_method).empty());
}

TEST(Ruvia, http1_client_request_context_binds_the_actual_close_signal) {
    const ruvia::http_header_view close_header("Connection", "close");
    http_client_request_view request;
    request.headers_ = std::span<const ruvia::http_header_view>(&close_header, 1);
    prepared_fixture explicit_close(http_origin_view::https({.host_ = "example.test"}), request);
    auto* explicit_prepared = std::get_if<prepared_http1_client_request>(&explicit_close.result_);
    EXPECT_TRUE(explicit_prepared != nullptr);
    if (explicit_prepared != nullptr) {
        auto parser = ruvia::http1_client_response_parser(std::move(*explicit_prepared).take_exchange_state());
        const auto response = parser.parse("HTTP/1.1 204 No Content\r\n\r\n");
        EXPECT_TRUE(response.parsed() != nullptr);
        if (response.parsed() != nullptr) {
            const auto* without_content = response.parsed()->plan().without_content();
            EXPECT_TRUE(without_content != nullptr);
            if (without_content != nullptr) {
                EXPECT_TRUE(without_content->persistence() == ruvia::http1_close_policy::close_after_response);
            }
        }
    }

    http_client_request_view generated_request;
    prepared_fixture generated_close(http_origin_view::https({.host_ = "example.test"}), generated_request, http1_client_request_wire_policy{.close_policy_ = http1_close_policy::close_after_response});
    const auto* generated_prepared = std::get_if<prepared_http1_client_request>(&generated_close.result_);
    EXPECT_TRUE(generated_prepared != nullptr);
    if (generated_prepared != nullptr) {
        EXPECT_EQ(generated_prepared->head(), std::string_view("GET / HTTP/1.1\r\nHost: example.test\r\nConnection: close\r\n\r\n"));
    }
}

TEST(Ruvia, http1_client_request_writer_prepare_connect_udp_preserves_specific_prepare_errors) {
    const auto origin = http_origin_view::https({.host_ = "example.test", .port_ = 443});
    const std::array valid_capsule{ruvia::http_header_view{"Capsule-Protocol", "?1"}};
    std::array<char, 256> untouched;
    untouched.fill('z');

    const auto expect_error = [&](std::string_view target, std::span<const ruvia::http_header_view> headers, http1_client_request_prepare_error expected) {
        const auto result = http1_client_request_writer().prepare_connect_udp(origin, target, headers, untouched);
        const auto* failure = std::get_if<http1_client_request_prepare_error>(&result);
        EXPECT_TRUE(failure != nullptr);
        if (failure != nullptr) {
            EXPECT_TRUE(*failure == expected);
        }
        EXPECT_TRUE(std::ranges::all_of(untouched, [](char c) {
            return c == 'z';
        }));
    };

    expect_error("relative", valid_capsule, http1_client_request_prepare_error::invalid_target);
    expect_error("*", valid_capsule, http1_client_request_prepare_error::invalid_target);

    const std::array host_headers{valid_capsule[0], ruvia::http_header_view{"Host", "example.test"}};
    expect_error("/udp", host_headers, http1_client_request_prepare_error::host_header_managed_by_writer);

    const std::array length_headers{valid_capsule[0], ruvia::http_header_view{"Content-Length", "0"}};
    expect_error("/udp", length_headers, http1_client_request_prepare_error::content_length_managed_by_writer);

    const std::array te_headers{valid_capsule[0], ruvia::http_header_view{"Transfer-Encoding", "chunked"}};
    expect_error("/udp", te_headers, http1_client_request_prepare_error::transfer_encoding_unsupported);

    const std::array trailer_headers{valid_capsule[0], ruvia::http_header_view{"Trailer", "X-Test"}};
    expect_error("/udp", trailer_headers, http1_client_request_prepare_error::trailer_requires_chunked_content);

    const std::array expect_headers{valid_capsule[0], ruvia::http_header_view{"Expect", "100-continue"}};
    expect_error("/udp", expect_headers, http1_client_request_prepare_error::expect_header_managed_by_writer);

    std::string huge_target(ruvia::max_http_header_bytes, 'a');
    huge_target.front() = '/';
    expect_error(huge_target, valid_capsule, http1_client_request_prepare_error::header_too_large);

    const std::array connection_headers{valid_capsule[0], ruvia::http_header_view{"Connection", "keep-alive"}};
    expect_error("/udp", connection_headers, http1_client_request_prepare_error::invalid_connection);

    const std::array upgrade_headers{valid_capsule[0], ruvia::http_header_view{"Upgrade", "websocket"}};
    expect_error("/udp", upgrade_headers, http1_client_request_prepare_error::invalid_upgrade);

    const std::array content_type_headers{valid_capsule[0], ruvia::http_header_view{"Content-Type", "application/octet-stream"}};
    expect_error("/udp", content_type_headers, http1_client_request_prepare_error::invalid_header);
}

TEST(Ruvia, http1_client_forwards_received_application_fields_without_regenerating_values) {
    const std::array cases{ruvia::http_header_view{"Content-Type", "not-a-media-type"}, ruvia::http_header_view{"Origin", "https://example.test/path"}, ruvia::http_header_view{"Access-Control-Request-Method", "GET, POST"}, ruvia::http_header_view{"Access-Control-Request-Headers", "X-Good, Bad Header"}, ruvia::http_header_view{"Content-Encoding", ""}, ruvia::http_header_view{"Content-Encoding", ",,"}, ruvia::http_header_view{"Content-Encoding", ", gzip,, br,"}, ruvia::http_header_view{"Content-Encoding", ", unknown-coding,,"}};
    for (const auto& field : cases) {
        std::string wire = "GET / HTTP/1.1\r\nHost: example.test\r\n";
        wire.append(field.name()).append(": ").append(field.value()).append("\r\n\r\n");
        ruvia::http1_server_request_parse_state incoming;
        incoming.parse_head(wire);
        EXPECT_TRUE(incoming.head_ready() != nullptr);
        const auto received = incoming.request().header(field.name());
        EXPECT_TRUE(received == std::optional<std::string_view>{field.value()});
        if (!received) {
            continue;
        }
        const std::array fields{ruvia::http_header_view{field.name(), *received}};
        for (unsigned entry = 0; entry != 4; ++entry) {
            std::array<char, 1024> output;
            output.fill('z');
            const auto generated = prepare_field_request(entry, fields, output, ruvia::http_request_field_source::generated);
            EXPECT_TRUE(std::holds_alternative<http1_client_request_prepare_error>(generated));
            EXPECT_TRUE(std::ranges::all_of(output, [](char c) {
                return c == 'z';
            }));
            auto forwarded = prepare_field_request(entry, fields, output, ruvia::http_request_field_source::forwarded);
            auto* prepared = std::get_if<prepared_http1_client_request>(&forwarded);
            EXPECT_TRUE(prepared != nullptr);
            if (!prepared) {
                continue;
            }
            ruvia::http1_server_request_parse_state upstream;
            upstream.parse_head(prepared->head());
            EXPECT_TRUE(upstream.head_ready() != nullptr);
            EXPECT_TRUE(upstream.request().header(field.name()) == received);
            EXPECT_TRUE(upstream.request().header("Host") == std::optional<std::string_view>{entry < 2 ? "example.test" : "example.test:443"});
        }
    }
}

TEST(Ruvia, http1_client_forwarding_retains_security_ownership_and_singleton_rules) {
    const std::array invalid{ruvia::http_header_view{"X-Test", "ok\r\nInjected: yes"}, ruvia::http_header_view{"Bad Name", "value"}, ruvia::http_header_view{"Host", "example.test"}, ruvia::http_header_view{"Content-Length", "0"}, ruvia::http_header_view{"Expect", "100-continue"}, ruvia::http_header_view{"Transfer-Encoding", "chunked"}, ruvia::http_header_view{"Content-Encoding", ", gzip;level=9,"}, ruvia::http_header_view{"Content-Encoding", ", bad coding,"}, ruvia::http_header_view{"Connection", "Content-Type"}, ruvia::http_header_view{"Connection", "X-Forwarded-For"}, ruvia::http_header_view{"Upgrade", ",websocket,"}, ruvia::http_header_view{"TE", ",trailers,"}};
    for (unsigned entry = 0; entry != 4; ++entry) {
        std::array<char, 1024> output;
        for (const auto& field : invalid) {
            output.fill('z');
            const std::array fields{field};
            const auto result = prepare_field_request(entry, fields, output, ruvia::http_request_field_source::forwarded);
            EXPECT_TRUE(std::holds_alternative<http1_client_request_prepare_error>(result));
            EXPECT_TRUE(std::ranges::all_of(output, [](char c) {
                return c == 'z';
            }));
        }
        for (const auto& field : std::array{ruvia::http_header_view{"Content-Type", "invalid"}, ruvia::http_header_view{"Origin", "invalid"}, ruvia::http_header_view{"Access-Control-Request-Method", "GET, POST"}, ruvia::http_header_view{"Authorization", "Bearer token"}, ruvia::http_header_view{"User-Agent", "agent"}, ruvia::http_header_view{"If-Modified-Since", "invalid"}, ruvia::http_header_view{"If-Unmodified-Since", "invalid"}}) {
            const std::array fields{field, field};
            const auto result = prepare_field_request(entry, fields, output, ruvia::http_request_field_source::forwarded);
            EXPECT_TRUE(std::holds_alternative<http1_client_request_prepare_error>(result));
        }
        const auto invalid_source = prepare_field_request(entry, {}, output, static_cast<ruvia::http_request_field_source>(0xff));
        EXPECT_TRUE(std::holds_alternative<http1_client_request_prepare_error>(invalid_source));
    }
}

TEST(Ruvia, http1_client_forwarded_upgrade_empty_members_preserve_protocol_transition) {
    constexpr std::string_view wire = "GET / HTTP/1.1\r\nHost: example.test\r\nConnection: Upgrade\r\nUpgrade: , websocket,,\r\n\r\n";
    ruvia::http1_server_request_parse_state incoming;
    incoming.parse_head(wire);
    EXPECT_TRUE(incoming.head_ready() != nullptr);
    const auto upgrade = incoming.request().header("Upgrade");
    EXPECT_TRUE(upgrade == std::optional<std::string_view>{", websocket,,"});
    if (!upgrade) {
        return;
    }
    const std::array fields{ruvia::http_header_view{"Connection", "Upgrade"}, ruvia::http_header_view{"Upgrade", *upgrade}};
    std::array<char, 1024> output;
    EXPECT_TRUE(std::holds_alternative<http1_client_request_prepare_error>(prepare_field_request(0, fields, output, ruvia::http_request_field_source::generated)));
    auto forwarded = prepare_field_request(0, fields, output, ruvia::http_request_field_source::forwarded);
    auto* prepared = std::get_if<prepared_http1_client_request>(&forwarded);
    EXPECT_TRUE(prepared != nullptr);
    if (!prepared) {
        return;
    }
    EXPECT_TRUE(prepared->head().find("Upgrade: , websocket,,\r\n") != std::string_view::npos);
    ruvia::http1_client_response_parser response_parser(std::move(*prepared).take_exchange_state());
    const auto response = response_parser.parse("HTTP/1.1 101 Switching Protocols\r\nConnection: Upgrade\r\nUpgrade: websocket\r\n\r\n");
    EXPECT_TRUE(response.parsed() != nullptr);
    if (const auto* parsed = response.parsed()) {
        EXPECT_TRUE(parsed->plan().protocol_upgrade() != nullptr);
    }
    auto second = prepare_field_request(0, fields, output, ruvia::http_request_field_source::forwarded);
    auto* second_prepared = std::get_if<prepared_http1_client_request>(&second);
    EXPECT_TRUE(second_prepared != nullptr);
    if (second_prepared) {
        ruvia::http1_client_response_parser unoffered_parser(std::move(*second_prepared).take_exchange_state());
        const auto unoffered = unoffered_parser.parse("HTTP/1.1 101 Switching Protocols\r\nConnection: Upgrade\r\nUpgrade: other\r\n\r\n");
        EXPECT_TRUE(unoffered.failure() != nullptr);
    }
    const std::array invalid_fields{fields[0], ruvia::http_header_view{"Upgrade", ",websocket/,"}};
    EXPECT_TRUE(std::holds_alternative<http1_client_request_prepare_error>(prepare_field_request(0, invalid_fields, output, ruvia::http_request_field_source::forwarded)));
}

TEST(Ruvia, http1_connect_udp_forwarding_keeps_protocol_owned_fields) {
    const auto origin = http_origin_view::https({.host_ = "example.test"});
    const std::array fields{ruvia::http_header_view{"Capsule-Protocol", "?1"}, ruvia::http_header_view{"Origin", "https://example.test/path"}};
    std::array<char, 1024> output;
    const http1_client_request_writer writer;
    const http1_client_request_wire_policy policy{.field_source_ = ruvia::http_request_field_source::forwarded};
    EXPECT_TRUE(std::holds_alternative<http1_client_request_prepare_error>(writer.prepare_connect_udp(origin, "/udp", fields, output)));
    auto forwarded = writer.prepare_connect_udp(origin, "/udp", fields, output, policy);
    const auto* prepared = std::get_if<prepared_http1_client_request>(&forwarded);
    EXPECT_TRUE(prepared != nullptr);
    if (prepared) {
        ruvia::http1_server_request_parse_state upstream;
        upstream.parse_head(prepared->head());
        EXPECT_TRUE(upstream.head_ready() != nullptr);
        EXPECT_TRUE(upstream.request().header("Origin") == std::optional<std::string_view>{fields[1].value()});
        EXPECT_TRUE(upstream.request().header("Upgrade") == std::optional<std::string_view>{"connect-udp"});
    }
    for (const auto& forbidden : std::array{ruvia::http_header_view{"Content-Type", "invalid"}, ruvia::http_header_view{"Content-Length", "0"}, ruvia::http_header_view{"Upgrade", "connect-udp"}, ruvia::http_header_view{"Connection", "Upgrade"}, ruvia::http_header_view{"Origin", "bad\r\nInjected: yes"}}) {
        const std::array invalid{fields[0], forbidden};
        EXPECT_TRUE(std::holds_alternative<http1_client_request_prepare_error>(writer.prepare_connect_udp(origin, "/udp", invalid, output, policy)));
    }
    EXPECT_TRUE(std::holds_alternative<http1_client_request_prepare_error>(writer.prepare_connect_udp(origin, "/udp", {}, output, {.field_source_ = static_cast<ruvia::http_request_field_source>(0xff)})));
}

TEST(Ruvia, http1_client_forwards_options_content_without_inventing_media_type) {
    constexpr std::string_view incoming_wire = "OPTIONS / HTTP/1.1\r\nHost: example.test\r\nContent-Length: 4\r\n\r\nbody";
    const auto incoming = ruvia::test::parse_http1_request_message(incoming_wire);
    EXPECT_TRUE(incoming.complete());
    EXPECT_EQ(incoming.body_, "body");
    const auto origin = http_origin_view::https({.host_ = "example.test"});
    for (unsigned entry = 0; entry != 3; ++entry) {
        std::array<char, 1024> output;
        const auto prepare = [&](std::string_view method, ruvia::http_request_field_source source) {
            const http1_client_request_wire_policy policy{.field_source_ = source};
            const http1_client_request_writer writer;
            if (entry == 0) {
                return writer.prepare(origin, http_client_request_view{.method_ = method, .content_ = http_client_request_content_view::bytes(incoming.body_)}, output, policy);
            }
            return writer.prepare_streaming(origin, ruvia::http1_client_request_head_view{.method_ = method, .content_length_ = entry == 1 ? std::optional<ruvia::http_content_length>{4} : std::nullopt}, output, policy);
        };
        const auto generated = prepare("OPTIONS", ruvia::http_request_field_source::generated);
        const auto* error = std::get_if<http1_client_request_prepare_error>(&generated);
        EXPECT_TRUE(error && *error == http1_client_request_prepare_error::options_content_type_required);
        const auto trace = prepare("TRACE", ruvia::http_request_field_source::forwarded);
        const auto* trace_error = std::get_if<http1_client_request_prepare_error>(&trace);
        EXPECT_TRUE(trace_error && *trace_error == http1_client_request_prepare_error::content_forbidden_for_method);
        auto forwarded = prepare("OPTIONS", ruvia::http_request_field_source::forwarded);
        const auto* prepared = std::get_if<prepared_http1_client_request>(&forwarded);
        EXPECT_TRUE(prepared != nullptr);
        if (!prepared) {
            continue;
        }
        std::string wire(prepared->head());
        if (entry == 0) {
            wire.append(prepared->content_plan().immediate()->bytes());
        } else {
            ruvia::http1_request_content_writer content(*prepared->content_plan().streaming());
            const auto chunk = content.plan_chunk(incoming.body_);
            EXPECT_TRUE(chunk.index() == 0);
            if (chunk.index() != 0) {
                continue;
            }
            wire.append(std::get<0>(chunk).prefix()).append(incoming.body_).append(std::get<0>(chunk).suffix());
            content.commit_chunk();
            std::array<char, 32> final_buffer;
            const auto finish = content.plan_finish(final_buffer);
            EXPECT_TRUE(finish.index() == 0);
            if (finish.index() != 0) {
                continue;
            }
            wire.append(std::get<0>(finish));
            content.commit_finish();
        }
        const auto upstream = ruvia::test::parse_http1_request_message(wire);
        EXPECT_TRUE(upstream.complete());
        EXPECT_EQ(upstream.body_, incoming.body_);
        EXPECT_TRUE(!upstream.state_.request().header("Content-Type"));
    }
}

namespace {

using ruvia::http_origin_view;
using ruvia::http_scheme;
using ruvia::make_http_origin_authority;
using ruvia::test::throws_on;

http_origin_view origin_for(std::string_view host, std::uint16_t port, http_scheme scheme = http_scheme::http) {
    return scheme == http_scheme::https ? http_origin_view::https({.host_ = host, .port_ = port}) : http_origin_view::http({.host_ = host, .port_ = port});
}

std::string authority_for(const http_origin_view& origin) {
    const auto authority = make_http_origin_authority(origin);
    return authority;
}

}  // namespace

TEST(Ruvia, http_origin_factory_makes_an_invalid_host_unrepresentable) {
    EXPECT_TRUE(!throws_on([] {
        (void)origin_for("example.com", 80);
    }));
    EXPECT_TRUE(!throws_on([] {
        (void)origin_for("[::1]", 443, http_scheme::https);
    }));
    EXPECT_TRUE(!throws_on([] {
        (void)origin_for("[v1.future]", 80);
    }));
    // Port zero is a syntactically distinct URI origin. Whether a transport can
    // connect to it is deliberately outside this protocol-only value type.
    EXPECT_TRUE(!throws_on([] {
        (void)origin_for("example.com", 0);
    }));

    EXPECT_TRUE(throws_on([] {
        (void)origin_for("", 80);
    }));
    EXPECT_TRUE(throws_on([] {
        (void)origin_for("bad host", 80);
    }));
    EXPECT_TRUE(throws_on([] {
        (void)origin_for("::1", 80);
    }));
    EXPECT_TRUE(throws_on([] {
        (void)origin_for("[::1", 80);
    }));
    EXPECT_TRUE(throws_on([] {
        (void)origin_for("example.com:80", 80);
    }));
    EXPECT_TRUE(throws_on([] {
        (void)origin_for("bad?host", 80);
    }));
    EXPECT_TRUE(throws_on([] {
        (void)origin_for("[::::]", 80);
    }));
    EXPECT_TRUE(throws_on([] {
        (void)origin_for("[v.future]", 80);
    }));
    EXPECT_TRUE(throws_on([] {
        (void)origin_for("caf\xC3\xA9.example", 80);
    }));
    EXPECT_TRUE(throws_on([] {
        (void)origin_for(std::string_view("api\0.internal", 13), 80);
    }));
}

TEST(Ruvia, http_serialized_origin_matches_fetch_wire_grammar) {
    for (const auto value : {"https://example.com", "https://example.com.", "https://sub.example.com.:8443", "https://exa_mple.com", "https://-example.com", "https://example..com", "https://.", "http://127.0.0.1:8080", "https://[::]", "https://[::1]", "https://[0:0:1::1]", "custom+scheme://sub-domain.example:65535", "custom+scheme://sub-domain.example:0"}) {
        EXPECT_TRUE(ruvia::is_valid_http_serialized_origin(value));
    }
    for (const auto value : {"", "null", "HTTPS://example.com", "https://EXAMPLE.com", "https://example.com/", "https://exa%6dple.com", "https://123", "https://1.2.3", "https://127.0.0.1.", "https://example.123", "https://example.0x10", "https://[v1.future]", "https://[::A]", "https://[::0001]", "https://[1:2:3:4:5:6:7::]", "https://[1::0:0:1]", "https://[0:0:1::1:1:1]", "https://example.com:", "https://example.com:0443", "http://example.com:80", "https://example.com:443", "ws://example.com:80", "wss://example.com:443", "ftp://example.com:21", "https://example.com:65536", "custom+scheme://sub-domain.example:00001", "custom+scheme://sub-domain.example:99999", "https://example.com:123456"}) {
        EXPECT_TRUE(!ruvia::is_valid_http_serialized_origin(value));
    }
}

TEST(Ruvia, http_origin_authority_brackets_ipv6_and_omits_default_port) {
    // The default port for the scheme is omitted: 80 for http, 443 for https.
    EXPECT_EQ(authority_for(http_origin_view::http({.host_ = "example.com"})), std::string("example.com"));
    EXPECT_EQ(authority_for(http_origin_view::https({.host_ = "example.com"})), std::string("example.com"));

    // A non-default port is appended.
    EXPECT_EQ(authority_for(origin_for("example.com", 8080)), std::string("example.com:8080"));

    // The default depends on the typed scheme: 80 is not default for https, and
    // 443 is not default for http, so each is included.
    EXPECT_EQ(authority_for(origin_for("example.com", 80, http_scheme::https)), std::string("example.com:80"));
    EXPECT_EQ(authority_for(origin_for("example.com", 443)), std::string("example.com:443"));

    // uri-host already carries the brackets required for an IP-literal; the
    // serializer no longer guesses host kind by searching for a colon.
    EXPECT_EQ(authority_for(origin_for("[::1]", 80)), std::string("[::1]"));
    EXPECT_EQ(authority_for(origin_for("[::1]", 8080)), std::string("[::1]:8080"));
    EXPECT_EQ(authority_for(origin_for("[2001:db8::1]", 443, http_scheme::https)), std::string("[2001:db8::1]"));
    EXPECT_EQ(authority_for(origin_for("[v1.future]", 8080)), std::string("[v1.future]:8080"));
    EXPECT_EQ(authority_for(origin_for("example.com", 0)), std::string("example.com:0"));
}

using namespace ruvia::test;

namespace {

using ruvia::classify_http_client_origin_authority;
using ruvia::http_client_origin_authority_status;
using ruvia::http_client_redirect_content_disposition;
using ruvia::http_client_redirect_resolution_error;
using ruvia::http_client_request_view;
using ruvia::http_client_resolved_redirect;
using ruvia::http_origin_view;
using ruvia::http_scheme;
using ruvia::http_unique_header_status;
using ruvia::is_http_redirect_status;
using ruvia::lookup_unique_http_client_response_header;
using ruvia::plan_http_client_redirect_request;
using ruvia::resolve_http_client_redirect_target;

// Same-origin resolution is a followable redirect whose destination stays on
// the request origin; the cross-origin-capable resolver reports that as a
// resolved destination with cross_origin() == false.
void check_resolved_target(const http_origin_view& origin, std::string_view current_target, std::string_view location, std::string_view expected) {
    const auto result_value = resolve_http_client_redirect_target(origin, {.current_target_ = current_target, .location_ = location});
    const auto* resolved = std::get_if<http_client_resolved_redirect>(&result_value);
    EXPECT_TRUE(resolved != nullptr);
    if (resolved != nullptr) {
        EXPECT_TRUE(!resolved->cross_origin());
        EXPECT_EQ(resolved->target(), expected);
    }
}

void check_cross_origin_redirect(const http_origin_view& origin, std::string_view current_target, std::string_view location) {
    const auto result_value = resolve_http_client_redirect_target(origin, {.current_target_ = current_target, .location_ = location});
    const auto* resolved = std::get_if<http_client_resolved_redirect>(&result_value);
    EXPECT_TRUE(resolved != nullptr);
    if (resolved != nullptr) {
        EXPECT_TRUE(resolved->cross_origin());
    }
}

void check_redirect_target_failure(const http_origin_view& origin, std::string_view current_target, std::string_view location, http_client_redirect_resolution_error expected) {
    const auto result_value = resolve_http_client_redirect_target(origin, {.current_target_ = current_target, .location_ = location});
    const auto* failure = std::get_if<http_client_redirect_resolution_error>(&result_value);
    EXPECT_TRUE(failure != nullptr);
    if (failure != nullptr) {
        EXPECT_TRUE(*failure == expected);
    }
}

}  // namespace

TEST(Ruvia, http_client_redirect_status_set) {
    for (const ruvia::http_status_code status : {ruvia::http_status::moved_permanently, ruvia::http_status::found, ruvia::http_status::see_other, ruvia::http_status::temporary_redirect, ruvia::http_status::permanent_redirect}) {
        EXPECT_TRUE(is_http_redirect_status(status));
    }
    for (const ruvia::http_status_code status : {ruvia::http_status::ok, ruvia::http_status::no_content, ruvia::http_status::multiple_choices, ruvia::http_status::not_modified, ruvia::http_status::use_proxy, ruvia::http_status_code::from_value(306), ruvia::http_status_code::from_value(399), ruvia::http_status::not_found}) {
        EXPECT_TRUE(!is_http_redirect_status(status));
    }
}

TEST(Ruvia, http_client_redirect_request_plan_follows_rfc) {
    // 303 selects a retrieval request. HEAD remains HEAD; every other method
    // becomes GET. The representation and content-specific fields are dropped.
    {
        http_client_request_view request;
        request.method_ = "PUT";
        request.content_ = ruvia::http_client_request_content_view::bytes("payload");
        const auto plan = plan_http_client_redirect_request(request, {.status_ = ruvia::http_status::see_other});
        EXPECT_EQ(plan.method(), std::string_view("GET"));
        EXPECT_TRUE(plan.content_disposition() == http_client_redirect_content_disposition::drop);
    }
    {
        http_client_request_view request;
        request.method_ = "HEAD";
        const auto plan = plan_http_client_redirect_request(request, {.status_ = ruvia::http_status::see_other});
        EXPECT_EQ(plan.method(), std::string_view("HEAD"));
        EXPECT_TRUE(plan.content_disposition() == http_client_redirect_content_disposition::drop);
    }

    // RFC 9110 permits the historical POST-to-GET rewrite for 301/302. Other
    // methods are not aliases for POST and retain both method and content.
    {
        http_client_request_view request;
        request.method_ = "POST";
        request.content_ = ruvia::http_client_request_content_view::bytes("payload");
        const auto plan = plan_http_client_redirect_request(request, {.status_ = ruvia::http_status::found});
        EXPECT_EQ(plan.method(), std::string_view("GET"));
        EXPECT_TRUE(plan.content_disposition() == http_client_redirect_content_disposition::drop);
    }
    {
        http_client_request_view request;
        request.method_ = "PUT";
        request.content_ = ruvia::http_client_request_content_view::bytes("payload");
        const auto plan = plan_http_client_redirect_request(request, {.status_ = ruvia::http_status::moved_permanently});
        EXPECT_EQ(plan.method(), std::string_view("PUT"));
        EXPECT_TRUE(plan.content_disposition() == http_client_redirect_content_disposition::preserve);
    }

    // Method tokens are case-sensitive: lowercase "post" is a distinct method.
    {
        http_client_request_view request;
        request.method_ = "post";
        const auto plan = plan_http_client_redirect_request(request, {.status_ = ruvia::http_status::moved_permanently});
        EXPECT_EQ(plan.method(), std::string_view("post"));
        EXPECT_TRUE(plan.content_disposition() == http_client_redirect_content_disposition::preserve);
    }

    // 307/308 never change method or content.
    for (const ruvia::http_status_code status : {ruvia::http_status::temporary_redirect, ruvia::http_status::permanent_redirect}) {
        http_client_request_view request;
        request.method_ = "POST";
        request.content_ = ruvia::http_client_request_content_view::bytes("payload");
        const auto plan = plan_http_client_redirect_request(request, {.status_ = status});
        EXPECT_EQ(plan.method(), std::string_view("POST"));
        EXPECT_TRUE(plan.content_disposition() == http_client_redirect_content_disposition::preserve);
    }
}

TEST(Ruvia, http_client_redirect_request_plan_owns_preserved_method) {
    std::string method = "PROPFIND";
    http_client_request_view request;
    request.method_ = method;

    const auto plan = plan_http_client_redirect_request(request, {.status_ = ruvia::http_status::temporary_redirect});
    for (char& ch : method) {
        ch = 'X';
    }

    EXPECT_EQ(plan.method(), std::string_view("PROPFIND"));
    EXPECT_TRUE(plan.content_disposition() == http_client_redirect_content_disposition::preserve);
}

TEST(Ruvia, http_client_response_header_lookup_distinguishes_empty_and_repeated) {
    auto parsed = ruvia::test::parse_response("GET", "HTTP/1.1 302 Found\r\nLocation:\r\nContent-Length: 0");
    const auto& head = parsed.head_;

    const auto empty = lookup_unique_http_client_response_header(head, "location");
    EXPECT_TRUE(empty.status_ == http_unique_header_status::found);
    EXPECT_TRUE(empty.value_.empty());

    const auto missing = lookup_unique_http_client_response_header(head, "missing");
    EXPECT_TRUE(missing.status_ == http_unique_header_status::absent);

    auto repeated_parsed = ruvia::test::parse_response("GET", "HTTP/1.1 302 Found\r\nLocation:\r\nLOCATION: /second\r\nContent-Length: 0");
    const auto repeated = lookup_unique_http_client_response_header(repeated_parsed.head_, "Location");
    EXPECT_TRUE(repeated.status_ == http_unique_header_status::repeated);
}

TEST(Ruvia, http_client_authority_matches_typed_origin) {
    const auto non_default = origin_for("example.com", 8080);
    const auto is = [](const http_origin_view& origin, std::string_view authority) {
        return classify_http_client_origin_authority(origin, authority);
    };
    EXPECT_TRUE(is(non_default, "example.com:8080") == http_client_origin_authority_status::same_origin);
    for (const std::string_view different : {"example.com", "example.com:9090", "other.com:8080", "example.com:0", "example.com:"}) {
        EXPECT_TRUE(is(non_default, different) == http_client_origin_authority_status::different_origin);
    }
    EXPECT_TRUE(is(non_default, "user@example.com:8080") == http_client_origin_authority_status::invalid_authority);
    EXPECT_TRUE(is(non_default, "example.com:99999") == http_client_origin_authority_status::invalid_authority);

    EXPECT_TRUE(is(http_origin_view::http({.host_ = "example.com"}), "example.com") == http_client_origin_authority_status::same_origin);
    EXPECT_TRUE(is(http_origin_view::http({.host_ = "example.com"}), "EXAMPLE.com:") == http_client_origin_authority_status::same_origin);
    EXPECT_TRUE(is(http_origin_view::http({.host_ = "example.com"}), "exa%6dple.com") == http_client_origin_authority_status::same_origin);
    EXPECT_TRUE(is(http_origin_view::http({.host_ = "!example"}), "%21example") == http_client_origin_authority_status::different_origin);
    EXPECT_TRUE(is(http_origin_view::https({.host_ = "example.com"}), "example.com") == http_client_origin_authority_status::same_origin);
    EXPECT_TRUE(is(http_origin_view::http({.host_ = "example.com", .port_ = 0}), "example.com:0") == http_client_origin_authority_status::same_origin);
    EXPECT_TRUE(is(http_origin_view::http({.host_ = "example.com", .port_ = 0}), "example.com") == http_client_origin_authority_status::different_origin);

    const auto v6 = origin_for("[::1]", 8080);
    EXPECT_TRUE(is(v6, "[::1]:8080") == http_client_origin_authority_status::same_origin);
    EXPECT_TRUE(is(v6, "[::2]:8080") == http_client_origin_authority_status::different_origin);
    EXPECT_TRUE(is(v6, "[::1]:") == http_client_origin_authority_status::different_origin);

    const auto future = http_origin_view::http({.host_ = "[v1.future]"});
    EXPECT_TRUE(is(future, "[V1.FUTURE]:") == http_client_origin_authority_status::same_origin);
}

TEST(Ruvia, http_client_ipv6_origin_compares_addresses_not_spellings) {
    struct equivalent_pair final {
        std::string_view compressed_;
        std::string_view expanded_;
    };
    constexpr equivalent_pair equivalents[]{{"[::]", "[0:0:0:0:0:0:0:0]"}, {"[::1]", "[0:0:0:0:0:0:0:1]"}, {"[::1]", "[::0.0.0.1]"}, {"[1::]", "[1:0:0:0:0:0:0:0]"}, {"[1:2:3:4:5:6:7::]", "[1:2:3:4:5:6:7:0]"}, {"[1::2:3:4:5:6:7]", "[1:0:2:3:4:5:6:7]"}, {"[2001:db8:0:1::2:3]", "[2001:0DB8:0:1:0:0:2:3]"}, {"[::ffff:c000:201]", "[::ffff:192.0.2.1]"}, {"[1:2:3:4:5:6:c000:201]", "[1:2:3:4:5:6:192.0.2.1]"}};
    for (const auto& pair : equivalents) {
        for (const bool reverse : {false, true}) {
            const auto source = reverse ? pair.expanded_ : pair.compressed_;
            const auto destination = reverse ? pair.compressed_ : pair.expanded_;
            const auto origin = http_origin_view::http({.host_ = source});
            EXPECT_TRUE(classify_http_client_origin_authority(origin, destination) == http_client_origin_authority_status::same_origin);
            const auto location = std::string("http://") + std::string(destination) + "/next";
            check_resolved_target(origin, "/previous", location, "/next");
            EXPECT_TRUE(classify_http_client_origin_authority(origin, std::string(destination) + ":81") == http_client_origin_authority_status::different_origin);
        }
    }
    const auto origin = http_origin_view::http({.host_ = "[::ffff:192.0.2.1]"});
    for (const auto different : {"[::ffff:192.0.2.2]", "[::192.0.2.1]", "192.0.2.1", "[v1.future]"}) {
        EXPECT_TRUE(classify_http_client_origin_authority(origin, different) == http_client_origin_authority_status::different_origin);
    }
    check_cross_origin_redirect(origin, "/", "https://[::ffff:c000:201]/");
}

TEST(Ruvia, http_client_same_origin_redirect_resolves_uri_references) {
    const auto origin = http_origin_view::http({.host_ = "example.com"});
    constexpr std::string_view current = "/base/dir/page?old=1";

    check_resolved_target(origin, current, "/new/path", "/new/path");
    check_resolved_target(origin, current, "http://example.com/next", "/next");
    check_resolved_target(origin, current, "//example.com/rel", "/rel");
    check_resolved_target(origin, current, "http://EXA%6dPLE.com:/normalized", "/normalized");
    check_resolved_target(origin, current, "next", "/base/dir/next");
    check_resolved_target(origin, current, "../other/./item", "/base/other/item");
    check_resolved_target(origin, current, "?new=2", "/base/dir/page?new=2");
    check_resolved_target(origin, current, "#fragment", current);
    check_resolved_target(origin, current, "/next#part/one?x=%2F:@!$&'()*+,;=", "/next");
    check_resolved_target(origin, current, "", current);
    check_resolved_target(origin, current, "/a/../b#section", "/b");
}

TEST(Ruvia, http_client_same_origin_redirect_reports_rejection_reason) {
    const auto origin = http_origin_view::http({.host_ = "example.com"});

    check_cross_origin_redirect(origin, "/current", "http://evil.com/next");
    check_cross_origin_redirect(origin, "/current", "https://example.com/next");
    for (const std::string_view invalid : {"https://user@example.com/next", "https://example.com:99999/next", "http://user@example.com/next", "http://example.com:99999/next", "http:/broken", "/next#bad fragment", "/next#%zz", "/next#[bad]", "/next#first#second"}) {
        check_redirect_target_failure(origin, "/current", invalid, http_client_redirect_resolution_error::invalid_location);
    }
    check_redirect_target_failure(origin, "*", "/next", http_client_redirect_resolution_error::invalid_current_target);
}

TEST(Ruvia, http_client_redirect_validates_path_before_dot_segment_removal) {
    const auto origin = http_origin_view::http({.host_ = "example.com"});
    constexpr std::string_view invalid_locations[] = {"/bad space/../next", "/%/../next", "/%zz/../next", "/[bad]/../next", "/bad\\path/../next", "/bad\tpath/../next", "/bad\r\npath/../next", std::string_view("/bad\0path/../next", 17), "/bad\x7f/../next", "/caf\xc3\xa9/../next", "bad space/../next", "%zz/../next", "[bad]/../next", "http://example.com/%zz/../next", "//example.com/[bad]/../next"};
    for (const auto invalid : invalid_locations) {
        check_redirect_target_failure(origin, "/base/page", invalid, http_client_redirect_resolution_error::invalid_location);
    }
    for (const std::string_view valid : {"/bad%20space/../next", "/%25/../next", "/%5Bbad%5D/../next", "http://example.com/%25/../next", "//example.com/%25/../next"}) {
        check_resolved_target(origin, "/base/page", valid, "/next");
    }
}

TEST(Ruvia, http_client_redirect_empty_reference_path_preserves_base_path) {
    const auto origin = http_origin_view::http({.host_ = "example.com"});
    for (const std::string_view path : {"/a/./page", "/a/../page", "/a//.", "/a/.."}) {
        const std::string current = std::string(path) + "?old=1";
        for (const std::string_view location : {"", "#fragment"}) {
            check_resolved_target(origin, current, location, current);
        }
        check_resolved_target(origin, current, "?new=2", std::string(path) + "?new=2");
        check_resolved_target(origin, current, "?", std::string(path) + "?");
    }
    check_resolved_target(origin, "/a/../page?old=1", "//example.com", "/");
    check_resolved_target(origin, "/a/../page?old=1", "next", "/next");
}

TEST(Ruvia, http_client_same_origin_redirect_supports_ipvfuture) {
    const auto origin = http_origin_view::http({.host_ = "[v1.future]"});
    check_resolved_target(origin, "/current", "http://[V1.FUTURE]:/next", "/next");
}

TEST(Ruvia, http_client_redirect_relative_resolution_matches_rfc3986_examples) {
    const auto origin = http_origin_view::http({.host_ = "a"});
    constexpr std::string_view current = "/b/c/d;p?q";

    struct example final {
        std::string_view reference_;
        std::string_view target_;
    };
    constexpr example examples[] = {
        {"g", "/b/c/g"},
        {"./g", "/b/c/g"},
        {"g/", "/b/c/g/"},
        {"/g", "/g"},
        {"?y", "/b/c/d;p?y"},
        {"g?y", "/b/c/g?y"},
        {"#s", "/b/c/d;p?q"},
        {"g#s", "/b/c/g"},
        {";p", "/b/c/;p"},
        {"", "/b/c/d;p?q"},
        {".", "/b/c/"},
        {"./", "/b/c/"},
        {"..", "/b/"},
        {"../", "/b/"},
        {"../g", "/b/g"},
        {"../..", "/"},
        {"../../g", "/g"},
        {"../../../g", "/g"},
        {"/./g", "/g"},
        {"/../g", "/g"},
        {"g/./h", "/b/c/g/h"},
        {"g/../h", "/b/c/h"},
        {"g;x=1/../y", "/b/c/y"},
        {"/a//.", "/a//"},
        {"g//.", "/b/c/g//"},
        {"g?y/./x", "/b/c/g?y/./x"},
    };

    for (const auto& example : examples) {
        check_resolved_target(origin, current, example.reference_, example.target_);
    }
}

namespace {

using ruvia::http_client_redirect_resolution_error;
using ruvia::resolve_http_client_redirect_target;

struct expected_resolved_redirect final {
    http_scheme scheme_;
    std::string_view host_;
    std::uint16_t port_;
    std::string_view target_;
    bool cross_origin_;
};

void check_resolved_redirect(const http_origin_view& origin, std::string_view current_target, std::string_view location, const expected_resolved_redirect& expected) {
    const auto result_value = resolve_http_client_redirect_target(origin, {.current_target_ = current_target, .location_ = location});
    const auto* resolved = std::get_if<http_client_resolved_redirect>(&result_value);
    EXPECT_TRUE(resolved != nullptr);
    if (resolved != nullptr) {
        EXPECT_TRUE(resolved->scheme() == expected.scheme_);
        EXPECT_EQ(resolved->host(), expected.host_);
        EXPECT_EQ(resolved->port(), expected.port_);
        EXPECT_EQ(resolved->target(), expected.target_);
        EXPECT_EQ(resolved->cross_origin(), expected.cross_origin_);
    }
}

void check_redirect_resolution_failure(const http_origin_view& origin, std::string_view current_target, std::string_view location, http_client_redirect_resolution_error expected) {
    const auto result_value = resolve_http_client_redirect_target(origin, {.current_target_ = current_target, .location_ = location});
    const auto* failure = std::get_if<http_client_redirect_resolution_error>(&result_value);
    EXPECT_TRUE(failure != nullptr);
    if (failure != nullptr) {
        EXPECT_TRUE(*failure == expected);
    }
}

}  // namespace

TEST(Ruvia, http_client_redirect_resolution_same_origin_stays_relative) {
    const auto origin = origin_for("example.com", 80);
    check_resolved_redirect(origin, "/a/b?old=1", "c?x=1", {http_scheme::http, "example.com", 80, "/a/c?x=1", false});
    check_resolved_redirect(origin, "/a/b", "http://example.com/x", {http_scheme::http, "example.com", 80, "/x", false});
    // Explicit default port and a case-different host are the same origin.
    check_resolved_redirect(origin, "/a/b", "HTTP://EXAMPLE.COM:80/x", {http_scheme::http, "EXAMPLE.COM", 80, "/x", false});
}

TEST(Ruvia, http_client_redirect_resolution_classifies_cross_origin) {
    const auto origin = origin_for("example.com", 80);
    // Different host, default port for the located scheme.
    check_resolved_redirect(origin, "/a/b", "https://other.example/path?q=1", {http_scheme::https, "other.example", 443, "/path?q=1", true});
    // Same host, different port.
    check_resolved_redirect(origin, "/a/b", "http://example.com:8080/x", {http_scheme::http, "example.com", 8080, "/x", true});
    // Scheme change alone crosses the origin even on the same host.
    check_resolved_redirect(origin, "/a/b", "https://example.com/x", {http_scheme::https, "example.com", 443, "/x", true});
    // A protocol-relative reference keeps the scheme but moves authority.
    check_resolved_redirect(origin, "/a/b", "//other.example/p", {http_scheme::http, "other.example", 80, "/p", true});
    // Path normalization and fragment stripping apply across origins too.
    check_resolved_redirect(origin, "/a/b", "https://other.example/a/../b#frag", {http_scheme::https, "other.example", 443, "/b", true});
    // IPv6 literals keep their brackets, matching the http_origin_view contract.
    check_resolved_redirect(origin, "/a/b", "http://[::1]:8080/x", {http_scheme::http, "[::1]", 8080, "/x", true});
}

TEST(Ruvia, http_client_redirect_resolution_builds_borrowing_origin) {
    const auto origin = origin_for("example.com", 80);
    const auto result_value = resolve_http_client_redirect_target(origin, {.current_target_ = "/a/b", .location_ = "https://other.example:8443/x"});
    const auto* resolved = std::get_if<http_client_resolved_redirect>(&result_value);
    EXPECT_TRUE(resolved != nullptr);
    if (resolved != nullptr) {
        const auto next_origin = resolved->origin();
        EXPECT_TRUE(next_origin.scheme() == http_scheme::https);
        EXPECT_EQ(next_origin.host(), std::string_view("other.example"));
        EXPECT_EQ(next_origin.port(), std::uint16_t{8443});
    }
}

TEST(Ruvia, http_client_redirect_resolution_reports_typed_failures) {
    const auto origin = origin_for("example.com", 80);
    check_redirect_resolution_failure(origin, "/a/b", "ftp://example.com/file", http_client_redirect_resolution_error::unsupported_scheme);
    check_redirect_resolution_failure(origin, "/a/b", "mailto:someone@example.com", http_client_redirect_resolution_error::unsupported_scheme);
    // Userinfo remains rejected: RFC 9110 deprecates it and clients must not
    // leak credentials embedded by the peer.
    check_redirect_resolution_failure(origin, "/a/b", "https://user@other.example/", http_client_redirect_resolution_error::invalid_location);
    check_redirect_resolution_failure(origin, "/a/b", "http:opaque-without-authority", http_client_redirect_resolution_error::invalid_location);
    check_redirect_resolution_failure(origin, "not-a-target", "/x", http_client_redirect_resolution_error::invalid_current_target);
}

TEST(Ruvia, http_client_redirect_target_options_accept_named_owners_and_copy_into_resolved_redirect) {
    const auto origin = origin_for("example.com", 80);
    std::string current_target("/a/b");
    std::string location("https://other.example:8443/next");
    const auto result_value = resolve_http_client_redirect_target(origin, {.current_target_ = current_target, .location_ = location});
    current_target.assign("/mutated");
    location.assign("https://wrong.example/");
    const auto* resolved = std::get_if<http_client_resolved_redirect>(&result_value);
    EXPECT_TRUE(resolved != nullptr);
    if (resolved != nullptr) {
        EXPECT_EQ(resolved->host(), std::string_view("other.example"));
        EXPECT_EQ(resolved->target(), std::string_view("/next"));
    }
}

namespace {

ruvia::http1_request_content_writer chunked_request_writer() {
    std::array<char, 1024> head{};
    auto prepared = ruvia::http1_client_request_writer{}.prepare_streaming(ruvia::http_origin_view::http({.host_ = "example.com"}), {.method_ = "POST"}, head);
    const auto* prepared_req = std::get_if<ruvia::prepared_http1_client_request>(&prepared);
    if (prepared_req == nullptr || prepared_req->content_plan().streaming() == nullptr) {
        throw std::logic_error("cannot prepare test request content");
    }
    return ruvia::http1_request_content_writer(*prepared_req->content_plan().streaming());
}

}  // namespace

TEST(Ruvia, http1_streaming_request_chunked_upload_and_trailers) {
    ruvia::http1_client_request_writer head_writer;
    std::array<char, 4096> head{};
    auto prepared = head_writer.prepare_streaming(ruvia::http_origin_view::https({.host_ = "example.com"}), {.method_ = "POST"}, head);
    const auto* prepared_req = std::get_if<ruvia::prepared_http1_client_request>(&prepared);
    EXPECT_TRUE(prepared_req != nullptr);
    auto* plan = prepared_req->content_plan().streaming();
    EXPECT_TRUE(plan != nullptr);
    EXPECT_EQ(prepared_req->head(), std::string_view("POST / HTTP/1.1\r\nHost: example.com\r\nTransfer-Encoding: chunked\r\n\r\n"));
    ruvia::http1_request_content_writer writer(*plan);
    const std::string payload_value = "hello";
    auto chunk = writer.plan_chunk(payload_value);
    EXPECT_TRUE((chunk.index() == 0));
    EXPECT_EQ(std::get<0>(chunk).prefix(), std::string_view("5\r\n"));
    EXPECT_EQ(std::get<0>(chunk).suffix(), std::string_view("\r\n"));
    writer.commit_chunk();
    std::array<char, 128> finish_value{};
    const std::array trailers{ruvia::http_header_view{"Digest", "value"}};
    auto final = writer.plan_finish(finish_value, trailers);
    EXPECT_TRUE((final.index() == 0));
    EXPECT_EQ(std::get<0>(final), std::string_view("0\r\nDigest: value\r\n\r\n"));
    EXPECT_TRUE(!(writer.plan_chunk(payload_value).index() == 0));
    writer.commit_finish();
    EXPECT_TRUE(writer.finished());
}

// RFC 9110 section 6.6.2: a sender that will generate trailer fields SHOULD
// declare them in a Trailer field; only chunked content carries trailers.
TEST(Ruvia, http1_streaming_request_declares_trailer_only_for_chunked_content) {
    const std::array declared{ruvia::http_header_view{"Trailer", "Digest"}};
    std::array<char, 1024> head{};
    auto prepared = ruvia::http1_client_request_writer{}.prepare_streaming(ruvia::http_origin_view::http({.host_ = "example.com"}), {.method_ = "POST", .headers_ = declared}, head);
    const auto* prepared_req = std::get_if<ruvia::prepared_http1_client_request>(&prepared);
    EXPECT_TRUE(prepared_req != nullptr);
    if (prepared_req != nullptr) {
        EXPECT_EQ(prepared_req->head(),
            std::string_view("POST / HTTP/1.1\r\nHost: example.com\r\nTrailer: Digest\r\n"
                             "Transfer-Encoding: chunked\r\n\r\n"));
        ruvia::http1_request_content_writer writer(*prepared_req->content_plan().streaming());
        std::array<char, 128> finish_value{};
        const std::array trailers{ruvia::http_header_view{"Digest", "value"}};
        auto final = writer.plan_finish(finish_value, trailers);
        EXPECT_TRUE((final.index() == 0));
    }

    auto known_length = ruvia::http1_client_request_writer{}.prepare_streaming(ruvia::http_origin_view::http({.host_ = "example.com"}), {.method_ = "POST", .headers_ = declared, .content_length_ = 3}, head);
    const auto* failure = std::get_if<ruvia::http1_client_request_prepare_error>(&known_length);
    EXPECT_TRUE(failure != nullptr && *failure == ruvia::http1_client_request_prepare_error::trailer_requires_chunked_content);

    const std::array forbidden{ruvia::http_header_view{"Trailer", "Content-Length"}};
    auto rejected = ruvia::http1_client_request_writer{}.prepare_streaming(ruvia::http_origin_view::http({.host_ = "example.com"}), {.method_ = "POST", .headers_ = forbidden}, head);
    const auto* invalid = std::get_if<ruvia::http1_client_request_prepare_error>(&rejected);
    EXPECT_TRUE(invalid != nullptr && *invalid == ruvia::http1_client_request_prepare_error::invalid_header);
}
TEST(Ruvia, http1_streaming_request_known_length_and_continue_gate) {
    ruvia::http1_client_request_writer head_writer;
    std::array<char, 4096> head{};
    auto prepared = head_writer.prepare_streaming(ruvia::http_origin_view::http({.host_ = "example.com"}), {.method_ = "PUT", .content_length_ = 3}, head, {.expectation_ = ruvia::http_client_request_expectation::continue_value});
    const auto* prepared_req = std::get_if<ruvia::prepared_http1_client_request>(&prepared);
    EXPECT_TRUE(prepared_req != nullptr);
    EXPECT_TRUE((prepared_req->head().find("Content-Length: 3\r\n") != std::string_view::npos));
    ruvia::http1_request_content_writer writer(*prepared_req->content_plan().streaming());
    const std::string payload_value = "abc";
    EXPECT_TRUE(!(writer.plan_chunk(payload_value).index() == 0));
    writer.release_content();
    EXPECT_TRUE(!(writer.plan_finish({}).index() == 0));
    auto chunk = writer.plan_chunk(payload_value);
    EXPECT_TRUE((chunk.index() == 0));
    EXPECT_TRUE(std::get<0>(chunk).prefix().empty());
    EXPECT_TRUE(std::get<0>(chunk).suffix().empty());
    writer.commit_chunk();
    EXPECT_TRUE(!(writer.plan_chunk(payload_value).index() == 0));
    auto finish_value = writer.plan_finish({});
    EXPECT_TRUE((finish_value.index() == 0));
    EXPECT_TRUE(std::get<0>(finish_value).empty());
    writer.commit_finish();
    EXPECT_TRUE(ruvia::test::throws_on([&] {
        writer.commit_chunk();
    }));
    EXPECT_TRUE(ruvia::test::throws_on([&] {
        writer.commit_finish();
    }));
}
TEST(Ruvia, http1_streaming_request_rejects_forbidden_trailer_without_mutating_buffer) {
    std::array<char, 1024> head{};
    auto prepared = ruvia::http1_client_request_writer{}.prepare_streaming(ruvia::http_origin_view::http({.host_ = "example.com"}), {.method_ = "POST"}, head);
    ruvia::http1_request_content_writer writer(*std::get<0>(prepared).content_plan().streaming());
    std::array<char, 128> finish_value{};
    finish_value.fill('X');
    const std::array trailers{ruvia::http_header_view{"Content-Length", "0"}};
    auto rejected = writer.plan_finish(finish_value, trailers);
    EXPECT_TRUE(!(rejected.index() == 0));
    EXPECT_EQ(finish_value.front(), 'X');
    EXPECT_TRUE((writer.plan_finish(finish_value).index() == 0));
}

TEST(Ruvia, http1_streaming_request_finish_accepts_trailers_borrowed_from_output) {
    enum class alias_kind { name,
        value,
        disjoint,
        overlapping,
        cross_field };
    for (const auto kind : {alias_kind::name, alias_kind::value, alias_kind::disjoint, alias_kind::overlapping, alias_kind::cross_field}) {
        auto writer = chunked_request_writer();
        std::array<char, 128> buffer{};
        buffer.fill('#');
        std::array<ruvia::http_header_view, 2> fields{};
        std::size_t count = 1;
        std::string expected = "0\r\nDigest: value\r\n\r\n";
        const auto copy = [&](std::size_t offset, std::string_view text) {
            std::copy(text.begin(), text.end(), buffer.begin() + static_cast<std::ptrdiff_t>(offset));
            return std::string_view(buffer.data() + offset, text.size());
        };
        switch (kind) {
            case alias_kind::name:
                fields[0] = {copy(0, "Digest"), "value"};
                break;
            case alias_kind::value:
                fields[0] = {"Digest", copy(0, "value")};
                break;
            case alias_kind::disjoint:
                fields[0] = {copy(0, "Digest"), copy(16, "value")};
                break;
            case alias_kind::overlapping: {
                const auto text = copy(0, "DigestValue");
                fields[0] = {text.substr(0, 6), text.substr(3)};
                expected = "0\r\nDigest: estValue\r\n\r\n";
                break;
            }
            case alias_kind::cross_field:
                fields[0] = {"X-First", copy(32, "first")};
                fields[1] = {copy(0, "Digest"), "second"};
                count = 2;
                expected = "0\r\nX-First: first\r\nDigest: second\r\n\r\n";
                break;
        }
        const auto trailers = std::span(fields).first(count);
        const auto original = buffer;
        const auto too_small = writer.plan_finish(std::span(buffer).first(expected.size() - 1), trailers);
        EXPECT_TRUE(too_small.index() == 1);
        if (too_small.index() == 1) {
            EXPECT_TRUE(std::get<1>(too_small) == ruvia::http1_request_content_write_error::output_too_small);
        }
        EXPECT_TRUE(buffer == original);
        EXPECT_TRUE(!writer.finished());

        const auto planned = writer.plan_finish(buffer, trailers);
        EXPECT_TRUE(planned.index() == 0);
        if (planned.index() == 0) {
            EXPECT_EQ(std::get<0>(planned), std::string_view(expected));
        }
        EXPECT_EQ(std::string_view(buffer.data() + expected.size(), buffer.size() - expected.size()), std::string_view(original.data() + expected.size(), original.size() - expected.size()));
        const auto repeated = writer.plan_finish(buffer, trailers);
        EXPECT_TRUE(repeated.index() == 1);
        if (repeated.index() == 1) {
            EXPECT_TRUE(std::get<1>(repeated) == ruvia::http1_request_content_write_error::write_pending);
        }
        writer.commit_finish();
        EXPECT_TRUE(writer.finished());
        const auto finished = writer.plan_finish(buffer);
        EXPECT_TRUE(finished.index() == 1);
        if (finished.index() == 1) {
            EXPECT_TRUE(std::get<1>(finished) == ruvia::http1_request_content_write_error::stopped);
        }
    }
}

TEST(Ruvia, http1_streaming_request_finish_accepts_output_alias_at_header_byte_limit) {
    auto writer = chunked_request_writer();
    constexpr std::size_t framing_bytes = std::string_view("0\r\nDigest: \r\n\r\n").size();
    const std::string value(ruvia::max_http_header_bytes - framing_bytes, 'v');
    std::string buffer(ruvia::max_http_header_bytes, 'v');
    const std::array trailers{ruvia::http_header_view{"Digest", std::string_view(buffer).substr(0, value.size())}};
    const auto planned = writer.plan_finish(std::span(buffer.data(), buffer.size()), trailers);
    EXPECT_TRUE(planned.index() == 0);
    if (planned.index() == 0) {
        const std::string expected = "0\r\nDigest: " + value + "\r\n\r\n";
        EXPECT_EQ(std::get<0>(planned), std::string_view(expected));
    }
    writer.commit_finish();
    EXPECT_TRUE(writer.finished());
}

TEST(Ruvia, http1_streaming_request_finish_accepts_trailer_descriptors_stored_in_output) {
    auto writer = chunked_request_writer();
    // The descriptor array itself lives where the trailer section is written, so
    // every descriptor must be consumed before the first output byte lands.
    alignas(ruvia::http_header_view) std::array<char, 256> buffer{};
    auto* const descriptors = ::new (static_cast<void*>(buffer.data())) ruvia::http_header_view[2]{{"X-One", "first"}, {"X-Two", "second"}};
    const std::span<const ruvia::http_header_view> trailers(descriptors, 2);
    const auto planned = writer.plan_finish(std::span(buffer.data(), buffer.size()), trailers);
    EXPECT_TRUE(planned.index() == 0);
    if (planned.index() == 0) {
        EXPECT_EQ(std::get<0>(planned), std::string_view("0\r\nX-One: first\r\nX-Two: second\r\n\r\n"));
    }
    writer.commit_finish();
}

TEST(Ruvia, http_content_length_rejects_negative_sources_without_mutating_existing_options) {
    ruvia::http1_client_request_head_view request{.method_ = "POST", .target_ = "/upload", .content_length_ = 3};
    for (const auto invalid : {std::numeric_limits<std::int64_t>::min(), std::int64_t{-1}}) {
        EXPECT_TRUE(!ruvia::http_content_length::try_from_value(invalid).has_value());
        EXPECT_TRUE(ruvia::test::throws_on([&] {
            request.content_length_ = invalid;
        }));
        EXPECT_TRUE(ruvia::test::throws_on([&] {
            (void)ruvia::http_request_content_plan::known_length(invalid);
        }));
        EXPECT_TRUE(ruvia::test::throws_on([&] {
            (void)ruvia::http_request_content_plan::streaming(invalid);
        }));
    }
    std::array<char, 256> head{};
    const auto prepared = ruvia::http1_client_request_writer{}.prepare_streaming(ruvia::http_origin_view::http({.host_ = "example.com"}), request, head);
    const auto* retained = std::get_if<ruvia::prepared_http1_client_request>(&prepared);
    EXPECT_TRUE(retained != nullptr && retained->content_plan().streaming() != nullptr);
    if (retained && retained->content_plan().streaming()) {
        EXPECT_TRUE(retained->content_plan().streaming()->content_length() == 3);
        EXPECT_TRUE(retained->head().find("Content-Length: 3\r\n") != std::string_view::npos);
    }
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    const auto h2_known = ruvia::http_request_content_plan::known_length(maximum);
    const auto h2_streaming = ruvia::http_request_content_plan::streaming(maximum);
    EXPECT_TRUE(h2_known.known_length_content() && h2_known.known_length_content()->length() == maximum);
    EXPECT_TRUE(h2_streaming.streaming_content() && h2_streaming.streaming_content()->expected_length() == maximum);
}

TEST(Ruvia, http1_chunk_header_encodes_lowercase_hex_and_crlf) {
    const ruvia::http1_chunk_header zero(0);
    const ruvia::http1_chunk_header fifteen(15);
    const ruvia::http1_chunk_header sixteen(16);
    const ruvia::http1_chunk_header abc(0xabc);
    EXPECT_EQ(zero.view(), std::string_view("0\r\n"));
    EXPECT_EQ(fifteen.view(), std::string_view("f\r\n"));
    EXPECT_EQ(sixteen.view(), std::string_view("10\r\n"));
    EXPECT_EQ(abc.view(), std::string_view("abc\r\n"));
}

TEST(Ruvia, http1_chunk_header_buffer_covers_size_t_max) {
    const ruvia::http1_chunk_header header_value((std::numeric_limits<std::size_t>::max)());
    const auto encoded = header_value.view();
    EXPECT_EQ(encoded.size(), sizeof(std::size_t) * 2 + 2);
    EXPECT_TRUE(encoded.ends_with("\r\n"));
}
