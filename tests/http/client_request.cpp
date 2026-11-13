#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <initializer_list>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ruvia/http/http_client.h"
#include "ruvia/http/http_content_coding.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_request.h"
#include "ruvia/http/http_response.h"

#include "content_decoding_fixture.h"
#include "test_exceptions.h"

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
    std::variant<prepared_http1_client_request, http1_client_request_buffer_too_small,
        http1_client_request_prepare_error>
        result_;

    prepared_fixture(const http_origin_view& origin, const http_client_request_view& request,
        http1_client_request_wire_policy policy = http1_client_request_wire_policy{})
        : result_(http1_client_request_writer().prepare(origin, request, buffer_, policy)) {}
};

[[nodiscard]] http1_client_request_prepare_error prepare_error(const http_client_request_view& request,
    http1_client_request_wire_policy policy = http1_client_request_wire_policy{}) {
    std::array<char, 512> buffer;
    const auto result_value = http1_client_request_writer().prepare(
        http_origin_view::https({.host_ = "example.test"}), request, buffer, policy);
    const auto* failure = std::get_if<http1_client_request_prepare_error>(&result_value);
    if (failure == nullptr) {
        throw std::runtime_error("test expected request preparation to fail");
    }
    return *failure;
}

auto prepare_field_request(unsigned entry, std::span<const ruvia::http_header_view> fields,
    std::span<char> output, ruvia::http_request_field_source source) {
    const auto origin = http_origin_view::https({.host_ = "example.test"});
    const http1_client_request_wire_policy policy{.field_source_ = source};
    const http1_client_request_writer writer;
    switch (entry) {
        case 0:
            return writer.prepare(origin, http_client_request_view{.headers_ = fields}, output, policy);
        case 1:
            return writer.prepare_streaming(origin,
                ruvia::http1_client_request_head_view{.method_ = "POST", .headers_ = fields, .content_length_ = 0},
                output, policy);
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

    prepared_fixture fixture_value(http_origin_view::https({.host_ = "example.test"}), request,
        http1_client_request_wire_policy{
            .expectation_ = ruvia::http_client_request_expectation::continue_value});
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
    const std::array<std::byte, 7> raw_bytes{std::byte{'p'}, std::byte{'a'}, std::byte{'y'},
        std::byte{'l'}, std::byte{'o'}, std::byte{'a'}, std::byte{'d'}};
    const auto byte_content =
        http_client_request_content_view::bytes(std::span<const std::byte>(raw_bytes));
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
    prepared_fixture extension_fixture(
        http_origin_view::https({.host_ = "example.test", .port_ = 8443}), extension);
    const auto* extension_prepared = std::get_if<prepared_http1_client_request>(&extension_fixture.result_);
    EXPECT_TRUE(extension_prepared != nullptr);
    if (extension_prepared != nullptr) {
        EXPECT_TRUE(extension_prepared->head().starts_with(
            "PROPFIND /dav HTTP/1.1\r\nHost: example.test:8443\r\n"));
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
    const auto result_value = http1_client_request_writer().prepare_connect(
        http_origin_view::https({.host_ = "example.test"}), {}, buffer);
    const auto* prepared = std::get_if<prepared_http1_client_request>(&result_value);
    EXPECT_TRUE(prepared != nullptr);
    if (prepared != nullptr) {
        EXPECT_TRUE(prepared->head() ==
                    "CONNECT example.test:443 HTTP/1.1\r\n"
                    "Host: example.test:443\r\n\r\n");
        EXPECT_TRUE(prepared->content_plan().without_content() != nullptr);
    }

    std::array<char, 512> ipv6_buffer;
    const auto ipv6 = http1_client_request_writer().prepare_connect(
        http_origin_view::https({.host_ = "[::1]"}), {}, ipv6_buffer);
    const auto* ipv6_prepared = std::get_if<prepared_http1_client_request>(&ipv6);
    EXPECT_TRUE(ipv6_prepared != nullptr);
    if (ipv6_prepared != nullptr) {
        EXPECT_TRUE(ipv6_prepared->head().starts_with("CONNECT [::1]:443 HTTP/1.1\r\nHost: [::1]:443\r\n"));
    }

    std::array<char, 128> invalid_buffer;
    const auto invalid = http1_client_request_writer().prepare_connect(
        http_origin_view::https({.host_ = "example.test", .port_ = 0}), {}, invalid_buffer);
    const auto* invalid_failure = std::get_if<http1_client_request_prepare_error>(&invalid);
    EXPECT_TRUE(invalid_failure != nullptr);
    EXPECT_TRUE(*invalid_failure == http1_client_request_prepare_error::invalid_connect_origin);

    const auto invalid_authority = http1_client_request_writer().prepare_connect("example.test:0", {}, invalid_buffer);
    const auto* invalid_authority_failure = std::get_if<http1_client_request_prepare_error>(&invalid_authority);
    EXPECT_TRUE(invalid_authority_failure != nullptr &&
                *invalid_authority_failure == http1_client_request_prepare_error::invalid_connect_origin);
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
        {"Transfer-Encoding", "chunked",
            http1_client_request_prepare_error::transfer_encoding_unsupported},
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
        {.headers_ = {ruvia::http_header_view("Connection", "close,"), {}},
            .count_ = 1,
            .error_ = http1_client_request_prepare_error::invalid_connection},
        {.headers_ = {ruvia::http_header_view("Connection", "close;parameter"), {}},
            .count_ = 1,
            .error_ = http1_client_request_prepare_error::invalid_connection},
        {.headers_ = {ruvia::http_header_view("Upgrade", "websocket"), {}},
            .count_ = 1,
            .error_ = http1_client_request_prepare_error::upgrade_connection_option_required},
        {.headers_ = {ruvia::http_header_view("Connection", "Upgrade"),
             ruvia::http_header_view("Upgrade", "websocket/")},
            .count_ = 2,
            .error_ = http1_client_request_prepare_error::invalid_upgrade},
        {.headers_ = {ruvia::http_header_view("TE", "trailers"), {}},
            .count_ = 1,
            .error_ = http1_client_request_prepare_error::te_connection_option_required},
    };
    for (const auto& test : failures) {
        http_client_request_view request;
        request.headers_ = std::span<const ruvia::http_header_view>(test.headers_.data(), test.count_);
        EXPECT_TRUE(prepare_error(request) == test.error_);
    }

    for (const std::string_view connection_options :
        {"Host", "close, content-length", "EXPECT, keep-alive", "Cache-Control", "Trailer",
            "Authorization", "Cookie", "Range", "Transfer-Encoding", "close, transfer-encoding",
            "Forwarded", "close, X-Forwarded-For", "x-forwarded-proto", "keep-alive, X-FORWARDED-HOST"}) {
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
    const auto prepare_with_te =
        [](std::string_view value) -> std::optional<http1_client_request_prepare_error> {
        const std::array headers{
            ruvia::http_header_view("Connection", "TE"),
            ruvia::http_header_view("TE", value),
        };
        http_client_request_view request;
        request.headers_ = headers;
        std::array<char, 512> buffer;
        const auto result_value = http1_client_request_writer().prepare(
            http_origin_view::https({.host_ = "example.test"}), request, buffer);
        if (const auto* failure = std::get_if<http1_client_request_prepare_error>(&result_value)) {
            return *failure;
        }
        EXPECT_TRUE(std::holds_alternative<prepared_http1_client_request>(result_value));
        return std::nullopt;
    };

    for (const std::string_view valid : {"", "trailers", "gzip", "deflate;q=0.5", "deflate;Q=0.5",
             "x-gzip ; q=1.000", "gzip;q=0, trailers", "trailers , TRAILERS"}) {
        EXPECT_TRUE(!prepare_with_te(valid).has_value());
    }

    for (const std::string_view invalid : {",trailers", "trailers,", "trailers,,gzip", "chunked",
             "br", "trailers;q=0.5", "gzip;q=1.001", "gzip;q=\"0.5\"", "gzip;q =0.5", "gzip;q= 0.5",
             "gzip; q = 0.5", "gzip;level=1", "gzip;q=0.5;level=1", "gzip; q", "gzip;q="}) {
        EXPECT_TRUE(prepare_with_te(invalid) == http1_client_request_prepare_error::invalid_header);
    }
}

TEST(Ruvia, http1_client_request_writer_enforces_expect_content_semantics) {
    const ruvia::http_header_view expect("Expect", "100-Continue");
    http_client_request_view raw_expectation;
    raw_expectation.method_ = "POST";
    raw_expectation.headers_ = std::span<const ruvia::http_header_view>(&expect, 1);
    raw_expectation.content_ = http_client_request_content_view::bytes("x");
    EXPECT_TRUE(prepare_error(raw_expectation) ==
                http1_client_request_prepare_error::expect_header_managed_by_writer);

    http_client_request_view absent;
    absent.method_ = "POST";
    prepared_fixture absent_fixture(http_origin_view::https({.host_ = "example.test"}), absent,
        http1_client_request_wire_policy{
            .expectation_ = ruvia::http_client_request_expectation::continue_value});
    const auto* absent_failure = std::get_if<http1_client_request_prepare_error>(&absent_fixture.result_);
    EXPECT_TRUE(absent_failure != nullptr);
    if (absent_failure != nullptr) {
        EXPECT_TRUE(*absent_failure ==
                    http1_client_request_prepare_error::expectation_without_content);
    }

    http_client_request_view empty = absent;
    empty.content_ = http_client_request_content_view::bytes("");
    prepared_fixture empty_fixture(http_origin_view::https({.host_ = "example.test"}), empty,
        http1_client_request_wire_policy{
            .expectation_ = ruvia::http_client_request_expectation::continue_value});
    const auto* empty_failure = std::get_if<http1_client_request_prepare_error>(&empty_fixture.result_);
    EXPECT_TRUE(empty_failure != nullptr);
    if (empty_failure != nullptr) {
        EXPECT_TRUE(*empty_failure ==
                    http1_client_request_prepare_error::expectation_without_content);
    }

    http_client_request_view nonempty;
    nonempty.method_ = "POST";
    nonempty.content_ = http_client_request_content_view::bytes("x");
    prepared_fixture fixture_value(http_origin_view::https({.host_ = "example.test"}), nonempty,
        http1_client_request_wire_policy{
            .expectation_ = ruvia::http_client_request_expectation::continue_value});
    const auto* prepared = std::get_if<prepared_http1_client_request>(&fixture_value.result_);
    EXPECT_TRUE(prepared != nullptr);
    if (prepared != nullptr) {
        EXPECT_TRUE(prepared->content_plan().continue_gated() != nullptr);
        EXPECT_TRUE(prepared->head().find("Expect: 100-continue\r\n") !=
                    std::string_view::npos);
    }
}

TEST(Ruvia, http1_client_request_writer_rejects_invalid_wire_policy) {
    http_client_request_view request;
    const auto invalid_without_expectation =
        http1_client_request_wire_policy{.close_policy_ = static_cast<http1_close_policy>(0xFF)};
    const auto invalid_expect_continue =
        http1_client_request_wire_policy{.close_policy_ = static_cast<http1_close_policy>(0xFF),
            .expectation_ = ruvia::http_client_request_expectation::continue_value};
    const auto invalid_expectation =
        http1_client_request_wire_policy{
            .expectation_ = static_cast<ruvia::http_client_request_expectation>(0xFF)};
    EXPECT_TRUE(prepare_error(request, invalid_without_expectation) ==
                http1_client_request_prepare_error::invalid_close_policy);
    EXPECT_TRUE(prepare_error(request, invalid_expect_continue) ==
                http1_client_request_prepare_error::invalid_close_policy);
    EXPECT_TRUE(prepare_error(request, invalid_expectation) ==
                http1_client_request_prepare_error::invalid_expectation);

    std::array<char, 512> buffer{};
    const auto connect =
        http1_client_request_writer().prepare_connect(http_origin_view::https({.host_ = "example.test"}),
            std::span<const ruvia::http_header_view>{}, buffer, invalid_without_expectation);
    const auto* connect_failure = std::get_if<http1_client_request_prepare_error>(&connect);
    EXPECT_TRUE(connect_failure != nullptr);
    if (connect_failure != nullptr) {
        EXPECT_TRUE(*connect_failure == http1_client_request_prepare_error::invalid_close_policy);
    }

    const auto invalid_connect_expectation =
        http1_client_request_writer().prepare_connect(http_origin_view::https({.host_ = "example.test"}),
            std::span<const ruvia::http_header_view>{}, buffer, invalid_expectation);
    const auto* invalid_connect_expectation_failure =
        std::get_if<http1_client_request_prepare_error>(&invalid_connect_expectation);
    EXPECT_TRUE(invalid_connect_expectation_failure != nullptr);
    if (invalid_connect_expectation_failure != nullptr) {
        EXPECT_TRUE(*invalid_connect_expectation_failure ==
                    http1_client_request_prepare_error::invalid_expectation);
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

    const prepared_fixture explicit_port(
        http_origin_view::https({.host_ = "[::1]", .port_ = 8443}), request, absolute);
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
    const auto streaming = http1_client_request_writer().prepare_streaming(
        http_origin_view::https({.host_ = "example.test"}),
        ruvia::http1_client_request_head_view{.method_ = "POST", .target_ = "/upload", .content_length_ = 3}, buffer,
        absolute);
    const auto* streamed = std::get_if<prepared_http1_client_request>(&streaming);
    ASSERT_TRUE(streamed != nullptr);
    EXPECT_EQ(streamed->head(),
        "POST https://example.test/upload HTTP/1.1\r\nHost: example.test\r\nContent-Length: 3\r\n\r\n");

    // The head budget counts the scheme and authority of the target.
    std::array<char, 40> small{};
    const auto too_small = http1_client_request_writer().prepare(
        http_origin_view::http({.host_ = "example.test"}), request, small, absolute);
    const auto* required = std::get_if<http1_client_request_buffer_too_small>(&too_small);
    ASSERT_TRUE(required != nullptr);
    EXPECT_EQ(required->required_head_bytes(), head_of(default_port).size());
}

TEST(Ruvia, http1_client_request_writer_rejects_invalid_target_forms) {
    EXPECT_EQ(prepare_error(http_client_request_view{},
                  http1_client_request_wire_policy{.target_form_ = static_cast<ruvia::http1_request_target_form>(0xFF)}),
        http1_client_request_prepare_error::invalid_target_form);
    // CONNECT is authority-form only (RFC 9112 section 3.2.3).
    const http1_client_request_wire_policy absolute{.target_form_ = ruvia::http1_request_target_form::absolute};
    std::array<char, 512> buffer{};
    const auto by_origin = http1_client_request_writer().prepare_connect(
        http_origin_view::https({.host_ = "example.test"}), std::span<const ruvia::http_header_view>{}, buffer, absolute);
    const auto by_authority = http1_client_request_writer().prepare_connect(
        "example.test:443", std::span<const ruvia::http_header_view>{}, buffer, absolute);
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
        const auto result = http1_client_request_writer().prepare_streaming(
            http_origin_view::http({.host_ = "example.test"}), streaming, head_buffer);
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
    for (const std::string_view value :
        {"text/plain; charset", "text/plain; charset=", "text/plain; charset =utf-8",
            "text/plain; charset=utf-8; CHARSET=latin1", "text/plain; charset=\"unterminated"}) {
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
    const auto too_small = http1_client_request_writer().prepare(
        http_origin_view::https({.host_ = "example.test"}), request, small);
    const auto* too_small_val = std::get_if<http1_client_request_buffer_too_small>(&too_small);
    EXPECT_TRUE(too_small_val != nullptr);
    EXPECT_TRUE(std::ranges::all_of(small, [](char value) { return value == 'z'; }));

    std::array<char, 512> enough;
    const auto prepared = http1_client_request_writer().prepare(
        http_origin_view::https({.host_ = "example.test"}), request, enough);
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
    const auto many_prepared = http1_client_request_writer().prepare(
        http_origin_view::https({.host_ = "example.test"}), many, many_output);
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
    const auto invalid = http1_client_request_writer().prepare(
        http_origin_view::https({.host_ = "example.test"}), invalid_method, untouched);
    const auto* invalid_failure = std::get_if<http1_client_request_prepare_error>(&invalid);
    EXPECT_TRUE(invalid_failure != nullptr);
    EXPECT_TRUE(*invalid_failure == http1_client_request_prepare_error::invalid_method);
    EXPECT_TRUE(std::ranges::all_of(untouched, [](char value) { return value == 'q'; }));
    EXPECT_TRUE(!ruvia::http1_client_request_prepare_error_message(
        http1_client_request_prepare_error::invalid_method)
            .empty());
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
    prepared_fixture generated_close(http_origin_view::https({.host_ = "example.test"}),
        generated_request,
        http1_client_request_wire_policy{.close_policy_ = http1_close_policy::close_after_response});
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

    const auto expect_error = [&](std::string_view target, std::span<const ruvia::http_header_view> headers,
                                  http1_client_request_prepare_error expected) {
        const auto result = http1_client_request_writer().prepare_connect_udp(origin, target, headers, untouched);
        const auto* failure = std::get_if<http1_client_request_prepare_error>(&result);
        EXPECT_TRUE(failure != nullptr);
        if (failure != nullptr) {
            EXPECT_TRUE(*failure == expected);
        }
        EXPECT_TRUE(std::ranges::all_of(untouched, [](char c) { return c == 'z'; }));
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
    const std::array cases{
        ruvia::http_header_view{"Content-Type", "not-a-media-type"},
        ruvia::http_header_view{"Origin", "https://example.test/path"},
        ruvia::http_header_view{"Access-Control-Request-Method", "GET, POST"},
        ruvia::http_header_view{"Access-Control-Request-Headers", "X-Good, Bad Header"},
        ruvia::http_header_view{"Content-Encoding", ""},
        ruvia::http_header_view{"Content-Encoding", ",,"},
        ruvia::http_header_view{"Content-Encoding", ", gzip,, br,"},
        ruvia::http_header_view{"Content-Encoding", ", unknown-coding,,"}};
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
            EXPECT_TRUE(std::ranges::all_of(output, [](char c) { return c == 'z'; }));
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
            EXPECT_TRUE(upstream.request().header("Host") ==
                        std::optional<std::string_view>{entry < 2 ? "example.test" : "example.test:443"});
        }
    }
}

TEST(Ruvia, http1_client_forwarding_retains_security_ownership_and_singleton_rules) {
    const std::array invalid{
        ruvia::http_header_view{"X-Test", "ok\r\nInjected: yes"},
        ruvia::http_header_view{"Bad Name", "value"},
        ruvia::http_header_view{"Host", "example.test"},
        ruvia::http_header_view{"Content-Length", "0"},
        ruvia::http_header_view{"Expect", "100-continue"},
        ruvia::http_header_view{"Transfer-Encoding", "chunked"},
        ruvia::http_header_view{"Content-Encoding", ", gzip;level=9,"},
        ruvia::http_header_view{"Content-Encoding", ", bad coding,"},
        ruvia::http_header_view{"Connection", "Content-Type"},
        ruvia::http_header_view{"Connection", "X-Forwarded-For"},
        ruvia::http_header_view{"Upgrade", ",websocket,"},
        ruvia::http_header_view{"TE", ",trailers,"}};
    for (unsigned entry = 0; entry != 4; ++entry) {
        std::array<char, 1024> output;
        for (const auto& field : invalid) {
            output.fill('z');
            const std::array fields{field};
            const auto result = prepare_field_request(entry, fields, output, ruvia::http_request_field_source::forwarded);
            EXPECT_TRUE(std::holds_alternative<http1_client_request_prepare_error>(result));
            EXPECT_TRUE(std::ranges::all_of(output, [](char c) { return c == 'z'; }));
        }
        for (const auto& field : std::array{
                 ruvia::http_header_view{"Content-Type", "invalid"},
                 ruvia::http_header_view{"Origin", "invalid"},
                 ruvia::http_header_view{"Access-Control-Request-Method", "GET, POST"},
                 ruvia::http_header_view{"Authorization", "Bearer token"},
                 ruvia::http_header_view{"User-Agent", "agent"},
                 ruvia::http_header_view{"If-Modified-Since", "invalid"},
                 ruvia::http_header_view{"If-Unmodified-Since", "invalid"}}) {
            const std::array fields{field, field};
            const auto result = prepare_field_request(entry, fields, output, ruvia::http_request_field_source::forwarded);
            EXPECT_TRUE(std::holds_alternative<http1_client_request_prepare_error>(result));
        }
        const auto invalid_source = prepare_field_request(entry, {}, output,
            static_cast<ruvia::http_request_field_source>(0xff));
        EXPECT_TRUE(std::holds_alternative<http1_client_request_prepare_error>(invalid_source));
    }
}

TEST(Ruvia, http1_client_forwarded_upgrade_empty_members_preserve_protocol_transition) {
    constexpr std::string_view wire =
        "GET / HTTP/1.1\r\nHost: example.test\r\nConnection: Upgrade\r\nUpgrade: , websocket,,\r\n\r\n";
    ruvia::http1_server_request_parse_state incoming;
    incoming.parse_head(wire);
    EXPECT_TRUE(incoming.head_ready() != nullptr);
    const auto upgrade = incoming.request().header("Upgrade");
    EXPECT_TRUE(upgrade == std::optional<std::string_view>{", websocket,,"});
    if (!upgrade) {
        return;
    }
    const std::array fields{ruvia::http_header_view{"Connection", "Upgrade"},
        ruvia::http_header_view{"Upgrade", *upgrade}};
    std::array<char, 1024> output;
    EXPECT_TRUE(std::holds_alternative<http1_client_request_prepare_error>(
        prepare_field_request(0, fields, output, ruvia::http_request_field_source::generated)));
    auto forwarded = prepare_field_request(0, fields, output, ruvia::http_request_field_source::forwarded);
    auto* prepared = std::get_if<prepared_http1_client_request>(&forwarded);
    EXPECT_TRUE(prepared != nullptr);
    if (!prepared) {
        return;
    }
    EXPECT_TRUE(prepared->head().find("Upgrade: , websocket,,\r\n") != std::string_view::npos);
    ruvia::http1_client_response_parser response_parser(std::move(*prepared).take_exchange_state());
    const auto response = response_parser.parse(
        "HTTP/1.1 101 Switching Protocols\r\nConnection: Upgrade\r\nUpgrade: websocket\r\n\r\n");
    EXPECT_TRUE(response.parsed() != nullptr);
    if (const auto* parsed = response.parsed()) {
        EXPECT_TRUE(parsed->plan().protocol_upgrade() != nullptr);
    }
    auto second = prepare_field_request(0, fields, output, ruvia::http_request_field_source::forwarded);
    auto* second_prepared = std::get_if<prepared_http1_client_request>(&second);
    EXPECT_TRUE(second_prepared != nullptr);
    if (second_prepared) {
        ruvia::http1_client_response_parser unoffered_parser(std::move(*second_prepared).take_exchange_state());
        const auto unoffered = unoffered_parser.parse(
            "HTTP/1.1 101 Switching Protocols\r\nConnection: Upgrade\r\nUpgrade: other\r\n\r\n");
        EXPECT_TRUE(unoffered.failure() != nullptr);
    }
    const std::array invalid_fields{fields[0], ruvia::http_header_view{"Upgrade", ",websocket/,"}};
    EXPECT_TRUE(std::holds_alternative<http1_client_request_prepare_error>(
        prepare_field_request(0, invalid_fields, output, ruvia::http_request_field_source::forwarded)));
}

TEST(Ruvia, http1_connect_udp_forwarding_keeps_protocol_owned_fields) {
    const auto origin = http_origin_view::https({.host_ = "example.test"});
    const std::array fields{ruvia::http_header_view{"Capsule-Protocol", "?1"},
        ruvia::http_header_view{"Origin", "https://example.test/path"}};
    std::array<char, 1024> output;
    const http1_client_request_writer writer;
    const http1_client_request_wire_policy policy{.field_source_ = ruvia::http_request_field_source::forwarded};
    EXPECT_TRUE(std::holds_alternative<http1_client_request_prepare_error>(
        writer.prepare_connect_udp(origin, "/udp", fields, output)));
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
    for (const auto& forbidden : std::array{
             ruvia::http_header_view{"Content-Type", "invalid"},
             ruvia::http_header_view{"Content-Length", "0"},
             ruvia::http_header_view{"Upgrade", "connect-udp"},
             ruvia::http_header_view{"Connection", "Upgrade"},
             ruvia::http_header_view{"Origin", "bad\r\nInjected: yes"}}) {
        const std::array invalid{fields[0], forbidden};
        EXPECT_TRUE(std::holds_alternative<http1_client_request_prepare_error>(
            writer.prepare_connect_udp(origin, "/udp", invalid, output, policy)));
    }
    EXPECT_TRUE(std::holds_alternative<http1_client_request_prepare_error>(
        writer.prepare_connect_udp(origin, "/udp", {}, output,
            {.field_source_ = static_cast<ruvia::http_request_field_source>(0xff)})));
}

TEST(Ruvia, http1_client_forwards_options_content_without_inventing_media_type) {
    constexpr std::string_view incoming_wire =
        "OPTIONS / HTTP/1.1\r\nHost: example.test\r\nContent-Length: 4\r\n\r\nbody";
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
                return writer.prepare(origin,
                    http_client_request_view{.method_ = method, .content_ = http_client_request_content_view::bytes(incoming.body_)},
                    output, policy);
            }
            return writer.prepare_streaming(origin,
                ruvia::http1_client_request_head_view{.method_ = method,
                    .content_length_ = entry == 1 ? std::optional<ruvia::http_content_length>{4} : std::nullopt},
                output, policy);
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

http_origin_view origin_for(
    std::string_view host, std::uint16_t port, http_scheme scheme = http_scheme::http) {
    return scheme == http_scheme::https ? http_origin_view::https({.host_ = host, .port_ = port})
                                        : http_origin_view::http({.host_ = host, .port_ = port});
}

std::string authority_for(const http_origin_view& origin) {
    const auto authority = make_http_origin_authority(origin);
    return authority;
}

}  // namespace

TEST(Ruvia, http_origin_factory_makes_an_invalid_host_unrepresentable) {
    EXPECT_TRUE(!throws_on([] { (void)origin_for("example.com", 80); }));
    EXPECT_TRUE(!throws_on([] { (void)origin_for("[::1]", 443, http_scheme::https); }));
    EXPECT_TRUE(!throws_on([] { (void)origin_for("[v1.future]", 80); }));
    // Port zero is a syntactically distinct URI origin. Whether a transport can
    // connect to it is deliberately outside this protocol-only value type.
    EXPECT_TRUE(!throws_on([] { (void)origin_for("example.com", 0); }));

    EXPECT_TRUE(throws_on([] { (void)origin_for("", 80); }));
    EXPECT_TRUE(throws_on([] { (void)origin_for("bad host", 80); }));
    EXPECT_TRUE(throws_on([] { (void)origin_for("::1", 80); }));
    EXPECT_TRUE(throws_on([] { (void)origin_for("[::1", 80); }));
    EXPECT_TRUE(throws_on([] { (void)origin_for("example.com:80", 80); }));
    EXPECT_TRUE(throws_on([] { (void)origin_for("bad?host", 80); }));
    EXPECT_TRUE(throws_on([] { (void)origin_for("[::::]", 80); }));
    EXPECT_TRUE(throws_on([] { (void)origin_for("[v.future]", 80); }));
    EXPECT_TRUE(throws_on([] { (void)origin_for("caf\xC3\xA9.example", 80); }));
    EXPECT_TRUE(throws_on([] { (void)origin_for(std::string_view("api\0.internal", 13), 80); }));
}

TEST(Ruvia, http_serialized_origin_matches_fetch_wire_grammar) {
    for (const auto value : {
             "https://example.com", "https://example.com.", "https://sub.example.com.:8443",
             "https://exa_mple.com", "https://-example.com", "https://example..com", "https://.",
             "http://127.0.0.1:8080", "https://[::]", "https://[::1]", "https://[0:0:1::1]",
             "custom+scheme://sub-domain.example:65535", "custom+scheme://sub-domain.example:0"}) {
        EXPECT_TRUE(ruvia::is_valid_http_serialized_origin(value));
    }
    for (const auto value : {
             "", "null", "HTTPS://example.com", "https://EXAMPLE.com", "https://example.com/",
             "https://exa%6dple.com", "https://123", "https://1.2.3", "https://127.0.0.1.",
             "https://example.123", "https://example.0x10", "https://[v1.future]", "https://[::A]",
             "https://[::0001]", "https://[1:2:3:4:5:6:7::]", "https://[1::0:0:1]", "https://[0:0:1::1:1:1]",
             "https://example.com:", "https://example.com:0443", "http://example.com:80",
             "https://example.com:443", "ws://example.com:80", "wss://example.com:443", "ftp://example.com:21",
             "https://example.com:65536", "custom+scheme://sub-domain.example:00001",
             "custom+scheme://sub-domain.example:99999", "https://example.com:123456"}) {
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
void check_resolved_target(const http_origin_view& origin,
    std::string_view current_target, std::string_view location, std::string_view expected) {
    const auto result_value =
        resolve_http_client_redirect_target(origin, {.current_target_ = current_target,
                                                        .location_ = location});
    const auto* resolved = std::get_if<http_client_resolved_redirect>(&result_value);
    EXPECT_TRUE(resolved != nullptr);
    if (resolved != nullptr) {
        EXPECT_TRUE(!resolved->cross_origin());
        EXPECT_EQ(resolved->target(), expected);
    }
}

void check_cross_origin_redirect(const http_origin_view& origin,
    std::string_view current_target, std::string_view location) {
    const auto result_value =
        resolve_http_client_redirect_target(origin, {.current_target_ = current_target,
                                                        .location_ = location});
    const auto* resolved = std::get_if<http_client_resolved_redirect>(&result_value);
    EXPECT_TRUE(resolved != nullptr);
    if (resolved != nullptr) {
        EXPECT_TRUE(resolved->cross_origin());
    }
}

void check_redirect_target_failure(const http_origin_view& origin, std::string_view current_target, std::string_view location,
    http_client_redirect_resolution_error expected) {
    const auto result_value =
        resolve_http_client_redirect_target(origin, {.current_target_ = current_target,
                                                        .location_ = location});
    const auto* failure = std::get_if<http_client_redirect_resolution_error>(&result_value);
    EXPECT_TRUE(failure != nullptr);
    if (failure != nullptr) {
        EXPECT_TRUE(*failure == expected);
    }
}

}  // namespace

TEST(Ruvia, http_client_redirect_status_set) {
    for (const ruvia::http_status_code status : {ruvia::http_status::moved_permanently,
             ruvia::http_status::found, ruvia::http_status::see_other,
             ruvia::http_status::temporary_redirect, ruvia::http_status::permanent_redirect}) {
        EXPECT_TRUE(is_http_redirect_status(status));
    }
    for (const ruvia::http_status_code status :
        {ruvia::http_status::ok, ruvia::http_status::no_content,
            ruvia::http_status::multiple_choices, ruvia::http_status::not_modified,
            ruvia::http_status::use_proxy, ruvia::http_status_code::from_value(306),
            ruvia::http_status_code::from_value(399), ruvia::http_status::not_found}) {
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
        const auto plan =
            plan_http_client_redirect_request(request, {.status_ = ruvia::http_status::see_other});
        EXPECT_EQ(plan.method(), std::string_view("GET"));
        EXPECT_TRUE(plan.content_disposition() == http_client_redirect_content_disposition::drop);
    }
    {
        http_client_request_view request;
        request.method_ = "HEAD";
        const auto plan =
            plan_http_client_redirect_request(request, {.status_ = ruvia::http_status::see_other});
        EXPECT_EQ(plan.method(), std::string_view("HEAD"));
        EXPECT_TRUE(plan.content_disposition() == http_client_redirect_content_disposition::drop);
    }

    // RFC 9110 permits the historical POST-to-GET rewrite for 301/302. Other
    // methods are not aliases for POST and retain both method and content.
    {
        http_client_request_view request;
        request.method_ = "POST";
        request.content_ = ruvia::http_client_request_content_view::bytes("payload");
        const auto plan =
            plan_http_client_redirect_request(request, {.status_ = ruvia::http_status::found});
        EXPECT_EQ(plan.method(), std::string_view("GET"));
        EXPECT_TRUE(plan.content_disposition() == http_client_redirect_content_disposition::drop);
    }
    {
        http_client_request_view request;
        request.method_ = "PUT";
        request.content_ = ruvia::http_client_request_content_view::bytes("payload");
        const auto plan = plan_http_client_redirect_request(
            request, {.status_ = ruvia::http_status::moved_permanently});
        EXPECT_EQ(plan.method(), std::string_view("PUT"));
        EXPECT_TRUE(plan.content_disposition() == http_client_redirect_content_disposition::preserve);
    }

    // Method tokens are case-sensitive: lowercase "post" is a distinct method.
    {
        http_client_request_view request;
        request.method_ = "post";
        const auto plan = plan_http_client_redirect_request(
            request, {.status_ = ruvia::http_status::moved_permanently});
        EXPECT_EQ(plan.method(), std::string_view("post"));
        EXPECT_TRUE(plan.content_disposition() == http_client_redirect_content_disposition::preserve);
    }

    // 307/308 never change method or content.
    for (const ruvia::http_status_code status :
        {ruvia::http_status::temporary_redirect, ruvia::http_status::permanent_redirect}) {
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

    const auto plan =
        plan_http_client_redirect_request(request, {.status_ = ruvia::http_status::temporary_redirect});
    for (char& ch : method) {
        ch = 'X';
    }

    EXPECT_EQ(plan.method(), std::string_view("PROPFIND"));
    EXPECT_TRUE(plan.content_disposition() == http_client_redirect_content_disposition::preserve);
}

TEST(Ruvia, http_client_response_header_lookup_distinguishes_empty_and_repeated) {
    auto parsed = ruvia::test::parse_response(
        "GET", "HTTP/1.1 302 Found\r\nLocation:\r\nContent-Length: 0");
    const auto& head = parsed.head_;

    const auto empty = lookup_unique_http_client_response_header(head, "location");
    EXPECT_TRUE(empty.status_ == http_unique_header_status::found);
    EXPECT_TRUE(empty.value_.empty());

    const auto missing = lookup_unique_http_client_response_header(head, "missing");
    EXPECT_TRUE(missing.status_ == http_unique_header_status::absent);

    auto repeated_parsed = ruvia::test::parse_response(
        "GET", "HTTP/1.1 302 Found\r\nLocation:\r\nLOCATION: /second\r\nContent-Length: 0");
    const auto repeated = lookup_unique_http_client_response_header(repeated_parsed.head_, "Location");
    EXPECT_TRUE(repeated.status_ == http_unique_header_status::repeated);
}

TEST(Ruvia, http_client_authority_matches_typed_origin) {
    const auto non_default = origin_for("example.com", 8080);
    const auto is = [](const http_origin_view& origin, std::string_view authority) {
        return classify_http_client_origin_authority(origin, authority);
    };
    EXPECT_TRUE(is(non_default, "example.com:8080") == http_client_origin_authority_status::same_origin);
    for (const std::string_view different :
        {"example.com", "example.com:9090", "other.com:8080", "example.com:0", "example.com:"}) {
        EXPECT_TRUE(is(non_default, different) == http_client_origin_authority_status::different_origin);
    }
    EXPECT_TRUE(is(non_default, "user@example.com:8080") ==
                http_client_origin_authority_status::invalid_authority);
    EXPECT_TRUE(is(non_default, "example.com:99999") == http_client_origin_authority_status::invalid_authority);

    EXPECT_TRUE(is(http_origin_view::http({.host_ = "example.com"}), "example.com") ==
                http_client_origin_authority_status::same_origin);
    EXPECT_TRUE(is(http_origin_view::http({.host_ = "example.com"}), "EXAMPLE.com:") ==
                http_client_origin_authority_status::same_origin);
    EXPECT_TRUE(is(http_origin_view::http({.host_ = "example.com"}), "exa%6dple.com") ==
                http_client_origin_authority_status::same_origin);
    EXPECT_TRUE(is(http_origin_view::http({.host_ = "!example"}), "%21example") ==
                http_client_origin_authority_status::different_origin);
    EXPECT_TRUE(is(http_origin_view::https({.host_ = "example.com"}), "example.com") ==
                http_client_origin_authority_status::same_origin);
    EXPECT_TRUE(is(http_origin_view::http({.host_ = "example.com", .port_ = 0}), "example.com:0") ==
                http_client_origin_authority_status::same_origin);
    EXPECT_TRUE(is(http_origin_view::http({.host_ = "example.com", .port_ = 0}), "example.com") ==
                http_client_origin_authority_status::different_origin);

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
    constexpr equivalent_pair equivalents[]{
        {"[::]", "[0:0:0:0:0:0:0:0]"},
        {"[::1]", "[0:0:0:0:0:0:0:1]"},
        {"[::1]", "[::0.0.0.1]"},
        {"[1::]", "[1:0:0:0:0:0:0:0]"},
        {"[1:2:3:4:5:6:7::]", "[1:2:3:4:5:6:7:0]"},
        {"[1::2:3:4:5:6:7]", "[1:0:2:3:4:5:6:7]"},
        {"[2001:db8:0:1::2:3]", "[2001:0DB8:0:1:0:0:2:3]"},
        {"[::ffff:c000:201]", "[::ffff:192.0.2.1]"},
        {"[1:2:3:4:5:6:c000:201]", "[1:2:3:4:5:6:192.0.2.1]"}};
    for (const auto& pair : equivalents) {
        for (const bool reverse : {false, true}) {
            const auto source = reverse ? pair.expanded_ : pair.compressed_;
            const auto destination = reverse ? pair.compressed_ : pair.expanded_;
            const auto origin = http_origin_view::http({.host_ = source});
            EXPECT_TRUE(classify_http_client_origin_authority(origin, destination) ==
                        http_client_origin_authority_status::same_origin);
            const auto location = std::string("http://") + std::string(destination) + "/next";
            check_resolved_target(origin, "/previous", location, "/next");
            EXPECT_TRUE(classify_http_client_origin_authority(origin, std::string(destination) + ":81") ==
                        http_client_origin_authority_status::different_origin);
        }
    }
    const auto origin = http_origin_view::http({.host_ = "[::ffff:192.0.2.1]"});
    for (const auto different : {"[::ffff:192.0.2.2]", "[::192.0.2.1]", "192.0.2.1", "[v1.future]"}) {
        EXPECT_TRUE(classify_http_client_origin_authority(origin, different) ==
                    http_client_origin_authority_status::different_origin);
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
    for (const std::string_view invalid :
        {"https://user@example.com/next", "https://example.com:99999/next",
            "http://user@example.com/next", "http://example.com:99999/next", "http:/broken",
            "/next#bad fragment", "/next#%zz", "/next#[bad]", "/next#first#second"}) {
        check_redirect_target_failure(origin, "/current", invalid,
            http_client_redirect_resolution_error::invalid_location);
    }
    check_redirect_target_failure(origin, "*", "/next", http_client_redirect_resolution_error::invalid_current_target);
}

TEST(Ruvia, http_client_redirect_validates_path_before_dot_segment_removal) {
    const auto origin = http_origin_view::http({.host_ = "example.com"});
    constexpr std::string_view invalid_locations[] = {
        "/bad space/../next", "/%/../next", "/%zz/../next", "/[bad]/../next",
        "/bad\\path/../next", "/bad\tpath/../next", "/bad\r\npath/../next",
        std::string_view("/bad\0path/../next", 17), "/bad\x7f/../next", "/caf\xc3\xa9/../next",
        "bad space/../next", "%zz/../next", "[bad]/../next",
        "http://example.com/%zz/../next", "//example.com/[bad]/../next"};
    for (const auto invalid : invalid_locations) {
        check_redirect_target_failure(origin, "/base/page", invalid,
            http_client_redirect_resolution_error::invalid_location);
    }
    for (const std::string_view valid : {
             "/bad%20space/../next", "/%25/../next", "/%5Bbad%5D/../next",
             "http://example.com/%25/../next", "//example.com/%25/../next"}) {
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

void check_resolved_redirect(const http_origin_view& origin,
    std::string_view current_target, std::string_view location,
    const expected_resolved_redirect& expected) {
    const auto result_value =
        resolve_http_client_redirect_target(origin, {.current_target_ = current_target,
                                                        .location_ = location});
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

void check_redirect_resolution_failure(const http_origin_view& origin, std::string_view current_target, std::string_view location,
    http_client_redirect_resolution_error expected) {
    const auto result_value =
        resolve_http_client_redirect_target(origin, {.current_target_ = current_target,
                                                        .location_ = location});
    const auto* failure = std::get_if<http_client_redirect_resolution_error>(&result_value);
    EXPECT_TRUE(failure != nullptr);
    if (failure != nullptr) {
        EXPECT_TRUE(*failure == expected);
    }
}

}  // namespace

TEST(Ruvia, http_client_redirect_resolution_same_origin_stays_relative) {
    const auto origin = origin_for("example.com", 80);
    check_resolved_redirect(origin, "/a/b?old=1", "c?x=1",
        {http_scheme::http, "example.com", 80, "/a/c?x=1", false});
    check_resolved_redirect(origin, "/a/b", "http://example.com/x",
        {http_scheme::http, "example.com", 80, "/x", false});
    // Explicit default port and a case-different host are the same origin.
    check_resolved_redirect(origin, "/a/b", "HTTP://EXAMPLE.COM:80/x",
        {http_scheme::http, "EXAMPLE.COM", 80, "/x", false});
}

TEST(Ruvia, http_client_redirect_resolution_classifies_cross_origin) {
    const auto origin = origin_for("example.com", 80);
    // Different host, default port for the located scheme.
    check_resolved_redirect(origin, "/a/b", "https://other.example/path?q=1",
        {http_scheme::https, "other.example", 443, "/path?q=1", true});
    // Same host, different port.
    check_resolved_redirect(origin, "/a/b", "http://example.com:8080/x",
        {http_scheme::http, "example.com", 8080, "/x", true});
    // Scheme change alone crosses the origin even on the same host.
    check_resolved_redirect(origin, "/a/b", "https://example.com/x",
        {http_scheme::https, "example.com", 443, "/x", true});
    // A protocol-relative reference keeps the scheme but moves authority.
    check_resolved_redirect(origin, "/a/b", "//other.example/p",
        {http_scheme::http, "other.example", 80, "/p", true});
    // Path normalization and fragment stripping apply across origins too.
    check_resolved_redirect(origin, "/a/b", "https://other.example/a/../b#frag",
        {http_scheme::https, "other.example", 443, "/b", true});
    // IPv6 literals keep their brackets, matching the http_origin_view contract.
    check_resolved_redirect(origin, "/a/b", "http://[::1]:8080/x",
        {http_scheme::http, "[::1]", 8080, "/x", true});
}

TEST(Ruvia, http_client_redirect_resolution_builds_borrowing_origin) {
    const auto origin = origin_for("example.com", 80);
    const auto result_value =
        resolve_http_client_redirect_target(origin, {.current_target_ = "/a/b",
                                                        .location_ = "https://other.example:8443/x"});
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
    check_redirect_resolution_failure(origin, "/a/b", "ftp://example.com/file",
        http_client_redirect_resolution_error::unsupported_scheme);
    check_redirect_resolution_failure(origin, "/a/b", "mailto:someone@example.com",
        http_client_redirect_resolution_error::unsupported_scheme);
    // Userinfo remains rejected: RFC 9110 deprecates it and clients must not
    // leak credentials embedded by the peer.
    check_redirect_resolution_failure(origin, "/a/b", "https://user@other.example/",
        http_client_redirect_resolution_error::invalid_location);
    check_redirect_resolution_failure(origin, "/a/b", "http:opaque-without-authority",
        http_client_redirect_resolution_error::invalid_location);
    check_redirect_resolution_failure(origin, "not-a-target", "/x",
        http_client_redirect_resolution_error::invalid_current_target);
}

TEST(Ruvia, http_client_redirect_target_options_accept_named_owners_and_copy_into_resolved_redirect) {
    const auto origin = origin_for("example.com", 80);
    std::string current_target("/a/b");
    std::string location("https://other.example:8443/next");
    const auto result_value = resolve_http_client_redirect_target(
        origin, {.current_target_ = current_target, .location_ = location});
    current_target.assign("/mutated");
    location.assign("https://wrong.example/");
    const auto* resolved = std::get_if<http_client_resolved_redirect>(&result_value);
    EXPECT_TRUE(resolved != nullptr);
    if (resolved != nullptr) {
        EXPECT_EQ(resolved->host(), std::string_view("other.example"));
        EXPECT_EQ(resolved->target(), std::string_view("/next"));
    }
}

TEST(Ruvia, set_cookie_parser_handles_deterministic_arbitrary_bytes) {
    std::uint64_t state_value = 0x5345'5443'4F4F'4B49ULL;
    const auto next_value = [&state_value]() {
        state_value ^= state_value << 7U;
        state_value ^= state_value >> 9U;
        return state_value;
    };
    const auto verify_borrowed_fields = [&](std::string_view input,
                                            const ruvia::http_set_cookie_view& parsed_value) {
        const auto is_borrowed_input_text = [input](std::string_view value) {
            return value.empty() || input.find(value) != std::string_view::npos;
        };
        EXPECT_TRUE(is_borrowed_input_text(parsed_value.name()));
        EXPECT_TRUE(is_borrowed_input_text(parsed_value.value()));
        EXPECT_TRUE(is_borrowed_input_text(parsed_value.path()));
        EXPECT_TRUE(is_borrowed_input_text(parsed_value.domain()));
        EXPECT_TRUE(!parsed_value.has(static_cast<ruvia::http_set_cookie_attribute>(0)));
        EXPECT_TRUE(!parsed_value.has(static_cast<ruvia::http_set_cookie_attribute>(
            static_cast<std::uint8_t>(ruvia::http_set_cookie_attribute::secure) |
            static_cast<std::uint8_t>(ruvia::http_set_cookie_attribute::partitioned))));
    };
    constexpr std::string_view cookie_octets =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789!#$%&'*+-.^_`|~";

    for (std::size_t sample = 0; sample < 4096; ++sample) {
        std::string input(static_cast<std::size_t>(next_value() % 1025U), '\0');
        for (auto& byte : input) {
            byte = static_cast<char>(next_value());
        }

        const auto parsed_value = ruvia::parse_set_cookie(input);
        if (parsed_value.has_value()) {
            verify_borrowed_fields(input, *parsed_value);
        }

        std::string accepted_input("sid=");
        for (std::size_t index = 0; index < next_value() % 129U; ++index) {
            accepted_input.push_back(cookie_octets[next_value() % cookie_octets.size()]);
        }
        accepted_input.append("; Path=/api; Domain=.example.test; Secure; SameSite=None");
        const auto accepted = ruvia::parse_set_cookie(accepted_input);
        EXPECT_TRUE(accepted.has_value());
        if (accepted.has_value()) {
            verify_borrowed_fields(accepted_input, *accepted);
        }
    }
}

TEST(Ruvia, set_cookie_parser_exposes_client_storage_fields) {
    const auto parsed_value = ruvia::parse_set_cookie(
        "sid=abc; Path=/api; Domain=.example.com; Max-Age=60; Secure; HttpOnly");
    EXPECT_TRUE(parsed_value.has_value());
    EXPECT_TRUE(parsed_value->name() == "sid");
    EXPECT_TRUE(parsed_value->value() == "abc");
    EXPECT_TRUE(parsed_value->path() == "/api");
    EXPECT_TRUE(parsed_value->domain() == "example.com");
    EXPECT_TRUE(parsed_value->max_age_seconds() == 60);
    EXPECT_TRUE(parsed_value->has(ruvia::http_set_cookie_attribute::secure));
    EXPECT_TRUE(parsed_value->has(ruvia::http_set_cookie_attribute::http_only));
    EXPECT_TRUE(parsed_value->http_only());
    EXPECT_TRUE(!parsed_value->same_site().has_value());
    EXPECT_TRUE(!parsed_value->same_site_none());
    EXPECT_TRUE(!parsed_value->has(static_cast<ruvia::http_set_cookie_attribute>(
        static_cast<std::uint8_t>(ruvia::http_set_cookie_attribute::secure) |
        static_cast<std::uint8_t>(ruvia::http_set_cookie_attribute::partitioned))));
}

TEST(Ruvia, set_cookie_parser_accepts_user_agent_cookie_pair_grammar) {
    const auto spaced_name = ruvia::parse_set_cookie("bad name=value");
    EXPECT_TRUE(spaced_name.has_value());
    if (!spaced_name) {
        return;
    }
    EXPECT_TRUE(spaced_name->name() == "bad name");

    const auto spaced_value = ruvia::parse_set_cookie("name=bad value");
    EXPECT_TRUE(spaced_value.has_value());
    if (!spaced_value) {
        return;
    }
    EXPECT_TRUE(spaced_value->value() == "bad value");
}

TEST(Ruvia, set_cookie_parser_ignores_nameless_cookies) {
    // RFC 6265 section 5.2 steps 2 and 5.
    EXPECT_TRUE(!ruvia::parse_set_cookie("sid; Path=/").has_value());
    EXPECT_TRUE(!ruvia::parse_set_cookie("=value").has_value());
    EXPECT_TRUE(!ruvia::parse_set_cookie(" =value").has_value());
}

TEST(Ruvia, set_cookie_parser_rejects_invalid_received_cookie) {
    EXPECT_TRUE(!ruvia::parse_set_cookie("=").has_value());
    EXPECT_TRUE(!ruvia::parse_set_cookie("name=bad\x01value").has_value());

    std::string oversized(4097, 'v');
    EXPECT_TRUE(!ruvia::parse_set_cookie(oversized).has_value());
}

TEST(Ruvia, set_cookie_parser_follows_user_agent_domain_and_path_algorithm) {
    // RFC 6265 section 5.2.3: "Domain=." leaves an empty cookie-domain (host-only);
    // an empty Domain value is ignored rather than overriding an earlier one.
    const auto dot = ruvia::parse_set_cookie("a=b; Domain=.");
    EXPECT_TRUE(dot.has_value() && dot->domain().empty());
    const auto later_empty = ruvia::parse_set_cookie("a=b; Domain=example.com; Domain=");
    EXPECT_TRUE(later_empty.has_value() && later_empty->domain() == "example.com");
    const auto later_dot = ruvia::parse_set_cookie("a=b; Domain=example.com; Domain=.");
    EXPECT_TRUE(later_dot.has_value() && later_dot->domain().empty());
    const auto underscore = ruvia::parse_set_cookie("a=b; Domain=my_host.example");
    EXPECT_TRUE(underscore.has_value() && underscore->domain() == "my_host.example");

    // Section 5.2.4: Path never rejects the cookie.
    const auto tab = ruvia::parse_set_cookie("a=b; Path=/a\tb");
    EXPECT_TRUE(tab.has_value() && tab->path() == "/a\tb");
    const auto utf8 = ruvia::parse_set_cookie("a=b; Path=/caf\xc3\xa9");
    EXPECT_TRUE(utf8.has_value() && utf8->path() == "/caf\xc3\xa9");
    const auto relative = ruvia::parse_set_cookie("a=b; Path=/api; Path=relative");
    EXPECT_TRUE(relative.has_value() && relative->path().empty());
}

TEST(Ruvia, set_cookie_parser_ignores_oversized_attribute_value) {
    std::string value = "sid=abc; Path=";
    value.append(1025, 'p');
    const auto parsed_value = ruvia::parse_set_cookie(value);
    EXPECT_TRUE(parsed_value.has_value());
    if (!parsed_value) {
        return;
    }
    EXPECT_TRUE(parsed_value->path().empty());
}

TEST(Ruvia, set_cookie_parser_tracks_storage_security_attributes) {
    const auto parsed_value = ruvia::parse_set_cookie("sid=abc; Path=/rel; SameSite=None");
    EXPECT_TRUE(parsed_value.has_value());
    if (!parsed_value) {
        return;
    }
    EXPECT_TRUE(parsed_value->path() == "/rel");
    EXPECT_TRUE(parsed_value->same_site_none());

    const auto overridden = ruvia::parse_set_cookie("sid=abc; SameSite=None; SameSite=Lax");
    EXPECT_TRUE(overridden.has_value());
    if (!overridden) {
        return;
    }
    EXPECT_TRUE(parsed_value->same_site_none());
    EXPECT_TRUE(parsed_value->same_site() == ruvia::cookie_same_site::none);
    EXPECT_TRUE(!overridden->same_site_none());
    EXPECT_TRUE(overridden->same_site() == ruvia::cookie_same_site::lax);
    const auto strict = ruvia::parse_set_cookie("sid=abc; samesite=STRICT");
    EXPECT_TRUE(strict.has_value() && strict->same_site() == ruvia::cookie_same_site::strict);
    const auto unknown_same_site =
        ruvia::parse_set_cookie("sid=abc; SameSite=Strict; SameSite=Unknown");
    EXPECT_TRUE(unknown_same_site.has_value() && !unknown_same_site->same_site().has_value() &&
                !unknown_same_site->same_site_none());
}

TEST(Ruvia, set_cookie_parser_preserves_quoted_cookie_value) {
    const auto parsed_value = ruvia::parse_set_cookie("name=\"value\"; Path=/");
    EXPECT_TRUE(parsed_value.has_value());
    EXPECT_TRUE(parsed_value->value() == "\"value\"");
}

TEST(Ruvia, set_cookie_parser_treats_empty_domain_as_host_only) {
    const auto parsed_value = ruvia::parse_set_cookie("sid=abc; Domain=; Path=/api");
    EXPECT_TRUE(parsed_value.has_value());
    EXPECT_TRUE(parsed_value->name() == "sid");
    EXPECT_TRUE(parsed_value->domain().empty());
    EXPECT_TRUE(parsed_value->path() == "/api");

    const std::string overlong_with_nul =
        std::string("sid=abc; Custom=") + std::string(1100, 'a') + std::string("\0b", 2);
    EXPECT_TRUE(!ruvia::parse_set_cookie(overlong_with_nul).has_value());
}

TEST(Ruvia, set_cookie_parser_saturates_max_age_overflow) {
    const auto positive =
        ruvia::parse_set_cookie("sid=abc; Max-Age=999999999999999999999999999999999999");
    EXPECT_TRUE(positive.has_value());
    EXPECT_TRUE(positive->max_age_seconds() ==
                std::chrono::duration_cast<std::chrono::seconds>(std::chrono::days(400)).count());

    const auto negative =
        ruvia::parse_set_cookie("sid=abc; Max-Age=-999999999999999999999999999999999999");
    EXPECT_TRUE(negative.has_value());
    EXPECT_TRUE(negative->max_age_seconds() == std::numeric_limits<std::int64_t>::min());
}

TEST(Ruvia, set_cookie_parser_ignores_overflowing_max_age_with_trailing_bytes) {
    const auto parsed_value = ruvia::parse_set_cookie(
        "sid=abc; Max-Age=60; Max-Age=999999999999999999999999suffix; Path=/api");
    ASSERT_TRUE(parsed_value.has_value());
    EXPECT_EQ(parsed_value->max_age_seconds(), 60);
    EXPECT_EQ(parsed_value->path(), "/api");
}

TEST(Ruvia, set_cookie_parser_ignores_invalid_later_expires_attribute) {
    const auto parsed_value =
        ruvia::parse_set_cookie("sid=abc; Expires=Sun, 06 Nov 1994 08:49:37 GMT; Expires=not-a-date");
    EXPECT_TRUE(parsed_value.has_value());
    EXPECT_TRUE(parsed_value->expires().has_value());
}

TEST(Ruvia, set_cookie_parser_uses_cookie_date_token_grammar) {
    const auto hyphenated = ruvia::parse_set_cookie("sid=abc; Expires=Wed, 09-Jun-2021 10:18:14 GMT");
    EXPECT_TRUE(hyphenated.has_value());
    if (!hyphenated) {
        return;
    }
    EXPECT_TRUE(hyphenated->expires() == ruvia::parse_http_date("Wed, 09 Jun 2021 10:18:14 GMT"));

    const auto short_year =
        ruvia::parse_set_cookie("sid=abc; Expires=Thursday, 01-Jan-70 00:00:00 GMT");
    EXPECT_TRUE(short_year.has_value());
    if (!short_year) {
        return;
    }
    EXPECT_TRUE(short_year->expires() == ruvia::parse_http_date("Thu, 01 Jan 1970 00:00:00 GMT"));
}

TEST(Ruvia, set_cookie_parser_accepts_date_token_suffixes) {
    const auto expected = std::chrono::system_clock::to_time_t(
        std::chrono::sys_days{std::chrono::year{2021} / 6 / 9} +
        std::chrono::hours{10} + std::chrono::minutes{18} + std::chrono::seconds{14});
    constexpr std::string_view dates[]{
        "Wed, 09th Jun 2021 10:18:14 GMT",
        "Wed, 09 June 2021 10:18:14 GMT",
        "Wed, 09 jUn123 2021 10:18:14 GMT",
        "Wed, 09 Jun 2021year123 10:18:14 GMT",
        "Wed, 09 Jun 2021 10:18:14clock123 GMT",
        "Wed, 09 Jun 2021 10:18:14:GMT",
        "10:18:14clock 09th June123 2021year",
        "2021year June123 09th 10:18:14clock"};
    for (const auto date : dates) {
        const auto input = std::string("sid=abc; Expires=") + std::string(date);
        const auto parsed_value = ruvia::parse_set_cookie(input);
        EXPECT_TRUE(parsed_value.has_value());
        if (parsed_value) {
            EXPECT_TRUE(parsed_value->expires() == expected);
        }
    }

    struct year_case final {
        std::string_view value_;
        int year_;
    };
    constexpr year_case years[]{
        {"70year", 1970}, {"69year", 2069}, {"00year", 2000}};
    for (const auto& entry : years) {
        const auto input = std::string("sid=abc; Expires=01 January ") + std::string(entry.value_) + " 0:0:0clock";
        const auto parsed_value = ruvia::parse_set_cookie(input);
        EXPECT_TRUE(parsed_value.has_value());
        if (parsed_value) {
            const auto year_start = std::chrono::system_clock::to_time_t(
                std::chrono::sys_days{std::chrono::year{entry.year_} / 1 / 1});
            EXPECT_TRUE(parsed_value->expires() == year_start);
        }
    }

    EXPECT_TRUE(!ruvia::parse_http_date("Wed, 09th June 2021year 10:18:14clock GMT"));
}

TEST(Ruvia, set_cookie_parser_preserves_date_token_widths_and_calendar_bounds) {
    constexpr std::string_view dates[]{
        "Wed, 009th Jun 2021 10:18:14clock",
        "Wed, 09 Jun 02021year 10:18:14clock",
        "Wed, 09 Jun 1year 10:18:14clock",
        "Wed, 09 Jun 2021 010:18:14clock",
        "Wed, 09 Jun 2021 10:018:14clock",
        "Wed, 09 Jun 2021 10:18:014clock",
        "Wed, 09 Jun 2021 10x:18:14clock",
        "Wed, 09 Jun 2021 10:18x:14clock",
        "Wed, 09 Jux 2021 10:18:14clock",
        "Wed, 32nd June 2021year 10:18:14clock",
        "Wed, 29th February 2021year 10:18:14clock",
        "Wed, 09th June 1600year 10:18:14clock",
        "Wed, 09th June 2021year 24:18:14clock",
        "Wed, 09th June 2021year 10:60:14clock",
        "Wed, 09th June 2021year 10:18:60clock",
        "32nd 09 June 2021year 10:18:14clock",
        "09th June 2021year 24:00:00clock 10:18:14clock"};
    for (const auto date : dates) {
        const auto input = std::string("sid=abc; Expires=") + std::string(date);
        const auto parsed_value = ruvia::parse_set_cookie(input);
        EXPECT_TRUE(parsed_value.has_value());
        if (parsed_value) {
            EXPECT_TRUE(!parsed_value->expires());
        }
    }
}

TEST(Ruvia, cookie_jar_deduplicates_and_expires_by_partitioned_four_tuple) {
    const auto now = std::chrono::system_clock::from_time_t(1'700'000'000);
    ruvia::http_cookie_jar jar(ruvia::http_scheme::https, "example.test");
    EXPECT_TRUE(jar.absorb_set_cookie("/", "sid=part1; Path=/; Secure; Partitioned", now));
    EXPECT_TRUE(jar.absorb_set_cookie("/", "sid=unpart1; Path=/; Secure", now));
    EXPECT_EQ(jar.size(), std::size_t{2});

    // Updating the unpartitioned cookie leaves the partitioned entry intact.
    EXPECT_TRUE(jar.absorb_set_cookie("/", "sid=unpart2; Path=/; Secure", now));
    EXPECT_EQ(jar.size(), std::size_t{2});
    const auto both = jar.cookie_header("/", true, now);
    EXPECT_NE(both.find("sid=part1"), std::string::npos);
    EXPECT_NE(both.find("sid=unpart2"), std::string::npos);

    // Expiring the partitioned cookie removes only the partitioned entry.
    EXPECT_TRUE(jar.absorb_set_cookie("/", "sid=; Path=/; Max-Age=0; Secure; Partitioned", now));
    EXPECT_EQ(jar.size(), std::size_t{1});
    EXPECT_EQ(jar.cookie_header("/", true, now), "sid=unpart2");
}

using namespace ruvia::test;

namespace {

// Published trailer fields as "name=value;" in received order.
std::string rendered(std::span<const ruvia::http_header> fields) {
    std::string text;
    for (const auto& field : fields) {
        text.append(field.name()).append("=").append(field.value()).append(";");
    }
    return text;
}

}  // namespace

TEST(Ruvia, http1_response_body_decoder_fixed_length_preserves_tail_and_eof) {
    const auto head = parse_head("GET", "HTTP/1.1 200 OK\r\nContent-Length: 3");
    ruvia::http1_client_response_body_decoder decoder(head.plan());
    std::array<char, 8> scratch{};
    auto first = decoder.decode("ab", scratch);
    EXPECT_TRUE(first.output() != nullptr);
    EXPECT_EQ(first.consumed_bytes(), std::size_t{2});
    EXPECT_EQ(first.output()->bytes(), "ab");
    auto second = decoder.finish_input("cNEXT", scratch);
    EXPECT_TRUE(second.output() != nullptr);
    EXPECT_EQ(second.consumed_bytes(), std::size_t{1});
    auto complete_value = decoder.finish_input("NEXT", scratch);
    EXPECT_TRUE(complete_value.complete() != nullptr);
    EXPECT_EQ(complete_value.consumed_bytes(), std::size_t{0});
}

TEST(Ruvia, http1_response_body_decoder_head_and_no_content_complete_without_consuming_tail) {
    for (const auto& [method, status] : {std::pair{"HEAD", "200 OK"},
             std::pair{"GET", "204 No Content"}, std::pair{"GET", "304 Not Modified"}}) {
        const auto head = parse_head(method, std::string("HTTP/1.1 ") + status);
        ruvia::http1_client_response_body_decoder decoder(head.plan());
        std::array<char, 4> scratch{};
        const auto result_value = decoder.decode("TAIL", scratch);
        EXPECT_TRUE(result_value.complete() != nullptr);
        EXPECT_EQ(result_value.consumed_bytes(), std::size_t{0});
    }
}

TEST(Ruvia, http1_response_body_decoder_head_205_keeps_metadata_and_following_response) {
    const auto head = parse_head("HEAD", "HTTP/1.1 205 Reset Content\r\nContent-Length: 7");
    EXPECT_TRUE(head.plan().without_content() != nullptr);
    const auto headers = head.head().headers();
    EXPECT_TRUE(!headers.empty());
    if (!headers.empty()) {
        EXPECT_EQ(headers.front().value(), "7");
    }
    ruvia::http1_client_response_body_decoder decoder(head.plan());
    std::array<char, 4> scratch{};
    constexpr std::string_view following = "HTTP/1.1 200 OK\r\nContent-Length: 1\r\n\r\nxNEXT";
    const auto complete = decoder.decode(following, scratch);
    EXPECT_TRUE(complete.complete() != nullptr);
    EXPECT_EQ(complete.consumed_bytes(), std::size_t{0});

    auto remaining = following.substr(complete.consumed_bytes());
    const auto next = ruvia::test::parse_wire("GET", remaining);
    EXPECT_TRUE(next.parsed() != nullptr);
    if (next.parsed() == nullptr) {
        return;
    }
    remaining.remove_prefix(next.parsed()->consumed_bytes());
    ruvia::http1_client_response_body_decoder next_decoder(
        next.parsed()->plan());
    const auto body = next_decoder.decode(remaining, scratch);
    EXPECT_TRUE(body.output() != nullptr);
    if (const auto* output = body.output()) {
        EXPECT_EQ(output->bytes(), "x");
    }
    remaining.remove_prefix(body.consumed_bytes());
    const auto next_complete = next_decoder.decode(remaining, scratch);
    EXPECT_TRUE(next_complete.complete() != nullptr);
    EXPECT_EQ(next_complete.consumed_bytes(), std::size_t{0});
    EXPECT_EQ(remaining, "NEXT");
}

TEST(Ruvia, http1_response_body_decoder_chunked_trailers_and_tail) {
    const auto head = parse_head("GET", "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked");
    ruvia::http1_client_response_body_decoder decoder(head.plan());
    std::array<char, 8> scratch{};
    auto body = decoder.decode("3\r\nabc\r\n0\r\nX-Test: yes\r\n\r\nTAIL", scratch);
    EXPECT_TRUE(body.output() != nullptr);
    EXPECT_EQ(body.output()->bytes(), "abc");
    auto trailer = decoder.decode("0\r\nX-Test: yes\r\n\r\nTAIL", scratch);
    EXPECT_TRUE(trailer.trailers() != nullptr);
    EXPECT_EQ(rendered(trailer.trailers()->fields()), "x-test=yes;");
    EXPECT_EQ(trailer.consumed_bytes(), std::size_t{18});
    auto complete_value = decoder.decode("TAIL", scratch);
    EXPECT_TRUE(complete_value.complete() != nullptr);
    EXPECT_EQ(complete_value.consumed_bytes(), std::size_t{0});
}

// RFC 9112 section 5.2: a user agent replaces each obs-fold received in a
// response with SP before interpreting the value; a folded trailer is not
// broken framing.

// RFC 9110 section 6.5.1 and RFC 9112 section 7.1.2: fields a server may not
// send as trailers are discarded rather than published, like the HTTP/2 and
// HTTP/3 clients do; a section without kept fields allocates nothing.

TEST(Ruvia, http1_response_body_decoder_close_delimited_completes_only_at_eof) {
    const auto head = parse_head("GET", "HTTP/1.1 200 OK");
    ruvia::http1_client_response_body_decoder decoder(head.plan());
    std::array<char, 3> scratch{};
    auto body = decoder.decode("abcdef", scratch);
    EXPECT_TRUE(body.output() != nullptr);
    EXPECT_EQ(body.consumed_bytes(), std::size_t{3});
    EXPECT_EQ(body.output()->bytes(), "abc");
    auto rest = decoder.finish_input("def", scratch);
    EXPECT_TRUE(rest.output() != nullptr);
    EXPECT_EQ(rest.consumed_bytes(), std::size_t{3});
    auto complete_value = decoder.finish_input({}, scratch);
    EXPECT_TRUE(complete_value.complete() != nullptr);
}

TEST(Ruvia, http1_response_body_decoder_transfer_gzip_and_deflate_stream_with_tiny_scratch) {
    constexpr std::string_view gzip_hello{
        "\x1f\x8b\x08\x00\x00\x00\x00\x00\x02\xff\xcb\x48\xcd\xc9"
        "\xc9\x07\x00\x86\xa6\x10\x36\x05\x00\x00\x00",
        25};
    constexpr std::string_view deflate_hello{
        "\x78\x9c\xcb\x48\xcd\xc9\xc9\x07\x00\x06\x2c\x02\x15", 13};
    for (const auto& [coding, bytes] : {std::pair{"gzip", gzip_hello},
             std::pair{"deflate", deflate_hello}}) {
        const auto head = parse_head("GET", std::string("HTTP/1.1 200 OK\r\nTransfer-Encoding: ") + coding);
        ruvia::http1_client_response_body_decoder decoder(head.plan());
        std::array<char, 1> scratch{};
        std::string output;
        std::string input(bytes);
        auto result_value = decoder.finish_input(input, scratch);
        for (std::size_t step = 0; step < 100 && !result_value.complete() &&
                                   !result_value.protocol_failure() && !result_value.decoder_failure();
            ++step) {
            input.erase(0, result_value.consumed_bytes());
            if (result_value.output()) {
                output.append(result_value.output()->bytes());
            }
            result_value = decoder.finish_input(input, scratch);
        }
        if (result_value.output()) {
            output.append(result_value.output()->bytes());
        }
        EXPECT_EQ(output, "hello");
        EXPECT_TRUE(result_value.complete() != nullptr);
    }
}

TEST(Ruvia, http1_response_body_decoder_gzip_concatenation_and_chunked_transfer) {
    constexpr std::string_view gzip_hello{
        "\x1f\x8b\x08\x00\x00\x00\x00\x00\x02\xff\xcb\x48\xcd\xc9"
        "\xc9\x07\x00\x86\xa6\x10\x36\x05\x00\x00\x00",
        25};
    std::string concatenated(gzip_hello);
    concatenated.append(gzip_hello);
    const auto close_head = parse_head("GET", "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip");
    ruvia::http1_client_response_body_decoder close_decoder(
        close_head.plan());
    std::array<char, 2> scratch{};
    std::string decoded;
    auto result_value = close_decoder.finish_input(concatenated, scratch);
    for (std::size_t step = 0; step < 100 && !result_value.complete() &&
                               !result_value.protocol_failure() && !result_value.decoder_failure();
        ++step) {
        concatenated.erase(0, result_value.consumed_bytes());
        if (result_value.output()) {
            decoded.append(result_value.output()->bytes());
        }
        result_value = close_decoder.finish_input(concatenated, scratch);
    }
    if (result_value.output()) {
        decoded.append(result_value.output()->bytes());
    }
    EXPECT_EQ(decoded, "hellohello");
    EXPECT_TRUE(result_value.complete() != nullptr);

    const auto chunked_head =
        parse_head("GET", "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, chunked");
    ruvia::http1_client_response_body_decoder chunked_decoder(
        chunked_head.plan());
    char size[3]{};
    std::snprintf(size, sizeof(size), "%zx", gzip_hello.size());
    std::string wire(size);
    wire.append("\r\n");
    wire.append(gzip_hello);
    wire.append("\r\n0\r\n\r\nTAIL");
    decoded.clear();
    bool trailers_seen = false;
    for (;;) {
        result_value = chunked_decoder.decode(wire, scratch);
        wire.erase(0, result_value.consumed_bytes());
        if (result_value.output() != nullptr) {
            decoded.append(result_value.output()->bytes());
        } else if (result_value.trailers() != nullptr) {
            trailers_seen = true;
        } else if (result_value.complete() != nullptr) {
            break;
        } else {
            EXPECT_TRUE(result_value.need_input() != nullptr);
        }
    }
    EXPECT_EQ(decoded, "hello");
    EXPECT_TRUE(trailers_seen);
    EXPECT_EQ(wire, "TAIL");
}

TEST(Ruvia, http1_response_body_decoder_transfer_completion_waits_for_http_framing_and_rejects_tail) {
    constexpr std::string_view deflate_hello{
        "\x78\x9c\xcb\x48\xcd\xc9\xc9\x07\x00\x06\x2c\x02\x15", 13};
    const auto head = parse_head("GET", "HTTP/1.1 200 OK\r\nTransfer-Encoding: deflate");
    ruvia::http1_client_response_body_decoder decoder(head.plan());
    std::array<char, 16> scratch{};
    const auto output = decoder.decode(deflate_hello, scratch);
    EXPECT_TRUE(output.output() != nullptr);
    EXPECT_EQ(output.output()->bytes(), "hello");
    const auto still_waiting = decoder.decode({}, scratch);
    EXPECT_TRUE(still_waiting.need_input() != nullptr);
    const auto illegal_tail = decoder.decode("X", scratch);
    EXPECT_TRUE(illegal_tail.protocol_failure() != nullptr);
    EXPECT_TRUE(illegal_tail.protocol_failure()->error() ==
                ruvia::http1_client_response_body_error::invalid_transfer_coding);
    EXPECT_EQ(illegal_tail.consumed_bytes(), std::size_t{0});

    const auto eof_head = parse_head("GET", "HTTP/1.1 200 OK\r\nTransfer-Encoding: deflate");
    ruvia::http1_client_response_body_decoder eof_decoder(
        eof_head.plan());
    auto eof_result = eof_decoder.finish_input(deflate_hello, scratch);
    while (eof_result.output() || eof_result.need_input()) {
        eof_result = eof_decoder.finish_input({}, scratch);
    }
    EXPECT_TRUE(eof_result.complete() != nullptr);
}

TEST(Ruvia, http1_response_body_decoder_chunked_deflate_waits_for_valid_trailers) {
    constexpr std::string_view deflate_hello{
        "\x78\x9c\xcb\x48\xcd\xc9\xc9\x07\x00\x06\x2c\x02\x15", 13};
    const auto head = parse_head("GET", "HTTP/1.1 200 OK\r\nTransfer-Encoding: deflate, chunked");
    ruvia::http1_client_response_body_decoder decoder(head.plan());
    std::string input = "d\r\n";
    input.append(deflate_hello);
    input.append("\r\n0\r\nX-End: yes\r\n\r\nTAIL");
    std::array<char, 8> scratch{};
    std::string body;
    bool saw_trailers = false;
    auto result_value = decoder.decode(input, scratch);
    for (int step = 0; step < 8 && !result_value.complete() && !result_value.protocol_failure(); ++step) {
        input.erase(0, result_value.consumed_bytes());
        if (result_value.output()) {
            body.append(result_value.output()->bytes());
        }
        saw_trailers = saw_trailers || result_value.trailers() != nullptr;
        result_value = decoder.decode(input, scratch);
    }
    if (result_value.output()) {
        body.append(result_value.output()->bytes());
    }
    EXPECT_TRUE(result_value.complete() != nullptr);
    EXPECT_EQ(body, "hello");
    EXPECT_TRUE(saw_trailers);
    EXPECT_EQ(input, "TAIL");
}

TEST(Ruvia, http1_response_body_decoder_terminal_result_is_stable_and_replayed) {
    const auto head = parse_head(
        "GET", "HTTP/1.1 205 Reset Content\r\nTransfer-Encoding: chunked");
    ruvia::http1_client_response_body_decoder decoder(head.plan());
    std::array<char, 4> scratch{};
    const auto failed = decoder.decode("1\r\nxXY0\r\n\r\nTAIL", scratch);
    EXPECT_TRUE(failed.protocol_failure() != nullptr);
    EXPECT_TRUE(failed.protocol_failure()->error() == ruvia::http1_client_response_body_error::invalid_framing);
    const auto replayed = decoder.decode("ignored", scratch);
    EXPECT_TRUE(replayed.protocol_failure() != nullptr);
    EXPECT_TRUE(replayed.protocol_failure()->error() == failed.protocol_failure()->error());
    EXPECT_EQ(replayed.consumed_bytes(), std::size_t{0});
    const auto eof_replay = decoder.finish_input({}, scratch);
    EXPECT_TRUE(eof_replay.protocol_failure() != nullptr);
    EXPECT_TRUE(eof_replay.protocol_failure()->error() == failed.protocol_failure()->error());

    const auto no_body = parse_head("GET", "HTTP/1.1 204 No Content");
    ruvia::http1_client_response_body_decoder complete_decoder(
        no_body.plan());
    const auto first_complete = complete_decoder.decode("TAIL", scratch);
    EXPECT_TRUE(first_complete.complete() != nullptr);
    const auto complete_replay = complete_decoder.decode("ignored", scratch);
    EXPECT_TRUE(complete_replay.complete() != nullptr);
    EXPECT_EQ(complete_replay.consumed_bytes(), std::size_t{0});
    const auto complete_eof_replay = complete_decoder.finish_input({}, scratch);
    EXPECT_TRUE(complete_eof_replay.complete() != nullptr);
    EXPECT_EQ(complete_eof_replay.consumed_bytes(), std::size_t{0});
}

TEST(Ruvia, http1_response_body_decoder_205_gzip_chunked_eof_drains_buffered_terminator) {
    constexpr std::string_view gzip_empty{
        "\x1f\x8b\x08\x00\x00\x00\x00\x00\x02\xff\x03"
        "\x00\x00\x00\x00\x00\x00\x00\x00\x00",
        20};
    const auto head = parse_head(
        "GET", "HTTP/1.1 205 Reset Content\r\nTransfer-Encoding: gzip, chunked");
    ruvia::http1_client_response_body_decoder decoder(head.plan());
    std::string input = "14\r\n";
    input.append(gzip_empty);
    input.append("\r\n0\r\n\r\nTAIL");
    std::array<char, 8> scratch{};
    bool trailers_seen = false;
    auto result_value = decoder.finish_input(input, scratch);
    for (std::size_t step = 0; step < 16 && !result_value.complete() &&
                               !result_value.protocol_failure() && !result_value.decoder_failure();
        ++step) {
        input.erase(0, result_value.consumed_bytes());
        trailers_seen = trailers_seen || result_value.trailers() != nullptr;
        result_value = decoder.finish_input(input, scratch);
    }
    EXPECT_TRUE(result_value.complete() != nullptr);
    EXPECT_TRUE(trailers_seen);
    EXPECT_EQ(input, "TAIL");
}

TEST(Ruvia, http1_response_body_decoder_205_accepts_empty_gzip_and_rejects_decoded_content) {
    constexpr std::string_view gzip_empty{
        "\x1f\x8b\x08\x00\x00\x00\x00\x00\x02\xff\x03"
        "\x00\x00\x00\x00\x00\x00\x00\x00\x00",
        20};
    const auto empty_head = parse_head(
        "GET", "HTTP/1.1 205 Reset Content\r\nTransfer-Encoding: gzip");
    ruvia::http1_client_response_body_decoder empty(empty_head.plan());
    std::array<char, 4> scratch{};
    auto accepted = empty.finish_input(gzip_empty, scratch);
    while (accepted.need_input() != nullptr) {
        accepted = empty.finish_input({}, scratch);
    }
    EXPECT_TRUE(accepted.protocol_failure() == nullptr);
    EXPECT_TRUE(accepted.decoder_failure() == nullptr);
    EXPECT_TRUE(accepted.complete() != nullptr);

    const auto nonempty_head = parse_head(
        "GET", "HTTP/1.1 205 Reset Content\r\nTransfer-Encoding: gzip");
    ruvia::http1_client_response_body_decoder nonempty(
        nonempty_head.plan());
    constexpr std::string_view gzip_hello{
        "\x1f\x8b\x08\x00\x00\x00\x00\x00\x02\xff\xcb\x48\xcd\xc9"
        "\xc9\x07\x00\x86\xa6\x10\x36\x05\x00\x00\x00",
        25};
    // RFC 9110 section 15.3.6 forbids only generating 205 content: it is
    // consumed and discarded, never emitted.
    const auto discarded = nonempty.finish_input(gzip_hello, scratch);
    EXPECT_TRUE(discarded.output() == nullptr);
    EXPECT_TRUE(discarded.complete() != nullptr);
    EXPECT_EQ(discarded.consumed_bytes(), gzip_hello.size());

    const auto fixed_head = parse_head("GET", "HTTP/1.1 205 Reset Content\r\nContent-Length: 2");
    EXPECT_TRUE(fixed_head.plan().discards_content());
    ruvia::http1_client_response_body_decoder fixed(fixed_head.plan());
    const auto fixed_result = fixed.decode("okNEXT", scratch);
    EXPECT_TRUE(fixed_result.complete() != nullptr);
    EXPECT_EQ(fixed_result.consumed_bytes(), std::size_t{2});
    if (const auto* complete = fixed_result.complete()) {
        EXPECT_TRUE(complete->persistence() == ruvia::http1_close_policy::allow_reuse);
    }
}

TEST(Ruvia, http1_response_body_decoder_rejects_truncated_transfer_coding) {
    const auto head = parse_head("GET", "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip");
    ruvia::http1_client_response_body_decoder decoder(head.plan());
    constexpr std::string_view truncated_gzip{"\x1f\x8b\x08", 3};
    std::array<char, 8> scratch{};
    auto result_value = decoder.finish_input(truncated_gzip, scratch);
    while (result_value.output() != nullptr ||
           result_value.need_input() != nullptr) {
        result_value = decoder.finish_input({}, scratch);
    }
    EXPECT_TRUE(result_value.protocol_failure() != nullptr);
    EXPECT_TRUE(result_value.protocol_failure()->error() == ruvia::http1_client_response_body_error::incomplete_body);
}

TEST(Ruvia, http1_response_body_decoder_accepts_empty_transfer_coded_body) {
    std::array<char, 8> scratch{};
    const auto chunked_head = parse_head("GET", "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, chunked");
    ruvia::http1_client_response_body_decoder chunked(
        chunked_head.plan());
    std::string input = "0\r\n\r\nTAIL";
    std::string body;
    auto result_value = chunked.decode(input, scratch);
    for (int step = 0; step < 8 && !result_value.complete() && !result_value.protocol_failure(); ++step) {
        input.erase(0, result_value.consumed_bytes());
        if (result_value.output()) {
            body.append(result_value.output()->bytes());
        }
        result_value = chunked.decode(input, scratch);
    }
    EXPECT_TRUE(result_value.complete() != nullptr);
    input.erase(0, result_value.consumed_bytes());
    EXPECT_TRUE(body.empty());
    EXPECT_EQ(input, "TAIL");

    const auto close_head = parse_head("GET", "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip");
    ruvia::http1_client_response_body_decoder close_delimited(
        close_head.plan());
    auto eof_result = close_delimited.finish_input({}, scratch);
    for (int step = 0; step < 8 && eof_result.need_input() != nullptr; ++step) {
        eof_result = close_delimited.finish_input({}, scratch);
    }
    EXPECT_TRUE(eof_result.complete() != nullptr);
}

TEST(Ruvia, http1_response_body_decoder_accepts_many_small_chunks) {
    constexpr std::size_t chunk_count = 20000;
    std::string wire;
    wire.reserve(chunk_count * 6 + 9);
    for (std::size_t index = 0; index < chunk_count; ++index) {
        wire.append("1\r\nx\r\n");
    }
    wire.append("0\r\n\r\nTAIL");

    const auto head = parse_head("GET", "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked");
    ruvia::http1_client_response_body_decoder decoder(head.plan());
    std::array<char, 64> scratch{};
    std::string_view remaining = wire;
    std::size_t body_bytes = 0;
    bool complete_value = false;
    for (std::size_t step = 0; step <= chunk_count * 2 + 8; ++step) {
        const auto result_value = decoder.decode(remaining, scratch);
        EXPECT_TRUE(result_value.protocol_failure() == nullptr);
        if (result_value.protocol_failure() != nullptr) {
            break;
        }
        remaining.remove_prefix(result_value.consumed_bytes());
        if (const auto* output = result_value.output()) {
            body_bytes += output->bytes().size();
        }
        if (result_value.complete() != nullptr) {
            complete_value = true;
            break;
        }
    }
    EXPECT_TRUE(complete_value);
    EXPECT_EQ(body_bytes, chunk_count);
    EXPECT_EQ(remaining, "TAIL");
}

TEST(Ruvia, http1_response_body_decoder_handles_chunk_extension_and_trailer_over_8k) {
    const auto head = parse_head("GET", "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked");
    ruvia::http1_client_response_body_decoder decoder(head.plan());
    std::string wire = "1;";
    wire.append(9000, 'a');
    wire.append("\r\nz\r\n0\r\nX-Large: ");
    wire.append(9000, 'b');
    wire.append("\r\n\r\nTAIL");
    std::array<char, 4> scratch{};
    bool saw_body = false;
    bool saw_trailers = false;
    for (int step = 0; step < 8; ++step) {
        const auto result_value = decoder.decode(wire, scratch);
        EXPECT_TRUE(result_value.protocol_failure() == nullptr);
        if (result_value.output()) {
            saw_body = result_value.output()->bytes() == "z";
        }
        if (result_value.trailers()) {
            const auto fields = result_value.trailers()->fields();
            saw_trailers = fields.size() == 1 && fields[0].value().size() == 9000;
        }
        wire.erase(0, result_value.consumed_bytes());
        if (saw_trailers) {
            break;
        }
        EXPECT_TRUE(result_value.output() != nullptr || result_value.need_input() != nullptr);
    }
    EXPECT_TRUE(saw_body);
    EXPECT_TRUE(saw_trailers);
    EXPECT_EQ(wire, "TAIL");
}

TEST(Ruvia, http1_response_body_decoder_fragmented_chunk_and_205_empty_chunk_complete) {
    const auto head = parse_head("GET", "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked");
    ruvia::http1_client_response_body_decoder decoder(head.plan());
    std::array<char, 4> scratch{};
    auto first = decoder.decode("3\r\nab", scratch);
    EXPECT_TRUE(first.output() != nullptr);
    EXPECT_EQ(first.output()->bytes(), "ab");
    EXPECT_EQ(first.consumed_bytes(), std::size_t{5});
    auto second = decoder.decode("c\r\n0\r\n\r\nTAIL", scratch);
    EXPECT_TRUE(second.output() != nullptr);
    EXPECT_EQ(second.output()->bytes(), "c");
    auto trailers = decoder.decode("0\r\n\r\nTAIL", scratch);
    EXPECT_TRUE(trailers.trailers() != nullptr);
    EXPECT_EQ(trailers.consumed_bytes(), std::size_t{5});
    EXPECT_TRUE(trailers.trailers()->fields().empty());
    const auto complete_value = decoder.decode("TAIL", scratch);
    EXPECT_TRUE(complete_value.complete() != nullptr);

    const auto reset_head = parse_head("GET", "HTTP/1.1 205 Reset Content\r\nTransfer-Encoding: chunked");
    ruvia::http1_client_response_body_decoder reset(
        reset_head.plan());
    auto empty_chunk = reset.decode("0\r\n\r\nTAIL", scratch);
    EXPECT_TRUE(empty_chunk.trailers() != nullptr);
    EXPECT_TRUE(empty_chunk.trailers()->fields().empty());
    const auto reset_complete = reset.decode("TAIL", scratch);
    EXPECT_TRUE(reset_complete.complete() != nullptr);
}

TEST(Ruvia, http1_response_body_decoder_fixed_length_eof_is_incomplete) {
    const auto head = parse_head("GET", "HTTP/1.1 200 OK\r\nContent-Length: 3");
    ruvia::http1_client_response_body_decoder decoder(head.plan());
    std::array<char, 4> scratch{};
    const auto body = decoder.finish_input("ab", scratch);
    EXPECT_TRUE(body.output() != nullptr);
    const auto failure = decoder.finish_input({}, scratch);
    EXPECT_TRUE(failure.protocol_failure() != nullptr);
    EXPECT_TRUE(failure.protocol_failure()->error() == ruvia::http1_client_response_body_error::incomplete_body);
}

TEST(Ruvia, http1_response_body_decoder_decodes_transfer_coding_stack_in_reverse_order) {
    constexpr std::string_view plain = "HTTP/1 transfer coding stack";
    const auto wire = zlib_deflate_compress(gzip_compress(plain));
    const auto head = parse_head("GET", "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, deflate");
    ruvia::http1_client_response_body_decoder decoder(head.plan());
    std::array<char, 1> scratch{};
    std::string output;
    std::string_view input(wire);
    bool complete_value = false;
    for (std::size_t count = 0; count < 1024 && !complete_value; ++count) {
        const auto result_value = decoder.finish_input(input, scratch);
        input.remove_prefix(result_value.consumed_bytes());
        if (const auto* decoded = result_value.output()) {
            output.append(decoded->bytes());
        }
        if (result_value.protocol_failure() != nullptr || result_value.decoder_failure() != nullptr) {
            EXPECT_TRUE(false);
            return;
        }
        complete_value = result_value.complete() != nullptr;
    }
    EXPECT_TRUE(complete_value);
    EXPECT_TRUE(input.empty());
    EXPECT_EQ(output, std::string(plain));
}

TEST(Ruvia, http1_response_body_decoder_drains_pending_transfer_output_without_wire_input) {
    constexpr std::string_view deflate_aaaa{"\x78\x9c\x73\x04\x02\x00\x02\x8e\x01\x05", 10};
    for (const bool chunked : {false, true}) {
        const auto head = parse_head("GET", chunked
                                                ? "HTTP/1.1 200 OK\r\nTransfer-Encoding: deflate, chunked"
                                                : "HTTP/1.1 200 OK\r\nTransfer-Encoding: deflate");
        ruvia::http1_client_response_body_decoder decoder(head.plan());
        std::array<char, 1> scratch{};
        std::string input(deflate_aaaa.substr(0, 5));
        if (chunked) {
            input = "5\r\n" + input + "\r\n";
        }
        std::string content;
        const auto first = decoder.decode(input, scratch);
        EXPECT_TRUE(first.output() != nullptr);
        if (first.output()) {
            content.append(first.output()->bytes());
        }
        input.erase(0, first.consumed_bytes());
        EXPECT_TRUE(input.empty());
        for (int count = 0; count < 3; ++count) {
            const auto pending = decoder.decode(input, scratch);
            EXPECT_TRUE(pending.output() != nullptr);
            EXPECT_EQ(pending.consumed_bytes(), std::size_t{0});
            if (!pending.output()) {
                break;
            }
            content.append(pending.output()->bytes());
        }
        // All four bytes must be observable before the peer supplies the rest
        // of the encoding or any subsequent chunk framing.
        EXPECT_EQ(content, "AAAA");
        const auto needs_input = decoder.decode(input, scratch);
        EXPECT_TRUE(needs_input.need_input() != nullptr);

        input.assign(deflate_aaaa.substr(5));
        if (chunked) {
            input = "5\r\n" + input + "\r\n0\r\n\r\nTAIL";
        }
        bool complete_value = false;
        for (int count = 0; count < 16 && !complete_value; ++count) {
            const auto result_value = decoder.finish_input(input, scratch);
            EXPECT_TRUE(result_value.protocol_failure() == nullptr);
            EXPECT_TRUE(result_value.decoder_failure() == nullptr);
            EXPECT_TRUE(result_value.need_input() == nullptr);
            if (result_value.output()) {
                content.append(result_value.output()->bytes());
            }
            input.erase(0, result_value.consumed_bytes());
            complete_value = result_value.complete() != nullptr;
            if (result_value.protocol_failure() || result_value.decoder_failure()) {
                break;
            }
        }
        EXPECT_TRUE(complete_value);
        EXPECT_EQ(content, "AAAA");
        EXPECT_EQ(input, chunked ? "TAIL" : "");
    }
}

TEST(Ruvia, http1_response_role_chunk_decoder_preserves_typed_views_and_wire_prefix) {
    constexpr std::string_view wire = "1\r\nx\r\n0\r\nAccept-Ranges: bytes\r\n\r\nNEXT";
    ruvia::http1_chunked_body_decoder decoder({.trailer_message_ = ruvia::http1_trailer_message::response});
    bool zero_budget_rejected = false;
    try {
        (void)decoder.decode(wire, 0);
    } catch (const std::invalid_argument&) {
        zero_budget_rejected = true;
    }
    EXPECT_TRUE(zero_budget_rejected);
    const auto body = decoder.decode(wire, 1);
    EXPECT_TRUE(body.body_chunk() != nullptr);
    EXPECT_EQ(body.consumed_bytes(), std::size_t{6});
    if (const auto* chunk = body.body_chunk()) {
        EXPECT_EQ(chunk->bytes(), "x");
        EXPECT_TRUE(chunk->bytes().data() == wire.data() + 3);
    }
    auto pending = wire.substr(body.consumed_bytes());
    const auto terminal = decoder.decode(pending, 1);
    EXPECT_TRUE(terminal.complete() != nullptr);
    if (const auto* complete = terminal.complete()) {
        EXPECT_EQ(rendered(complete->trailers()), "accept-ranges=bytes;");
    }
    pending.remove_prefix(terminal.consumed_bytes());
    EXPECT_EQ(pending, "NEXT");
    const auto replay = decoder.decode(pending);
    EXPECT_TRUE(replay.complete() != nullptr);
    EXPECT_EQ(replay.consumed_bytes(), std::size_t{0});

    ruvia::http1_chunked_body_decoder invalid;
    const auto failure = invalid.decode("1\r\nxXX");
    EXPECT_TRUE(failure.failure() != nullptr);
    if (const auto* error = failure.failure()) {
        EXPECT_TRUE(error->error() == ruvia::http1_chunk_decode_error::invalid_framing);
    }
    const auto repeated_failure = invalid.decode("NEXT", 1);
    EXPECT_TRUE(repeated_failure.failure() != nullptr);
    EXPECT_EQ(repeated_failure.consumed_bytes(), std::size_t{0});
    if (const auto* error = repeated_failure.failure()) {
        EXPECT_TRUE(error->error() == ruvia::http1_chunk_decode_error::invalid_framing);
    }
}

TEST(Ruvia, http1_response_body_decoder_requires_nonempty_scratch) {
    const auto head = parse_head("GET", "HTTP/1.1 200 OK\r\nContent-Length: 0");
    ruvia::http1_client_response_body_decoder decoder(head.plan());
    bool rejected = false;
    try {
        (void)decoder.decode({}, {});
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    EXPECT_TRUE(rejected);
}

TEST(Ruvia, response_body_decode_continues_draining_after_framing_eof) {
    const std::string plain(128, 'a');
    for (const std::string_view framing : {std::string_view("fixed"), std::string_view("chunked"),
             std::string_view("gzip"), std::string_view("deflate"),
             std::string_view("gzip, chunked"), std::string_view("deflate, chunked")}) {
        const bool fixed = framing == "fixed";
        const bool chunk_framed = framing.ends_with("chunked");
        const bool compressed = framing != "fixed" && framing != "chunked";
        std::string header = "HTTP/1.1 200 OK\r\n";
        header += fixed ? "Content-Length: 128" : "Transfer-Encoding: " + std::string(framing);
        const auto head = ruvia::test::parse_head("GET", header);
        ruvia::http1_client_response_body_decoder decoder(head.plan());
        std::string pending = compressed
                                  ? (framing.starts_with("gzip") ? gzip_compress(plain) : zlib_deflate_compress(plain))
                                  : plain;
        if (chunk_framed) {
            pending = chunked(pending);
        }
        if (fixed || chunk_framed) {
            pending.append("NEXT");
        }
        std::array<char, 1> scratch{};
        std::string decoded;
        auto result = decoder.finish_input(pending, scratch);
        bool complete = false;
        for (std::size_t step = 0; step < plain.size() * 2 + 16; ++step) {
            EXPECT_TRUE(result.need_input() == nullptr);
            EXPECT_TRUE(result.protocol_failure() == nullptr);
            EXPECT_TRUE(result.decoder_failure() == nullptr);
            if (const auto* output = result.output()) {
                decoded.append(output->bytes());
            }
            pending.erase(0, result.consumed_bytes());
            if (result.complete() != nullptr) {
                complete = true;
                break;
            }
            if (result.protocol_failure() != nullptr || result.decoder_failure() != nullptr) {
                break;
            }
            result = decoder.decode(pending, scratch);
        }
        EXPECT_TRUE(complete);
        EXPECT_EQ(decoded, plain);
        EXPECT_EQ(pending, fixed || chunk_framed ? "NEXT" : "");
        if (complete) {
            const auto replay = decoder.decode("NEXT", scratch);
            EXPECT_TRUE(replay.complete() != nullptr);
            EXPECT_EQ(replay.consumed_bytes(), std::size_t{0});
        }
    }
}

TEST(Ruvia, response_body_decode_preserves_incomplete_eof_after_output) {
    const auto head = ruvia::test::parse_head(
        "GET", "HTTP/1.1 200 OK\r\nContent-Length: 2");
    ruvia::http1_client_response_body_decoder decoder(head.plan());
    std::array<char, 1> scratch{};
    const auto output = decoder.finish_input("a", scratch);
    EXPECT_TRUE(output.output() != nullptr);
    EXPECT_EQ(output.consumed_bytes(), std::size_t{1});
    const auto failure = decoder.decode({}, scratch);
    EXPECT_TRUE(failure.protocol_failure() != nullptr);
    if (const auto* error = failure.protocol_failure()) {
        EXPECT_TRUE(error->error() == ruvia::http1_client_response_body_error::incomplete_body);
    }
    const auto replay = decoder.finish_input("NEXT", scratch);
    EXPECT_TRUE(replay.protocol_failure() != nullptr);
    EXPECT_EQ(replay.consumed_bytes(), std::size_t{0});
}

using namespace ruvia::test;

// HTTP/1 client responses: what the head says about the body.

namespace {

}  // namespace

TEST(Ruvia, http_client_response_plan_alternatives_are_exclusive) {
    const ruvia::http_header_view upgrade_headers[] = {
        {"Connection", "Upgrade"},
        {"Upgrade", "websocket"},
    };
    const auto informational = parse_head("GET", "HTTP/1.1 103 Early Hints");
    const auto without_content = parse_head("GET", "HTTP/1.1 204 No Content");
    const auto known_length = parse_head("GET", "HTTP/1.1 200 OK\r\nContent-Length: 1");
    const auto chunked = parse_head("GET", "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked");
    const auto close_delimited = parse_head("GET", "HTTP/1.1 200 OK");
    const auto tunnel = parse_head("CONNECT", "HTTP/1.1 200 Connection Established");
    const auto upgrade = parse_head("GET",
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Connection: Upgrade\r\nUpgrade: websocket",
        http1_close_policy::allow_reuse, upgrade_headers);

    for (const auto* plan : {&informational.plan(), &without_content.plan(), &known_length.plan(),
             &chunked.plan(), &close_delimited.plan(), &tunnel.plan(), &upgrade.plan()}) {
        EXPECT_EQ(active_plan_alternative_count(*plan), std::size_t{1});
    }
}

TEST(Ruvia, http_client_response_plan_owns_content_length_framing) {
    constexpr std::string_view header_value = "HTTP/1.1 200 OK\r\nContent-Length: 5";
    const auto head = parse_head("GET", header_value);
    const auto& known_length = require_known_length(head.plan());
    EXPECT_EQ(known_length.content_length(), std::size_t{5});
    EXPECT_TRUE(known_length.requires_body_consumption());
    EXPECT_TRUE(known_length.persistence() == http1_close_policy::allow_reuse);
    EXPECT_EQ(head.consumed_bytes(), header_value.size() + 4);

    const auto empty = parse_head("GET", "HTTP/1.1 200 OK\r\nContent-Length: 0");
    EXPECT_TRUE(!require_known_length(empty.plan()).requires_body_consumption());
}

TEST(Ruvia, http_client_content_length_combined_and_repeated_equal_values) {
    const auto combined = parse_head("GET", "HTTP/1.1 200 OK\r\nContent-Length: 5, 5");
    EXPECT_EQ(require_known_length(combined.plan()).content_length(), std::size_t{5});

    const auto repeated =
        parse_head("GET", "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Length: 5");
    EXPECT_EQ(require_known_length(repeated.plan()).content_length(), std::size_t{5});

    EXPECT_TRUE(parse_fails("GET", "HTTP/1.1 200 OK\r\nContent-Length: 5, 6"));
    EXPECT_TRUE(parse_fails("GET", "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Length: 6"));
}

TEST(Ruvia, http_client_response_unfolds_fields_before_framing_and_owns_values) {
    ruvia::http_client_request_view request;
    request.method_ = "GET";
    http1_client_response_parser parser =
        exchange_parser(ruvia::http_origin_view::https({.host_ = "example.test"}), request);
    std::string wire =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 0,\r\n \t0\r\n"
        "X-Trace: one \t\r\n\t two\r\n\r\n";
    for (std::size_t size = 1; size < wire.size(); ++size) {
        const auto partial = parser.parse(std::string_view(wire).substr(0, size));
        EXPECT_TRUE(partial.need_more() != nullptr);
    }

    const auto result_value = parser.parse(wire);
    const auto* parsed = result_value.parsed();
    EXPECT_TRUE(parsed != nullptr);
    if (parsed == nullptr) {
        return;
    }
    EXPECT_EQ(parsed->consumed_bytes(), wire.size());
    EXPECT_EQ(require_known_length(parsed->plan()).content_length(), std::size_t{0});
    wire.assign(wire.size(), 'x');
    const auto headers = parsed->head().headers();
    EXPECT_EQ(headers.size(), std::size_t{2});
    if (headers.size() == 2) {
        EXPECT_EQ(headers[0].name(), "Content-Length");
        EXPECT_EQ(headers[0].value(), "0, 0");
        EXPECT_EQ(headers[1].name(), "X-Trace");
        EXPECT_EQ(headers[1].value(), "one two");
    }
}

TEST(Ruvia, http_client_response_unfolding_preserves_framing_validation) {
    const auto transfer = parse_head("GET",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip,\r\n\tchunked");
    const auto& chunked = require_chunked(transfer.plan());
    EXPECT_EQ(chunked.transfer_codings().size(), std::size_t{1});
    EXPECT_TRUE(chunked.transfer_codings()[0] == ruvia::http_transfer_coding::gzip);
    EXPECT_EQ(transfer.head().headers().front().value(), "gzip, chunked");

    const std::pair<std::string_view, http1_client_response_parse_error> invalid[] = {
        {"Content-Length: 4\r\n 2", http1_client_response_parse_error::invalid_content_length},
        {"Content-Length: 4,\r\n 2", http1_client_response_parse_error::conflicting_content_length},
        {"Transfer-Encoding: chunked,\r\n gzip", http1_client_response_parse_error::invalid_transfer_encoding},
        {"Content-Length: 0\r\nTransfer-Encoding: gzip,\r\n chunked",
            http1_client_response_parse_error::content_length_and_transfer_encoding},
        {" Content-Length: 0", http1_client_response_parse_error::invalid_header},
        {"\tContent-Length: 0\r\nX-Trace: one\r\n two", http1_client_response_parse_error::invalid_header},
    };
    for (const auto& [fields, expected_error] : invalid) {
        const std::string head = "HTTP/1.1 200 OK\r\n" + std::string(fields);
        EXPECT_TRUE(parse_failure_error("GET", head) == expected_error);
    }
}

TEST(Ruvia, http_client_response_plan_owns_chunked_framing_and_reuse) {
    const auto head = parse_head("GET", "HTTP/1.1 200 OK\r\nTransfer-Encoding: Chunked");
    const auto& chunked = require_chunked(head.plan());
    EXPECT_TRUE(chunked.transfer_codings().empty());
    EXPECT_TRUE(chunked.persistence() == http1_close_policy::allow_reuse);
}

TEST(Ruvia, http_client_transfer_coding_before_final_chunked_is_typed) {
    const auto combined = parse_head("GET", "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, chunked");
    const auto& combined_chunked = require_chunked(combined.plan());
    EXPECT_EQ(combined_chunked.transfer_codings().size(), std::size_t{1});
    EXPECT_TRUE(combined_chunked.transfer_codings()[0] == ruvia::http_transfer_coding::gzip);
    EXPECT_TRUE(combined_chunked.persistence() == http1_close_policy::allow_reuse);

    // Transfer-Encoding is list-based: split field lines retain wire order.
    const auto split = parse_head("GET",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: deflate\r\n"
        "Transfer-Encoding: chunked");
    const auto& split_chunked = require_chunked(split.plan());
    EXPECT_EQ(split_chunked.transfer_codings().size(), std::size_t{1});
    EXPECT_TRUE(split_chunked.transfer_codings()[0] == ruvia::http_transfer_coding::deflate);

    const auto repeated = parse_head("GET",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip\r\n"
        "Transfer-Encoding: deflate, chunked");
    const auto& repeated_chunked = require_chunked(repeated.plan());
    EXPECT_EQ(repeated_chunked.transfer_codings().size(), std::size_t{2});
    EXPECT_TRUE(repeated_chunked.transfer_codings()[0] == ruvia::http_transfer_coding::gzip);
    EXPECT_TRUE(repeated_chunked.transfer_codings()[1] == ruvia::http_transfer_coding::deflate);
}

TEST(Ruvia, http_client_transfer_encoding_ignores_empty_recipient_list_items) {
    for (const std::string_view fields : {
             "Transfer-Encoding: , chunked",
             "Transfer-Encoding: chunked,",
             "Transfer-Encoding: , , chunked, ,",
             "Transfer-Encoding: gzip, , chunked",
             "Transfer-Encoding: \r\nTransfer-Encoding: chunked",
             "Transfer-Encoding: chunked\r\nTransfer-Encoding: , ,",
             "Transfer-Encoding: ,\r\nTransfer-Encoding: gzip\r\nTransfer-Encoding: \r\nTransfer-Encoding: chunked\r\nTransfer-Encoding: ,",
         }) {
        const std::string head = "HTTP/1.1 200 OK\r\n" + std::string(fields) + "\r\n\r\n";
        const std::string wire = head + "0\r\n\r\nHTTP/1.1 204 No Content\r\n\r\n";
        const auto result = parse_wire("GET", wire);
        const auto* parsed = result.parsed();
        EXPECT_TRUE(parsed != nullptr);
        if (parsed == nullptr) {
            continue;
        }
        const auto& chunked = require_chunked(parsed->plan());
        EXPECT_TRUE(chunked.persistence() == http1_close_policy::allow_reuse);
        const bool gzip = fields.find("gzip") != std::string_view::npos;
        EXPECT_EQ(chunked.transfer_codings().size(), gzip ? std::size_t{1} : std::size_t{0});
        if (gzip && !chunked.transfer_codings().empty()) {
            EXPECT_TRUE(chunked.transfer_codings()[0] == ruvia::http_transfer_coding::gzip);
        }
        EXPECT_EQ(parsed->consumed_bytes(), head.size());
        EXPECT_EQ(std::string_view(wire).substr(parsed->consumed_bytes()), "0\r\n\r\nHTTP/1.1 204 No Content\r\n\r\n");
    }

    const auto non_chunked = parse_head("GET",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: , gzip, ,\r\nTransfer-Encoding: ");
    const auto& close_delimited = require_close_delimited(non_chunked.plan());
    EXPECT_EQ(close_delimited.transfer_codings().size(), std::size_t{1});
    EXPECT_TRUE(close_delimited.transfer_codings()[0] == ruvia::http_transfer_coding::gzip);

    // Empty items do not make the field absent or relax framing conflicts.
    const auto empty = parse_head("GET", "HTTP/1.1 200 OK\r\nTransfer-Encoding: , ,");
    EXPECT_TRUE(require_close_delimited(empty.plan()).transfer_codings().empty());
    EXPECT_TRUE(parse_failure_error("GET",
                    "HTTP/1.1 200 OK\r\nTransfer-Encoding: , ,\r\nContent-Length: 0") ==
                http1_client_response_parse_error::content_length_and_transfer_encoding);
}

TEST(Ruvia, http_client_non_chunked_transfer_coding_is_close_delimited) {
    const auto head = parse_head("GET", "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip");
    const auto& close_delimited = require_close_delimited(head.plan());
    EXPECT_EQ(close_delimited.transfer_codings().size(), std::size_t{1});
}

TEST(Ruvia, http_client_rejects_invalid_or_unsupported_transfer_coding) {
    EXPECT_TRUE(parse_fails("GET", "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked, gzip"));
    EXPECT_TRUE(parse_fails("GET", "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked;foo=bar"));
    EXPECT_TRUE(parse_fails("GET", "HTTP/1.1 200 OK\r\nTransfer-Encoding: compress, chunked"));
    const auto malformed_after_unknown = parse_result("GET",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: compress\r\n"
        "Transfer-Encoding: deflate, chunked, gzip");
    EXPECT_TRUE(malformed_after_unknown.failure() != nullptr);
    if (const auto* failure = malformed_after_unknown.failure()) {
        EXPECT_TRUE(*failure == http1_client_response_parse_error::invalid_transfer_encoding);
    }
    const auto stacked = parse_head(
        "GET", "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, deflate, chunked");
    const auto& stacked_chunked = require_chunked(stacked.plan());
    EXPECT_EQ(stacked_chunked.transfer_codings().size(), std::size_t{2});
    EXPECT_TRUE(stacked_chunked.transfer_codings()[0] == ruvia::http_transfer_coding::gzip);
    EXPECT_TRUE(stacked_chunked.transfer_codings()[1] == ruvia::http_transfer_coding::deflate);
}

TEST(Ruvia, http_client_content_length_and_transfer_encoding_rejected_for_body) {
    EXPECT_TRUE(parse_fails("GET",
        "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n"
        "Transfer-Encoding: chunked"));
    EXPECT_TRUE(parse_fails("GET",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n"
        "Content-Length: 5"));
}

TEST(Ruvia, http_client_no_body_precedence_ignores_framing_fields) {
    const auto head = parse_head("HEAD",
        "HTTP/1.1 200 OK\r\nContent-Length: 7\r\n"
        "Transfer-Encoding: custom-coding");
    const auto& without_content = require_without_content(head.plan());
    EXPECT_TRUE(without_content.persistence() == http1_close_policy::allow_reuse);

    const auto not_modified = parse_head("GET",
        "HTTP/1.1 304 Not Modified\r\nContent-Length: 7\r\n"
        "Transfer-Encoding: custom-coding");
    EXPECT_TRUE(not_modified.plan().without_content() != nullptr);

    const auto no_content = parse_head("GET", "HTTP/1.1 204 No Content");
    EXPECT_TRUE(no_content.plan().without_content() != nullptr);
}

TEST(Ruvia, http_client_trailer_declaration_metadata_preserves_framed_body_and_next_response) {
    for (const std::string_view declaration :
        {"Content-Length", "Host", "Bad Field", "\"x-name\"", ",", "X-One,,X-Two"}) {
        for (const bool chunked : {false, true}) {
            std::string head = "HTTP/1.1 200 OK\r\nTrailer: \t";
            head.append(declaration);
            head.append(" \t\r\n");
            head.append(chunked ? "Transfer-Encoding: chunked\r\n\r\n" : "Content-Length: 3\r\n\r\n");
            const std::string wire = head + (chunked ? "3\r\nabc\r\n0\r\n\r\n" : "abc") +
                                     "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok";
            const auto result = parse_wire("GET", wire);
            EXPECT_TRUE(result.parsed() != nullptr);
            if (const auto* parsed = result.parsed()) {
                EXPECT_EQ(parsed->head().headers().front().value(), declaration);
                EXPECT_EQ(parsed->consumed_bytes(), head.size());
                if (chunked) {
                    EXPECT_TRUE(parsed->plan().chunked() != nullptr);
                } else {
                    EXPECT_EQ(require_known_length(parsed->plan()).content_length(), std::size_t{3});
                }
                ruvia::http1_client_response_body_decoder decoder(
                    parsed->plan());
                std::array<char, 8> scratch{};
                auto remaining = std::string_view(wire).substr(parsed->consumed_bytes());
                std::string body;
                bool complete = false;
                for (int step = 0; step < 8; ++step) {
                    const auto decoded = decoder.decode(remaining, scratch);
                    EXPECT_TRUE(decoded.protocol_failure() == nullptr);
                    EXPECT_TRUE(decoded.decoder_failure() == nullptr);
                    remaining.remove_prefix(decoded.consumed_bytes());
                    if (const auto* output = decoded.output()) {
                        body.append(output->bytes());
                    }
                    if (decoded.complete()) {
                        complete = true;
                        break;
                    }
                }
                EXPECT_TRUE(complete);
                EXPECT_EQ(body, "abc");
                const auto next = parse_wire("GET", remaining);
                EXPECT_TRUE(next.parsed() != nullptr);
                if (const auto* next_head = next.parsed()) {
                    EXPECT_EQ(require_known_length(next_head->plan()).content_length(), std::size_t{2});
                    EXPECT_EQ(remaining.substr(next_head->consumed_bytes()), "ok");
                }
            }
        }
    }
}

TEST(Ruvia, http_client_bodyless_trailer_declarations_preserve_message_boundaries) {
    for (const auto& [method, status_line] : {
             std::pair{std::string_view("HEAD"), std::string_view("HTTP/1.1 200 OK")},
             std::pair{std::string_view("GET"), std::string_view("HTTP/1.1 304 Not Modified")},
             std::pair{std::string_view("GET"), std::string_view("HTTP/1.1 204 No Content")},
         }) {
        ruvia::http_client_request_view request;
        request.method_ = method;
        for (const bool transfer_encoding : {false, true}) {
            std::string head(status_line);
            head.append("\r\n");
            if (transfer_encoding) {
                head.append("Transfer-Encoding: chunked\r\n");
            }
            // RFC 9110 section 6.6.2 makes Trailer a declaration, not a promise
            // that trailer fields follow. RFC 9112 section 6.3 ends both of
            // these responses at the empty line regardless of their fields.
            head.append("Trailer: Content-Length, Bad Field, \"x-name\"\r\n\r\n");
            const std::string wire = head + "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok";
            http1_client_response_parser parser =
                exchange_parser(ruvia::http_origin_view::https({.host_ = "example.test"}), request);
            for (std::size_t size = 1; size < head.size(); ++size) {
                const auto partial = parser.parse(std::string_view(wire).substr(0, size));
                EXPECT_TRUE(partial.need_more() != nullptr);
            }
            const auto result = parser.parse(wire);
            const auto* parsed = result.parsed();
            EXPECT_TRUE(parsed != nullptr);
            if (parsed == nullptr) {
                continue;
            }
            EXPECT_TRUE(parsed->plan().without_content() != nullptr);
            EXPECT_EQ(parsed->consumed_bytes(), head.size());
            EXPECT_EQ(parsed->head().headers().back().name(), "Trailer");
            EXPECT_EQ(parsed->head().headers().back().value(), "Content-Length, Bad Field, \"x-name\"");
            const auto following = std::string_view(wire).substr(parsed->consumed_bytes());
            const auto next = parse_wire("GET", following);
            EXPECT_TRUE(next.parsed() != nullptr);
            if (const auto* next_head = next.parsed()) {
                EXPECT_EQ(require_known_length(next_head->plan()).content_length(), std::size_t{2});
                EXPECT_EQ(following.substr(next_head->consumed_bytes()), "ok");
            }
        }
    }

    constexpr std::string_view tunnel_wire =
        "HTTP/1.1 200 Connection Established\r\nTrailer: Content-Length, Bad Field\r\n\r\ntunnel bytes";
    const auto tunnel = parse_wire("CONNECT", tunnel_wire);
    EXPECT_TRUE(tunnel.parsed() != nullptr);
    if (const auto* parsed = tunnel.parsed()) {
        EXPECT_TRUE(parsed->plan().connect_tunnel() != nullptr);
        EXPECT_EQ(tunnel_wire.substr(parsed->consumed_bytes()), "tunnel bytes");
    }

    const ruvia::http_header_view upgrade_headers[] = {{"Connection", "Upgrade"}, {"Upgrade", "websocket"}};
    constexpr std::string_view upgrade_wire =
        "HTTP/1.1 101 Switching Protocols\r\nConnection: Upgrade\r\nUpgrade: websocket\r\n"
        "Trailer: X-Checksum\r\n\r\nprotocol bytes";
    const auto upgrade = parse_wire("GET", upgrade_wire, http1_close_policy::allow_reuse, upgrade_headers);
    EXPECT_TRUE(upgrade.parsed() != nullptr);
    if (const auto* parsed = upgrade.parsed()) {
        EXPECT_TRUE(parsed->plan().protocol_upgrade() != nullptr);
        EXPECT_EQ(upgrade_wire.substr(parsed->consumed_bytes()), "protocol bytes");
    }
}

TEST(Ruvia, http_client_auxiliary_response_fields_do_not_change_content_framing) {
    constexpr std::string_view auxiliary =
        "Upgrade: websocket\r\nTrailer: X-Checksum\r\nTE: trailers\r\n";
    for (const std::string_view framing : {
             std::string_view("Content-Length: 2\r\n"),
             std::string_view("Transfer-Encoding: chunked\r\n"),
             std::string_view("Transfer-Encoding: gzip\r\n"),
             std::string_view{},
         }) {
        std::string head("HTTP/1.1 200 OK\r\n");
        head.append(auxiliary);
        head.append(framing);
        head.append("\r\n");
        const std::string wire = head + "body bytes";
        const auto result = parse_wire("GET", wire);
        EXPECT_TRUE(result.parsed() != nullptr);
        if (const auto* parsed = result.parsed()) {
            EXPECT_EQ(parsed->consumed_bytes(), head.size());
            EXPECT_EQ(std::string_view(wire).substr(parsed->consumed_bytes()), "body bytes");
            if (framing.starts_with("Content-Length")) {
                const auto& body = require_known_length(parsed->plan());
                EXPECT_EQ(body.content_length(), std::size_t{2});
                EXPECT_TRUE(body.persistence() == http1_close_policy::allow_reuse);
            } else if (framing == "Transfer-Encoding: chunked\r\n") {
                EXPECT_TRUE(require_chunked(parsed->plan()).persistence() == http1_close_policy::allow_reuse);
            } else {
                EXPECT_TRUE(parsed->plan().close_delimited() != nullptr);
            }
        }
    }
    for (const std::string_view malformed : {
             std::string_view("Content-Length: invalid\r\n"),
             std::string_view("Content-Length: 2\r\nTransfer-Encoding: chunked\r\n"),
             std::string_view("Transfer-Encoding: chunked, gzip\r\n"),
             std::string_view("Transfer-Encoding: custom-coding\r\n"),
         }) {
        std::string head("HTTP/1.1 200 OK\r\n");
        head.append(auxiliary);
        head.append(malformed);
        EXPECT_TRUE(parse_fails("GET", head));
    }
}

TEST(Ruvia, http_client_informational_auxiliary_fields_preserve_the_final_response_boundary) {
    for (const std::string_view status_line :
        {"HTTP/1.1 100 Continue\r\n", "HTTP/1.1 103 Early Hints\r\n"}) {
        ruvia::http_client_request_view request;
        std::string head(status_line);
        head.append(
            "Content-Length: ignored\r\nTransfer-Encoding: ignored\r\n"
            "Trailer: Content-Length, Bad Field\r\nTE: trailers\r\nUpgrade: websocket\r\n"
            "Content-Type: text/plain\r\nContent-Type: application/json\r\n"
            "Server: first\r\nServer: second\r\n\r\n");
        const std::string wire = head + "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok";
        http1_client_response_parser parser =
            exchange_parser(ruvia::http_origin_view::https({.host_ = "example.test"}), request);
        for (std::size_t size = 1; size < head.size(); ++size) {
            const auto partial = parser.parse(std::string_view(wire).substr(0, size));
            EXPECT_TRUE(partial.need_more() != nullptr);
        }
        const auto interim = parser.parse(wire);
        EXPECT_TRUE(interim.parsed() != nullptr);
        if (const auto* parsed = interim.parsed()) {
            EXPECT_TRUE(parsed->plan().informational() != nullptr);
            if (const auto* informational = parsed->plan().informational()) {
                EXPECT_TRUE(informational->persistence() == http1_close_policy::allow_reuse);
            }
            EXPECT_EQ(parsed->consumed_bytes(), head.size());
            EXPECT_EQ(parsed->head().headers().size(), std::size_t{9});
            const auto final_response = parser.parse(std::string_view(wire).substr(parsed->consumed_bytes()));
            EXPECT_TRUE(final_response.parsed() != nullptr);
            if (const auto* final_head = final_response.parsed()) {
                EXPECT_EQ(require_known_length(final_head->plan()).content_length(), std::size_t{2});
                EXPECT_TRUE(require_known_length(final_head->plan()).persistence() == http1_close_policy::allow_reuse);
            }
        }
        head.insert(head.size() - 2, "Connection: close\r\n");
        const auto closing = parse_head("GET", head);
        EXPECT_TRUE(closing.plan().informational() != nullptr);
        if (const auto* informational = closing.plan().informational()) {
            // A 1xx cannot close the connection; its final response decides.
            EXPECT_TRUE(informational->persistence() == http1_close_policy::allow_reuse);
        }
    }
}

TEST(Ruvia, http_client_no_body_content_length_metadata_does_not_fail_the_response) {
    for (const auto& [method, response] : {std::pair{"HEAD", "HTTP/1.1 200 OK\r\nContent-Length: invalid"},
             std::pair{"HEAD", "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Length: 6"},
             std::pair{"GET", "HTTP/1.1 304 Not Modified\r\nContent-Length: invalid"},
             std::pair{"GET", "HTTP/1.1 304 Not Modified\r\nContent-Length: 5, 6"}}) {
        const auto parsed = parse_head(method, response);
        EXPECT_TRUE(parsed.plan().without_content() != nullptr);
        EXPECT_FALSE(parsed.head().content_length().has_value());
    }

    const auto repeated =
        parse_head("GET", "HTTP/1.1 304 Not Modified\r\nContent-Length: 5\r\nContent-Length: 5");
    EXPECT_TRUE(repeated.plan().without_content() != nullptr);
    EXPECT_EQ(repeated.head().content_length(), std::optional<std::uint64_t>(5));
    const auto head = parse_head("HEAD", "HTTP/1.1 200 OK\r\nContent-Length: 42");
    EXPECT_EQ(head.head().content_length(), std::optional<std::uint64_t>(42));
    const auto invalid_then_valid = parse_head(
        "HEAD", "HTTP/1.1 200 OK\r\nContent-Length: invalid\r\nContent-Length: 42");
    EXPECT_FALSE(invalid_then_valid.head().content_length().has_value());
}

TEST(Ruvia, http_client_bodyless_responses_ignore_framing_fields) {
    // RFC 9112 section 6.3 rule 1: 1xx, 204, 304 and HEAD responses end at the
    // header section whatever Content-Length or Transfer-Encoding they carry.
    for (const std::string_view head_value : {
             std::string_view("HTTP/1.1 204 No Content\r\nContent-Length: 0"),
             std::string_view("HTTP/1.1 204 No Content\r\nContent-Length: 5"),
             std::string_view("HTTP/1.1 204 No Content\r\nContent-Length: invalid"),
             std::string_view("HTTP/1.1 204 No Content\r\nTransfer-Encoding: chunked"),
             std::string_view("HTTP/1.1 204 No Content\r\nTransfer-Encoding: custom-coding"),
         }) {
        const auto head = parse_head("GET", head_value);
        const auto& without_content = require_without_content(head.plan());
        EXPECT_TRUE(without_content.persistence() == http1_close_policy::allow_reuse);
        EXPECT_EQ(head.consumed_bytes(), head_value.size() + 4);
    }
}

TEST(Ruvia, http_client_bodyless_framing_fields_do_not_desynchronize_the_connection) {
    // A smuggled body after a bodyless head is the next response, not content.
    constexpr std::string_view no_content_head =
        "HTTP/1.1 204 No Content\r\nContent-Length: 5\r\nTransfer-Encoding: chunked\r\n\r\n";
    std::string wire(no_content_head);
    wire.append("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok");
    const auto no_content = parse_wire("GET", wire);
    EXPECT_TRUE(no_content.parsed() != nullptr);
    if (const auto* parsed = no_content.parsed()) {
        EXPECT_TRUE(parsed->plan().without_content() != nullptr);
        EXPECT_EQ(parsed->consumed_bytes(), no_content_head.size());
        // The next exchange starts exactly after the empty line.
        const auto next = parse_wire("GET", std::string_view(wire).substr(parsed->consumed_bytes()));
        EXPECT_TRUE(next.parsed() != nullptr);
        if (const auto* next_head = next.parsed()) {
            EXPECT_EQ(require_known_length(next_head->plan()).content_length(), std::size_t{2});
        }
    }

    ruvia::http_client_request_view request;
    request.method_ = "GET";
    http1_client_response_parser parser =
        exchange_parser(ruvia::http_origin_view::https({.host_ = "example.test"}), request);
    constexpr std::string_view interim_head = "HTTP/1.1 103 Early Hints\r\nContent-Length: 5\r\n\r\n";
    std::string interim_wire(interim_head);
    interim_wire.append("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok");
    const auto interim = parser.parse(interim_wire);
    EXPECT_TRUE(interim.parsed() != nullptr);
    if (interim.parsed() == nullptr) {
        return;
    }
    EXPECT_TRUE(interim.parsed()->plan().informational() != nullptr);
    EXPECT_EQ(interim.parsed()->consumed_bytes(), interim_head.size());
    const auto final_response =
        parser.parse(std::string_view(interim_wire).substr(interim.parsed()->consumed_bytes()));
    EXPECT_TRUE(final_response.parsed() != nullptr);
    if (final_response.parsed() != nullptr) {
        EXPECT_EQ(require_known_length(final_response.parsed()->plan()).content_length(), std::size_t{2});
    }
}

TEST(Ruvia, http_client_205_owns_zero_content_framing) {
    const auto zero_length = parse_head("GET", "HTTP/1.1 205 Reset Content\r\nContent-Length: 0");
    EXPECT_TRUE(zero_length.plan().discards_content());
    const auto* zero_length_body = zero_length.plan().known_length();
    EXPECT_TRUE(zero_length_body != nullptr);
    if (zero_length_body == nullptr) {
        return;
    }
    EXPECT_TRUE(!zero_length_body->requires_body_consumption());
    EXPECT_TRUE(zero_length_body->persistence() == http1_close_policy::allow_reuse);
    EXPECT_EQ(active_plan_alternative_count(zero_length.plan()), std::size_t{1});

    // RFC 9110 section 15.3.6 binds the sender: delimited content is consumed.
    const auto with_content = parse_head("GET", "HTTP/1.1 205 Reset Content\r\nContent-Length: 3");
    EXPECT_TRUE(with_content.plan().discards_content());
    EXPECT_EQ(require_known_length(with_content.plan()).content_length(), std::size_t{3});
    EXPECT_TRUE(require_known_length(with_content.plan()).persistence() == http1_close_policy::allow_reuse);

    const auto chunked = parse_head("GET",
        "HTTP/1.1 205 Reset Content\r\n"
        "Transfer-Encoding: gzip, chunked");
    const auto& chunked_zero = chunked.plan();
    EXPECT_TRUE(chunked_zero.discards_content());
    EXPECT_TRUE(chunked_zero.chunked() != nullptr);
    EXPECT_TRUE(chunked_zero.close_delimited() == nullptr);
    if (chunked_zero.chunked() != nullptr) {
        EXPECT_EQ(chunked_zero.chunked()->transfer_codings().size(), std::size_t{1});
        EXPECT_TRUE(chunked_zero.chunked()->transfer_codings()[0] == ruvia::http_transfer_coding::gzip);
    }
    EXPECT_EQ(active_plan_alternative_count(chunked.plan()), std::size_t{1});

    const auto transfer_coded =
        parse_head("GET", "HTTP/1.1 205 Reset Content\r\nTransfer-Encoding: gzip");
    const auto& transfer_coded_zero = transfer_coded.plan();
    EXPECT_TRUE(transfer_coded_zero.discards_content());
    EXPECT_TRUE(transfer_coded_zero.close_delimited() != nullptr);
    if (transfer_coded_zero.close_delimited() != nullptr) {
        EXPECT_EQ(transfer_coded_zero.close_delimited()->transfer_codings().size(), std::size_t{1});
        EXPECT_TRUE(transfer_coded_zero.close_delimited()->transfer_codings()[0] ==
                    ruvia::http_transfer_coding::gzip);
    }

    const auto unframed = parse_head("GET", "HTTP/1.1 205 Reset Content");
    const auto& close_zero = unframed.plan();
    EXPECT_TRUE(close_zero.discards_content());
    EXPECT_TRUE(close_zero.close_delimited() != nullptr);
    EXPECT_TRUE(close_zero.chunked() == nullptr);
    EXPECT_EQ(active_plan_alternative_count(unframed.plan()), std::size_t{1});

    const auto connect =
        parse_head("CONNECT", "HTTP/1.1 205 Reset Content\r\nContent-Length: invalid");
    EXPECT_TRUE(connect.plan().connect_tunnel() != nullptr);
    EXPECT_TRUE(!connect.plan().discards_content());
}

// RFC 9110 section 15: a received 000..099 or 600..999 is processed as a 5xx
// response; the status-code is still exactly three digits.
TEST(Ruvia, http_client_processes_out_of_range_status_as_server_error) {
    for (const auto& [line, value] : {std::pair{std::string_view("HTTP/1.1 600 Odd"), 600},
             std::pair{std::string_view("HTTP/1.1 999 "), 999}, std::pair{std::string_view("HTTP/1.1 099 Low"), 99},
             std::pair{std::string_view("HTTP/1.1 000 "), 0}}) {
        std::string response(line);
        response.append("\r\nContent-Length: 2");
        const auto head = parse_head("GET", response);
        EXPECT_TRUE(head.head().status() == ruvia::http_status::internal_server_error);
        EXPECT_EQ(head.head().received_status().value(), value);
        EXPECT_EQ(require_known_length(head.plan()).content_length(), std::size_t{2});
    }
    for (const std::string_view line : {"HTTP/1.1 -00 Signed", "HTTP/1.1 +99 Signed", "HTTP/1.1 0x1 Hex"}) {
        std::string response(line);
        response.append("\r\nContent-Length: 0");
        EXPECT_TRUE(parse_fails("GET", response));
    }
}

// RFC 9112 section 6.3 rule 1 precedes rule 5: HEAD and 304 responses end at
// the header section, so an invalid Content-Length does not fail them.
TEST(Ruvia, http_client_ignores_invalid_content_length_without_content) {
    for (const auto& [method, response] : {std::pair{"HEAD", "HTTP/1.1 200 OK\r\nContent-Length: abc"},
             std::pair{"GET", "HTTP/1.1 304 Not Modified\r\nContent-Length: 1, 2"}}) {
        const auto head = parse_head(method, response);
        EXPECT_TRUE(head.plan().without_content() != nullptr);
    }
}

TEST(Ruvia, http_client_final_after_continue_does_not_cancel_released_content) {
    ruvia::http_client_request_view request;
    request.method_ = "POST";
    request.content_ = ruvia::http_client_request_content_view::bytes("payload");
    http1_client_response_parser parser = exchange_parser(
        ruvia::http_origin_view::https({.host_ = "example.test"}), request,
        http1_client_request_wire_policy{
            .expectation_ = ruvia::http_client_request_expectation::continue_value});
    const auto continue_response = parser.parse("HTTP/1.1 100 Continue\r\n\r\n");
    EXPECT_TRUE(continue_response.parsed() != nullptr);
    if (continue_response.parsed() != nullptr) {
        EXPECT_TRUE(continue_response.parsed()->plan().request_content_signal() ==
                    http_client_request_content_signal::continue_value);
    }

    const auto final_response = parser.parse("HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n");
    EXPECT_TRUE(final_response.parsed() != nullptr);
    if (final_response.parsed() != nullptr) {
        EXPECT_TRUE(!final_response.parsed()->plan().request_content_signal());
        EXPECT_TRUE(require_known_length(final_response.parsed()->plan()).persistence() ==
                    http1_close_policy::allow_reuse);
    }
}

TEST(Ruvia, http_client_closing_final_stops_unfinished_request_content) {
    ruvia::http_client_request_view request;
    request.method_ = "POST";
    request.content_ = ruvia::http_client_request_content_view::bytes("payload");
    http1_client_response_parser parser = exchange_parser(
        ruvia::http_origin_view::https({.host_ = "example.test"}), request,
        http1_client_request_wire_policy{
            .expectation_ = ruvia::http_client_request_expectation::continue_value});
    const auto continue_response = parser.parse("HTTP/1.1 100 Continue\r\n\r\n");
    EXPECT_TRUE(continue_response.parsed() != nullptr);

    const auto closing_final = parser.parse(
        "HTTP/1.1 413 Content Too Large\r\n"
        "Connection: close\r\nContent-Length: 0\r\n\r\n");
    EXPECT_TRUE(closing_final.parsed() != nullptr);
    if (closing_final.parsed() != nullptr) {
        EXPECT_TRUE(closing_final.parsed()->plan().request_content_signal() ==
                    http_client_request_content_signal::exchange_complete);
        EXPECT_TRUE(require_known_length(closing_final.parsed()->plan()).persistence() ==
                    http1_close_policy::close_after_response);
    }
}

TEST(Ruvia, http_client_response_preserves_typed_protocol_version) {
    const auto http10 = parse_head("GET", "HTTP/1.0 204 No Content");
    const auto http11 = parse_head("GET", "HTTP/1.1 204 No Content");

    EXPECT_TRUE(http10.head().protocol_version() == http_protocol_version::http10);
    EXPECT_TRUE(http11.head().protocol_version() == http_protocol_version::http11);
}

TEST(Ruvia, http_client_unframed_body_response_is_close_delimited) {
    const auto head = parse_head("GET", "HTTP/1.1 200 OK");
    EXPECT_TRUE(head.plan().close_delimited() != nullptr);
}

TEST(Ruvia, http_client_http10_transfer_encoding_is_faulty_framing_only_with_content) {
    // RFC 9112 section 6.1: the codings are ignored; the message falls back to
    // Content-Length, else close delimiting, and closes after it.
    const auto unframed = parse_head("GET", "HTTP/1.0 200 OK\r\nTransfer-Encoding: chunked");
    const auto* close_delimited = unframed.plan().close_delimited();
    EXPECT_TRUE(close_delimited != nullptr);
    if (close_delimited != nullptr) {
        EXPECT_TRUE(close_delimited->transfer_codings().empty());
    }
    const auto framed = parse_head(
        "GET", "HTTP/1.0 200 OK\r\nTransfer-Encoding: chunked\r\nContent-Length: 5");
    EXPECT_EQ(require_known_length(framed.plan()).content_length(), std::size_t{5});
    EXPECT_TRUE(require_known_length(framed.plan()).persistence() ==
                http1_close_policy::close_after_response);
    // RFC 9112 section 6.3 rule 1: HEAD, 204 and 304 responses end at the
    // header section regardless of framing fields. RFC 9112 section 6.1 still
    // treats the HTTP/1.0 message as faulty: the connection closes after it,
    // whatever keep-alive it requests.
    for (const auto& [method, response] : {
             std::pair{"HEAD", "HTTP/1.0 200 OK\r\nTransfer-Encoding: gzip\r\nConnection: keep-alive"},
             std::pair{"GET", "HTTP/1.0 304 Not Modified\r\nTransfer-Encoding: chunked\r\nConnection: keep-alive"},
             std::pair{"GET", "HTTP/1.0 204 No Content\r\nTransfer-Encoding: chunked\r\nConnection: keep-alive"},
         }) {
        const auto head = parse_head(method, response);
        EXPECT_TRUE(require_without_content(head.plan()).persistence() == http1_close_policy::close_after_response);
    }
    const auto keep_alive = parse_head("GET", "HTTP/1.0 304 Not Modified\r\nConnection: keep-alive");
    EXPECT_TRUE(require_without_content(keep_alive.plan()).persistence() == http1_close_policy::allow_reuse);
}

TEST(Ruvia, http_client_head_method_is_case_sensitive) {
    const auto head = parse_head("HEAD", "HTTP/1.1 200 OK");
    const auto lowercase = parse_head("head", "HTTP/1.1 200 OK");
    EXPECT_TRUE(head.plan().without_content() != nullptr);
    EXPECT_TRUE(lowercase.plan().close_delimited() != nullptr);
}

using namespace ruvia::test;

// HTTP/1 client responses: the exchange around it: interim responses, Expect, upgrades and tunnels.

TEST(Ruvia, http_client_informational_responses_ignore_framing_fields) {
    for (const auto status :
        {std::string_view("HTTP/1.1 100 Continue"), std::string_view("HTTP/1.1 103 Early Hints")}) {
        const auto head = parse_head("GET", status);
        EXPECT_TRUE(head.plan().informational() != nullptr);
    }

    const auto valid = parse_head("GET",
        "HTTP/1.1 103 Early Hints\r\n"
        "Link: </style.css>; rel=preload\r\n"
        "Content-Type: text/html; charset=utf-8");
    EXPECT_TRUE(valid.plan().informational() != nullptr);

    // RFC 9112 section 6.3 rule 1: a 1xx ends at its header section, so the
    // framing fields a sender must not emit are ignored rather than fatal.
    for (const auto fields : {std::string_view("Content-Length: 5"),
             std::string_view("Transfer-Encoding: chunked"),
             std::string_view("Content-Length: invalid\r\nTransfer-Encoding: gzip")}) {
        std::string head("HTTP/1.1 103 Early Hints\r\n");
        head.append(fields);
        const auto ignored = parse_head("GET", head);
        EXPECT_TRUE(ignored.plan().informational() != nullptr);
        EXPECT_EQ(ignored.consumed_bytes(), head.size() + 4);
    }
}

TEST(Ruvia, http_client_long_informational_sequence_preserves_continue_and_final_boundary) {
    ruvia::http_client_request_view request;
    request.method_ = "POST";
    request.content_ = ruvia::http_client_request_content_view::bytes("payload");
    http1_client_response_parser parser = exchange_parser(
        ruvia::http_origin_view::https({.host_ = "example.test"}), request,
        http1_client_request_wire_policy{
            .expectation_ = ruvia::http_client_request_expectation::continue_value});
    constexpr std::string_view early_hints =
        "HTTP/1.1 103 Early Hints\r\nLink: </asset>; rel=preload\r\n\r\n";
    for (std::size_t i = 0; i < 300; ++i) {
        const auto interim = parser.parse(early_hints);
        const auto* parsed = interim.parsed();
        EXPECT_TRUE(parsed != nullptr);
        if (parsed == nullptr) {
            return;
        }
        EXPECT_TRUE(parsed->plan().informational() != nullptr);
        EXPECT_TRUE(!parsed->plan().request_content_signal());
        EXPECT_EQ(parsed->consumed_bytes(), early_hints.size());
    }
    const auto continue_response = parser.parse("HTTP/1.1 100 Continue\r\n\r\n");
    EXPECT_TRUE(continue_response.parsed() != nullptr);
    if (continue_response.parsed() == nullptr) {
        return;
    }
    EXPECT_TRUE(continue_response.parsed()->plan().request_content_signal() ==
                http_client_request_content_signal::continue_value);
    EXPECT_TRUE(parser.complete_request_content() == http1_client_request_content_completion_status::completed);

    constexpr std::string_view final_wire =
        "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nabcNEXT";
    const auto final_response = parser.parse(final_wire);
    EXPECT_TRUE(final_response.parsed() != nullptr);
    if (const auto* parsed = final_response.parsed()) {
        EXPECT_EQ(parsed->head().status().value(), std::uint16_t{200});
        EXPECT_EQ(require_known_length(parsed->plan()).content_length(), std::size_t{3});
        EXPECT_EQ(final_wire.substr(parsed->consumed_bytes()), "abcNEXT");
        EXPECT_TRUE(!parsed->plan().request_content_signal());
    }
    EXPECT_TRUE(ruvia::test::throws_on([&] { (void)parser.parse({}); }));
}

TEST(Ruvia, http_client_expect_continue_is_one_stateful_exchange_contract) {
    ruvia::http_client_request_view request;
    request.method_ = "POST";
    request.content_ = ruvia::http_client_request_content_view::bytes("payload");
    std::array<char, 512> request_head;
    auto prepared_result = ruvia::http1_client_request_writer().prepare(
        ruvia::http_origin_view::https({.host_ = "example.test"}), request, request_head,
        http1_client_request_wire_policy{
            .expectation_ = ruvia::http_client_request_expectation::continue_value});
    auto* prepared = std::get_if<ruvia::prepared_http1_client_request>(&prepared_result);
    EXPECT_TRUE(prepared != nullptr);
    if (prepared == nullptr) {
        return;
    }
    EXPECT_TRUE(prepared->content_plan().continue_gated() != nullptr);

    http1_client_response_parser parser(std::move(*prepared).take_exchange_state());
    auto early_hints = parser.parse("HTTP/1.1 103 Early Hints\r\n\r\n");
    EXPECT_TRUE(early_hints.parsed() != nullptr);
    if (early_hints.parsed() != nullptr) {
        EXPECT_TRUE(!early_hints.parsed()->plan().request_content_signal());
        EXPECT_TRUE(early_hints.parsed()->plan().informational() != nullptr);
    }

    auto continue_response = parser.parse("HTTP/1.1 100 Continue\r\n\r\n");
    EXPECT_TRUE(continue_response.parsed() != nullptr);
    if (continue_response.parsed() != nullptr) {
        EXPECT_TRUE(continue_response.parsed()->plan().request_content_signal() ==
                    http_client_request_content_signal::continue_value);
    }
    const auto duplicate_continue = parser.parse("HTTP/1.1 100 Continue\r\n\r\n");
    EXPECT_TRUE(duplicate_continue.parsed() != nullptr);
    if (duplicate_continue.parsed() != nullptr) {
        EXPECT_TRUE(!duplicate_continue.parsed()->plan().request_content_signal());
    }
    EXPECT_TRUE(parser.complete_request_content() == http1_client_request_content_completion_status::completed);
    EXPECT_TRUE(parser.complete_request_content() ==
                http1_client_request_content_completion_status::already_complete);

    auto final_response = parser.parse("HTTP/1.1 201 Created\r\nContent-Length: 0\r\n\r\n");
    EXPECT_TRUE(final_response.parsed() != nullptr);
    if (final_response.parsed() != nullptr) {
        EXPECT_TRUE(!final_response.parsed()->plan().request_content_signal());
    }

    EXPECT_TRUE(ruvia::test::throws_on([&] { (void)parser.parse("HTTP/1.1 204 No Content\r\n\r\n"); }));
    EXPECT_TRUE(parser.complete_request_content() ==
                http1_client_request_content_completion_status::exchange_terminal);
}

TEST(Ruvia, http_client_closing_informational_response_keeps_exchange_open) {
    ruvia::http_client_request_view get_request;
    get_request.method_ = "GET";
    const auto origin = ruvia::http_origin_view::https({.host_ = "example.test"});
    http1_client_response_parser get_parser = exchange_parser(origin, get_request);
    const auto early_hints =
        get_parser.parse("HTTP/1.1 103 Early Hints\r\nConnection: close\r\n\r\n");
    EXPECT_TRUE(early_hints.parsed() != nullptr);
    if (early_hints.parsed() != nullptr) {
        const auto* const informational = early_hints.parsed()->plan().informational();
        EXPECT_TRUE(informational != nullptr);
        if (informational != nullptr) {
            EXPECT_TRUE(informational->persistence() == http1_close_policy::allow_reuse);
        }
    }
    // RFC 9110 Section 15.2: any number of 1xx responses precede the final
    // response, whose own persistence decides the connection. A close option on
    // the 1xx is metadata only.
    const auto final_response = get_parser.parse("HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n");
    EXPECT_TRUE(final_response.parsed() != nullptr);
    if (final_response.parsed() != nullptr) {
        EXPECT_TRUE(require_known_length(final_response.parsed()->plan()).persistence() ==
                    http1_close_policy::allow_reuse);
    }

    ruvia::http_client_request_view post_request;
    post_request.method_ = "POST";
    post_request.content_ = ruvia::http_client_request_content_view::bytes("payload");
    const http1_client_request_wire_policy continue_policy{
        .expectation_ = ruvia::http_client_request_expectation::continue_value};
    http1_client_response_parser post_parser = exchange_parser(origin, post_request, continue_policy);
    const auto closing_continue =
        post_parser.parse("HTTP/1.1 100 Continue\r\nConnection: close\r\n\r\n");
    EXPECT_TRUE(closing_continue.parsed() != nullptr);
    if (closing_continue.parsed() != nullptr) {
        EXPECT_TRUE(closing_continue.parsed()->plan().request_content_signal() ==
                    http_client_request_content_signal::continue_value);
    }

    http1_client_response_parser closing_hints_parser = exchange_parser(origin, post_request, continue_policy);
    const auto closing_hints =
        closing_hints_parser.parse("HTTP/1.1 103 Early Hints\r\nConnection: close\r\n\r\n");
    EXPECT_TRUE(closing_hints.parsed() != nullptr);
    if (closing_hints.parsed() != nullptr) {
        EXPECT_TRUE(!closing_hints.parsed()->plan().request_content_signal());
    }

    http1_client_response_parser request_close_parser = exchange_parser(origin, get_request,
        http1_client_request_wire_policy{.close_policy_ = http1_close_policy::close_after_response});
    const auto non_closing_hints = request_close_parser.parse("HTTP/1.1 103 Early Hints\r\n\r\n");
    EXPECT_TRUE(non_closing_hints.parsed() != nullptr);
    if (non_closing_hints.parsed() != nullptr) {
        const auto* const informational = non_closing_hints.parsed()->plan().informational();
        EXPECT_TRUE(informational != nullptr);
        if (informational != nullptr) {
            EXPECT_TRUE(informational->persistence() == http1_close_policy::allow_reuse);
        }
    }
    const auto request_close_final = request_close_parser.parse("HTTP/1.1 204 No Content\r\n\r\n");
    EXPECT_TRUE(request_close_final.parsed() != nullptr);
    if (request_close_final.parsed() != nullptr) {
        EXPECT_TRUE(require_without_content(request_close_final.parsed()->plan()).persistence() ==
                    http1_close_policy::close_after_response);
    }
}

TEST(Ruvia, http_client_expect_final_cancels_only_pending_request_content) {
    ruvia::http_client_request_view request;
    request.method_ = "POST";
    request.content_ = ruvia::http_client_request_content_view::bytes("payload");
    const auto origin = ruvia::http_origin_view::https({.host_ = "example.test"});
    const http1_client_request_wire_policy continue_policy{
        .expectation_ = ruvia::http_client_request_expectation::continue_value};

    http1_client_response_parser parser = exchange_parser(origin, request, continue_policy);
    const auto final_response =
        parser.parse("HTTP/1.1 417 Expectation Failed\r\nContent-Length: 0\r\n\r\n");
    EXPECT_TRUE(final_response.parsed() != nullptr);
    if (final_response.parsed() != nullptr) {
        EXPECT_TRUE(final_response.parsed()->plan().request_content_signal() ==
                    http_client_request_content_signal::exchange_complete);
        // The head committed Content-Length framing for content never sent.
        EXPECT_TRUE(require_known_length(final_response.parsed()->plan()).persistence() ==
                    http1_close_policy::close_after_response);
    }

    http1_client_response_parser completed_parser = exchange_parser(origin, request, continue_policy);
    EXPECT_TRUE(completed_parser.complete_request_content() ==
                http1_client_request_content_completion_status::completed);
    const auto completed_final =
        completed_parser.parse("HTTP/1.1 417 Expectation Failed\r\nContent-Length: 0\r\n\r\n");
    EXPECT_TRUE(completed_final.parsed() != nullptr);
    if (completed_final.parsed() != nullptr) {
        EXPECT_TRUE(!completed_final.parsed()->plan().request_content_signal());
        EXPECT_TRUE(require_known_length(completed_final.parsed()->plan()).persistence() ==
                    http1_close_policy::allow_reuse);
    }
}

TEST(Ruvia, http_client_timeout_released_content_follows_final_response_persistence) {
    const ruvia::http1_client_request_head_view request{
        .method_ = "POST", .target_ = "/upload", .content_length_ = 6};
    for (const bool closing : {false, true}) {
        http1_client_response_parser parser = exchange_parser(
            ruvia::http_origin_view::https({.host_ = "example.test"}), request,
            {.expectation_ = ruvia::http_client_request_expectation::continue_value});
        parser.release_request_content();
        const auto response = parser.parse(closing
                                               ? "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"
                                               : "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n");
        EXPECT_TRUE(response.parsed() != nullptr);
        if (response.parsed() != nullptr) {
            const auto& plan = response.parsed()->plan();
            EXPECT_TRUE(require_known_length(plan).persistence() ==
                        (closing ? http1_close_policy::close_after_response : http1_close_policy::allow_reuse));
            if (closing) {
                EXPECT_TRUE(plan.request_content_signal() == http_client_request_content_signal::exchange_complete);
            } else {
                EXPECT_TRUE(!plan.request_content_signal());
            }
        }
    }
}

TEST(Ruvia, http_client_upgrade_accepts_omitted_continue_after_content_complete) {
    const ruvia::http_header_view upgrade_headers[] = {
        {"Connection", "Upgrade"},
        {"Upgrade", "websocket"},
    };
    ruvia::http_client_request_view request;
    request.method_ = "POST";
    request.headers_ = upgrade_headers;
    request.content_ = ruvia::http_client_request_content_view::bytes("payload");
    constexpr std::string_view switching =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Connection: Upgrade\r\nUpgrade: websocket\r\n\r\n";
    const auto origin = ruvia::http_origin_view::https({.host_ = "example.test"});
    const http1_client_request_wire_policy continue_policy{
        .expectation_ = ruvia::http_client_request_expectation::continue_value};

    http1_client_response_parser rejected_parser = exchange_parser(origin, request, continue_policy);
    const auto rejected = rejected_parser.parse(switching);
    EXPECT_TRUE(rejected.failure() != nullptr);
    if (rejected.failure() != nullptr) {
        EXPECT_TRUE(*rejected.failure() == http1_client_response_parse_error::invalid_protocol_switch);
    }
    EXPECT_TRUE(ruvia::test::throws_on([&] { (void)rejected_parser.parse(switching); }));

    // RFC 9110 Section 10.1.1 lets the server omit 100 once it has received
    // some or all of the content, so a 101 after the client finished writing
    // the body is a valid switch.
    http1_client_response_parser completed_without_continue_parser =
        exchange_parser(origin, request, continue_policy);
    completed_without_continue_parser.release_request_content();
    EXPECT_TRUE(completed_without_continue_parser.complete_request_content() ==
                http1_client_request_content_completion_status::completed);
    const auto completed_without_continue = completed_without_continue_parser.parse(switching);
    EXPECT_TRUE(completed_without_continue.parsed() != nullptr);
    if (completed_without_continue.parsed() != nullptr) {
        EXPECT_TRUE(completed_without_continue.parsed()->plan().protocol_upgrade() != nullptr);
    }

    http1_client_response_parser late_continue_parser = exchange_parser(origin, request, continue_policy);
    late_continue_parser.release_request_content();
    EXPECT_TRUE(late_continue_parser.complete_request_content() ==
                http1_client_request_content_completion_status::completed);
    const auto late_continue = late_continue_parser.parse("HTTP/1.1 100 Continue\r\n\r\n");
    EXPECT_TRUE(late_continue.parsed() != nullptr);
    if (late_continue.parsed() != nullptr) {
        EXPECT_TRUE(!late_continue.parsed()->plan().request_content_signal());
    }
    const auto accepted_after_late_continue = late_continue_parser.parse(switching);
    EXPECT_TRUE(accepted_after_late_continue.parsed() != nullptr);
    if (accepted_after_late_continue.parsed() != nullptr) {
        EXPECT_TRUE(accepted_after_late_continue.parsed()->plan().protocol_upgrade() != nullptr);
    }

    http1_client_response_parser pending_parser = exchange_parser(origin, request, continue_policy);
    const auto pending_continue = pending_parser.parse("HTTP/1.1 100 Continue\r\n\r\n");
    EXPECT_TRUE(pending_continue.parsed() != nullptr);
    const auto pending_upgrade = pending_parser.parse(switching);
    EXPECT_TRUE(pending_upgrade.failure() != nullptr);
    if (pending_upgrade.failure() != nullptr) {
        EXPECT_TRUE(*pending_upgrade.failure() ==
                    http1_client_response_parse_error::invalid_protocol_switch);
    }

    http1_client_response_parser accepted_parser = exchange_parser(origin, request, continue_policy);
    accepted_parser.release_request_content();
    const auto continue_response = accepted_parser.parse("HTTP/1.1 100 Continue\r\n\r\n");
    EXPECT_TRUE(continue_response.parsed() != nullptr);
    if (continue_response.parsed() != nullptr) {
        EXPECT_TRUE(!continue_response.parsed()->plan().request_content_signal());
    }
    EXPECT_TRUE(accepted_parser.complete_request_content() ==
                http1_client_request_content_completion_status::completed);
    const auto accepted = accepted_parser.parse(switching);
    EXPECT_TRUE(accepted.parsed() != nullptr);
    if (accepted.parsed() != nullptr) {
        EXPECT_TRUE(accepted.parsed()->plan().protocol_upgrade() != nullptr);
    }
}

TEST(Ruvia, http_client_upgrade_requires_complete_request_content) {
    const ruvia::http_header_view upgrade_headers[] = {
        {"Connection", "Upgrade"},
        {"Upgrade", "websocket"},
    };
    ruvia::http_client_request_view request;
    request.method_ = "POST";
    request.headers_ = upgrade_headers;
    request.content_ = ruvia::http_client_request_content_view::bytes("payload");
    constexpr std::string_view switching =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Connection: Upgrade\r\nUpgrade: websocket\r\n\r\n";

    const auto origin = ruvia::http_origin_view::https({.host_ = "example.test"});
    http1_client_response_parser incomplete_parser = exchange_parser(origin, request);
    const auto incomplete = incomplete_parser.parse(switching);
    EXPECT_TRUE(incomplete.failure() != nullptr);
    if (incomplete.failure() != nullptr) {
        EXPECT_TRUE(*incomplete.failure() == http1_client_response_parse_error::invalid_protocol_switch);
    }

    http1_client_response_parser complete_parser = exchange_parser(origin, request);
    EXPECT_TRUE(complete_parser.complete_request_content() ==
                http1_client_request_content_completion_status::completed);
    const auto complete_value = complete_parser.parse(switching);
    EXPECT_TRUE(complete_value.parsed() != nullptr);
    if (complete_value.parsed() != nullptr) {
        EXPECT_TRUE(complete_value.parsed()->plan().protocol_upgrade() != nullptr);
    }
}

TEST(Ruvia, http_client_switching_protocols_is_an_exclusive_upgrade_transition) {
    const ruvia::http_header_view request_headers[] = {
        {"Connection", "keep-alive, Upgrade"},
        {"Upgrade", "websocket, IRC/6.9"},
    };
    const auto upgraded = parse_head("GET",
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Connection: Upgrade\r\nUpgrade: WebSocket",
        http1_close_policy::allow_reuse, request_headers);
    EXPECT_TRUE(upgraded.head().status() == ruvia::http_status::switching_protocols);
    EXPECT_TRUE(upgraded.plan().protocol_upgrade() != nullptr);
    EXPECT_TRUE(upgraded.plan().connect_tunnel() == nullptr);

    // Protocol names compare case-insensitively; versions remain exact tokens.
    const auto versioned = parse_head("GET",
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Connection: upgrade\r\nUpgrade: irc/6.9",
        http1_close_policy::allow_reuse, request_headers);
    EXPECT_TRUE(versioned.plan().protocol_upgrade() != nullptr);
    EXPECT_TRUE(parse_fails("GET",
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Connection: upgrade\r\nUpgrade: IRC/6.10",
        http1_close_policy::allow_reuse, request_headers));
}

TEST(Ruvia, http_client_connection_fields_use_recipient_list_semantics) {
    const auto reusable = parse_head("GET",
        "HTTP/1.0 200 OK\r\n"
        "Connection: , keep-alive,\r\nContent-Length: 0");
    EXPECT_TRUE(require_known_length(reusable.plan()).persistence() == http1_close_policy::allow_reuse);
    EXPECT_TRUE(reusable.head().protocol_version() == http_protocol_version::http10);

    EXPECT_TRUE(parse_failure_error("GET",
                    "HTTP/1.1 200 OK\r\nConnection: close;invalid\r\n"
                    "Content-Length: 0") == http1_client_response_parse_error::invalid_connection);
    EXPECT_TRUE(parse_failure_error("GET",
                    "HTTP/1.1 200 OK\r\nUpgrade: websocket/\r\n"
                    "Content-Length: 0") == http1_client_response_parse_error::invalid_upgrade);
    const auto advertised = parse_head("GET",
        "HTTP/1.1 200 OK\r\nUpgrade: websocket\r\nContent-Length: 0");
    EXPECT_EQ(require_known_length(advertised.plan()).content_length(), std::size_t{0});
    EXPECT_TRUE(require_known_length(advertised.plan()).persistence() == http1_close_policy::allow_reuse);

    const ruvia::http_header_view offered[] = {
        {"Connection", "Upgrade"},
        {"Upgrade", "websocket"},
    };
    const auto upgraded = parse_head("GET",
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Connection: , Upgrade,\r\n"
        "Upgrade: , websocket,",
        http1_close_policy::allow_reuse, offered);
    EXPECT_TRUE(upgraded.plan().protocol_upgrade() != nullptr);
}

TEST(Ruvia, http_client_rejects_only_protected_connection_options) {
    // Only framing fields are rejected on received responses (RFC 9110 §7.6.1).
    for (const std::string_view option : {"Content-Length", "Transfer-Encoding"}) {
        std::string response = "HTTP/1.1 200 OK\r\nConnection: ";
        response.append(option);
        response.append("\r\nContent-Length: 0");
        EXPECT_TRUE(parse_failure_error("GET", response) ==
                    http1_client_response_parse_error::invalid_connection);
    }

    // RFC 9110 section 7.6.1 forbids sending end-to-end field names in
    // Connection, but recipients accept the message without 400-style rejection.
    for (const std::string_view option :
        {"Set-Cookie", "X-Hop", "DATE", "Range", "Cache-Control", "Host", "Authorization", "Origin", "Cookie"}) {
        std::string response = "HTTP/1.1 200 OK\r\nConnection: ";
        response.append(option);
        response.append("\r\nSet-Cookie: sid=1\r\nX-Hop: local\r\nContent-Length: 0");
        const auto accepted = parse_head("GET", response);
        EXPECT_TRUE(accepted.plan().known_length() != nullptr);
    }
}

TEST(Ruvia, http_client_switching_protocols_requires_wire_agreement) {
    const ruvia::http_header_view offered[] = {
        {"Connection", "Upgrade"},
        {"Upgrade", "websocket"},
    };
    const ruvia::http_header_view closing_offer[] = {
        {"Connection", "close, Upgrade"},
        {"Upgrade", "websocket"},
    };
    constexpr std::string_view valid_response =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Connection: Upgrade\r\nUpgrade: websocket";

    EXPECT_TRUE(parse_fails("GET", valid_response));
    EXPECT_TRUE(parse_fails("GET", "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket",
        http1_close_policy::allow_reuse, offered));
    EXPECT_TRUE(parse_fails("GET", "HTTP/1.1 101 Switching Protocols\r\nConnection: Upgrade",
        http1_close_policy::allow_reuse, offered));
    EXPECT_TRUE(parse_fails("GET",
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Connection: Upgrade\r\nUpgrade: IRC",
        http1_close_policy::allow_reuse, offered));
    for (const std::string_view framing : {
             std::string_view("Content-Length: 0\r\n"),
             std::string_view("Content-Length: 5\r\n"),
             std::string_view("Transfer-Encoding: chunked\r\n"),
         }) {
        std::string head(valid_response);
        head.append("\r\n");
        head.append(framing);
        head.append("\r\n");
        const std::string wire = head + "protocol_bytes";
        const auto switched = parse_wire("GET", wire, http1_close_policy::allow_reuse, offered);
        EXPECT_TRUE(switched.parsed() != nullptr);
        if (const auto* parsed = switched.parsed()) {
            EXPECT_TRUE(parsed->plan().protocol_upgrade() != nullptr);
            EXPECT_EQ(parsed->consumed_bytes(), head.size());
        }
    }
    EXPECT_TRUE(parse_fails("GET",
        "HTTP/1.0 101 Switching Protocols\r\n"
        "Connection: Upgrade\r\nUpgrade: websocket",
        http1_close_policy::allow_reuse, offered));
    EXPECT_TRUE(parse_fails("GET", valid_response, http1_close_policy::allow_reuse, closing_offer));
}

TEST(Ruvia, http_client_http10_informational_transfer_encoding_latches_faulty_framing_close) {
    ruvia::http_client_request_view post_request;
    post_request.method_ = "POST";
    post_request.target_ = "/upload";
    post_request.content_ = ruvia::http_client_request_content_view::bytes("hello");
    http1_client_response_parser post_parser = exchange_parser(
        ruvia::http_origin_view::http({.host_ = "example.test"}), post_request,
        {.expectation_ = ruvia::http_client_request_expectation::continue_value});
    const auto interim = post_parser.parse(
        "HTTP/1.0 100 Continue\r\n"
        "Transfer-Encoding: chunked\r\n\r\n");
    EXPECT_TRUE(interim.parsed() != nullptr);
    EXPECT_TRUE(post_parser.complete_request_content() ==
                http1_client_request_content_completion_status::completed);
    const auto final_res = post_parser.parse(
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 0\r\n\r\n");
    EXPECT_TRUE(final_res.parsed() != nullptr);
    if (final_res.parsed() != nullptr) {
        EXPECT_TRUE(require_known_length(final_res.parsed()->plan()).persistence() ==
                    http1_close_policy::close_after_response);
    }

    const ruvia::http_header_view offered[] = {
        {"Connection", "Upgrade"},
        {"Upgrade", "websocket"},
    };
    ruvia::http_client_request_view ws_request;
    ws_request.method_ = "GET";
    ws_request.target_ = "/ws";
    ws_request.headers_ = offered;
    http1_client_response_parser ws_parser =
        exchange_parser(ruvia::http_origin_view::http({.host_ = "example.test"}), ws_request);
    const auto hints = ws_parser.parse(
        "HTTP/1.0 103 Early Hints\r\n"
        "Transfer-Encoding: chunked\r\n\r\n");
    EXPECT_TRUE(hints.parsed() != nullptr);
    const auto upgrade_res = ws_parser.parse(
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Connection: Upgrade\r\n"
        "Upgrade: websocket\r\n\r\n");
    EXPECT_TRUE(upgrade_res.failure() != nullptr);
    if (upgrade_res.failure() != nullptr) {
        EXPECT_TRUE(*upgrade_res.failure() ==
                    http1_client_response_parse_error::invalid_protocol_switch);
    }
}

TEST(Ruvia, http_client_response_plan_owns_version_and_connection_persistence) {
    const auto http10 = parse_head("GET", "HTTP/1.0 200 OK\r\nContent-Length: 3");
    const auto& http10_body = require_known_length(http10.plan());
    EXPECT_TRUE(http10_body.persistence() == http1_close_policy::close_after_response);

    const auto http10_keep_alive =
        parse_head("GET", "HTTP/1.0 200 OK\r\nConnection: keep-alive\r\nContent-Length: 3");
    EXPECT_TRUE(require_known_length(http10_keep_alive.plan()).persistence() == http1_close_policy::allow_reuse);

    const auto response_close =
        parse_head("GET", "HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Length: 3");
    EXPECT_TRUE(require_known_length(response_close.plan()).persistence() ==
                http1_close_policy::close_after_response);

    const auto request_close = parse_head(
        "GET", "HTTP/1.1 200 OK\r\nContent-Length: 3", http1_close_policy::close_after_response);
    EXPECT_TRUE(require_known_length(request_close.plan()).persistence() ==
                http1_close_policy::close_after_response);
}

TEST(Ruvia, http_client_successful_connect_transitions_to_tunnel) {
    const auto tunnel = parse_head("CONNECT",
        "HTTP/1.1 200 Connection Established\r\nContent-Length: invalid\r\n"
        "Transfer-Encoding: chunked;invalid=parameter");
    EXPECT_TRUE(tunnel.plan().connect_tunnel() != nullptr);

    const auto rejected =
        parse_head("CONNECT", "HTTP/1.1 407 Proxy Authentication Required\r\nContent-Length: 3");
    EXPECT_TRUE(rejected.plan().known_length() != nullptr);

    // Methods are case-sensitive. A custom lowercase token is not CONNECT.
    const auto lowercase = parse_head("connect", "HTTP/1.1 200 OK");
    EXPECT_TRUE(lowercase.plan().close_delimited() != nullptr);
}

TEST(Ruvia, http_client_rejected_connect_never_reuses_the_proxy_connection) {
    for (const std::string_view response : {
             "HTTP/1.1 407 Proxy Authentication Required\r\nContent-Length: 0",
             "HTTP/1.1 403 Forbidden\r\nContent-Length: 3",
             "HTTP/1.0 407 Proxy Authentication Required\r\nConnection: keep-alive\r\nContent-Length: 0",
         }) {
        const auto rejected = parse_head("CONNECT", response);
        EXPECT_TRUE(require_known_length(rejected.plan()).persistence() ==
                    http1_close_policy::close_after_response);
    }

    const auto chunked = parse_head("CONNECT",
        "HTTP/1.1 502 Bad Gateway\r\nTransfer-Encoding: chunked");
    EXPECT_TRUE(require_chunked(chunked.plan()).persistence() ==
                http1_close_policy::close_after_response);
    const auto without_content = parse_head("CONNECT", "HTTP/1.1 304 Not Modified");
    EXPECT_TRUE(require_without_content(without_content.plan()).persistence() ==
                http1_close_policy::close_after_response);

    // Persistence belongs to the rejected CONNECT, not to these otherwise
    // reusable response fields or the normal non-CONNECT framing alternative.
    const auto ordinary = parse_head("GET",
        "HTTP/1.1 407 Proxy Authentication Required\r\nContent-Length: 0");
    EXPECT_TRUE(require_known_length(ordinary.plan()).persistence() ==
                http1_close_policy::allow_reuse);
    for (const std::string_view response : {
             "HTTP/1.1 200 Connection Established",
             "HTTP/1.1 205 Reset Content",
             "HTTP/1.1 299 Tunnel Established",
         }) {
        const auto tunnel = parse_head("CONNECT", response);
        EXPECT_TRUE(tunnel.plan().connect_tunnel() != nullptr);
    }
}

TEST(Ruvia, http1_client_exchange_state_owns_every_long_lived_response_fact) {
    std::optional<ruvia::http1_client_exchange_state> head_exchange;
    std::array<char, 512> head_wire{};
    std::string method = "HEAD";
    {
        ruvia::http_client_request_view request{.method_ = method, .target_ = "/resource"};
        auto prepared = ruvia::http1_client_request_writer().prepare(
            ruvia::http_origin_view::https({.host_ = "example.test"}), request, head_wire);
        auto* prepared_req = std::get_if<ruvia::prepared_http1_client_request>(&prepared);
        if (prepared_req == nullptr) {
            throw std::runtime_error("HEAD request preparation failed");
        }
        head_exchange.emplace(std::move(*prepared_req).take_exchange_state());
    }
    method.assign("GET");
    ruvia::http1_client_response_parser head_parser(std::move(*head_exchange));
    auto head_result = head_parser.parse("HTTP/1.1 200 OK\r\nContent-Length: 7\r\n\r\n");
    if (head_result.parsed() == nullptr) {
        throw std::runtime_error("HEAD response parsing failed");
    }
    EXPECT_TRUE(head_result.parsed()->plan().without_content() != nullptr);

    std::optional<ruvia::http1_client_exchange_state> upgrade_exchange;
    std::array<char, 512> upgrade_wire{};
    std::string connection = "Upgrade";
    std::string protocol = "websocket";
    {
        const std::array headers{
            ruvia::http_header_view{"Connection", connection},
            ruvia::http_header_view{"Upgrade", protocol},
        };
        ruvia::http_client_request_view request{
            .method_ = "GET", .target_ = "/socket", .headers_ = headers};
        auto prepared = ruvia::http1_client_request_writer().prepare(
            ruvia::http_origin_view::https({.host_ = "example.test"}), request, upgrade_wire);
        auto* prepared_req = std::get_if<ruvia::prepared_http1_client_request>(&prepared);
        if (prepared_req == nullptr) {
            throw std::runtime_error("Upgrade request preparation failed");
        }
        upgrade_exchange.emplace(std::move(*prepared_req).take_exchange_state());
    }
    connection.assign("close");
    protocol.assign("not-websocket");
    ruvia::http1_client_response_parser upgrade_parser(std::move(*upgrade_exchange));
    auto upgrade_result = upgrade_parser.parse(
        "HTTP/1.1 101 Switching Protocols\r\nConnection: Upgrade\r\nUpgrade: websocket\r\n\r\n");
    if (upgrade_result.parsed() == nullptr) {
        throw std::runtime_error("Upgrade response parsing failed");
    }
    EXPECT_TRUE(upgrade_result.parsed()->plan().protocol_upgrade() != nullptr);
}

using namespace ruvia::test;

// HTTP/1 client responses: Content-Encoding and decoding the body.

TEST(Ruvia, http_client_content_encoding_has_one_authoritative_path) {
    using ruvia::http_content_coding;

    struct case_value final {
        std::string_view headers_;
        std::vector<http_content_coding> expected_;
        bool unsupported_{false};
    };
    const case_value cases[] = {
        {"HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\nContent-Length: 0",
            {http_content_coding::gzip}},
        {"HTTP/1.1 200 OK\r\nContent-Encoding: x-gzip\r\nContent-Length: 0",
            {http_content_coding::gzip}},
        {"HTTP/1.1 200 OK\r\nContent-Encoding: GZIP\r\nContent-Length: 0",
            {http_content_coding::gzip}},
        {"HTTP/1.1 200 OK\r\nContent-Encoding: br\r\nContent-Length: 0",
            {http_content_coding::brotli}},
        {"HTTP/1.1 200 OK\r\nContent-Encoding: zstd\r\nContent-Length: 0",
            {http_content_coding::zstd}},
        {"HTTP/1.1 200 OK\r\nContent-Encoding: deflate\r\nContent-Length: 0",
            {http_content_coding::deflate}},
        {"HTTP/1.1 200 OK\r\nContent-Encoding: identity\r\nContent-Length: 0", {}},
        {"HTTP/1.1 200 OK\r\nContent-Encoding: identity, identity, gzip, identity\r\nContent-Length: 0",
            {http_content_coding::gzip}},
        {"HTTP/1.1 200 OK\r\nContent-Encoding: gzip, br\r\nContent-Length: 0",
            {http_content_coding::gzip, http_content_coding::brotli}},
        {"HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\n"
         "Content-Encoding: br\r\nContent-Length: 0",
            {http_content_coding::gzip, http_content_coding::brotli}},
        {"HTTP/1.1 200 OK\r\nContent-Encoding: compress\r\nContent-Length: 0", {}, true},
        {"HTTP/1.1 200 OK\r\nContent-Length: 0", {}},
    };

    for (const auto& test : cases) {
        auto parsed_value = parse_response("GET", test.headers_);
        EXPECT_EQ(parsed_value.head_.status(), ruvia::http_status::ok);
        const auto coding = ruvia::parse_http_content_coding_headers(parsed_value.head_.headers());
        EXPECT_TRUE(coding.invalid() == nullptr);
        EXPECT_TRUE((coding.unsupported() != nullptr) == test.unsupported_);
        EXPECT_EQ(coding.codings().size(), test.expected_.size());
        for (std::size_t i = 0; i < test.expected_.size(); ++i) {
            EXPECT_TRUE(coding.codings()[i] == test.expected_[i]);
        }
    }
}

TEST(Ruvia, http_client_content_encoding_ignores_empty_list_members) {
    const auto tolerant = parse_response("GET",
        "HTTP/1.1 200 OK\r\n"
        "Content-Encoding: , gzip,,\r\n"
        "Content-Length: 0");
    const auto coding = ruvia::parse_http_content_coding_headers(tolerant.head_.headers());
    EXPECT_TRUE(coding.unsupported() == nullptr);
    EXPECT_EQ(coding.codings().size(), 1U);
    if (!coding.codings().empty()) {
        EXPECT_TRUE(coding.codings().front() == ruvia::http_content_coding::gzip);
    }
}

TEST(Ruvia, http_client_coding_metadata_preserves_framing_without_decoder_fallback) {
    for (const std::string_view value : {"gzip;level=9", "bad coding", "gzip/deflate", "future-coding"}) {
        std::string wire = "HTTP/1.1 200 OK\r\nContent-Encoding: ";
        wire.append(value);
        wire.append("\r\nContent-Encoding: identity\r\nContent-Length: 3\r\n\r\nabcNEXT");
        const auto result = parse_wire("GET", wire);
        const auto* parsed = result.parsed();
        EXPECT_TRUE(parsed != nullptr);
        if (parsed == nullptr) {
            continue;
        }
        EXPECT_EQ(parsed->head().headers().front().value(), value);
        const auto length = require_known_length(parsed->plan()).content_length();
        EXPECT_EQ(length, std::size_t{3});
        EXPECT_EQ(std::string_view(wire).substr(parsed->consumed_bytes(), length), "abc");
        EXPECT_EQ(std::string_view(wire).substr(parsed->consumed_bytes() + length), "NEXT");
        const auto coding = ruvia::parse_http_content_coding_headers(parsed->head().headers());
        const bool unsupported = value == "future-coding";
        EXPECT_TRUE((coding.unsupported() != nullptr) == unsupported);
        EXPECT_TRUE((coding.invalid() != nullptr) == !unsupported);
    }
}

using namespace ruvia::test;

// HTTP/1 client responses: reading a response head off the wire.

namespace {

constexpr std::size_t repeated_field_count = 160;

std::string repeated_response_wire(std::string_view status_line, std::string_view trailing_fields) {
    std::string wire(status_line);
    wire.append("\r\n");
    for (std::size_t index = 0; index < repeated_field_count; ++index) {
        wire.append("X-Repeat: value-");
        wire.append(std::to_string(index));
        wire.append("\r\n");
    }
    wire.append(trailing_fields);
    wire.append("\r\n");
    return wire;
}

void check_repeated_response_fields(const http_client_response_head& head, std::size_t expected_count) {
    const auto fields = head.headers();
    EXPECT_EQ(fields.size(), expected_count);
    if (fields.size() < repeated_field_count) {
        return;
    }
    for (std::size_t index = 0; index < repeated_field_count; ++index) {
        EXPECT_EQ(fields[index].name(), std::string_view("X-Repeat"));
        EXPECT_EQ(fields[index].value(), std::string("value-") + std::to_string(index).c_str());
    }
}

}  // namespace

TEST(Ruvia, http_client_upgrade_after_many_response_fields_validates_offered_protocol) {
    const ruvia::http_header_view offered[] = {
        {"Connection", "Upgrade"},
        {"Upgrade", "websocket, IRC/6.9"},
    };
    const auto wire = repeated_response_wire("HTTP/1.1 101 Switching Protocols",
        "Connection: Upgrade\r\nUpgrade: irc/6.9\r\n");
    const auto accepted = parse_wire("GET", wire, http1_close_policy::allow_reuse, offered);
    EXPECT_TRUE(accepted.parsed() != nullptr);
    if (const auto* parsed = accepted.parsed()) {
        EXPECT_TRUE(parsed->head().status() == ruvia::http_status::switching_protocols);
        EXPECT_EQ(parsed->consumed_bytes(), wire.size());
        EXPECT_TRUE(parsed->plan().protocol_upgrade() != nullptr);
        EXPECT_EQ(active_plan_alternative_count(parsed->plan()), std::size_t{1});
        check_repeated_response_fields(parsed->head(), repeated_field_count + 2);
        const auto fields = parsed->head().headers();
        if (fields.size() == repeated_field_count + 2) {
            EXPECT_EQ(fields[repeated_field_count].name(), std::string_view("Connection"));
            EXPECT_EQ(fields[repeated_field_count].value(), std::string_view("Upgrade"));
            EXPECT_EQ(fields.back().name(), std::string_view("Upgrade"));
            EXPECT_EQ(fields.back().value(), std::string_view("irc/6.9"));
        }
    }
    const auto unoffered_wire = repeated_response_wire("HTTP/1.1 101 Switching Protocols",
        "Connection: Upgrade\r\nUpgrade: IRC/6.10\r\n");
    const auto unoffered = parse_wire("GET", unoffered_wire, http1_close_policy::allow_reuse, offered);
    EXPECT_TRUE(unoffered.failure() != nullptr);
    if (unoffered.failure() != nullptr) {
        EXPECT_TRUE(*unoffered.failure() == http1_client_response_parse_error::invalid_protocol_switch);
    }
    const auto unsolicited = parse_wire("GET", wire);
    EXPECT_TRUE(unsolicited.failure() != nullptr);
    if (unsolicited.failure() != nullptr) {
        EXPECT_TRUE(*unsolicited.failure() == http1_client_response_parse_error::invalid_protocol_switch);
    }
}
