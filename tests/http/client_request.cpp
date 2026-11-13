#include <gtest/gtest.h>
#include <zlib.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
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
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "ruvia/http/http_client.h"

#include "http_server_fixture.h"
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

using namespace ruvia::test;

using ruvia::http_content_encoder;

namespace {

class codec_test_allocation_error final : public std::bad_alloc {
public:
    [[nodiscard]] const char* what() const noexcept override {
        return "codec test allocation failure";
    }
};

}  // namespace

TEST(Ruvia, http_content_coding_field_mapping_is_protocol_generic) {
    const auto check_coding = [&](std::string_view value, http_content_coding expected) {
        const auto parsed_value = parse_http_content_coding(value);
        EXPECT_TRUE(parsed_value.invalid() == nullptr);
        EXPECT_TRUE(parsed_value.unsupported() == nullptr);
        EXPECT_EQ(parsed_value.codings().size(), 1U);
        if (!parsed_value.codings().empty()) {
            EXPECT_TRUE(parsed_value.codings().front() == expected);
        }
    };
    check_coding("gzip", http_content_coding::gzip);
    check_coding("x-gzip", http_content_coding::gzip);
    check_coding("X-GZIP", http_content_coding::gzip);
    check_coding("GZIP", http_content_coding::gzip);
    check_coding("  br ", http_content_coding::brotli);
    check_coding("zstd", http_content_coding::zstd);
    check_coding("deflate", http_content_coding::deflate);
    for (const std::string_view value : {std::string_view(""), std::string_view("identity"),
             std::string_view("IDENTITY, identity")}) {
        const auto empty = parse_http_content_coding(value);
        EXPECT_TRUE(empty.invalid() == nullptr);
        EXPECT_TRUE(empty.unsupported() == nullptr);
        EXPECT_TRUE(empty.codings().empty());
    }

    constexpr std::array mappings{
        std::pair{http_content_coding::identity, std::string_view("identity")},
        std::pair{http_content_coding::gzip, std::string_view("gzip")},
        std::pair{http_content_coding::deflate, std::string_view("deflate")},
        std::pair{http_content_coding::brotli, std::string_view("br")},
        std::pair{http_content_coding::zstd, std::string_view("zstd")},
    };
    for (const auto& [coding, token] : mappings) {
        EXPECT_EQ(ruvia::http_content_coding_token(coding), token);
        const auto parsed_value = parse_http_content_coding(token);
        if (coding == http_content_coding::identity) {
            EXPECT_TRUE(parsed_value.codings().empty());
        } else {
            EXPECT_EQ(parsed_value.codings().size(), 1U);
            EXPECT_TRUE(parsed_value.codings().front() == coding);
        }
        EXPECT_TRUE(ruvia::http_content_coding_from_token(token) == coding);
    }
    EXPECT_TRUE(ruvia::http_content_coding_from_token("X-GZIP") == http_content_coding::gzip);
    EXPECT_TRUE(ruvia::http_content_coding_from_token("BR") == http_content_coding::brotli);
    EXPECT_TRUE(!ruvia::http_content_coding_from_token("compress").has_value());
    EXPECT_TRUE(!ruvia::http_content_coding_from_token("").has_value());

    const auto unsupported = parse_http_content_coding("compress");
    const auto stacked = parse_http_content_coding("gzip, br");
    EXPECT_TRUE(unsupported.invalid() == nullptr);
    EXPECT_TRUE(unsupported.unsupported() != nullptr);
    EXPECT_TRUE(stacked.invalid() == nullptr);
    EXPECT_TRUE(stacked.unsupported() == nullptr);
    EXPECT_EQ(stacked.codings().size(), 2U);
    EXPECT_TRUE(stacked.codings()[0] == http_content_coding::gzip);
    EXPECT_TRUE(stacked.codings()[1] == http_content_coding::brotli);

    for (const std::string_view value : {"gzip;level=9", "bad coding", "gzip/deflate"}) {
        const auto invalid = parse_http_content_coding(value);
        EXPECT_TRUE(invalid.unsupported() == nullptr);
        EXPECT_TRUE(invalid.invalid() != nullptr);
        if (invalid.invalid() != nullptr) {
            EXPECT_EQ(invalid.invalid()->status(), ruvia::http_status::bad_request);
        }
    }
}

TEST(Ruvia, http_content_coding_field_bounds_the_coding_stack) {
    std::string value("identity");
    for (std::size_t index = 0; index < ruvia::max_content_codings; ++index) {
        value += ", gzip, identity";
    }
    const auto bounded = parse_http_content_coding(value);
    EXPECT_TRUE(bounded.unsupported() == nullptr);
    EXPECT_EQ(bounded.codings().size(), ruvia::max_content_codings);

    value += ", identity";
    const auto still_bounded = parse_http_content_coding(value);
    EXPECT_TRUE(still_bounded.unsupported() == nullptr);
    EXPECT_EQ(still_bounded.codings().size(), ruvia::max_content_codings);

    value += ", gzip";
    const auto oversized = parse_http_content_coding(value);
    EXPECT_TRUE(oversized.invalid() == nullptr);
    EXPECT_TRUE(oversized.unsupported() != nullptr);
    if (oversized.unsupported() != nullptr) {
        EXPECT_TRUE(oversized.unsupported()->depth_exceeded());
    }
    EXPECT_TRUE(oversized.codings().empty());

    const auto oversized_with_unknown = parse_http_content_coding(
        "gzip, gzip, gzip, gzip, gzip, gzip, gzip, gzip, gzip, x-custom");
    EXPECT_TRUE(oversized_with_unknown.invalid() == nullptr);
    EXPECT_TRUE(oversized_with_unknown.unsupported() != nullptr);
    if (oversized_with_unknown.unsupported() != nullptr) {
        EXPECT_TRUE(oversized_with_unknown.unsupported()->depth_exceeded());
    }

    const auto oversized_unknown_only =
        parse_http_content_coding("x1, x2, x3, x4, x5, x6, x7, x8, x9");
    EXPECT_TRUE(oversized_unknown_only.invalid() == nullptr);
    EXPECT_TRUE(oversized_unknown_only.unsupported() != nullptr);
    if (oversized_unknown_only.unsupported() != nullptr) {
        EXPECT_TRUE(oversized_unknown_only.unsupported()->depth_exceeded());
    }

    const auto eight_known_plus_unknown = parse_http_content_coding(
        "gzip, gzip, gzip, gzip, gzip, gzip, gzip, gzip, x-custom");
    EXPECT_TRUE(eight_known_plus_unknown.invalid() == nullptr);
    EXPECT_TRUE(eight_known_plus_unknown.unsupported() != nullptr);
    if (eight_known_plus_unknown.unsupported() != nullptr) {
        EXPECT_TRUE(eight_known_plus_unknown.unsupported()->depth_exceeded());
    }
    value += ", bad coding";
    const auto malformed = parse_http_content_coding(value);
    EXPECT_TRUE(malformed.invalid() != nullptr);

    // identity stages transform nothing but still observe the decoded limit.
    constexpr std::array identity_stack{http_content_coding::identity, http_content_coding::identity};
    const auto identity = decode_http_content(identity_stack, "plain", {.max_decoded_bytes_ = 5});
    EXPECT_TRUE(std::holds_alternative<std::string>(identity));
    if (const auto* decoded = std::get_if<std::string>(&identity)) {
        EXPECT_EQ(std::string_view(*decoded), std::string_view("plain"));
    }
    const auto oversized_identity = decode_http_content(identity_stack, "plain", {.max_decoded_bytes_ = 4});
    EXPECT_TRUE(std::holds_alternative<http_content_decode_error>(oversized_identity));
    if (const auto* failure = std::get_if<http_content_decode_error>(&oversized_identity)) {
        EXPECT_TRUE(*failure == ruvia::http_content_decode_error::decoded_size_exceeded);
    }
}

TEST(Ruvia, http_deflate_content_uses_the_rfc9110_zlib_wrapper) {
    const std::string input(4096, 'd');
    auto encoded = encode_http_content(
        http_content_coding::deflate, input, {.max_encoded_bytes_ = input.size()});
    EXPECT_TRUE(encoded.has_value());
    if (encoded.has_value()) {
        auto decoded = decode_http_content(std::array{http_content_coding::deflate}, *encoded,
            {.max_decoded_bytes_ = input.size()});
        EXPECT_TRUE(std::holds_alternative<std::string>(decoded));
        if (const auto* output = std::get_if<std::string>(&decoded)) {
            EXPECT_EQ(std::string_view(*output), input);
        }
    }
}

TEST(Ruvia, http_zstd_content_rejects_window_above_rfc9659_limit) {
    const std::string plain(9 * 1024 * 1024, 'w');
    const std::string encoded = zstd_compress_with_window(plain, 24);
    EXPECT_TRUE(!encoded.empty());
    EXPECT_TRUE(decode_error(http_content_coding::zstd, encoded, plain.size()) ==
                http_content_decode_error::invalid_content);

    auto conformant = encode_http_content(http_content_coding::zstd, plain,
        {.max_encoded_bytes_ = plain.size()});
    EXPECT_TRUE(conformant.has_value());
    if (conformant.has_value()) {
        EXPECT_EQ(decoded(http_content_coding::zstd, *conformant, plain.size()), plain);
    }
}

TEST(Ruvia, http_content_encode_enforces_exact_cap_without_partial_output) {
    const std::string input(2048, 'e');
    for (const auto coding :
        {http_content_coding::gzip, http_content_coding::deflate, http_content_coding::brotli, http_content_coding::zstd}) {
        const auto full = encode_http_content(coding, input,
            {.max_encoded_bytes_ = input.size()});
        EXPECT_TRUE(full.has_value());
        if (!full.has_value()) {
            continue;
        }
        const auto encoded_size = full->size();
        EXPECT_TRUE(encoded_size > 1);

        const auto exact = encode_http_content(coding, input,
            {.max_encoded_bytes_ = encoded_size});
        EXPECT_TRUE(exact.has_value());
        if (exact.has_value()) {
            EXPECT_EQ(exact->size(), encoded_size);
        }

        const auto too_small = encode_http_content(coding, input,
            {.max_encoded_bytes_ = encoded_size - 1});
        EXPECT_TRUE(!too_small.has_value());
        EXPECT_TRUE(!encode_http_content(coding, {}, {.max_encoded_bytes_ = 0}).has_value());
        EXPECT_TRUE(!encode_http_content(coding, input, {.max_encoded_bytes_ = 0}).has_value());
    }

    const auto identity = encode_http_content(http_content_coding::identity, "identity",
        {.max_encoded_bytes_ = 8});
    EXPECT_TRUE(identity.has_value());
    if (identity.has_value()) {
        EXPECT_EQ(std::string_view(*identity), std::string_view("identity"));
    }
    const auto identity_too_large = encode_http_content(http_content_coding::identity, "identity",
        {.max_encoded_bytes_ = 0});
    EXPECT_TRUE(!identity_too_large.has_value());
    const auto empty_identity = encode_http_content(http_content_coding::identity, {}, {.max_encoded_bytes_ = 0});
    EXPECT_TRUE(empty_identity.has_value() && empty_identity->empty());
}

TEST(Ruvia, http_content_encode_round_trips_across_output_block_boundaries) {
    for (const std::size_t size : {std::size_t{0}, std::size_t{8191}, std::size_t{8192},
             std::size_t{8193}, std::size_t{65537}, std::size_t{(4u << 20) + 1}}) {
        std::string input;
        input.reserve(size);
        std::uint32_t state_value = 0x12345678;
        for (std::size_t i = 0; i < size; ++i) {
            state_value ^= state_value << 13;
            state_value ^= state_value >> 17;
            state_value ^= state_value << 5;
            input.push_back(static_cast<char>(state_value >> 24));
        }
        for (const auto coding : {http_content_coding::gzip, http_content_coding::brotli, http_content_coding::zstd}) {
            const auto full = encode_http_content(coding, input, {.max_encoded_bytes_ = size * 2 + 1024});
            EXPECT_TRUE(full.has_value());
            if (!full.has_value()) {
                continue;
            }
            const auto bytes_value = std::string_view(*full);
            EXPECT_TRUE(!bytes_value.empty());
            EXPECT_EQ(decoded(coding, bytes_value, input.size()), input);
            if (size >= 8192) {
                EXPECT_TRUE(bytes_value.size() > 8192);
            }
            const auto exact = encode_http_content(coding, input, {.max_encoded_bytes_ = bytes_value.size()});
            EXPECT_TRUE(exact.has_value());
            if (exact.has_value()) {
                EXPECT_EQ(std::string_view(*exact), bytes_value);
            }
            if (!bytes_value.empty()) {
                const auto too_small = encode_http_content(coding, input, {.max_encoded_bytes_ = bytes_value.size() - 1});
                EXPECT_TRUE(!too_small.has_value());
            }
        }
    }
}

TEST(Ruvia, http_content_encoder_round_trips_incremental_chunks) {
    const std::string input =
        "incremental HTTP response data with enough repetition to exercise "
        "each encoder's pending output and finalization state. ";
    const std::string repeated = input + input + input + input + input;

    for (const auto coding : {http_content_coding::identity, http_content_coding::gzip,
             http_content_coding::brotli, http_content_coding::zstd}) {
        std::string encoded;
        std::string chunk;
        http_content_encoder encoder(coding);
        for (std::size_t offset = 0; offset < repeated.size();) {
            const auto size = std::min<std::size_t>(13, repeated.size() - offset);
            chunk.clear();
            encoder.write(std::string_view(repeated).substr(offset, size), chunk);
            encoded.append(chunk);
            offset += size;
        }
        chunk.clear();
        encoder.finish(chunk);
        encoded.append(chunk);
        EXPECT_EQ(decoded(coding, encoded, repeated.size()), repeated);
    }
}

TEST(Ruvia, http_content_encoder_rejects_writes_after_finish) {
    for (const auto coding : {http_content_coding::identity, http_content_coding::gzip,
             http_content_coding::brotli, http_content_coding::zstd}) {
        http_content_encoder encoder(coding);
        std::string output;
        encoder.write("body", output);
        output.clear();
        encoder.finish(output);
        output.clear();
        bool rejected = false;
        try {
            encoder.write("late body", output);
        } catch (const std::logic_error&) {
            rejected = true;
        }
        EXPECT_TRUE(rejected);
        EXPECT_TRUE(output.empty());
        encoder.finish(output);
        EXPECT_TRUE(output.empty());
    }
}

TEST(Ruvia, http_content_encoder_flushes_each_incremental_chunk) {
    const std::string input(4096, 's');

    for (const auto coding :
        {http_content_coding::gzip, http_content_coding::brotli, http_content_coding::zstd}) {
        std::string encoded;
        std::string chunk;
        http_content_encoder encoder(coding);
        for (std::size_t offset = 0; offset < input.size();) {
            const auto size = std::min<std::size_t>(257, input.size() - offset);
            chunk.clear();
            encoder.write(std::string_view(input).substr(offset, size), chunk, true);
            encoded.append(chunk);
            offset += size;
        }
        chunk.clear();
        encoder.finish(chunk);
        encoded.append(chunk);
        EXPECT_EQ(decoded(coding, encoded, input.size()), input);
    }
}

TEST(Ruvia, http_content_encoder_accepts_empty_flushes_between_chunks) {
    for (const auto coding : {http_content_coding::identity, http_content_coding::gzip,
             http_content_coding::deflate, http_content_coding::brotli, http_content_coding::zstd}) {
        std::string encoded;
        http_content_encoder encoder(coding);
        encoder.write({}, encoded, true);
        encoder.write({}, encoded, true);
        encoder.write("first", encoded, true);
        encoder.write({}, encoded, true);
        encoder.write({}, encoded, true);
        encoder.write("second", encoded, true);
        encoder.write({}, encoded, true);
        encoder.write({}, encoded, true);
        encoder.finish(encoded);
        EXPECT_EQ(decoded(coding, encoded, 11), std::string("firstsecond"));
    }
}

TEST(Ruvia, http_brotli_decode_checks_limits_and_complete_stream_after_output_blocks) {
    const std::string input(65537, 'b');
    const auto encoded = brotli_compress(input);
    for (const std::size_t cap : {std::size_t{0}, std::size_t{1}, std::size_t{16383},
             std::size_t{16384}, std::size_t{65536}}) {
        EXPECT_EQ(decode_error(http_content_coding::brotli, encoded, cap), http_content_decode_error::decoded_size_exceeded);
    }
    EXPECT_EQ(decoded(http_content_coding::brotli, encoded, input.size()), input);
    EXPECT_EQ(decode_error(http_content_coding::brotli, std::string_view(encoded).substr(0, encoded.size() - 1), input.size()), http_content_decode_error::invalid_content);
    EXPECT_EQ(decode_error(http_content_coding::brotli, encoded + "trailing", input.size()), http_content_decode_error::invalid_content);
}

TEST(Ruvia, http_content_decode_accepts_empty_encoded_input_as_empty_content) {
    for (const auto coding : {http_content_coding::identity, http_content_coding::gzip,
             http_content_coding::deflate, http_content_coding::brotli, http_content_coding::zstd}) {
        EXPECT_EQ(decoded(coding, {}, 0), std::string{});
    }

    constexpr std::array codings{http_content_coding::gzip, http_content_coding::brotli,
        http_content_coding::zstd};
    auto stacked = decode_http_content(
        codings, {}, {.max_decoded_bytes_ = 0});
    EXPECT_TRUE(std::holds_alternative<std::string>(stacked));
    if (const auto* content = std::get_if<std::string>(&stacked)) {
        EXPECT_TRUE(content->empty());
    }
}

TEST(Ruvia, http_content_decode_zero_cap_allows_only_empty_content) {
    const struct {
        http_content_coding coding_;
        std::string encoded_;
    } empty_cases[] = {
        {http_content_coding::gzip, gzip_compress({})},
        {http_content_coding::brotli, brotli_compress({})},
        {http_content_coding::zstd, zstd_compress({})},
    };
    for (const auto& test : empty_cases) {
        auto result_value = decode_http_content(std::array{test.coding_}, test.encoded_,
            {.max_decoded_bytes_ = 0});
        EXPECT_TRUE(std::holds_alternative<std::string>(result_value));
        if (const auto* content = std::get_if<std::string>(&result_value)) {
            EXPECT_TRUE(content->empty());
        }
    }
    EXPECT_TRUE(decode_error(http_content_coding::gzip, gzip_compress("x"), 0) ==
                http_content_decode_error::decoded_size_exceeded);
}

TEST(Ruvia, zstd_decode_full_frame_succeeds) {
    const std::string plain(4096, 'z');  // compressible payload spanning a block
    const auto decoded = zstd_round_trip(plain, 0);
    EXPECT_TRUE(decoded.has_value());
    if (decoded) {
        EXPECT_EQ(*decoded, plain);
    }
}

TEST(Ruvia, zstd_decode_truncated_frame_rejected) {
    const std::string plain(4096, 'z');
    // Dropping the final bytes yields an incomplete frame that must be rejected,
    // matching the zlib/brotli decoders (regression guard for silent-truncation).
    EXPECT_TRUE(!zstd_round_trip(plain, 4).has_value());
}

TEST(Ruvia, response_coding_sets_and_selection_snapshots_distinguish_every_supported_coding) {
    constexpr ruvia::http_content_coding codings[]{ruvia::http_content_coding::identity,
        ruvia::http_content_coding::gzip, ruvia::http_content_coding::deflate,
        ruvia::http_content_coding::brotli, ruvia::http_content_coding::zstd};
    for (const auto coding : codings) {
        auto candidates = ruvia::http_response_coding_candidates::empty();
        candidates.include(coding);
        ruvia::http_response_coding_qualities qualities;
        const std::string field = std::string(ruvia::http_content_coding_token(coding)) +
                                  ";q=1,*;q=0" + (coding == ruvia::http_content_coding::identity ? "" : ",identity;q=0");
        qualities.update(field);
        const auto selected = ruvia::http_response_coding_selection::select(qualities, candidates);
        EXPECT_TRUE(selected.acceptable());
        EXPECT_TRUE(selected.coding() == coding);
        for (const auto other : codings) {
            EXPECT_TRUE(candidates.contains(other) == (other == coding));
            EXPECT_TRUE(selected.accepts(other) == (other == coding));
            EXPECT_TRUE(ruvia::http_response_coding_candidates::all().contains(other));
        }
    }
}

namespace {

using ruvia::http_accepted_encoding_quality;
using ruvia::http_content_coding;
using ruvia::http_response_coding_candidates;
using ruvia::http_response_coding_qualities;
using ruvia::http_response_coding_selection;

// Reference form: one full Accept-Encoding scan per coding. The aggregate
// single-pass update must produce identical qualities.
struct reference_qualities final {
    http_accepted_encoding_quality gzip_;
    http_accepted_encoding_quality brotli_;
    http_accepted_encoding_quality zstd_;
    http_accepted_encoding_quality identity_;
};

reference_qualities reference_three_pass(std::string_view header_value) {
    reference_qualities ref;
    ref.gzip_.update(header_value, "gzip");
    ref.brotli_.update(header_value, "br");
    ref.zstd_.update(header_value, "zstd");
    ref.identity_.update(header_value, "identity");
    return ref;
}

bool same_quality(const http_accepted_encoding_quality& a, const http_accepted_encoding_quality& b) {
    return a.explicit_quality_ == b.explicit_quality_ && a.wildcard_quality_ == b.wildcard_quality_;
}

http_response_coding_selection select_field(std::string_view header_value) {
    http_response_coding_qualities qualities;
    qualities.update(header_value);
    return http_response_coding_selection::select(qualities);
}

}  // namespace

TEST(Ruvia, response_coding_single_pass_matches_per_coding_scans) {
    const std::string_view cases[] = {
        "gzip, br, zstd",
        "gzip;q=0.5, br;q=0.8, zstd;q=0.3",
        "*",
        "*;q=0.1, gzip;q=0.9",
        "br;q=0, *;q=0.5",
        "identity, gzip",
        "  gzip ,  br ",
        "GZIP, Br, ZSTD",  // token match is case-insensitive
        "x-gzip, br",
        "X-GZIP;q=0.8, gzip;q=0.2, *;q=0.1",
        "gzip;q=0.2, x-gzip;q=0.8",
        "",
        "deflate;q=0.2",  // unknown coding: leaves all three untouched
        "gzip;q=0, gzip;q=0.9",
        ", gzip, , br,",  // empty items are skipped
        "gzip;q=1.0, br;q=0.500",
        R"(gzip;note="a,b";q=0, br;q=0.5)",
    };
    for (const auto header : cases) {
        http_response_coding_qualities qualities;
        qualities.update(header);

        const auto ref = reference_three_pass(header);
        EXPECT_TRUE(same_quality(qualities.gzip_, ref.gzip_));
        EXPECT_TRUE(same_quality(qualities.brotli_, ref.brotli_));
        EXPECT_TRUE(same_quality(qualities.zstd_, ref.zstd_));
        EXPECT_TRUE(same_quality(qualities.identity_, ref.identity_));
    }
}

TEST(Ruvia, response_coding_selection_end_to_end) {
    const auto select = [](std::string_view header_value) {
        const auto selected = select_field(header_value);
        if (!selected.acceptable()) {
            throw std::runtime_error("test expected an acceptable response coding");
        }
        return selected.coding();
    };
    // Server tie-break prefers br > zstd > gzip > deflate at equal q.
    EXPECT_TRUE(select("gzip, br, zstd") == http_content_coding::brotli);
    // Explicit q ordering wins over the tie-break.
    EXPECT_TRUE(select("identity;q=0, gzip;q=0.9, br;q=0.1") == http_content_coding::gzip);
    // A coding at q=0 is excluded even under a permissive wildcard.
    EXPECT_TRUE(select("identity;q=0, br;q=0, *;q=0.5") == http_content_coding::zstd);
    // identity is implicitly q=1, so a lower-quality coding must not override it.
    EXPECT_TRUE(select("gzip;q=0.9") == http_content_coding::identity);
    // A positive wildcard does not lower identity's implicit quality, while an
    // explicit identity preference does.
    EXPECT_TRUE(select("*;q=0.5") == http_content_coding::identity);
    EXPECT_TRUE(select("identity;q=0.1, gzip;q=0.5") == http_content_coding::gzip);
    EXPECT_TRUE(select("identity;q=0.1, deflate;q=0.5") == http_content_coding::deflate);
    // Repeating the same coding is equivalent to multiple matching alternatives:
    // the highest qvalue wins, independently of list order.
    EXPECT_TRUE(select("identity;q=0.5, gzip;q=0.9, gzip;q=0.1") == http_content_coding::gzip);
    EXPECT_TRUE(select("identity;q=0.5, gzip;q=0.1, gzip;q=0.9") == http_content_coding::gzip);
    // The same rule applies to repeated wildcard entries for an unlisted coding.
    EXPECT_TRUE(select("identity;q=0, *;q=0.8, *;q=0.1") == http_content_coding::brotli);
    http_response_coding_qualities repeated;
    repeated.update("gzip;q=0.9, gzip;q=0.1, *;q=0.8, *;q=0.2");
    EXPECT_EQ(repeated.gzip_.explicit_quality_, 900);
    EXPECT_EQ(repeated.brotli_.wildcard_quality_, 800);
    http_response_coding_qualities split_lines;
    split_lines.update("gzip;q=0.9, *;q=0.8");
    split_lines.update("gzip;q=0.1, *;q=0.2");
    EXPECT_EQ(split_lines.gzip_.explicit_quality_, 900);
    EXPECT_EQ(split_lines.brotli_.wildcard_quality_, 800);
    // Accept-Encoding allows only an optional weight after a coding. Unknown
    // parameters and whitespace around q's '=' make the item invalid; they must
    // not inherit the default q=1 and outrank identity.
    EXPECT_TRUE(select("identity;q=0.5, gzip;level=9") == http_content_coding::identity);
    EXPECT_TRUE(select("identity;q=0.5, gzip;q =1") == http_content_coding::identity);
    EXPECT_TRUE(select("identity;q=0.5, gzip;q= 1") == http_content_coding::identity);
    // OWS around the weight delimiter itself is explicitly allowed.
    EXPECT_TRUE(select("identity;q=0.5, gzip \t; \tq=0.8") == http_content_coding::gzip);
    // No acceptable coding is an explicit protocol outcome, not identity.
    for (const auto header : {std::string_view{"identity;q=0"}, std::string_view{"*;q=0"}}) {
        const auto result_value = select_field(header);
        EXPECT_TRUE(!result_value.acceptable());
        EXPECT_TRUE(!result_value.identity_accepted());
    }
    const auto explicit_empty = select_field("");
    EXPECT_TRUE(explicit_empty.acceptable());
    EXPECT_TRUE(explicit_empty.coding() == http_content_coding::identity);
    EXPECT_TRUE(explicit_empty.identity_accepted());
    EXPECT_TRUE(explicit_empty.accepts(http_content_coding::identity));
    EXPECT_TRUE(!explicit_empty.accepts(http_content_coding::gzip));

    const auto no_header = http_response_coding_selection::select(http_response_coding_qualities{});
    EXPECT_TRUE(no_header.acceptable());
    EXPECT_TRUE(no_header.coding() == http_content_coding::identity);
    EXPECT_TRUE(no_header.identity_accepted());
    EXPECT_TRUE(no_header.accepts(http_content_coding::identity));
    EXPECT_TRUE(no_header.accepts(http_content_coding::gzip));

    auto gzip_only = http_response_coding_candidates::empty();
    gzip_only.include(http_content_coding::gzip);
    const auto absent_header_gzip =
        http_response_coding_selection::select(http_response_coding_qualities{}, gzip_only);
    EXPECT_TRUE(absent_header_gzip.acceptable());
    EXPECT_TRUE(absent_header_gzip.coding() == http_content_coding::gzip);
    EXPECT_TRUE(absent_header_gzip.accepts(http_content_coding::gzip));

    http_response_coding_qualities empty_header;
    empty_header.update("");
    // An explicitly empty Accept-Encoding allows only an uncoded representation,
    // so a response policy with only gzip has no acceptable candidate.
    const auto explicit_empty_gzip = http_response_coding_selection::select(empty_header, gzip_only);
    EXPECT_TRUE(!explicit_empty_gzip.acceptable());
}

TEST(Ruvia, accepted_encoding_quality_treats_gzip_aliases_as_the_same_coding) {
    for (const auto coding : {std::string_view{"gzip"}, std::string_view{"x-gzip"}, std::string_view{"X-GZIP"}}) {
        EXPECT_TRUE(ruvia::http_accepts_encoding("x-gzip", coding));
        EXPECT_TRUE(ruvia::http_accepts_encoding("GZIP", coding));
        EXPECT_TRUE(!ruvia::http_accepts_encoding("x-gzip;q=0, *;q=1", coding));
        EXPECT_TRUE(!ruvia::http_accepts_encoding("gzip;q=0, *;q=1", coding));
        EXPECT_TRUE(ruvia::http_accepts_encoding("x-gzip;q=0.8, *;q=0", coding));
    }
}

TEST(Ruvia, response_coding_selection_accepts_the_gzip_alias) {
    for (const auto header : {std::string_view{"x-gzip, identity;q=0"}, std::string_view{"X-GZIP;q=0.8, identity;q=0"}}) {
        const auto selected = select_field(header);
        EXPECT_TRUE(selected.acceptable());
        EXPECT_TRUE(selected.coding() == http_content_coding::gzip);
        EXPECT_TRUE(selected.accepts(http_content_coding::gzip));
        EXPECT_TRUE(!selected.identity_accepted());
        EXPECT_TRUE(!selected.accepts(http_content_coding::identity));
        EXPECT_EQ(ruvia::http_content_coding_token(selected.coding()), "gzip");
    }
}

TEST(Ruvia, response_coding_gzip_aliases_share_weights_across_field_lines) {
    for (const bool alias_first : {false, true}) {
        const auto first = alias_first ? "x-gzip;q=0.8" : "gzip;q=0.2";
        const auto second = alias_first ? "gzip;q=0.2" : "X-GZIP;q=0.8";
        http_response_coding_qualities qualities;
        qualities.update(first);
        qualities.update(second);
        EXPECT_EQ(qualities.gzip_.explicit_quality_, 800);
        for (const auto coding : {std::string_view{"gzip"}, std::string_view{"x-gzip"}}) {
            http_accepted_encoding_quality quality;
            quality.update(first, coding);
            quality.update(second, coding);
            EXPECT_EQ(quality.explicit_quality_, qualities.gzip_.explicit_quality_);
        }
    }
}

TEST(Ruvia, response_coding_selection_retains_client_preference_until_representation_policy) {
    // The selector records the client's acceptable representation set. Whether
    // a Web runtime can create gzip is enforced only after the representation
    // source (sidecar, buffered body, or stream) is known.
    const auto coding = select_field("gzip, identity;q=0");
    EXPECT_TRUE(coding.acceptable());
    EXPECT_TRUE(coding.coding() == http_content_coding::gzip);
    EXPECT_TRUE(!coding.identity_accepted());
    EXPECT_TRUE(coding.accepts(http_content_coding::gzip));
    EXPECT_TRUE(!coding.accepts(http_content_coding::identity));
}

TEST(Ruvia, response_coding_selection_uses_only_available_representations) {
    http_response_coding_qualities qualities;
    qualities.update("br, gzip, identity;q=0");

    auto gzip_only = http_response_coding_candidates::identity_only();
    gzip_only.include(http_content_coding::gzip);
    const auto gzip_selection = http_response_coding_selection::select(qualities, gzip_only);
    EXPECT_TRUE(gzip_selection.acceptable());
    EXPECT_TRUE(gzip_selection.coding() == http_content_coding::gzip);

    auto identity_only = http_response_coding_candidates::identity_only();
    const auto identity_selection = http_response_coding_selection::select(qualities, identity_only);
    EXPECT_TRUE(!identity_selection.acceptable());

    // A failed ranking keeps the client's snapshot: the same selection still
    // answers for codings it accepts and ranks another representation set.
    auto brotli_only = http_response_coding_candidates::empty();
    brotli_only.include(http_content_coding::brotli);
    EXPECT_TRUE(identity_selection.accepts(http_content_coding::brotli));
    const auto brotli_selection = identity_selection.select(brotli_only);
    EXPECT_TRUE(brotli_selection.acceptable());
    EXPECT_TRUE(brotli_selection.coding() == http_content_coding::brotli);
}

TEST(Ruvia, http_accepts_encoding_rfc9110_identity_rules) {
    using ruvia::http_accepts_encoding;

    // RFC 9110 §12.5.3: Empty field-value accepts only identity.
    EXPECT_TRUE(http_accepts_encoding("", "identity"));
    EXPECT_TRUE(!http_accepts_encoding("", "gzip"));

    // RFC 9110 §12.5.3: "identity" is always acceptable unless specifically refused.
    EXPECT_TRUE(http_accepts_encoding("gzip", "identity"));
    EXPECT_TRUE(http_accepts_encoding("gzip, br", "identity"));
    EXPECT_TRUE(http_accepts_encoding("*;q=0.5", "identity"));

    // Explicitly refused with identity;q=0.
    EXPECT_TRUE(!http_accepts_encoding("identity;q=0", "identity"));
    EXPECT_TRUE(!http_accepts_encoding("gzip, identity;q=0", "identity"));

    // Refused by wildcard *;q=0 when identity is not specifically included.
    EXPECT_TRUE(!http_accepts_encoding("*;q=0", "identity"));
    EXPECT_TRUE(!http_accepts_encoding("gzip, *;q=0", "identity"));

    // Explicit identity;q>0 overrides *;q=0.
    EXPECT_TRUE(http_accepts_encoding("gzip, *;q=0, identity;q=0.5", "identity"));

    // Explicit identity;q>0.
    EXPECT_TRUE(http_accepts_encoding("identity;q=0.1", "identity"));

    // http_accepted_encoding_quality accepts(bool) overload check.
    http_accepted_encoding_quality quality;
    quality.update("gzip", "identity");
    EXPECT_TRUE(quality.accepts(true));
    EXPECT_TRUE(!quality.accepts(false));
}

TEST(Ruvia, accepted_encoding_weights_do_not_allow_media_parameter_slots) {
    for (const std::string_view item : {"gzip;", "gzip;;q=1", "gzip;q=1;", "gzip; ;q=1;"}) {
        EXPECT_TRUE(!ruvia::http_accepts_encoding(item, "gzip"));
        http_response_coding_qualities qualities;
        qualities.update("identity;q=0.5");
        qualities.update(item);
        const auto selected = http_response_coding_selection::select(qualities);
        EXPECT_TRUE(selected.acceptable());
        EXPECT_TRUE(selected.coding() == http_content_coding::identity);
        EXPECT_TRUE(!selected.accepts(http_content_coding::gzip));
    }
    EXPECT_TRUE(!ruvia::http_accepts_encoding("*;;q=1", "extension"));
    EXPECT_TRUE(!ruvia::http_accepts_encoding("extension;q=0.8;", "extension"));
    EXPECT_TRUE(ruvia::http_accepts_encoding("extension;q=0.8", "extension"));
}

TEST(Ruvia, response_custom_coding_distinguishes_missing_and_empty_preferences) {
    const auto missing = ruvia::test::parse_http1_request_message("GET / HTTP/1.1\r\nHost: x\r\n\r\n");
    const auto empty = ruvia::test::parse_http1_request_message("GET / HTTP/1.1\r\nHost: x\r\nAccept-Encoding:\r\n\r\n");
    ASSERT_TRUE(missing.complete());
    ASSERT_TRUE(empty.complete());
    EXPECT_TRUE(ruvia::http_accepts_encoding(missing.state_.request(), "custom"));
    EXPECT_FALSE(ruvia::http_accepts_encoding(empty.state_.request(), "custom"));
    EXPECT_TRUE(ruvia::http_accepts_encoding(empty.state_.request(), "identity"));
    ruvia::http_response response;
    response.header("Content-Encoding", "custom");
    EXPECT_EQ(ruvia::evaluate_http_response_content_coding(
                  ruvia::http_response_coding_selection_for(missing.state_.request()), response, &missing.state_.request()),
        ruvia::http_response_content_coding_status::accepted);
    EXPECT_EQ(ruvia::evaluate_http_response_content_coding(
                  ruvia::http_response_coding_selection_for(empty.state_.request()), response, &empty.state_.request()),
        ruvia::http_response_content_coding_status::not_acceptable);
}

TEST(Ruvia, response_custom_coding_explicit_quality_overrides_wildcard_across_lines) {
    const auto parsed = ruvia::test::parse_http1_request_message(
        "GET / HTTP/1.1\r\nHost: x\r\n"
        "Accept-Encoding: *;q=0.8, custom;q=0, gzip\r\n"
        "Accept-Encoding: CUSTOM;q=0.4, x-compress;q=0\r\n\r\n");
    ASSERT_TRUE(parsed.complete());
    const auto selection = ruvia::http_response_coding_selection_for(parsed.state_.request());
    EXPECT_TRUE(selection.accepts("CuStOm", parsed.state_.request()));
    EXPECT_TRUE(selection.accepts("another", parsed.state_.request()));
    EXPECT_FALSE(selection.accepts("compress", parsed.state_.request()));
    ruvia::http_response response;
    response.header("Content-Encoding", "gzip, CUSTOM");
    EXPECT_EQ(ruvia::evaluate_http_response_content_coding(selection, response, &parsed.state_.request()),
        ruvia::http_response_content_coding_status::accepted);
    response.header("Content-Encoding", "compress", {.mode_ = ruvia::http_response_header_mode::append});
    EXPECT_EQ(ruvia::evaluate_http_response_content_coding(selection, response, &parsed.state_.request()),
        ruvia::http_response_content_coding_status::not_acceptable);
}

TEST(Ruvia, response_coding_stack_checks_every_known_and_custom_member_without_common_case_allocation) {
    const auto parsed = ruvia::test::parse_http1_request_message(
        "GET / HTTP/1.1\r\nHost: x\r\nAccept-Encoding: gzip, custom, other, br;q=0\r\n\r\n");
    ASSERT_TRUE(parsed.complete());
    const auto selection = ruvia::http_response_coding_selection_for(parsed.state_.request());
    ruvia::http_response response;
    EXPECT_EQ(ruvia::evaluate_http_response_content_coding(selection, response, &parsed.state_.request()),
        ruvia::http_response_content_coding_status::absent);
    response.header("Content-Encoding", "gzip, CUSTOM, other, custom");
    EXPECT_EQ(ruvia::evaluate_http_response_content_coding(selection, response, &parsed.state_.request()),
        ruvia::http_response_content_coding_status::accepted);
    response.header("Content-Encoding", "br", {.mode_ = ruvia::http_response_header_mode::append});
    EXPECT_EQ(ruvia::evaluate_http_response_content_coding(selection, response, &parsed.state_.request()),
        ruvia::http_response_content_coding_status::not_acceptable);
    EXPECT_THROW(response.header("Content-Encoding", "gzip;param=value"), std::invalid_argument);
}

TEST(Ruvia, response_coding_extension_stack_is_not_limited_by_decoder_depth) {
    const auto parsed = ruvia::test::parse_http1_request_message(
        "GET / HTTP/1.1\r\nHost: x\r\nAccept-Encoding: *, ninth;q=0\r\n\r\n");
    ASSERT_TRUE(parsed.complete());
    const auto selection = ruvia::http_response_coding_selection_for(parsed.state_.request());
    ruvia::http_response response;
    response.header("Content-Encoding", "first, second, third, fourth, fifth, sixth, seventh, eighth, ninth");
    EXPECT_EQ(ruvia::evaluate_http_response_content_coding(selection, response, &parsed.state_.request()),
        ruvia::http_response_content_coding_status::not_acceptable);
    response.header("Content-Encoding", "first, second, third, fourth, fifth, sixth, seventh, eighth, tenth");
    EXPECT_EQ(ruvia::evaluate_http_response_content_coding(selection, response, &parsed.state_.request()),
        ruvia::http_response_content_coding_status::accepted);
}

using ruvia::decode_http_content;
using ruvia::encode_http_content;
using ruvia::http_content_coding;
using ruvia::http_content_encode_context;

namespace {

constexpr std::array compressing_codings{http_content_coding::gzip, http_content_coding::deflate,
    http_content_coding::brotli, http_content_coding::zstd};

// Compressible but not trivially repetitive, so encoders emit several output
// blocks for the larger sizes.
[[nodiscard]] std::string sample_body(std::size_t size, unsigned seed) {
    std::string body(size, '\0');
    unsigned state_value = seed * 2654435761U + 1U;
    for (std::size_t i = 0; i < size; ++i) {
        state_value = state_value * 1103515245U + 12345U;
        body[i] = static_cast<char>('a' + ((state_value >> 16U) % 7U) + ((i / 64U) % 3U));
    }
    return body;
}

[[nodiscard]] std::string one_shot(http_content_coding coding, std::string_view input, std::size_t cap) {
    auto result_value = encode_http_content(coding, input, {.max_encoded_bytes_ = cap});
    return result_value.has_value() ? std::move(*result_value) : std::string();
}

[[nodiscard]] std::optional<std::string> encode_with(http_content_encode_context& context,
    std::string_view input, std::size_t cap) {
    auto result_value = context.encode(input, {.max_encoded_bytes_ = cap});
    if (!result_value.has_value()) {
        return std::nullopt;
    }
    return std::string(*result_value);
}

}  // namespace

TEST(Ruvia, http_content_encode_context_reuse_matches_one_shot_bytes) {
    constexpr std::array sizes{std::size_t{0}, std::size_t{1}, std::size_t{1000}, std::size_t{8192},
        std::size_t{70'000}, std::size_t{300'000}};
    for (const auto coding : compressing_codings) {
        http_content_encode_context context(coding);
        EXPECT_TRUE(context.coding() == coding);
        unsigned seed = 0;
        // Twice through, so every size also follows a larger representation.
        for (int round = 0; round < 2; ++round) {
            for (const auto size : sizes) {
                const auto input = sample_body(size, ++seed);
                const auto expected = one_shot(coding, input, input.size() + 1024);
                const auto actual = encode_with(context, input, input.size() + 1024);
                EXPECT_TRUE(actual.has_value());
                EXPECT_TRUE(actual.value_or(std::string()) == expected);
                const auto decoded = decode_http_content(
                    std::array{coding}, actual.value_or(std::string()), {.max_decoded_bytes_ = size});
                EXPECT_TRUE(std::holds_alternative<std::string>(decoded));
                if (const auto* content = std::get_if<std::string>(&decoded)) {
                    EXPECT_TRUE(std::string_view(*content) == input);
                }
            }
        }
    }
}

namespace {

void check_encoder_alias(ruvia::http_content_coding coding) {
    std::string input(8 * 1024 * 1024 + 37, '\0');
    std::uint64_t random = 0x2b19ab247313fc6dULL;
    for (auto& byte : input) {
        random ^= random << 13;
        random ^= random >> 7;
        random ^= random << 17;
        byte = static_cast<char>(random);
    }

    for (const std::size_t input_offset : {std::size_t{0}, std::size_t{23}}) {
        {
            std::string output(input_offset, 'p');
            output.append(input);
            const auto padding_offset = output.size();
            output.resize(output.capacity(), '#');
            const auto prefix_size = output.size();
            {
                ruvia::http_content_encoder encoder(coding);
                encoder.write(std::string_view(output).substr(input_offset, input.size()), output, true);
                encoder.finish(output);
            }
            EXPECT_EQ(std::string_view(output).substr(0, input_offset), std::string(input_offset, 'p'));
            EXPECT_EQ(std::string_view(output).substr(input_offset, input.size()), std::string_view(input));
            EXPECT_EQ(std::string_view(output).substr(padding_offset, prefix_size - padding_offset), std::string(prefix_size - padding_offset, '#'));

            const auto decoded = ruvia::decode_http_content(std::array{coding},
                std::string_view(output).substr(prefix_size),
                {.max_decoded_bytes_ = input.size()});
            EXPECT_TRUE(std::holds_alternative<std::string>(decoded));
            if (const auto* content = std::get_if<std::string>(&decoded)) {
                EXPECT_EQ(std::string_view(*content), std::string_view(input));
            }
        }
    }
}

}  // namespace

TEST(Ruvia, http_content_encoder_identity_accepts_input_borrowed_from_output) {
    check_encoder_alias(ruvia::http_content_coding::identity);
}

TEST(Ruvia, http_content_encoder_gzip_accepts_input_borrowed_from_output) {
    check_encoder_alias(ruvia::http_content_coding::gzip);
}

TEST(Ruvia, http_content_encoder_deflate_accepts_input_borrowed_from_output) {
    check_encoder_alias(ruvia::http_content_coding::deflate);
}

TEST(Ruvia, http_content_encoder_brotli_accepts_input_borrowed_from_output) {
    check_encoder_alias(ruvia::http_content_coding::brotli);
}

TEST(Ruvia, http_content_encoder_zstd_accepts_input_borrowed_from_output) {
    check_encoder_alias(ruvia::http_content_coding::zstd);
}

TEST(Ruvia, response_content_encoding_folds_header_lines_in_application_order) {
    ruvia::http_response response;
    const auto missing = ruvia::parse_http_content_coding_headers(response.headers());
    EXPECT_TRUE(missing.codings().empty());
    EXPECT_TRUE(missing.invalid() == nullptr);
    EXPECT_TRUE(missing.unsupported() == nullptr);

    response.header("Content-Encoding", "gzip");
    response.header("Content-Encoding", "deflate",
        {.mode_ = ruvia::http_response_header_mode::append});
    response.header("Content-Encoding", "br",
        {.mode_ = ruvia::http_response_header_mode::append});
    const auto stacked = ruvia::parse_http_content_coding_headers(response.headers());
    constexpr std::array expected{ruvia::http_content_coding::gzip,
        ruvia::http_content_coding::deflate, ruvia::http_content_coding::brotli};
    EXPECT_TRUE(stacked.invalid() == nullptr);
    EXPECT_TRUE(stacked.unsupported() == nullptr);
    EXPECT_EQ(stacked.codings().size(), expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        EXPECT_TRUE(stacked.codings()[i] == expected[i]);
    }
}

TEST(Ruvia, content_encoding_field_lines_fold_in_order_ignoring_other_fields) {
    const std::array unrelated{ruvia::http_header("Content-Type", "text/plain")};
    const auto missing = ruvia::parse_http_content_coding_headers(unrelated);
    EXPECT_TRUE(missing.codings().empty());
    EXPECT_TRUE(missing.invalid() == nullptr);
    EXPECT_TRUE(missing.unsupported() == nullptr);

    const std::array lines{ruvia::http_header("Content-Encoding", "gzip"),
        ruvia::http_header("Content-Type", "text/plain"), ruvia::http_header("content-encoding", "deflate"),
        ruvia::http_header("Content-Encoding", "br")};
    const auto stacked = ruvia::parse_http_content_coding_headers(lines);
    constexpr std::array expected{ruvia::http_content_coding::gzip,
        ruvia::http_content_coding::deflate, ruvia::http_content_coding::brotli};
    EXPECT_TRUE(stacked.invalid() == nullptr);
    EXPECT_TRUE(stacked.unsupported() == nullptr);
    EXPECT_EQ(stacked.codings().size(), expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        EXPECT_TRUE(stacked.codings()[i] == expected[i]);
    }
}

namespace {

// True if a public cookie plan rejects the options.
bool rejects(const ruvia::cookie_options& options) {
    try {
        (void)ruvia::set_cookie_plan("sid", "value", options);
        return false;
    } catch (const std::invalid_argument&) {
        return true;
    }
}

}  // namespace

TEST(Ruvia, cookie_public_validation_and_prefix_parsing) {
    EXPECT_TRUE(ruvia::is_valid_cookie_value("session-token"));
    EXPECT_TRUE(!ruvia::is_valid_cookie_value("invalid;value"));
    EXPECT_TRUE(ruvia::is_valid_cookie_value("\"abc\""));
    EXPECT_TRUE(ruvia::is_valid_cookie_value("\"\""));
    EXPECT_TRUE(!ruvia::is_valid_cookie_value("\"abc"));
    EXPECT_TRUE(!ruvia::is_valid_cookie_value("\"a\"b\""));
    EXPECT_TRUE(!ruvia::is_valid_cookie_value("\""));
    const ruvia::cookie_options no_attributes{.path_ = ""};
    const ruvia::set_cookie_plan quoted("sid", "\"abc\"", no_attributes);
    std::string quoted_wire(quoted.size(), '\0');
    quoted.write(quoted_wire.data());
    EXPECT_EQ(quoted_wire, "sid=\"abc\"");
    EXPECT_TRUE(ruvia::cookie_name_prefix("__secure-id") == ruvia::cookie_prefix::secure);
    EXPECT_TRUE(ruvia::cookie_name_prefix("__HOST-id") == ruvia::cookie_prefix::host);
    EXPECT_TRUE(!ruvia::cookie_name_prefix("id").has_value());
    EXPECT_TRUE(ruvia::cookie_prefix_requirements_met(ruvia::cookie_prefix::secure, true, "/a", "example.com"));
    EXPECT_TRUE(!ruvia::cookie_prefix_requirements_met(ruvia::cookie_prefix::secure, false, "/", ""));
    EXPECT_TRUE(ruvia::cookie_prefix_requirements_met(ruvia::cookie_prefix::host, true, "/", ""));
    EXPECT_TRUE(!ruvia::cookie_prefix_requirements_met(ruvia::cookie_prefix::host, true, "", ""));
    EXPECT_TRUE(!ruvia::cookie_prefix_requirements_met(ruvia::cookie_prefix::host, true, "/", "example.com"));
    EXPECT_EQ(ruvia::http_cookie_prefix_text(ruvia::cookie_prefix::host), std::string_view("__Host-"));
}

TEST(Ruvia, cookie_borrowed_text_accepts_stable_string_owners) {
    const std::string path = "/account";
    const std::string domain = "example.com";
    std::string name = "sid";
    const std::string value = "value";

    ruvia::cookie_options options;
    options.path_ = path;
    options.domain_ = domain;
    EXPECT_TRUE(!rejects(options));

    const ruvia::set_cookie_plan plan(name, value, options);
    std::string wire(plan.size(), '\0');
    plan.write(wire.data());
    name.assign("changed");
    EXPECT_EQ(wire, std::string("sid=value; Path=/account; Domain=example.com"));
    const auto parsed_value = ruvia::parse_set_cookie(wire);
    EXPECT_TRUE(parsed_value.has_value());
    if (parsed_value) {
        EXPECT_EQ(parsed_value->name(), "sid");
        EXPECT_EQ(parsed_value->path(), "/account");
        EXPECT_EQ(parsed_value->domain(), "example.com");
        EXPECT_TRUE(parsed_value->name().data() == wire.data());
        EXPECT_TRUE(parsed_value->path().data() == wire.data() + wire.find("/account"));
        EXPECT_TRUE(parsed_value->domain().data() == wire.data() + wire.find("example.com"));
    }
}

TEST(Ruvia, cookie_expires_formats_historical_dates_at_second_resolution) {
    using namespace std::chrono;
    ruvia::cookie_options options;
    options.expires_ = system_clock::time_point{seconds{-315619200}};
    EXPECT_TRUE(!rejects(options));
    const ruvia::set_cookie_plan historical("sid", "value", options);
    std::string wire(historical.size(), '\0');
    historical.write(wire.data());
    EXPECT_EQ(wire, std::string("sid=value; Path=/; Expires=Fri, 01 Jan 1960 00:00:00 GMT"));

    options.expires_ = system_clock::time_point{} - system_clock::duration{1};
    const ruvia::set_cookie_plan fractional("sid", "value", options);
    wire.resize(fractional.size());
    fractional.write(wire.data());
    EXPECT_EQ(wire, std::string("sid=value; Path=/; Expires=Wed, 31 Dec 1969 23:59:59 GMT"));
}

TEST(Ruvia, cookie_expires_rejects_dates_before_the_cookie_calendar_range) {
    using namespace std::chrono;
    constexpr auto cutoff = duration_cast<seconds>(sys_days{year{1601} / January / 1}.time_since_epoch());
    // Clocks with nanosecond int64 storage cannot represent dates this early;
    // wider calendar ranges (including MSVC's 100ns clock) exercise the edge.
    if constexpr (ceil<seconds>(system_clock::duration::min()) <= cutoff) {
        ruvia::cookie_options options;
        options.expires_ = system_clock::time_point{duration_cast<system_clock::duration>(cutoff)};
        EXPECT_TRUE(!rejects(options));
        const ruvia::set_cookie_plan first("sid", "value", options);
        std::string wire(first.size(), '\0');
        first.write(wire.data());
        EXPECT_TRUE(wire.find("01 Jan 1601") != std::string::npos);
        options.expires_ = *options.expires_ - system_clock::duration{1};
        EXPECT_TRUE(rejects(options));
        bool rejected = false;
        try {
            (void)ruvia::set_cookie_plan("sid", "value", options);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        EXPECT_TRUE(rejected);
    }
}

TEST(Ruvia, cookie_samesite_enum_maps_to_wire_tokens) {
    ruvia::cookie_options strict;
    strict.same_site_ = ruvia::cookie_same_site::strict;
    EXPECT_TRUE(!rejects(strict));

    ruvia::cookie_options lax;
    lax.same_site_ = ruvia::cookie_same_site::lax;
    EXPECT_TRUE(!rejects(lax));

    ruvia::cookie_options none;
    none.same_site_ = ruvia::cookie_same_site::none;
    none.secure_ = ruvia::cookie_attribute_policy::emit;
    EXPECT_TRUE(!rejects(none));

    EXPECT_TRUE(!ruvia::cookie_options{}.same_site_.has_value());
}

TEST(Ruvia, cookie_samesite_none_requires_secure) {
    ruvia::cookie_options insecure_none;
    insecure_none.same_site_ = ruvia::cookie_same_site::none;
    insecure_none.secure_ = ruvia::cookie_attribute_policy::omit;
    EXPECT_TRUE(rejects(insecure_none));  // RFC 6265bis §5.5

    ruvia::cookie_options secure_none;
    secure_none.same_site_ = ruvia::cookie_same_site::none;
    secure_none.secure_ = ruvia::cookie_attribute_policy::emit;
    EXPECT_TRUE(!rejects(secure_none));
}

TEST(Ruvia, cookie_domain_requires_dns_subdomain_syntax) {
    const auto accepts_domain = [](std::string_view domain) {
        ruvia::cookie_options options;
        options.domain_ = domain;
        return !rejects(options);
    };

    EXPECT_TRUE(accepts_domain("example.com"));
    EXPECT_TRUE(accepts_domain("EXAMPLE.com"));
    EXPECT_TRUE(accepts_domain("localhost"));
    EXPECT_TRUE(accepts_domain("3.example"));
    EXPECT_TRUE(accepts_domain("xn--bcher-kva.example"));

    // Domain= is a DNS subdomain, not the generic Set-Cookie av-octet syntax
    // used by Path. Emitting any of these values produces a non-conforming
    // Set-Cookie field that user agents ignore or interpret inconsistently.
    EXPECT_TRUE(!accepts_domain(".example.com"));
    EXPECT_TRUE(!accepts_domain("example.com."));
    EXPECT_TRUE(!accepts_domain("bad domain.example"));
    EXPECT_TRUE(!accepts_domain("bad_domain.example"));
    EXPECT_TRUE(!accepts_domain("-bad.example"));
    EXPECT_TRUE(!accepts_domain("bad-.example"));
    EXPECT_TRUE(!accepts_domain("bad..example"));
    EXPECT_TRUE(!accepts_domain("caf\xc3\xa9.example"));
    EXPECT_TRUE(!accepts_domain(std::string(64, 'a') + ".example"));
}

TEST(Ruvia, cookie_validation_rejects_injection_and_bad_options) {
    const auto rejects_cookie = [](std::string_view name, std::string_view value,
                                    const ruvia::cookie_options& options) {
        try {
            (void)ruvia::set_cookie_plan(name, value, options);
            return false;
        } catch (const std::invalid_argument&) {
            return true;
        }
    };
    const ruvia::cookie_options clean;
    EXPECT_TRUE(!rejects_cookie("sid", "value", clean));
    EXPECT_TRUE(rejects_cookie("bad name", "value", clean));  // space -> not a token name
    EXPECT_TRUE(rejects_cookie("sid", "a;b", clean));         // ';' in value
    ruvia::cookie_options bad_path;
    bad_path.path_ = "a;b";
    EXPECT_TRUE(rejects_cookie("sid", "value", bad_path));  // ';' in path attribute
    // Inputs the Set-Cookie parser would trim, drop, or ignore cannot be found
    // again for same-name replacement.
    for (const std::string_view path : {" /", "/ ", " "}) {
        ruvia::cookie_options spaced_path;
        spaced_path.path_ = path;
        EXPECT_TRUE(rejects_cookie("sid", "value", spaced_path));
    }
    const std::string long_path(1025, 'p');
    ruvia::cookie_options oversized_path;
    oversized_path.path_ = long_path;
    EXPECT_TRUE(rejects_cookie("sid", "value", oversized_path));
    oversized_path.path_ = std::string_view(long_path).substr(1);
    EXPECT_TRUE(!rejects_cookie("sid", "value", oversized_path));
    const std::string long_value(4093, 'v');
    EXPECT_TRUE(!rejects_cookie("sid", long_value, clean));
    EXPECT_TRUE(rejects_cookie("sidx", long_value, clean));
    ruvia::cookie_options typed_prefix;
    typed_prefix.secure_ = ruvia::cookie_attribute_policy::emit;
    typed_prefix.prefix_ = ruvia::cookie_prefix::secure;
    EXPECT_TRUE(rejects_cookie("sid", long_value, typed_prefix));
    ruvia::cookie_options bad_priority;
    bad_priority.priority_ = static_cast<ruvia::cookie_priority>(255);
    EXPECT_TRUE(rejects_cookie("sid", "value", bad_priority));
    ruvia::cookie_options bad_same_site;
    bad_same_site.same_site_ = static_cast<ruvia::cookie_same_site>(255);
    EXPECT_TRUE(rejects_cookie("sid", "value", bad_same_site));
    ruvia::cookie_options bad_prefix;
    bad_prefix.prefix_ = static_cast<ruvia::cookie_prefix>(255);
    EXPECT_TRUE(rejects_cookie("sid", "value", bad_prefix));
    ruvia::cookie_options partitioned;
    partitioned.partitioned_ = ruvia::cookie_attribute_policy::emit;  // partitioned requires Secure
    EXPECT_TRUE(rejects_cookie("sid", "value", partitioned));
    ruvia::cookie_options bad_http_only;
    bad_http_only.http_only_ = static_cast<ruvia::cookie_attribute_policy>(255);
    EXPECT_TRUE(rejects_cookie("sid", "value", bad_http_only));
    ruvia::cookie_options bad_secure;
    bad_secure.secure_ = static_cast<ruvia::cookie_attribute_policy>(255);
    EXPECT_TRUE(rejects_cookie("sid", "value", bad_secure));
    ruvia::cookie_options bad_partitioned;
    bad_partitioned.partitioned_ = static_cast<ruvia::cookie_attribute_policy>(255);
    EXPECT_TRUE(rejects_cookie("sid", "value", bad_partitioned));
    ruvia::cookie_options negative_max_age;
    negative_max_age.max_age_ = std::chrono::seconds{-1};
    EXPECT_TRUE(rejects_cookie("sid", "value", negative_max_age));
}

TEST(Ruvia, cookie_secure_prefix_requires_secure) {
    ruvia::cookie_options secured;
    secured.prefix_ = ruvia::cookie_prefix::secure;
    secured.secure_ = ruvia::cookie_attribute_policy::emit;
    EXPECT_TRUE(!rejects(secured));

    ruvia::cookie_options insecure;
    insecure.prefix_ = ruvia::cookie_prefix::secure;
    insecure.secure_ = ruvia::cookie_attribute_policy::omit;
    EXPECT_TRUE(rejects(insecure));  // __Secure- requires Secure
}

TEST(Ruvia, cookie_host_prefix_requires_secure_root_path_no_domain) {
    // __Host- is the strictest prefix (RFC 6265bis 4.1.3.2).
    ruvia::cookie_options valid;
    valid.prefix_ = ruvia::cookie_prefix::host;
    valid.secure_ = ruvia::cookie_attribute_policy::emit;  // path defaults to "/", domain is empty
    EXPECT_TRUE(!rejects(valid));

    ruvia::cookie_options not_secure = valid;
    not_secure.secure_ = ruvia::cookie_attribute_policy::omit;
    EXPECT_TRUE(rejects(not_secure));

    ruvia::cookie_options sub_path = valid;
    sub_path.path_ = "/sub";
    EXPECT_TRUE(rejects(sub_path));  // Path must be exactly "/"

    ruvia::cookie_options with_domain = valid;
    with_domain.domain_ = "example.com";
    EXPECT_TRUE(rejects(with_domain));  // Domain must be absent
}

TEST(Ruvia, cookie_literal_prefix_name_enforces_requirements) {
    const auto rejects_with_name = [](std::string_view name, const ruvia::cookie_options& options) {
        try {
            (void)ruvia::set_cookie_plan(name, "value", options);
            return false;
        } catch (const std::exception&) {
            return true;
        }
    };

    // RFC 6265bis user agents match these prefixes case-insensitively. Reject
    // every spelling the UA would reject instead of emitting a silently dropped
    // cookie.
    ruvia::cookie_options insecure;                            // Secure defaults to omitted
    EXPECT_TRUE(rejects_with_name("__Secure-tok", insecure));  // __Secure- requires Secure
    EXPECT_TRUE(rejects_with_name("__secure-tok", insecure));
    EXPECT_TRUE(rejects_with_name("__SeCuRe-tok", insecure));

    ruvia::cookie_options host_bad_domain;
    host_bad_domain.secure_ = ruvia::cookie_attribute_policy::emit;
    host_bad_domain.domain_ = "example.com";  // __Host- forbids Domain
    EXPECT_TRUE(rejects_with_name("__Host-sid", host_bad_domain));
    EXPECT_TRUE(rejects_with_name("__HOST-sid", host_bad_domain));
    EXPECT_TRUE(rejects_with_name("__hOsT-sid", host_bad_domain));

    // A literal-prefixed name that meets the constraints is accepted.
    ruvia::cookie_options ok_secure;
    ok_secure.secure_ = ruvia::cookie_attribute_policy::emit;
    EXPECT_TRUE(!rejects_with_name("__Secure-tok", ok_secure));
    EXPECT_TRUE(!rejects_with_name("__sEcUrE-tok", ok_secure));
    ruvia::cookie_options ok_host;
    ok_host.secure_ = ruvia::cookie_attribute_policy::emit;  // path defaults to "/", domain empty
    EXPECT_TRUE(!rejects_with_name("__Host-sid", ok_host));
    EXPECT_TRUE(!rejects_with_name("__hOsT-sid", ok_host));

    // A name that only resembles a prefix (missing the trailing '-') is unaffected.
    ruvia::cookie_options plain;
    EXPECT_TRUE(!rejects_with_name("__Secured", plain));
    EXPECT_TRUE(!rejects_with_name("__Hostname", plain));
}

TEST(Ruvia, cookie_writer_leaves_lifetime_caps_to_user_agents) {
    // RFC 6265bis' 400-day cookie-age-limit binds user agents, not servers.
    ruvia::cookie_options over_cap;
    over_cap.max_age_ = std::chrono::seconds(34560001);
    EXPECT_TRUE(!rejects(over_cap));

    ruvia::cookie_options far_expires;
    far_expires.expires_ = std::chrono::system_clock::now() + std::chrono::hours(24 * 500);
    EXPECT_TRUE(!rejects(far_expires));

    ruvia::cookie_options deletion;
    deletion.max_age_ = std::chrono::seconds(0);
    EXPECT_TRUE(!rejects(deletion));

    ruvia::cookie_options negative;
    negative.max_age_ = std::chrono::seconds(-1);
    EXPECT_TRUE(rejects(negative));
}

TEST(Ruvia, cookie_request_pair_keeps_equals_for_empty_names) {
    const auto pair = [&](std::string_view name, std::string_view value) {
        std::string wire(ruvia::cookie_request_pair_size(name, value), '\0');
        const auto* const end = ruvia::write_cookie_request_pair(wire.data(), name, value);
        EXPECT_TRUE(end == wire.data() + wire.size());
        return wire;
    };
    EXPECT_EQ(pair("sid", "abc"), std::string("sid=abc"));
    // A nameless value that looks like a cookie-pair must stay behind '='.
    EXPECT_EQ(pair("", "session=forged"), std::string("=session=forged"));
    EXPECT_EQ(pair("", "sid"), std::string("=sid"));
    EXPECT_EQ(pair("sid", ""), std::string("sid="));
    EXPECT_EQ(pair(std::string_view("\0", 1), std::string_view("\0", 1)), std::string("\0=\0", 3));
}

TEST(Ruvia, cookie_request_pair_accepts_inputs_inside_the_written_bytes) {
    // Each input is consumed before an output byte overwrites it.
    std::string buffer("valuename");
    buffer.resize(16, '#');
    const std::string_view value(buffer.data(), 5);
    const std::string_view name(buffer.data() + 5, 4);
    const auto* const end = ruvia::write_cookie_request_pair(buffer.data(), name, value);
    EXPECT_TRUE(end == buffer.data() + 10);
    EXPECT_EQ(std::string_view(buffer.data(), 10), std::string_view("name=value"));
}

TEST(Ruvia, cookie_request_pair_preserves_aliased_inputs_and_terminators) {
    std::string header = "a";
    header.reserve(16);
    const std::string_view terminator(header.data() + header.size(), 1);
    ruvia::append_cookie_request_pair(header, "b", terminator);
    EXPECT_EQ(header, std::string("a; b=\0", 6));

    std::string empty;
    const std::string_view empty_terminator(empty.data(), 1);
    ruvia::append_cookie_request_pair(empty, empty_terminator, empty_terminator);
    EXPECT_EQ(std::string_view(empty), std::string_view("\0=\0", 3));

    std::string growing = "sid=abc";
    growing.resize(growing.capacity(), 'x');
    const auto original = growing;
    const std::string_view name(growing.data(), 3);
    const std::string_view value(growing.data() + 4, growing.size() - 3);
    ruvia::append_cookie_request_pair(growing, name, value);
    auto expected = original + "; sid=" + original.substr(4);
    expected.push_back('\0');
    EXPECT_EQ(growing, expected);
}

TEST(Ruvia, cookie_request_pair_preserves_contents_when_allocations_are_rejected) {
    {
        std::string header;
        header.reserve(1024);
        const std::string value(300, 'v');
        const auto expected = std::string("\0=", 2) + value;
        try {
            ruvia::append_cookie_request_pair(header, std::string_view(header.data(), 1), value);
        } catch (const std::bad_alloc&) {
            ASSERT_TRUE(header.empty());
            ruvia::append_cookie_request_pair(header, std::string_view(header.data(), 1), value);
        }
        EXPECT_EQ(std::string_view(header), std::string_view(expected));
    }
}

TEST(Ruvia, cookie_request_pair_snapshots_only_overlapping_inputs) {
    for (const bool alias_name : {false, true}) {
        for (const auto aliased_size : {3U, 300U}) {
            SCOPED_TRACE(alias_name);
            SCOPED_TRACE(aliased_size);
            const std::string aliased(aliased_size, alias_name ? 'n' : 'v');
            const std::string independent(300, alias_name ? 'v' : 'n');
            const auto expected = alias_name ? aliased + "=" + independent : independent + "=" + aliased;
            std::string output = aliased;
            output.resize(expected.size());
            const std::string_view borrowed(output.data(), aliased.size());

            (void)ruvia::write_cookie_request_pair(output.data(),
                alias_name ? borrowed : independent, alias_name ? independent : borrowed);

            EXPECT_EQ(output, expected);
        }
    }
}

TEST(Ruvia, cookie_request_pair_preserves_both_long_aliased_inputs_on_failure_and_retry) {
    const std::string name(300, 'n');
    const std::string value(300, 'v');
    std::string output = value + name;
    output.resize(name.size() + 1 + value.size());
    const auto original = output;
    const std::string_view borrowed_name(output.data() + value.size(), name.size());
    const std::string_view borrowed_value(output.data(), value.size());

    try {
        (void)ruvia::write_cookie_request_pair(output.data(), borrowed_name, borrowed_value);
    } catch (const std::bad_alloc&) {
        ASSERT_EQ(output, original);
        (void)ruvia::write_cookie_request_pair(output.data(), borrowed_name, borrowed_value);
    }

    EXPECT_EQ(output, name + "=" + value);
}

TEST(Ruvia, cookie_plan_serialization_emits_fields_without_attributes) {
    const ruvia::cookie_options options{.path_ = ""};
    const ruvia::set_cookie_plan plan("sid", "value", options);
    std::string wire(plan.size(), '\0');
    plan.write(wire.data());
    EXPECT_EQ(wire, "sid=value");
    const auto parsed_value = ruvia::parse_set_cookie(wire);
    EXPECT_TRUE(parsed_value.has_value());
    if (parsed_value) {
        EXPECT_EQ(parsed_value->name(), "sid");
        EXPECT_TRUE(parsed_value->name().data() == wire.data());
        EXPECT_TRUE(parsed_value->path().empty());
        EXPECT_TRUE(parsed_value->domain().empty());
    }
}

TEST(Ruvia, cookie_plan_serialization_formats_exact_max_age_widths_and_prefixed_fields) {
    constexpr std::int64_t ages[]{1, 9, 10, 99, 100, 999, 1000, ruvia::max_cookie_age_seconds};
    for (const auto age : ages) {
        const ruvia::cookie_options options{
            .path_ = "/", .max_age_ = std::chrono::seconds{age}, .prefix_ = ruvia::cookie_prefix::host, .secure_ = ruvia::cookie_attribute_policy::emit};
        const ruvia::set_cookie_plan plan("sid", "value", options);
        std::string wire(plan.size(), '\0');
        plan.write(wire.data());
        EXPECT_EQ(wire, std::string("__Host-sid=value; Path=/; Max-Age=") + std::to_string(age).c_str() + "; Secure");
        const auto parsed_value = ruvia::parse_set_cookie(wire);
        EXPECT_TRUE(parsed_value.has_value());
        if (parsed_value) {
            EXPECT_EQ(parsed_value->name(), "__Host-sid");
            EXPECT_EQ(parsed_value->path(), "/");
            EXPECT_TRUE(parsed_value->domain().empty());
            EXPECT_TRUE(parsed_value->name().data() == wire.data());
            EXPECT_TRUE(parsed_value->path().data() == wire.data() + wire.find('/'));
        }
    }
}

TEST(Ruvia, cookie_plan_serialization_writes_zero_max_age_as_deletion_expires) {
    // max-age-av = "Max-Age=" non-zero-digit *DIGIT (RFC 6265 section 4.1.1).
    const ruvia::cookie_options options{.path_ = "/",
        .expires_ = std::chrono::system_clock::time_point{std::chrono::hours{24 * 365 * 40}},
        .max_age_ = std::chrono::seconds{0}};
    const ruvia::set_cookie_plan plan("sid", "", options);
    std::string wire(plan.size(), '\0');
    plan.write(wire.data());
    EXPECT_EQ(wire, "sid=; Path=/; Expires=Thu, 01 Jan 1970 00:00:00 GMT");
}

namespace {
using ruvia::decode_url_component;
using ruvia::has_url_encoding;
using mode_type = ruvia::url_decode_mode;

std::optional<std::string> url_decode(std::string_view in, ruvia::url_decode_mode mode) {
    std::string storage;
    const auto decoded = ruvia::decode_url_component(in, storage, mode);
    if (!decoded.has_value()) {
        return std::nullopt;
    }
    return std::string(*decoded);
}

}  // namespace

// Percent-encoding: decoding a component or a form field, validating one, and walking encoded
// pairs.

TEST(Ruvia, url_decode_percent) {
    using m_type = ruvia::url_decode_mode;
    EXPECT_EQ(url_decode("hello", m_type::percent).value(), std::string("hello"));
    EXPECT_EQ(url_decode("%41%42%43", m_type::percent).value(), std::string("ABC"));
    EXPECT_EQ(url_decode("a%2Fb", m_type::percent).value(), std::string("a/b"));
    EXPECT_EQ(url_decode("%00", m_type::percent).value(), std::string(1, '\0'));
    // '+' is literal in percent mode
    EXPECT_EQ(url_decode("a+b", m_type::percent).value(), std::string("a+b"));
}

TEST(Ruvia, url_decode_form) {
    using m_type = ruvia::url_decode_mode;
    EXPECT_EQ(url_decode("a+b", m_type::form).value(), std::string("a b"));
    EXPECT_EQ(url_decode("a+b%20c", m_type::form).value(), std::string("a b c"));
}

TEST(Ruvia, percent_byte_decoding_rejects_out_of_range_and_truncated_triplets) {
    EXPECT_TRUE(ruvia::http_try_decode_percent_byte("a%2Fb", 1) == std::optional<unsigned char>('/'));
    EXPECT_TRUE(!ruvia::http_try_decode_percent_byte("abc", std::string_view::npos).has_value());
    EXPECT_TRUE(!ruvia::http_try_decode_percent_byte("abc", 3).has_value());
    EXPECT_TRUE(!ruvia::http_try_decode_percent_byte("%2", 0).has_value());
    EXPECT_TRUE(!ruvia::http_try_decode_percent_byte("%G0", 0).has_value());
    EXPECT_TRUE(!ruvia::http_try_decode_percent_byte("x20", 0).has_value());
}

TEST(Ruvia, uri_reference_encoding_borrows_or_writes_storage) {
    std::string storage("keep");
    constexpr std::string_view canonical = "/a%20b?x=1#frag";
    const auto unchanged = ruvia::percent_encode_uri_reference(canonical, storage);
    EXPECT_TRUE(unchanged.data() == canonical.data() && unchanged.size() == canonical.size());
    EXPECT_EQ(std::string_view(storage), std::string_view("keep"));

    const auto encoded = ruvia::percent_encode_uri_reference("/a b?x=%zz", storage);
    EXPECT_TRUE(encoded.data() == storage.data());
    EXPECT_EQ(encoded, std::string_view("/a%20b?x=%25zz"));

    // The reference may be the storage itself.
    storage.assign("/c d");
    EXPECT_EQ(ruvia::percent_encode_uri_reference(storage, storage), std::string_view("/c%20d"));
    EXPECT_EQ(std::string_view(storage), std::string_view("/c%20d"));
}

TEST(Ruvia, url_decode_invalid) {
    using m_type = ruvia::url_decode_mode;
    EXPECT_TRUE(!url_decode("%", m_type::percent).has_value());
    EXPECT_TRUE(!url_decode("%4", m_type::percent).has_value());
    EXPECT_TRUE(!url_decode("%zz", m_type::percent).has_value());
    EXPECT_TRUE(!url_decode("ab%2", m_type::percent).has_value());
    EXPECT_TRUE(!url_decode("%g0", m_type::percent).has_value());
}

TEST(Ruvia, url_decode_form_keeps_lone_percent_literally) {
    using m_type = ruvia::url_decode_mode;
    EXPECT_EQ(url_decode("100%", m_type::form).value_or("<none>"), std::string("100%"));
    EXPECT_EQ(url_decode("%zz+%41", m_type::form).value_or("<none>"), std::string("%zz A"));
    EXPECT_EQ(url_decode("%4%41", m_type::form).value_or("<none>"), std::string("%4A"));
}

TEST(Ruvia, url_decode_all_bytes_and_malformed_suffix) {
    using m_type = ruvia::url_decode_mode;
    constexpr std::string_view hex = "0123456789aBcDeF";
    std::string encoded;
    std::string expected;
    for (unsigned int byte = 0; byte < 256; ++byte) {
        encoded.push_back('%');
        encoded.push_back(hex[byte >> 4]);
        encoded.push_back(hex[byte & 15]);
        expected.push_back(static_cast<char>(byte));
    }
    for (const auto mode : {m_type::percent, m_type::form}) {
        const auto result_value = url_decode(encoded, mode);
        EXPECT_TRUE(result_value.has_value());
        if (result_value) {
            EXPECT_EQ(*result_value, expected);
        }
        for (const std::string_view suffix : {"%", "%0", "%xz"}) {
            const auto suffixed = url_decode(encoded + std::string(suffix), mode);
            if (mode == m_type::percent) {
                EXPECT_TRUE(!suffixed.has_value());
            } else {
                // WHATWG percent-decode keeps a '%' that starts no valid escape.
                EXPECT_TRUE(suffixed.has_value() && *suffixed == expected + std::string(suffix));
            }
        }
    }
}

TEST(Ruvia, url_validate_encoding) {
    using ruvia::validate_url_encoding;
    EXPECT_TRUE(validate_url_encoding("plain"));
    EXPECT_TRUE(validate_url_encoding("%41%42"));
    EXPECT_TRUE(!validate_url_encoding("%4"));
    EXPECT_TRUE(!validate_url_encoding("%zz"));
    EXPECT_TRUE(validate_url_encoding(""));
}

TEST(Ruvia, url_uri_paths_equivalent_and_ordered_by_canonical_form) {
    using ruvia::compare_uri_paths;
    using ruvia::uri_paths_equivalent;
    EXPECT_TRUE(uri_paths_equivalent("/users/admin", "/users/%61dmin"));
    EXPECT_TRUE(uri_paths_equivalent("/caf%C3%A9", "/caf%c3%a9"));
    EXPECT_TRUE(uri_paths_equivalent("/a:b", "/a%3Ab"));  // decoded, like route parameters
    EXPECT_TRUE(uri_paths_equivalent("", ""));
    EXPECT_TRUE(!uri_paths_equivalent("/a/b", "/a%2Fb"));  // an encoded slash is no separator
    EXPECT_TRUE(uri_paths_equivalent("/a%2fb", "/a%2Fb"));
    EXPECT_TRUE(!uri_paths_equivalent("/a+b", "/a%20b"));  // '+' is literal in a path
    EXPECT_TRUE(!uri_paths_equivalent("/api", "/apix"));
    EXPECT_TRUE(!uri_paths_equivalent("/%zz", "/%zz"));  // malformed escape
    EXPECT_TRUE(!uri_paths_equivalent("/%2", "/%2"));    // truncated escape

    EXPECT_EQ(compare_uri_paths("%61", "a"), 0);
    EXPECT_TRUE(compare_uri_paths("a", "%62") < 0);
    EXPECT_TRUE(compare_uri_paths("%62", "a") > 0);
    EXPECT_TRUE(compare_uri_paths("ab", "%61") > 0);
    EXPECT_TRUE(compare_uri_paths("a", "a%2F") < 0);
    // A separator orders after every octet, an encoded slash among them.
    EXPECT_TRUE(compare_uri_paths("a/", "a%FF") > 0);
    EXPECT_TRUE(compare_uri_paths("a%2F", "a/") < 0);
}

TEST(Ruvia, url_visit_pairs_count) {
    std::vector<std::pair<std::string, std::string>> pairs;
    (void)ruvia::visit_url_encoded_pairs(
        "x=1&y=2&z=3", [&](std::string_view n, std::string_view v) {
            pairs.emplace_back(std::string(n), std::string(v));
        });
    EXPECT_EQ(pairs.size(), std::size_t(3));
    if (pairs.size() == 3) {
        EXPECT_EQ(pairs[0].first, std::string("x"));
        EXPECT_EQ(pairs[2].second, std::string("3"));
    }
}

TEST(Ruvia, url_visit_pairs_stops_early) {
    std::size_t visited = 0;
    const bool completed = ruvia::visit_url_encoded_pairs("a=1&b=2&c=3",
        [&visited](std::string_view, std::string_view) {
            return ++visited < 2;
        });
    EXPECT_TRUE(!completed);
    EXPECT_EQ(visited, std::size_t(2));
}

TEST(Ruvia, url_visit_pairs_skips_empty_segments) {
    std::vector<std::pair<std::string, std::string>> pairs;
    // Leading, doubled, and trailing '&' produce empty segments that must NOT yield ("","") pairs.
    (void)ruvia::visit_url_encoded_pairs(
        "&a=1&&b=2&", [&](std::string_view n, std::string_view v) {
            pairs.emplace_back(std::string(n), std::string(v));
        });
    EXPECT_EQ(pairs.size(), std::size_t(2));
    if (pairs.size() == 2) {
        EXPECT_EQ(pairs[0].first, std::string("a"));
        EXPECT_EQ(pairs[1].first, std::string("b"));
    }
    // A key with an empty value ("k=") is still a real field and must be kept.
    std::vector<std::pair<std::string, std::string>> kept;
    (void)ruvia::visit_url_encoded_pairs("k=&=v", [&](std::string_view n, std::string_view v) {
        kept.emplace_back(std::string(n), std::string(v));
    });
    EXPECT_EQ(kept.size(), std::size_t(2));  // "k=" (name k, empty value) and "=v" (empty name, value v)
}

TEST(Ruvia, has_url_encoding_detection) {
    EXPECT_TRUE(has_url_encoding("a%20b", mode_type::percent));
    EXPECT_TRUE(!has_url_encoding("abc", mode_type::percent));
    // '+' is a literal in percent mode but triggers decoding in form mode.
    EXPECT_TRUE(!has_url_encoding("a+b", mode_type::percent));
    EXPECT_TRUE(has_url_encoding("a+b", mode_type::form));
    EXPECT_TRUE(has_url_encoding("a%20b", mode_type::form));
    EXPECT_TRUE(!has_url_encoding("plain", mode_type::form));
    EXPECT_TRUE(!has_url_encoding("", mode_type::form));
    EXPECT_TRUE(!has_url_encoding("", mode_type::percent));
}

TEST(Ruvia, has_url_encoding_handles_binary_components) {
    std::string value(129, 'a');
    for (std::size_t position = 0; position < value.size(); ++position) {
        for (unsigned int byte = 0; byte < 256; ++byte) {
            value[position] = static_cast<char>(byte);
            EXPECT_EQ(has_url_encoding(value, mode_type::percent), byte == '%');
            EXPECT_EQ(has_url_encoding(value, mode_type::form), byte == '%' || byte == '+');
        }
        value[position] = 'a';
    }
}

TEST(Ruvia, has_url_encoding_respects_component_bounds) {
    const std::string storage = "%+" + std::string(129, 'a') + "+%";
    const auto value = std::string_view(storage).substr(2, 129);
    for (const auto mode : {mode_type::percent, mode_type::form}) {
        EXPECT_TRUE(!has_url_encoding(value, mode));
        EXPECT_TRUE(!has_url_encoding(value.substr(0, 0), mode));
        EXPECT_TRUE(!has_url_encoding(value.substr(value.size()), mode));
    }
    const std::string_view binary("a\0b%00c", 7);
    EXPECT_TRUE(has_url_encoding(binary, mode_type::percent));
    EXPECT_TRUE(has_url_encoding(binary, mode_type::form));
}

TEST(Ruvia, no_encoding_means_decode_borrows_the_input) {
    // Callers size their decoded-storage reservations with has_url_encoding:
    // a component it reports unencoded is returned as the input itself.
    std::string storage;
    for (const std::string_view value : {std::string_view(""), std::string_view("plain"),
             std::string_view("a/b?c=d"), std::string_view("no-plus")}) {
        for (const auto mode : {mode_type::percent, mode_type::form}) {
            EXPECT_TRUE(!has_url_encoding(value, mode));
            const auto decoded = decode_url_component(value, storage, mode);
            EXPECT_TRUE(decoded.has_value() && decoded->data() == value.data() && decoded->size() == value.size());
        }
    }
    EXPECT_TRUE(storage.empty());
}

TEST(Ruvia, hex_nibbles_decode_and_encode_ascii_digits) {
    EXPECT_EQ(ruvia::http_decode_hex_nibble('0'), 0);
    EXPECT_EQ(ruvia::http_decode_hex_nibble('a'), 10);
    EXPECT_EQ(ruvia::http_decode_hex_nibble('F'), 15);
    EXPECT_EQ(ruvia::http_decode_hex_nibble('g'), -1);
    EXPECT_EQ(ruvia::http_lower_hex_digit(15), 'f');
    EXPECT_EQ(ruvia::http_upper_hex_digit(15), 'F');
}

TEST(Ruvia, hex_encode_accepts_output_overlapping_input) {
    constexpr std::string_view text =
        "ab\x01\xff"
        "cdef";
    constexpr std::size_t input_start = 16;
    for (std::size_t size = 0; size <= text.size(); ++size) {
        const auto input = text.substr(0, size);
        std::string expected(2 * size, '\0');
        ruvia::http_encode_lower_hex(expected.data(), input);
        // Every output start from wholly before to wholly after the input,
        // including in place and starts inside it.
        for (auto output_start = input_start - expected.size(); output_start <= input_start + size; ++output_start) {
            std::array<char, 48> buffer{};
            input.copy(buffer.data() + input_start, size);
            const auto* const end =
                ruvia::http_encode_lower_hex(buffer.data() + output_start, std::string_view(buffer.data() + input_start, size));
            EXPECT_EQ(end, buffer.data() + output_start + expected.size());
            EXPECT_EQ(std::string(buffer.data() + output_start, expected.size()), expected);
        }
    }
}

TEST(Ruvia, hex_append_accepts_input_aliasing_the_output) {
    std::string whole("\x01\xab");
    whole.shrink_to_fit();
    ruvia::http_append_lower_hex(whole, std::string_view(whole));
    EXPECT_EQ(std::string_view(whole), std::string_view("\x01\xab"
                                                        "01ab"));

    // A long prefix forces reallocation while the source is a strict subrange.
    std::string part(std::string(64, 'z') + "\xff");
    part.shrink_to_fit();
    const std::span<const unsigned char> tail(reinterpret_cast<const unsigned char*>(part.data()) + 63, 2);
    ruvia::http_append_lower_hex(part, tail);
    EXPECT_EQ(std::string_view(part).substr(65), std::string_view("7aff"));

    // The terminating NUL is readable input: it ends where the digits begin.
    std::string terminated("A");
    terminated.shrink_to_fit();
    ruvia::http_append_lower_hex(terminated, std::string_view(terminated.data(), terminated.size() + 1));
    EXPECT_EQ(std::string_view(terminated), std::string_view("A4100"));

    // A full string reallocates while the NUL-only input borrows the old buffer.
    std::string nul_only("A");
    nul_only.resize(nul_only.capacity(), 'B');
    const auto filled = nul_only.size();
    ruvia::http_append_lower_hex(nul_only, std::string_view(nul_only.data() + filled, 1));
    EXPECT_EQ(nul_only.size(), filled + 2);
    EXPECT_EQ(std::string_view(nul_only).substr(filled - 1), std::string_view("B00"));
}

namespace {

std::string encode(std::string_view input, ruvia::http_base64_padding padding = ruvia::http_base64_padding::required) {
    std::string output(ruvia::http_base64_encoded_size(input.size(), padding), '\0');
    ruvia::http_base64_encode(output.data(),
        std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(input.data()), input.size()),
        ruvia::http_base64_charset::standard, padding);
    return output;
}

}  // namespace

TEST(Ruvia, http_base64_encodes_with_standard_alphabet_and_padding) {
    EXPECT_EQ(encode(""), std::string(""));
    EXPECT_EQ(encode("f"), std::string("Zg=="));
    EXPECT_EQ(encode("fo"), std::string("Zm8="));
    EXPECT_EQ(encode("foo"), std::string("Zm9v"));
    EXPECT_EQ(encode("foobar"), std::string("Zm9vYmFy"));
    EXPECT_EQ(encode("\xfb\xff"), std::string("+/8="));
}

TEST(Ruvia, http_base64_encode_accepts_output_overlapping_input) {
    constexpr std::string_view text = "abcdefghij";
    constexpr std::size_t input_start = 16;
    for (const auto padding : {ruvia::http_base64_padding::required, ruvia::http_base64_padding::forbidden}) {
        for (std::size_t size = 0; size <= text.size(); ++size) {
            const auto input = text.substr(0, size);
            const auto expected = encode(input, padding);
            // Every output start from wholly before to wholly after the input,
            // including in place and starts inside it.
            for (auto output_start = input_start - expected.size(); output_start <= input_start + size; ++output_start) {
                std::array<char, 48> buffer{};
                input.copy(buffer.data() + input_start, size);
                ruvia::http_base64_encode(buffer.data() + output_start,
                    std::span(reinterpret_cast<const std::uint8_t*>(buffer.data()) + input_start, size),
                    ruvia::http_base64_charset::standard, padding);
                EXPECT_EQ(std::string(buffer.data() + output_start, expected.size()), expected);
            }
        }
    }
}

TEST(Ruvia, http_base64_encoded_size_preserves_padding_at_numeric_boundaries) {
    constexpr auto maximum = (std::numeric_limits<std::size_t>::max)();
    constexpr auto maximum_groups = maximum / 4;
    constexpr auto maximum_input = maximum_groups * 3;
    constexpr auto maximum_output = maximum_groups * 4;
    constexpr auto small_output = ruvia::http_base64_encoded_size(4);
    EXPECT_EQ(small_output, std::size_t{8});
    for (const auto input_size : {maximum_input - 2, maximum_input - 1, maximum_input}) {
        EXPECT_EQ(ruvia::http_base64_encoded_size(input_size), maximum_output);
    }
    for (const auto input_size : {maximum_input + 1, maximum - 1, maximum}) {
        bool rejected = false;
        try {
            (void)ruvia::http_base64_encoded_size(input_size);
        } catch (const std::length_error&) {
            rejected = true;
        }
        EXPECT_TRUE(rejected);
    }
}

TEST(Ruvia, http_base64_unpadded_size_uses_actual_tail_at_numeric_boundary) {
    constexpr auto maximum = (std::numeric_limits<std::size_t>::max)();
    constexpr auto groups = maximum / 4;
    constexpr auto forbidden = ruvia::http_base64_padding::forbidden;
    EXPECT_EQ(ruvia::http_base64_encoded_size(3 * groups + 1, forbidden), maximum - 1);
    EXPECT_EQ(ruvia::http_base64_encoded_size(3 * groups + 2, forbidden), maximum);
    bool rejected = false;
    try {
        (void)ruvia::http_base64_encoded_size(3 * groups + 3, forbidden);
    } catch (const std::length_error&) {
        rejected = true;
    }
    EXPECT_TRUE(rejected);
}

TEST(Ruvia, http_base64_decodes_single_characters_per_alphabet) {
    for (std::size_t i = 0; i < ruvia::http_base64_alphabet.size(); ++i) {
        EXPECT_EQ(ruvia::http_base64_decode_char(ruvia::http_base64_alphabet[i]), static_cast<int>(i));
        EXPECT_EQ(ruvia::http_base64_url_decode_char(ruvia::http_base64_url_alphabet[i]), static_cast<int>(i));
    }
    EXPECT_EQ(ruvia::http_base64_decode_char('='), -1);
    EXPECT_EQ(ruvia::http_base64_decode_char('-'), -1);
    EXPECT_EQ(ruvia::http_base64_url_decode_char('+'), -1);
    EXPECT_EQ(ruvia::http_base64_url_decode_char('/'), -1);
    EXPECT_EQ(ruvia::http_base64_url_decode_char('='), -1);
}

TEST(Ruvia, http_base64_decode_rejects_output_overwriting_unread_input) {
    constexpr auto standard = ruvia::http_base64_charset::standard;
    constexpr auto required = ruvia::http_base64_padding::required;
    const auto decoded_text = [](const std::uint8_t* bytes, std::size_t size) {
        return std::string(reinterpret_cast<const char*>(bytes), size);
    };

    std::uint8_t shifted[16] = {'Q', 'U', 'J', 'D', 'R', 'E', 'V', 'G'};
    const std::string_view shifted_input(reinterpret_cast<const char*>(shifted), 8);
    EXPECT_TRUE(!ruvia::http_base64_decode(shifted_input, std::span(shifted + 4, 6), standard, required));
    EXPECT_EQ(decoded_text(shifted, 8), std::string("QUJDREVG"));

    std::uint8_t in_place[8] = {'Q', 'U', 'J', 'D', 'R', 'E', 'V', 'G'};
    const std::string_view in_place_input(reinterpret_cast<const char*>(in_place), 8);
    const auto in_place_size = ruvia::http_base64_decode(in_place_input, std::span(in_place), standard, required);
    EXPECT_TRUE(in_place_size && *in_place_size == 6);
    EXPECT_EQ(decoded_text(in_place, 6), std::string("ABCDEF"));

    std::uint8_t after[16] = {'Q', 'U', 'J', 'D', 'R', 'E', 'V', 'G'};
    const std::string_view after_input(reinterpret_cast<const char*>(after), 8);
    const auto after_size = ruvia::http_base64_decode(after_input, std::span(after + 8, 8), standard, required);
    EXPECT_TRUE(after_size && *after_size == 6);
    EXPECT_EQ(decoded_text(after + 8, 6), std::string("ABCDEF"));
}

namespace {

using ruvia::http_ascii_equals_ignore_case;

}  // namespace

TEST(Ruvia, http_ascii_equals_ignore_case_folds_only_letters) {
    EXPECT_TRUE(http_ascii_equals_ignore_case("Text/HTML", "text/html"));
    EXPECT_TRUE(http_ascii_equals_ignore_case("", ""));
    EXPECT_TRUE(!http_ascii_equals_ignore_case("abc", "abcd"));
    EXPECT_TRUE(!http_ascii_equals_ignore_case("abc", "abd"));

    EXPECT_TRUE(!http_ascii_equals_ignore_case("[", "{"));
    EXPECT_TRUE(!http_ascii_equals_ignore_case("@", "`"));

    EXPECT_TRUE(http_ascii_equals_ignore_case(
        std::string_view("\xC3\xA9", 2), std::string_view("\xC3\xA9", 2)));
    EXPECT_TRUE(!http_ascii_equals_ignore_case(
        std::string_view("\xC3\xA9", 2), std::string_view("\xC3\x89", 2)));
}

TEST(Ruvia, number_format_decimal_width_and_string_output) {
    EXPECT_EQ(ruvia::http_unsigned_decimal_size(0), std::size_t{1});
    EXPECT_EQ(ruvia::http_unsigned_decimal_size(999), std::size_t{3});
    EXPECT_EQ(ruvia::http_unsigned_decimal_size((std::numeric_limits<std::uint64_t>::max)()), std::size_t{20});

    std::string output;
    ruvia::http_append_formatted_number(output, std::int64_t{-42}, "format failure");
    ruvia::http_append_formatted_number(output, std::uint64_t{123}, "format failure");
    EXPECT_EQ(output, "-42123");
    ruvia::http_append_formatted_finite_number(output, 1.5, "nonfinite", "format failure");
    EXPECT_EQ(output, "-421231.5");
}

TEST(Ruvia, number_format_rejects_nonfinite_without_modifying_output) {
    std::string output("prefix");
    bool rejected = false;
    try {
        ruvia::http_append_formatted_finite_number(output, std::numeric_limits<double>::infinity(),
            "nonfinite", "format failure");
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    EXPECT_TRUE(rejected);
    EXPECT_EQ(output, "prefix");
}

TEST(Ruvia, number_format_signed_extremes) {
    std::string output;
    ruvia::http_append_signed(output, (std::numeric_limits<std::int64_t>::min)());
    EXPECT_EQ(output, "-9223372036854775808");
    output.clear();
    ruvia::http_append_signed(output, (std::numeric_limits<std::int64_t>::max)());
    EXPECT_EQ(output, "9223372036854775807");
}

namespace {

using ruvia::http_request;
using ruvia::validate_websocket_handshake;
using ruvia::test::parse_http1_request_message;

http_request parse_request(std::string_view raw_request) {
    auto parsed_value = parse_http1_request_message(raw_request);
    return std::move(parsed_value.state_).take_request();
}

[[nodiscard]] auto validate_request(std::string_view raw_request) {
    const auto parsed_value = parse_http1_request_message(raw_request);
    return validate_websocket_handshake(parsed_value.state_.request(), parsed_value.state_.body_plan());
}

[[nodiscard]] bool accepts_request(std::string_view raw_request) {
    const auto result_value = validate_request(raw_request);
    return result_value.accepted() != nullptr;
}

[[nodiscard]] bool rejects_request(std::string_view raw_request) {
    const auto result_value = validate_request(raw_request);
    return result_value.failure() != nullptr;
}

std::string_view valid_handshake() {
    return "GET /ws HTTP/1.1\r\n"
           "Host: example.test\r\n"
           "Connection: Upgrade\r\n"
           "Upgrade: websocket\r\n"
           "Sec-WebSocket-Version: 13\r\n"
           "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
           "\r\n";
}

std::string_view post_handshake() {
    return "POST /ws HTTP/1.1\r\n"
           "Host: example.test\r\n"
           "Connection: Upgrade\r\n"
           "Upgrade: websocket\r\n"
           "Sec-WebSocket-Version: 13\r\n"
           "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
           "\r\n";
}

std::string_view http10_handshake() {
    return "GET /ws HTTP/1.0\r\n"
           "Connection: Upgrade\r\n"
           "Upgrade: websocket\r\n"
           "Sec-WebSocket-Version: 13\r\n"
           "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
           "\r\n";
}

std::string_view bad_version_handshake() {
    return "GET /ws HTTP/1.1\r\n"
           "Host: example.test\r\n"
           "Connection: Upgrade\r\n"
           "Upgrade: websocket\r\n"
           "Sec-WebSocket-Version: 8\r\n"
           "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
           "\r\n";
}

std::string_view content_length_zero_handshake() {
    return "GET /ws HTTP/1.1\r\n"
           "Host: example.test\r\n"
           "Connection: Upgrade\r\n"
           "Upgrade: websocket\r\n"
           "Sec-WebSocket-Version: 13\r\n"
           "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
           "Content-Length: 0\r\n"
           "\r\n";
}

std::string_view content_length_one_handshake() {
    return "GET /ws HTTP/1.1\r\n"
           "Host: example.test\r\n"
           "Connection: Upgrade\r\n"
           "Upgrade: websocket\r\n"
           "Sec-WebSocket-Version: 13\r\n"
           "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
           "Content-Length: 1\r\n"
           "\r\n"
           "x";
}

}  // namespace

TEST(Ruvia, ws_h1_handshake_preserves_application_request_headers) {
    constexpr std::string_view raw =
        "GET /ws HTTP/1.1\r\n"
        "Host: example.test\r\n"
        "Connection: keep-alive, Upgrade\r\n"
        "Upgrade: websocket\r\n"
        "Origin: https://origin.example\r\n"
        "Authorization: Bearer credential\r\n"
        "Cookie: sid=one; theme=dark\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Protocol: chat\r\n"
        "Sec-WebSocket-Extensions: permessage-deflate\r\n"
        "X-End-To-End: retained\r\n"
        "\r\n";
    const auto request = parse_request(raw);

    EXPECT_TRUE(request.header("Host") == "example.test");
    EXPECT_TRUE(request.header("Origin") == "https://origin.example");
    EXPECT_TRUE(request.header("Authorization") == "Bearer credential");
    EXPECT_TRUE(request.header("Cookie") == "sid=one; theme=dark");
    EXPECT_TRUE(request.header("Sec-WebSocket-Protocol") == "chat");
    EXPECT_TRUE(request.header("Sec-WebSocket-Extensions") == "permessage-deflate");
    EXPECT_TRUE(request.header("X-End-To-End") == "retained");
    EXPECT_TRUE(accepts_request(raw));
}

TEST(Ruvia, ws_public_subprotocol_set_validates_unique_tokens) {
    ruvia::websocket_subprotocol_set protocols;
    EXPECT_TRUE(protocols.append_list(", chat, superchat,"));
    EXPECT_TRUE(protocols.contains("chat"));
    EXPECT_TRUE(protocols.contains("superchat"));
    EXPECT_TRUE(!protocols.append("chat"));
    EXPECT_TRUE(!protocols.append_list("other, chat"));
    EXPECT_TRUE(!protocols.append("bad token"));

    // A received offer skips repeats instead of failing (RFC 6455 4.2.1).
    ruvia::websocket_subprotocol_set offered;
    EXPECT_TRUE(offered.append_offered_list("chat, chat"));
    EXPECT_TRUE(offered.append_offered_list("superchat, chat"));
    EXPECT_TRUE(offered.contains("chat"));
    EXPECT_TRUE(offered.contains("superchat"));
    EXPECT_TRUE(!offered.append_offered_list("chat, bad token"));
}

TEST(Ruvia, ws_subprotocol_offers_require_http_tokens_and_skip_repeats) {
    const auto with_protocols = [](std::string_view fields_value) {
        std::string request(valid_handshake());
        request.insert(request.size() - 2, fields_value);
        return request;
    };

    EXPECT_TRUE(accepts_request(with_protocols("Sec-WebSocket-Protocol: , chat,, superchat,\r\n")));
    // Subprotocol identifiers are case-sensitive, so these are distinct.
    EXPECT_TRUE(accepts_request(with_protocols("Sec-WebSocket-Protocol: chat, Chat\r\n")));

    EXPECT_TRUE(rejects_request(with_protocols("Sec-WebSocket-Protocol: bad token\r\n")));
    EXPECT_TRUE(rejects_request(with_protocols("Sec-WebSocket-Protocol: \"chat\"\r\n")));
    EXPECT_TRUE(rejects_request(with_protocols("Sec-WebSocket-Protocol: , ,\r\n")));
    // Uniqueness binds the sending client (RFC 6455 section 4.1 item 10); the
    // server's checks (section 4.2.1) have no rule for a repeated offer.
    EXPECT_TRUE(accepts_request(with_protocols("Sec-WebSocket-Protocol: chat, chat\r\n")));
    EXPECT_TRUE(accepts_request(with_protocols(
        "Sec-WebSocket-Protocol: chat\r\n"
        "Sec-WebSocket-Protocol: superchat, chat\r\n")));
    constexpr std::array<std::string_view, 1> supported{"chat"};
    // 请求头借用原始报文，握手完成前必须保持报文存储存活。
    const auto repeated_wire = with_protocols("Sec-WebSocket-Protocol: chat, chat\r\n");
    const auto repeated = parse_request(repeated_wire);
    const auto selected = ruvia::make_websocket_server_handshake(repeated, {.supported_subprotocols_ = supported});
    EXPECT_TRUE(selected.subprotocol() == "chat");

    std::string too_many = "Sec-WebSocket-Protocol: ";
    for (std::size_t i = 0; i <= ruvia::max_websocket_subprotocols; ++i) {
        if (i != 0) {
            too_many.append(", ");
        }
        too_many.append("protocol-");
        too_many.append(std::to_string(i));
    }
    too_many.append("\r\n");
    EXPECT_TRUE(rejects_request(with_protocols(too_many)));
}

TEST(Ruvia, ws_required_subprotocol_rejects_offers_without_a_supported_entry) {
    const auto with_protocols = [](std::string_view fields_value) {
        std::string request(valid_handshake());
        request.insert(request.size() - 2, fields_value);
        return request;
    };
    constexpr std::array<std::string_view, 2> supported{"chat.v2", "chat.v1"};
    const auto validate = [&](std::string_view raw_request, bool required) {
        const auto parsed_value = parse_http1_request_message(raw_request);
        return validate_websocket_handshake(parsed_value.state_.request(), parsed_value.state_.body_plan(),
            {.supported_subprotocols_ = std::span<const std::string_view>(supported),
                .require_subprotocol_ = required});
    };
    const auto accepts = [&](std::string_view raw_request, bool required) {
        const auto result_value = validate(raw_request, required);
        return result_value.accepted() != nullptr;
    };

    // Not required: an offer sharing nothing is still a valid handshake.
    EXPECT_TRUE(accepts(valid_handshake(), false));
    EXPECT_TRUE(accepts(with_protocols("Sec-WebSocket-Protocol: other\r\n"), false));

    EXPECT_TRUE(accepts(with_protocols("Sec-WebSocket-Protocol: other, chat.v1\r\n"), true));
    for (const std::string_view raw_request :
        {valid_handshake(), std::string_view("GET /ws HTTP/1.1\r\nHost: x\r\nConnection: Upgrade\r\n"
                                             "Upgrade: websocket\r\nSec-WebSocket-Version: 13\r\n"
                                             "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                                             "Sec-WebSocket-Protocol: other, Chat.v1\r\n\r\n")}) {
        const auto result_value = validate(raw_request, true);
        const auto* failure = result_value.failure();
        EXPECT_TRUE(failure != nullptr);
        if (failure != nullptr) {
            EXPECT_TRUE(failure->protocol_error().status() == ruvia::http_status::bad_request);
            ruvia::http_response response;
            failure->apply_required_response_headers(response);
            EXPECT_TRUE(!response.header("Sec-WebSocket-Version").has_value());
        }
    }
}

TEST(Ruvia, ws_extension_offers_must_match_the_rfc6455_abnf) {
    const auto with_extensions = [](std::string_view fields_value) {
        std::string request(valid_handshake());
        request.insert(request.size() - 2, fields_value);
        return request;
    };

    EXPECT_TRUE(accepts_request(
        with_extensions("Sec-WebSocket-Extensions: , x-test; flag; value=token,,\r\n")));
    EXPECT_TRUE(accepts_request(with_extensions("Sec-WebSocket-Extensions: x-test; value=\"to\\ken\"\r\n")));
    EXPECT_TRUE(accepts_request(with_extensions(
        "Sec-WebSocket-Extensions: x-test\r\n"
        "Sec-WebSocket-Extensions: y-test; value=token\r\n")));

    EXPECT_TRUE(rejects_request(with_extensions("Sec-WebSocket-Extensions: , ,\r\n")));
    EXPECT_TRUE(rejects_request(with_extensions("Sec-WebSocket-Extensions: \"x-test\"\r\n")));
    EXPECT_TRUE(rejects_request(with_extensions("Sec-WebSocket-Extensions: x test\r\n")));
    EXPECT_TRUE(rejects_request(with_extensions("Sec-WebSocket-Extensions: x-test;\r\n")));
    EXPECT_TRUE(rejects_request(with_extensions("Sec-WebSocket-Extensions: x-test;; flag\r\n")));
    EXPECT_TRUE(rejects_request(with_extensions("Sec-WebSocket-Extensions: x-test; =value\r\n")));
    EXPECT_TRUE(rejects_request(with_extensions("Sec-WebSocket-Extensions: x-test; value=\r\n")));
    EXPECT_TRUE(rejects_request(
        with_extensions("Sec-WebSocket-Extensions: x-test; value=\"bad value\"\r\n")));
    EXPECT_TRUE(rejects_request(
        with_extensions("Sec-WebSocket-Extensions: x-test; value=\"unterminated\r\n")));
    EXPECT_TRUE(rejects_request(
        with_extensions("Sec-WebSocket-Extensions: x-test; value=\"token\"junk\r\n")));
}

TEST(Ruvia, ws_valid_request_requires_all_conditions) {
    EXPECT_TRUE(accepts_request(valid_handshake()));

    // Every individual requirement is necessary.
    EXPECT_TRUE(rejects_request(post_handshake()));
    EXPECT_TRUE(rejects_request(http10_handshake()));
    // RFC 6455 permits additional HTTP fields, and RFC 9112 framing makes a
    // zero Content-Length an empty request. Its mere presence must not block
    // an otherwise valid protocol switch.
    EXPECT_TRUE(accepts_request(content_length_zero_handshake()));
    // Actual request content still prevents switching protocols because those
    // octets belong to the HTTP message rather than to the websocket stream.
    EXPECT_TRUE(rejects_request(content_length_one_handshake()));

    constexpr std::string_view no_connection_upgrade =
        "GET /ws HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n";
    EXPECT_TRUE(rejects_request(no_connection_upgrade));

    constexpr std::string_view duplicate_key =
        "GET /ws HTTP/1.1\r\nHost: x\r\nConnection: Upgrade\r\n"
        "Upgrade: websocket\r\nSec-WebSocket-Version: 13\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n";
    EXPECT_TRUE(rejects_request(duplicate_key));

    constexpr std::string_view duplicate_version =
        "GET /ws HTTP/1.1\r\nHost: x\r\nConnection: Upgrade\r\n"
        "Upgrade: websocket\r\nSec-WebSocket-Version: 13\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n";
    EXPECT_TRUE(rejects_request(duplicate_version));

    // The Upgrade header must name "websocket", not another protocol token.
    constexpr std::string_view wrong_upgrade =
        "GET /ws HTTP/1.1\r\nHost: x\r\nConnection: Upgrade\r\n"
        "Upgrade: not-websocket\r\n"
        "Sec-WebSocket-Version: 13\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n";
    EXPECT_TRUE(rejects_request(wrong_upgrade));

    // Sec-websocket-Key present exactly once but not a 16-byte base64 value
    // (RFC 6455 4.1) -> invalid. "YWJj" decodes to 3 bytes.
    constexpr std::string_view bad_key =
        "GET /ws HTTP/1.1\r\nHost: x\r\nConnection: Upgrade\r\n"
        "Upgrade: websocket\r\n"
        "Sec-WebSocket-Version: 13\r\nSec-WebSocket-Key: YWJj\r\n\r\n";
    EXPECT_TRUE(rejects_request(bad_key));

    // "...ZR==" decodes to the same 16 bytes as the canonical "...ZQ==",
    // but sets unused base64 padding bits and must therefore be rejected.
    constexpr std::string_view non_canonical_key =
        "GET /ws HTTP/1.1\r\nHost: x\r\nConnection: Upgrade\r\n"
        "Upgrade: websocket\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZR==\r\n\r\n";
    EXPECT_TRUE(rejects_request(non_canonical_key));

    const auto unsupported_version = validate_request(bad_version_handshake());
    EXPECT_TRUE(unsupported_version.failure() != nullptr);
    if (const auto* failure = unsupported_version.failure()) {
        const auto error = failure->protocol_error();
        EXPECT_EQ(error.status(), ruvia::http_status::bad_request);
        EXPECT_EQ(std::string_view(error.what()), std::string_view("unsupported WebSocket version"));
        ruvia::http_response response;
        failure->apply_required_response_headers(response);
        EXPECT_EQ(response.header("Sec-WebSocket-Version"), std::string_view("13"));
    }
}

TEST(Ruvia, ws_upgrade_uses_the_shared_recipient_list_semantics) {
    constexpr std::string_view request =
        "GET /ws HTTP/1.1\r\n"
        "Host: example.test\r\n"
        "Connection: keep-alive\r\n"
        "Connection: , Upgrade,\r\n"
        "Upgrade: , custom/1, websocket,\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "\r\n";
    EXPECT_TRUE(accepts_request(request));
}

TEST(Ruvia, ws_server_handshake_response_serialization_is_http_owned) {
    const auto request = parse_request(
        "GET /ws HTTP/1.1\r\n"
        "Host: example.test\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Protocol: chat, superchat\r\n"
        "Sec-WebSocket-Extensions: permessage-deflate; server_max_window_bits=15\r\n"
        "\r\n");
    std::string supported = "chat";
    const std::array<std::string_view, 1> supported_views{supported};
    const auto handshake =
        ruvia::make_websocket_server_handshake(request, {.supported_subprotocols_ = supported_views});
    supported.front() = 'X';

    std::vector<std::string_view> response_parts;
    handshake.for_each_response_part(
        [&response_parts](std::string_view part) { response_parts.push_back(part); });
    std::string response;
    for (const auto part : response_parts) {
        response.append(part);
    }
    EXPECT_EQ(response, std::string("HTTP/1.1 101 Switching Protocols\r\n"
                                    "Upgrade: websocket\r\n"
                                    "Connection: Upgrade\r\n"
                                    "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n"
                                    "Sec-WebSocket-Protocol: chat\r\n"
                                    "Sec-WebSocket-Extensions: permessage-deflate; "
                                    "server_no_context_takeover; client_no_context_takeover; "
                                    "server_max_window_bits=15\r\n"
                                    "\r\n"));
    EXPECT_TRUE(handshake.compression() ==
                (ruvia::websocket_compression{.enabled_ = true, .server_max_window_bits_ = 15}));
    EXPECT_EQ(handshake.subprotocol(), "chat");
}

TEST(Ruvia, ws_server_handshake_negotiates_directional_context_takeover_independently) {
    const auto request = parse_request(
        "GET /ws HTTP/1.1\r\n"
        "Host: example.test\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Extensions: permessage-deflate; client_no_context_takeover\r\n"
        "\r\n");
    const auto handshake = ruvia::make_websocket_server_handshake(
        request, {.deflate_ = {.enabled_ = true, .context_takeover_ = true}});
    EXPECT_TRUE(handshake.compression().enabled_);
    EXPECT_TRUE(!handshake.compression().server_no_context_takeover_);
    EXPECT_TRUE(handshake.compression().client_no_context_takeover_);
}

TEST(Ruvia, ws_handshake_copies_application_headers_and_preserves_multiple_cookies) {
    const auto request = parse_request(valid_handshake());
    std::string cookie = "sid=0123456789abcdef; HttpOnly";
    const std::array fields_value{ruvia::http_header_view("Set-Cookie", cookie),
        ruvia::http_header_view("Set-Cookie", "theme=dark"),
        ruvia::http_header_view("X-Request-Id", "request-1"),
        ruvia::http_header_view("Alt-Svc", "h3=\":443\"; ma=86400")};
    const auto handshake = ruvia::make_websocket_server_handshake(request, {.response_headers_ = fields_value});
    cookie.assign(cookie.size(), 'x');
    std::string response;
    handshake.for_each_response_part([&](std::string_view part) { response.append(part); });
    EXPECT_TRUE((response.find("set-cookie: sid=0123456789abcdef; HttpOnly\r\n") != std::string_view::npos));
    EXPECT_TRUE((response.find("set-cookie: theme=dark\r\n") != std::string_view::npos));
    EXPECT_TRUE((response.find("x-request-id: request-1\r\n") != std::string_view::npos));
    EXPECT_TRUE((response.find("alt-svc: h3=\":443\"; ma=86400\r\n") != std::string_view::npos));
    EXPECT_TRUE(response.ends_with("\r\n\r\n"));
}

TEST(Ruvia, ws_handshake_rejects_application_framing_and_invalid_fields) {
    const auto request = parse_request(valid_handshake());
    for (const auto name : {"Connection", "Upgrade", "Content-Length", "Transfer-Encoding",
             "Sec-WebSocket-Accept", "Sec-WebSocket-Protocol", "TE", "Trailer", "bad name"}) {
        const std::array fields_value{ruvia::http_header_view(name, "value")};
        bool rejected = false;
        try {
            (void)ruvia::make_websocket_server_handshake(request, {.response_headers_ = fields_value});
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        EXPECT_TRUE(rejected);
    }
    const std::array fields_value{ruvia::http_header_view("Set-Cookie", "sid=a\r\nInjected: value")};
    bool rejected = false;
    try {
        (void)ruvia::make_websocket_server_handshake(request, {.response_headers_ = fields_value});
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    EXPECT_TRUE(rejected);
}

TEST(Ruvia, ws_handshake_bounds_application_header_size) {
    const auto request = parse_request(valid_handshake());
    const std::vector<ruvia::http_header_view> many(80, {"x", "v"});
    (void)ruvia::make_websocket_server_handshake(request, {.response_headers_ = many});
    const std::string oversized(ruvia::max_http_header_bytes, 'a');
    const std::array fields_value{ruvia::http_header_view("x", oversized)};
    bool size_rejected = false;
    try {
        (void)ruvia::make_websocket_server_handshake(request, {.response_headers_ = fields_value});
    } catch (const std::length_error&) {
        size_rejected = true;
    }
    EXPECT_TRUE(size_rejected);
}

TEST(Ruvia, ws_handshake_budget_covers_status_line_and_final_empty_line) {
    const auto request = parse_request(valid_handshake());
    const auto serialize = [](const ruvia::websocket_server_handshake& handshake) {
        std::string response;
        handshake.for_each_response_part([&](std::string_view part) { response.append(part); });
        return response;
    };
    const auto base_size = serialize(ruvia::make_websocket_server_handshake(request, {})).size();
    // "x: " + value + CRLF fills the head to exactly max_http_header_bytes.
    const std::string largest(ruvia::max_http_header_bytes - base_size - 5, 'a');
    const std::array fitting{ruvia::http_header_view("x", largest)};
    EXPECT_EQ(serialize(ruvia::make_websocket_server_handshake(request, {.response_headers_ = fitting})).size(), ruvia::max_http_header_bytes);
    const std::string oversized = largest + 'a';
    const std::array too_large{ruvia::http_header_view("x", oversized)};
    bool rejected = false;
    try {
        (void)ruvia::make_websocket_server_handshake(request, {.response_headers_ = too_large});
    } catch (const std::length_error&) {
        rejected = true;
    }
    EXPECT_TRUE(rejected);
}

TEST(Ruvia, ws_handshake_application_values_are_valid_for_both_http_versions) {
    const auto request = parse_request(valid_handshake());
    for (const auto value : {" leading", "trailing ", "\tleading", "trailing\t"}) {
        const std::array headers{ruvia::http_header_view("X-Test", value)};
        bool rejected = false;
        try {
            (void)ruvia::make_websocket_server_handshake(request, {.response_headers_ = headers});
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        EXPECT_TRUE(rejected);
    }
    const std::array headers{ruvia::http_header_view("X-Test", "")};
    const auto handshake = ruvia::make_websocket_server_handshake(request, {.response_headers_ = headers});
    std::string response;
    handshake.for_each_response_part([&](std::string_view part) { response.append(part); });
    EXPECT_TRUE((response.find("x-test: \r\n") != std::string_view::npos));
}

namespace {

template <typename subprotocols_type>
concept ws_server_handshake_accepts_subprotocols = requires(subprotocols_type&& subprotocols) {
    ruvia::websocket_server_handshake_options{.supported_subprotocols_ = std::forward<subprotocols_type>(subprotocols)};
};

template <typename subprotocols_type>
concept ws_handshake_validation_accepts_subprotocols = requires(subprotocols_type&& subprotocols) {
    ruvia::websocket_handshake_validation_options{.supported_subprotocols_ = std::forward<subprotocols_type>(subprotocols)};
};

static_assert(ws_server_handshake_accepts_subprotocols<const std::vector<std::string_view>&>);
static_assert(!ws_server_handshake_accepts_subprotocols<std::vector<std::string_view>>);
static_assert(!ws_server_handshake_accepts_subprotocols<std::array<std::string_view, 1>>);
static_assert(ws_handshake_validation_accepts_subprotocols<const std::vector<std::string_view>&>);
static_assert(!ws_handshake_validation_accepts_subprotocols<std::vector<std::string_view>>);
static_assert(!ws_handshake_validation_accepts_subprotocols<std::array<std::string_view, 1>>);

template <typename protocol_type>
concept ws_subprotocol_set_accepts_append = requires(ruvia::websocket_subprotocol_set& set, protocol_type&& protocol) {
    set.append(std::forward<protocol_type>(protocol));
};

template <typename list_type>
concept ws_subprotocol_set_accepts_append_list = requires(ruvia::websocket_subprotocol_set& set, list_type&& list) {
    set.append_list(std::forward<list_type>(list));
};

template <typename list_type>
concept ws_subprotocol_set_accepts_append_offered_list =
    requires(ruvia::websocket_subprotocol_set& set, list_type&& list) {
        set.append_offered_list(std::forward<list_type>(list));
    };

static_assert(ws_subprotocol_set_accepts_append<const std::string&>);
static_assert(!ws_subprotocol_set_accepts_append<std::string>);
static_assert(ws_subprotocol_set_accepts_append_list<const std::string&>);
static_assert(!ws_subprotocol_set_accepts_append_list<std::string>);
static_assert(ws_subprotocol_set_accepts_append_offered_list<const std::string&>);
static_assert(!ws_subprotocol_set_accepts_append_offered_list<std::string>);

constexpr std::array<std::uint8_t, 16> nonce{
    't', 'h', 'e', ' ', 's', 'a', 'm', 'p', 'l', 'e', ' ', 'n', 'o', 'n', 'c', 'e'};

constexpr std::string_view accept = "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=";

// What a client holds across one opening handshake: the encoded key, the
// negotiation and the application headers it offers.
class client_handshake final {
public:
    explicit client_handshake(std::span<const ruvia::http_header_view> headers = {},
        std::span<const std::string_view> protocols = {})
        : negotiation_({.subprotocols_ = protocols}) {
        ruvia::validate_http1_websocket_client_handshake_configuration(headers);
        ruvia::http_base64_encode(key_.data(), nonce);
        for (const auto header : headers) {
            headers_.emplace_back(header.name(), header.value());
        }
    }

    [[nodiscard]] std::variant<ruvia::prepared_http1_client_request,
        ruvia::http1_client_request_buffer_too_small, ruvia::http1_client_request_prepare_error>
    prepare(const ruvia::http_origin_view& origin, std::string_view target, std::span<char> buffer) const {
        return ruvia::prepare_http1_websocket_client_request(origin, target, key_, negotiation_, headers_, buffer);
    }

    // The response parser takes a freshly prepared request's exchange state.
    [[nodiscard]] ruvia::http1_client_response_parser parser() const {
        std::array<char, 2048> buffer{};
        auto prepared = prepare(ruvia::http_origin_view::https({.host_ = "example.com"}), "/", buffer);
        if (prepared.index() != 0) {
            throw std::runtime_error("expected prepared handshake request");
        }
        return ruvia::http1_client_response_parser(std::get<0>(std::move(prepared)).take_exchange_state());
    }

    [[nodiscard]] ruvia::http1_parsed_client_response_head parse(std::string_view wire) const {
        auto response_parser = parser();
        auto result_value = response_parser.parse(wire);
        if (result_value.parsed() == nullptr) {
            throw std::runtime_error("expected parsed response");
        }
        return std::move(*result_value.parsed());
    }

    [[nodiscard]] std::variant<ruvia::websocket_client_negotiation_result_view,
        ruvia::websocket_client_handshake_error>
    validate(const ruvia::http1_parsed_client_response_head& response) const {
        return ruvia::validate_http1_websocket_client_response(key_, negotiation_, response);
    }

private:
    std::array<char, 24> key_{};
    ruvia::websocket_client_negotiation negotiation_;
    std::vector<ruvia::http_header> headers_;
};

std::string valid_response(std::string_view extra = {}) {
    std::string wire =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Accept: ";
    wire += accept;
    wire += "\r\n";
    wire += extra;
    wire += "\r\n";
    return wire;
}

bool rejects_configuration(std::span<const ruvia::http_header_view> headers = {},
    std::span<const std::string_view> protocols = {}) {
    try {
        const client_handshake handshake(headers, protocols);
        (void)handshake;
        return false;
    } catch (const std::invalid_argument&) {
        return true;
    }
}

}  // namespace

TEST(Ruvia, websocket_client_handshake_prepares_rfc_request) {
    const std::array protocols{std::string_view{"chat"}};
    const std::array headers{
        ruvia::http_header_view{"X-Trace", "one"}, ruvia::http_header_view{"User-Agent", "RuviaTest/1"}};
    const client_handshake handshake(headers, protocols);
    std::array<char, 2048> buffer{};
    const auto prepared = handshake.prepare(
        ruvia::http_origin_view::https({.host_ = "example.com"}), "/chat", buffer);
    const auto* prepared_req = std::get_if<ruvia::prepared_http1_client_request>(&prepared);
    EXPECT_TRUE(prepared_req != nullptr);
    const std::string wire(prepared_req->head());
    EXPECT_TRUE(wire.find("GET /chat HTTP/1.1\r\n") == 0);
    EXPECT_TRUE(wire.find("Host: example.com\r\n") != std::string::npos);
    EXPECT_TRUE(wire.find("Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n") !=
                std::string::npos);
    EXPECT_TRUE(wire.find("sec-websocket-protocol: chat\r\n") != std::string::npos);
    EXPECT_TRUE(wire.find("User-Agent: RuviaTest/1\r\n") != std::string::npos);
    EXPECT_TRUE(wire.find("X-Trace: one\r\n") != std::string::npos);
}

TEST(Ruvia, websocket_client_handshake_matches_subprotocol_case_exactly) {
    const std::array protocols{std::string_view{"chat"}, std::string_view{"Chat"}};
    const client_handshake handshake({}, protocols);
    const auto response = handshake.parse(valid_response("Sec-WebSocket-Protocol: Chat\r\n"));
    const auto result_value = handshake.validate(response);
    EXPECT_TRUE((result_value.index() == 0));
    if ((result_value.index() == 0)) {
        EXPECT_EQ(std::get<0>(result_value).selected_subprotocol_, std::string_view("Chat"));
    }
}

TEST(Ruvia, websocket_client_handshake_rejects_unoffered_or_duplicate_protocol) {
    const std::array protocols{std::string_view{"chat"}};
    const client_handshake handshake({}, protocols);
    for (const auto response_protocol : {std::string_view{"Chat"}, std::string_view{"chat"}}) {
        const std::string extra = response_protocol == "chat"
                                      ? "Sec-WebSocket-Protocol: chat\r\n"
                                        "Sec-WebSocket-Protocol: chat\r\n"
                                      : "Sec-WebSocket-Protocol: Chat\r\n";
        const auto response = handshake.parse(valid_response(extra));
        const auto result_value = handshake.validate(response);
        EXPECT_TRUE(!(result_value.index() == 0));
        if (!(result_value.index() == 0)) {
            EXPECT_TRUE(std::get<1>(result_value) == ruvia::websocket_client_handshake_error::subprotocol);
        }
    }
}

TEST(Ruvia, websocket_client_handshake_rejects_bad_accept_and_extensions) {
    const client_handshake handshake;
    const auto bad = handshake.parse(
        "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Accept: wrong\r\n\r\n");
    EXPECT_TRUE(bad.plan().protocol_upgrade() != nullptr);
    const auto bad_result = handshake.validate(bad);
    EXPECT_TRUE(!(bad_result.index() == 0));
    if (!(bad_result.index() == 0)) {
        EXPECT_TRUE(std::get<1>(bad_result) == ruvia::websocket_client_handshake_error::accept);
    }

    const auto duplicate =
        handshake.parse(valid_response("Sec-WebSocket-Accept: " + std::string(accept) + "\r\n"));
    const auto duplicate_result = handshake.validate(duplicate);
    EXPECT_TRUE(!(duplicate_result.index() == 0));
    if (!(duplicate_result.index() == 0)) {
        EXPECT_TRUE(std::get<1>(duplicate_result) == ruvia::websocket_client_handshake_error::accept);
    }

    const auto response =
        handshake.parse(valid_response("Sec-WebSocket-Extensions: permessage-deflate\r\n"));
    const auto extension_result = handshake.validate(response);
    EXPECT_TRUE(!(extension_result.index() == 0));
    if (!(extension_result.index() == 0)) {
        EXPECT_TRUE(std::get<1>(extension_result) == ruvia::websocket_client_handshake_error::extensions);
    }
}

TEST(Ruvia, websocket_client_handshake_parser_accepts_informational_before_upgrade) {
    const client_handshake handshake;
    auto parser = handshake.parser();
    auto informational = parser.parse("HTTP/1.1 103 Early Hints\r\n\r\n");
    EXPECT_TRUE(informational.parsed() != nullptr);
    if (informational.parsed() != nullptr) {
        EXPECT_TRUE(informational.parsed()->plan().informational() != nullptr);
    }
    auto final = parser.parse(valid_response());
    EXPECT_TRUE(final.parsed() != nullptr);
    if (final.parsed() != nullptr) {
        const auto result_value = handshake.validate(*final.parsed());
        EXPECT_TRUE((result_value.index() == 0));
    }
}

TEST(Ruvia, websocket_client_handshake_rejects_invalid_configuration_and_reserved_headers) {
    const std::array bad_space{ruvia::http_header_view{"X-Test", "bad value\r\n"}};
    const std::array bad_comma{ruvia::http_header_view{"X-Test", "bad,\r\nvalue"}};
    const std::array reserved{ruvia::http_header_view{"Host", "evil.example"}};
    const std::array transfer_encoding{
        ruvia::http_header_view{"Transfer-Encoding", "chunked"}};
    const std::array bad_user_agent{ruvia::http_header_view{"User-Agent", "bad\r\nagent"}};
    const std::array trailer{ruvia::http_header_view{"Trailer", "X-End"}};
    const std::array expect{ruvia::http_header_view{"Expect", "100-continue"}};
    const std::array pseudo{ruvia::http_header_view{":authority", "evil.example"}};
    const std::array extensions{ruvia::http_header_view{"Sec-WebSocket-Extensions", "permessage-deflate"}};
    const std::array duplicate{std::string_view{"chat"}, std::string_view{"chat"}};
    const std::array empty{std::string_view{""}};
    const std::array bad_token{std::string_view{"bad token"}};
    const std::array comma_token{std::string_view{"chat,superchat"}};
    EXPECT_TRUE(rejects_configuration(bad_space));
    EXPECT_TRUE(rejects_configuration(bad_comma));
    EXPECT_TRUE(rejects_configuration(reserved));
    EXPECT_TRUE(rejects_configuration(transfer_encoding));
    EXPECT_TRUE(rejects_configuration(trailer));
    EXPECT_TRUE(rejects_configuration(expect));
    EXPECT_TRUE(rejects_configuration(pseudo));
    EXPECT_TRUE(rejects_configuration(extensions));
    EXPECT_TRUE(rejects_configuration({}, duplicate));
    EXPECT_TRUE(rejects_configuration({}, empty));
    EXPECT_TRUE(rejects_configuration({}, bad_token));
    EXPECT_TRUE(rejects_configuration({}, comma_token));
    EXPECT_TRUE(rejects_configuration(bad_user_agent));
}

TEST(Ruvia, websocket_client_handshake_prepares_ipv6_host) {
    const client_handshake handshake;
    std::array<char, 2048> buffer{};
    const auto prepared = handshake.prepare(
        ruvia::http_origin_view::https({.host_ = "[2001:db8::1]", .port_ = 8443}), "/before", buffer);
    const auto* prepared_req = std::get_if<ruvia::prepared_http1_client_request>(&prepared);
    EXPECT_TRUE(prepared_req != nullptr);
    const std::string wire(prepared_req->head());
    EXPECT_TRUE(wire.find("Host: [2001:db8::1]:8443\r\n") != std::string::npos);
}

TEST(Ruvia, websocket_client_handshake_result_view_is_used_while_response_lives) {
    const std::array protocols{std::string_view{"chat"}};
    const client_handshake handshake({}, protocols);
    const auto response = handshake.parse(valid_response("Sec-WebSocket-Protocol: chat\r\n"));
    const auto result_value = handshake.validate(response);
    EXPECT_TRUE((result_value.index() == 0));
    if ((result_value.index() == 0)) {
        const auto selected = std::get<0>(result_value).selected_subprotocol_;
        EXPECT_EQ(selected, std::string_view("chat"));
        EXPECT_EQ(response.head().headers().back().value(), selected);
    }
}

namespace {
void add(std::vector<ruvia::http_header>& head, std::string_view name, std::string_view value) {
    head.emplace_back(name, value);
}

}  // namespace

TEST(Ruvia, websocket_client_negotiation_accepts_asymmetric_context_and_window_parameters) {
    const std::array protocols{std::string_view("chat")};
    ruvia::websocket_client_negotiation client({.subprotocols_ = protocols, .deflate_ = {.compression_ = {.enabled_ = true, .server_no_context_takeover_ = false, .client_no_context_takeover_ = false, .server_max_window_bits_ = 10, .client_max_window_bits_ = 12}}});
    std::vector<ruvia::http_header> head;

    add(head, "sec-websocket-protocol", "chat");
    add(head, "sec-websocket-extensions", "permessage-deflate; client_no_context_takeover; server_max_window_bits=10; client_max_window_bits=12");
    auto accepted = client.validate_fields(head);
    EXPECT_TRUE((accepted.index() == 0));
    EXPECT_EQ(std::get<0>(accepted).selected_subprotocol_, std::string_view("chat"));
    EXPECT_TRUE(!std::get<0>(accepted).compression_.server_no_context_takeover_);
    EXPECT_TRUE(std::get<0>(accepted).compression_.client_no_context_takeover_);
    EXPECT_EQ(std::get<0>(accepted).compression_.server_max_window_bits_.value(), 10);
    EXPECT_EQ(std::get<0>(accepted).compression_.client_max_window_bits_.value(), 12);
}
TEST(Ruvia, websocket_client_negotiation_rejects_duplicate_unknown_or_unoffered_selection) {
    ruvia::websocket_client_negotiation client({.deflate_ = {.compression_ = {.enabled_ = true, .server_max_window_bits_ = 10}, .offer_client_max_window_bits_ = false}});
    for (auto extension : std::array{"permessage-deflate", "permessage-deflate; server_no_context_takeover; server_max_window_bits=11", "permessage-deflate; server_no_context_takeover; server_max_window_bits=10; client_max_window_bits=9", "permessage-deflate; server_no_context_takeover; server_max_window_bits=10; unknown", "permessage-deflate; server_no_context_takeover; server_max_window_bits=10; server_max_window_bits=10"}) {
        std::vector<ruvia::http_header> head;

        add(head, "sec-websocket-extensions", extension);
        EXPECT_TRUE(!(client.validate_fields(head).index() == 0));
    }
    // A server may add its own window bound without an offer.
    ruvia::websocket_client_negotiation no_server_window_client({.deflate_ = {.compression_ = {.enabled_ = true, .server_no_context_takeover_ = false, .client_no_context_takeover_ = false}}});
    {
        std::vector<ruvia::http_header> head;

        add(head, "sec-websocket-extensions", "permessage-deflate; server_max_window_bits=10");
        EXPECT_TRUE((no_server_window_client.validate_fields(head).index() == 0));
    }
    // The server may select a larger client window than the offered hint, or
    // none, but the client still never compresses with more than it offered.
    ruvia::websocket_client_negotiation limited_client_window({.deflate_ = {.compression_ = {.enabled_ = true, .server_no_context_takeover_ = false, .client_no_context_takeover_ = false, .client_max_window_bits_ = 9}}});
    for (auto extension : std::array{"permessage-deflate; client_max_window_bits=10", "permessage-deflate"}) {
        std::vector<ruvia::http_header> head;

        add(head, "sec-websocket-extensions", extension);
        auto accepted = limited_client_window.validate_fields(head);
        EXPECT_TRUE((accepted.index() == 0));
        if (accepted.index() == 0) {
            EXPECT_EQ(std::get<0>(accepted).compression_.client_max_window_bits_.value_or(15), 9);
        }
    }
    {
        std::vector<ruvia::http_header> head;

        add(head, "sec-websocket-extensions", "permessage-deflate; client_max_window_bits=8");
        auto accepted = limited_client_window.validate_fields(head);
        EXPECT_TRUE((accepted.index() == 0));
        if (accepted.index() == 0) {
            EXPECT_EQ(std::get<0>(accepted).compression_.client_max_window_bits_.value_or(15), 8);
        }
    }
    // A client takeover preference in an offer is a hint.
    ruvia::websocket_client_negotiation require_client_no_takeover({.deflate_ = {.compression_ = {.enabled_ = true, .server_no_context_takeover_ = false, .client_no_context_takeover_ = true}}});
    {
        std::vector<ruvia::http_header> head;

        add(head, "sec-websocket-extensions", "permessage-deflate");
        auto accepted = require_client_no_takeover.validate_fields(head);
        EXPECT_TRUE((accepted.index() == 0));
        EXPECT_TRUE(std::get<0>(accepted).compression_.client_no_context_takeover_);
    }
    std::vector<ruvia::http_header> head;

    add(head, "sec-websocket-protocol", "unoffered");
    EXPECT_TRUE(!(client.validate_fields(head).index() == 0));
}
TEST(Ruvia, websocket_h1_client_handshake_negotiates_deflate) {
    constexpr std::array<std::uint8_t, 16> nonce{'t', 'h', 'e', ' ', 's', 'a', 'm', 'p', 'l', 'e', ' ', 'n', 'o', 'n', 'c', 'e'};
    std::array<char, 24> encoded_key{};
    ruvia::http_base64_encode(encoded_key.data(), nonce);
    const ruvia::websocket_client_negotiation negotiation({.deflate_ = {.compression_ = {.enabled_ = true}}});
    std::array<char, 2048> buffer{};
    auto prepared = ruvia::prepare_http1_websocket_client_request(
        ruvia::http_origin_view::https({.host_ = "example.com"}), "/", encoded_key, negotiation, std::span<const ruvia::http_header>{}, buffer);
    auto* prepared_req = std::get_if<ruvia::prepared_http1_client_request>(&prepared);
    EXPECT_TRUE(prepared_req != nullptr);
    EXPECT_TRUE((prepared_req->head().find("sec-websocket-extensions: permessage-deflate") != std::string_view::npos));
    ruvia::http1_client_response_parser parser(std::move(*prepared_req).take_exchange_state());
    auto response = parser.parse("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\nSec-WebSocket-Extensions: permessage-deflate; server_no_context_takeover; client_no_context_takeover\r\n\r\n");
    EXPECT_TRUE(response.parsed() != nullptr);
    auto accepted = ruvia::validate_http1_websocket_client_response(encoded_key, negotiation, *response.parsed());
    EXPECT_TRUE((accepted.index() == 0));
    EXPECT_TRUE(std::get<0>(accepted).compression_.enabled_);
}

TEST(Ruvia, websocket_client_negotiation_supports_multiline_and_comma_extensions_while_rejecting_duplicates) {
    ruvia::websocket_client_negotiation client(
        {.deflate_ = {.compression_ = {.enabled_ = true, .server_no_context_takeover_ = false, .server_max_window_bits_ = 12}}});

    // Multi-line Sec-WebSocket-Extensions with empty/comma lines per RFC 6455 §9.1 / RFC 9110 §5.3.
    {
        std::vector<ruvia::http_header> head;
        add(head, "sec-websocket-extensions", ",");
        add(head, "sec-websocket-extensions", " , permessage-deflate; server_max_window_bits=10 , ");
        auto accepted = client.validate_fields(head);
        ASSERT_EQ(accepted.index(), 0U);
        EXPECT_TRUE(std::get<0>(accepted).compression_.enabled_);
        EXPECT_EQ(std::get<0>(accepted).compression_.server_max_window_bits_, std::optional<int>(10));
    }

    // Duplicate permessage-deflate across multiple header lines is rejected.
    {
        std::vector<ruvia::http_header> head;
        add(head, "sec-websocket-extensions", "permessage-deflate; server_max_window_bits=10");
        add(head, "sec-websocket-extensions", "permessage-deflate; server_max_window_bits=10");
        auto rejected = client.validate_fields(head);
        ASSERT_EQ(rejected.index(), 1U);
        EXPECT_EQ(std::get<1>(rejected), ruvia::websocket_client_handshake_error::extensions);
    }

    // Duplicate permessage-deflate in a single comma-separated line is rejected.
    {
        std::vector<ruvia::http_header> head;
        add(head, "sec-websocket-extensions",
            "permessage-deflate; server_max_window_bits=10, permessage-deflate; server_max_window_bits=10");
        auto rejected = client.validate_fields(head);
        ASSERT_EQ(rejected.index(), 1U);
        EXPECT_EQ(std::get<1>(rejected), ruvia::websocket_client_handshake_error::extensions);
    }

    // Unknown extension on a second header line or comma-separated item is rejected.
    {
        std::vector<ruvia::http_header> head;
        add(head, "sec-websocket-extensions", "permessage-deflate; server_max_window_bits=10");
        add(head, "sec-websocket-extensions", "x-unknown-ext");
        auto rejected = client.validate_fields(head);
        ASSERT_EQ(rejected.index(), 1U);
        EXPECT_EQ(std::get<1>(rejected), ruvia::websocket_client_handshake_error::extensions);
    }
    {
        std::vector<ruvia::http_header> head;
        add(head, "sec-websocket-extensions", "permessage-deflate; server_max_window_bits=10, x-unknown-ext");
        auto rejected = client.validate_fields(head);
        ASSERT_EQ(rejected.index(), 1U);
        EXPECT_EQ(std::get<1>(rejected), ruvia::websocket_client_handshake_error::extensions);
    }

    // Present but empty Sec-WebSocket-Extensions violates 1#extension.
    {
        std::vector<ruvia::http_header> head;
        add(head, "sec-websocket-extensions", " , ");
        auto rejected = client.validate_fields(head);
        ASSERT_EQ(rejected.index(), 1U);
        EXPECT_EQ(std::get<1>(rejected), ruvia::websocket_client_handshake_error::extensions);
    }
}

namespace {

template <typename fields_type>
concept ws_client_negotiation_accepts_fields =
    requires(const ruvia::websocket_client_negotiation& negotiation, fields_type&& fields) {
        negotiation.validate_fields(std::forward<fields_type>(fields));
    };

static_assert(ws_client_negotiation_accepts_fields<const std::vector<ruvia::http_header>&>);
static_assert(!ws_client_negotiation_accepts_fields<std::vector<ruvia::http_header>>);
static_assert(!ws_client_negotiation_accepts_fields<std::array<ruvia::http_header, 1>>);

using namespace ruvia;

bool zero_mask(void*, websocket_mask_key_type& key) noexcept {
    key = {};
    return true;
}

struct client_endpoint final {
    explicit client_endpoint(websocket_compression compression)
        : protocol_(input_, {.message_limit_ = protocol_byte_limit::unlimited(),
                                .compression_ = compression,
                                .role_ = websocket_connection_role::client,
                                .mask_key_generator_ = &zero_mask}) {}

    http_io_buffer input_;
    websocket_connection protocol_;
};

// Takes every pending output byte of a sender.
template <typename sender_type>
[[nodiscard]] std::string drain(sender_type& sender) {
    const auto plan = sender.output_plan();
    std::string bytes_value(plan.bytes());
    bytes_value.append(plan.borrowed_bytes());
    EXPECT_TRUE(sender.consume_output(bytes_value.size()) == websocket_output_consume_status::drained);
    return bytes_value;
}

struct server_endpoint final {
    explicit server_endpoint(websocket_connection_options options)
        : input_(),
          protocol_(input_, [&] {
              options.message_limit_ = protocol_byte_limit::unlimited();
              return options;
          }()) {}

    http_io_buffer input_;
    websocket_connection protocol_;
};

// A direction without context takeover produces the same wire bytes whether
// it borrows the shared codecs or owns a per-connection codec, and with the
// shared codecs the connection's resource never holds codec state.

using namespace ruvia;

using namespace ruvia;

struct mask_source {
    unsigned calls_{0};
    bool fail_{false};
    static bool generate(void* context_value, websocket_mask_key_type& key) noexcept {
        auto& self = *static_cast<mask_source*>(context_value);
        ++self.calls_;
        key = {static_cast<char>(self.calls_), '\x23', '\x45', '\x67'};
        return !self.fail_;
    }
};

websocket_connection client(http_io_buffer& input, mask_source& source_value, protocol_byte_limit limit = protocol_byte_limit::unlimited()) {
    return websocket_connection(input, {.message_limit_ = limit, .role_ = websocket_connection_role::client, .mask_key_generator_ = &mask_source::generate, .mask_key_context_ = &source_value});
}

std::string drain(websocket_connection& connection) {
    const std::string bytes_value(connection.output_plan().bytes());
    if (connection.consume_output(bytes_value.size()) != websocket_output_consume_status::drained) {
        throw std::runtime_error("output did not drain");
    }
    return bytes_value;
}
}  // namespace

TEST(Ruvia, ws_public_close_code_rejects_wide_and_reserved_values_before_narrowing) {
    const auto rejects = [](auto input) {
        try {
            (void)websocket_close_code::from_value(input);
        } catch (const std::invalid_argument&) {
            return true;
        }
        return false;
    };
    const std::array<std::int64_t, 14> invalid_values{
        std::numeric_limits<std::int64_t>::min(), -64536, -1, 0, 999,
        1004, 1005, 1006, 1015, 1016, 2999, 5000, 66536,
        std::numeric_limits<std::int64_t>::max()};
    for (const auto input : invalid_values) {
        EXPECT_TRUE(!websocket_close_code::try_from_value(input).has_value());
        EXPECT_TRUE(rejects(input));
    }
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    EXPECT_TRUE(!websocket_close_code::try_from_value(maximum).has_value());
    EXPECT_TRUE(rejects(maximum));
}

TEST(Ruvia, ws_public_checked_close_codes_preserve_wire_value_and_role_rules) {
    for (const std::uint64_t input : {1000u, 1003u, 1007u, 1014u, 3000u, 3999u, 4000u, 4999u, 1010u}) {
        mask_source source;
        http_io_buffer client_input;
        auto sender = client(client_input, source);
        http_io_buffer server_input;
        websocket_connection receiver(server_input);
        const auto code = websocket_close_code::from_value(input);
        EXPECT_TRUE(sender.submit_close(code, "checked") == websocket_close_submit_status::accepted);
        server_input.append(drain(sender));
        const auto event = receiver.next_event();
        EXPECT_TRUE(event && event->close() && event->close()->close_code() == input);
        EXPECT_TRUE(event && event->close() && event->close()->reason() == "checked");
    }

    http_io_buffer server_input;
    websocket_connection sender(server_input);
    EXPECT_TRUE(sender.submit_close(websocket_close_code::from_value(1010), {}) == websocket_close_submit_status::invalid_code);
    EXPECT_TRUE(sender.submit_frame(websocket_opcode::text, "still-open") == websocket_frame_submit_status::accepted);
    EXPECT_TRUE(sender.submit_close(websocket_close_status::normal_closure, std::string_view("\xff", 1)) == websocket_close_submit_status::invalid_reason);
    EXPECT_TRUE(sender.submit_close(websocket_close_status::normal_closure, std::string(124, 'x')) == websocket_close_submit_status::reason_too_large);
    const std::string reason(123, 'x');
    EXPECT_TRUE(sender.submit_close(websocket_close_status::normal_closure, reason) == websocket_close_submit_status::accepted);
    mask_source source;
    http_io_buffer client_input;
    auto receiver = client(client_input, source);
    client_input.append(drain(sender));
    const auto message = receiver.next_event();
    EXPECT_TRUE(message && message->message() && message->message()->payload() == "still-open");
    const auto closed = receiver.next_event();
    EXPECT_TRUE(closed && closed->close() && closed->close()->close_code() == 1000);
    EXPECT_TRUE(closed && closed->close() && closed->close()->reason() == reason);
}

TEST(Ruvia, ws_public_compression_accepts_final_blocks_and_preserves_required_history) {
    for (const bool no_takeover : {false, true}) {
        mask_source mask;
        http_io_buffer input;
        websocket_connection receiver(input, {.compression_ = (websocket_compression{.enabled_ = true, .server_no_context_takeover_ = no_takeover}), .role_ = websocket_connection_role::client, .mask_key_generator_ = &mask_source::generate, .mask_key_context_ = &mask});
        // RFC 7692 section 7.2.3.4: BFINAL=1, padding, then an empty block.
        input.append(std::string_view("\xc1\x08\xf3\x48\xcd\xc9\xc9\x07\x00\x00", 10));
        auto event = receiver.next_event();
        EXPECT_TRUE(event && event->message() && event->message()->payload() == "Hello");
        // Two terminated streams within one message share their LZ77 history
        // even when context takeover between messages is disabled.
        input.append(std::string_view("\xc1\x0c\xf3\x48\xcd\xc9\xc9\x07\x00\xf3\x00\x11\x00\x00", 14));
        event = receiver.next_event();
        EXPECT_TRUE(event && event->message() && event->message()->payload() == "HelloHello");
        input.append(std::string_view("\xc1\x01\x00", 3));
        event = receiver.next_event();
        EXPECT_TRUE(event && event->message() && event->message()->payload().empty());
        // This message refers back to the preceding message's dictionary.
        input.append(std::string_view("\xc1\x05\xf2\x00\x11\x00\x00", 7));
        event = receiver.next_event();
        if (no_takeover) {
            EXPECT_TRUE(event && event->protocol_error() && event->protocol_error()->close_code() == 1002);
        } else {
            EXPECT_TRUE(event && event->message() && event->message()->payload() == "Hello");
        }
    }
}

TEST(Ruvia, ws_public_compression_rejects_incomplete_empty_block_headers) {
    for (const auto wire : {std::string_view("\xc1\x00", 2), std::string_view("\xc1\x02\x03\x00", 4)}) {
        mask_source mask;
        http_io_buffer input;
        websocket_connection receiver(input, {.compression_ = (websocket_compression{.enabled_ = true}), .role_ = websocket_connection_role::client, .mask_key_generator_ = &mask_source::generate, .mask_key_context_ = &mask});
        input.append(wire);
        const auto event = receiver.next_event();
        EXPECT_TRUE(event && event->protocol_error() && event->protocol_error()->close_code() == 1002);
        EXPECT_TRUE(receiver.submit_frame(websocket_opcode::text, "after-error") == websocket_frame_submit_status::not_open);
    }
}

TEST(Ruvia, ws_public_server_events_use_public_payload_types) {
    mask_source mask;
    http_io_buffer sender_input;
    auto sender = client(sender_input, mask);
    http_io_buffer input;
    websocket_connection protocol(input);

    EXPECT_TRUE(sender.submit_frame(websocket_opcode::text, "hey") ==
                websocket_frame_submit_status::accepted);
    const std::string message_wire(sender.output_plan().bytes());
    EXPECT_TRUE(sender.consume_output(message_wire.size()) == websocket_output_consume_status::drained);
    input.append(message_wire);
    auto message = protocol.next_event();
    EXPECT_TRUE(message && message->kind() == websocket_event_kind::message);
    EXPECT_TRUE(message && message->message());
    if (message && message->message()) {
        EXPECT_TRUE(message->message()->opcode() == websocket_opcode::text);
        EXPECT_EQ(message->message()->payload(), "hey");
    }

    EXPECT_TRUE(sender.submit_frame(websocket_opcode::ping, "p") ==
                websocket_frame_submit_status::accepted);
    const std::string ping_wire(sender.output_plan().bytes());
    EXPECT_TRUE(sender.consume_output(ping_wire.size()) == websocket_output_consume_status::drained);
    input.append(ping_wire);
    auto ping = protocol.next_event();
    EXPECT_TRUE(ping && ping->kind() == websocket_event_kind::ping);
    EXPECT_TRUE(ping && ping->ping());
    if (ping && ping->ping()) {
        EXPECT_EQ(ping->ping()->payload(), "p");
    }
}

TEST(Ruvia, ws_public_server_protocol_reports_protocol_error_as_public_value) {
    http_io_buffer input;
    websocket_connection protocol(input);
    // A server-side connection must reject an unmasked client frame.
    input.append(std::string_view("\x81\x01x", 3));
    auto error = protocol.next_event();
    EXPECT_TRUE(error && error->kind() == websocket_event_kind::protocol_error);
    EXPECT_TRUE(error && error->protocol_error() && error->protocol_error()->close_code() == 1002);
}

TEST(Ruvia, ws_public_server_protocol_controls_permessage_deflate_per_frame) {
    http_io_buffer input;
    websocket_connection protocol(input, {.message_limit_ = protocol_byte_limit::unlimited(),
                                             .compression_ = websocket_compression{.enabled_ = true},
                                             .deflater_ = {.compression_level_ = 6}});
    const std::string payload_value(200, 'x');

    EXPECT_TRUE(protocol.submit_frame(websocket_opcode::text, payload_value, {.compress_ = false}) ==
                websocket_frame_submit_status::accepted);
    auto output = protocol.output_plan().bytes();
    EXPECT_TRUE(!output.empty());
    if (!output.empty()) {
        EXPECT_TRUE((static_cast<unsigned char>(output[0]) & 0x40U) == 0);
    }
    EXPECT_TRUE(protocol.consume_output(output.size()) ==
                websocket_output_consume_status::drained);

    EXPECT_TRUE(protocol.submit_frame(websocket_opcode::text, payload_value) ==
                websocket_frame_submit_status::accepted);
    output = protocol.output_plan().bytes();
    EXPECT_TRUE(!output.empty());
    if (!output.empty()) {
        EXPECT_TRUE((static_cast<unsigned char>(output[0]) & 0x40U) != 0);
    }
}

TEST(Ruvia, ws_public_server_protocol_preserves_transport_end_semantics) {
    http_io_buffer input;
    websocket_connection protocol(input);
    EXPECT_TRUE(protocol.submit_close(websocket_close_status::normal_closure, "done") == websocket_close_submit_status::accepted);
    EXPECT_TRUE(protocol.output_plan().disposition() ==
                websocket_transport_disposition::keep_open);
    const auto output = protocol.output_plan().bytes();
    EXPECT_TRUE(!output.empty());
    EXPECT_TRUE(protocol.consume_output(output.size()) ==
                websocket_output_consume_status::drained);
    // After our Close frame is sent, the protocol still awaits the peer's
    // Close; transport EOF terminates that wait without fabricating a reply.
    EXPECT_TRUE(protocol.output_plan().disposition() ==
                websocket_transport_disposition::keep_open);
    protocol.notify_transport_eof();
    EXPECT_TRUE(protocol.output_plan().disposition() ==
                websocket_transport_disposition::end_transport);
}

TEST(Ruvia, ws_public_server_eof_discards_pending_output_before_transport_end) {
    http_io_buffer input;
    websocket_connection protocol(input);
    EXPECT_TRUE(protocol.submit_frame(websocket_opcode::text, "unsent") ==
                websocket_frame_submit_status::accepted);
    const auto output = protocol.output_plan().bytes();
    EXPECT_TRUE(output.size() > 1);
    EXPECT_TRUE(protocol.consume_output(1) == websocket_output_consume_status::pending);

    protocol.notify_transport_eof();
    const auto end = protocol.output_plan();
    EXPECT_TRUE(end.bytes().empty());
    EXPECT_TRUE(end.disposition() == websocket_transport_disposition::end_transport);
    // The driver can finish its direction without consuming bytes discarded by EOF.
    EXPECT_TRUE(protocol.consume_output(end.bytes().size()) ==
                websocket_output_consume_status::drained);
    protocol.commit_transport_end();
    EXPECT_TRUE(protocol.abort() == websocket_abort_disposition::no_transport_action);
}

TEST(Ruvia, ws_public_server_keeps_pending_pong_before_peer_close) {
    http_io_buffer input;
    websocket_connection protocol(input);
    EXPECT_TRUE(protocol.submit_close(websocket_close_status::normal_closure, "done") ==
                websocket_close_submit_status::accepted);
    auto output = protocol.output_plan().bytes();
    EXPECT_TRUE(!output.empty());
    EXPECT_TRUE(protocol.consume_output(output.size()) ==
                websocket_output_consume_status::drained);

    // Masked client Ping("p") followed by an empty masked Close.
    input.append(std::string_view(
        "\x89\x81\x01\x02\x03\x04q"
        "\x88\x80\x05\x06\x07\x08",
        13));
    const auto ping = protocol.next_event();
    EXPECT_TRUE(ping && ping->kind() == websocket_event_kind::ping);
    EXPECT_TRUE(ping && ping->ping() && ping->ping()->payload() == "p");
    const auto close = protocol.next_event();
    EXPECT_TRUE(close && close->kind() == websocket_event_kind::close);

    const auto plan = protocol.output_plan();
    EXPECT_EQ(plan.bytes(), std::string_view("\x8a\x01p", 3));
    EXPECT_TRUE(plan.disposition() ==
                websocket_transport_disposition::end_transport);
    EXPECT_TRUE(protocol.consume_output(plan.bytes().size()) ==
                websocket_output_consume_status::drained);
    protocol.commit_transport_end();
    EXPECT_TRUE(protocol.liveness_mode() == websocket_liveness_mode::inactive);
}

TEST(Ruvia, ws_public_client_server_exchange_and_partial_output) {
    mask_source source;
    http_io_buffer sender_input;
    auto sender = client(sender_input, source);
    http_io_buffer receiver_input;
    websocket_connection receiver(receiver_input);
    const std::string payload_value(65536, '\xff');
    for (unsigned round = 0; round < 3; ++round) {
        EXPECT_TRUE(sender.submit_frame(websocket_opcode::binary, payload_value) == websocket_frame_submit_status::accepted);
        const std::string wire(sender.output_plan().bytes());
        EXPECT_TRUE((static_cast<unsigned char>(wire[1]) & 0x80U) != 0);
        EXPECT_TRUE(sender.consume_output(wire.size() + 1) == websocket_output_consume_status::out_of_range);
        EXPECT_TRUE(sender.consume_output(1) == websocket_output_consume_status::pending);
        EXPECT_EQ(sender.output_plan().bytes(), std::string_view(wire).substr(1));
        EXPECT_TRUE(sender.consume_output(wire.size() - 1) == websocket_output_consume_status::drained);
        receiver_input.append(std::string_view(wire).substr(0, 5));
        EXPECT_TRUE(!receiver.next_event());
        receiver_input.append(std::string_view(wire).substr(5));
        const auto event = receiver.next_event();
        EXPECT_TRUE(event && event->message());
        if (!event || !event->message()) {
            return;
        }
        EXPECT_EQ(event->message()->payload(), payload_value);
        EXPECT_TRUE(receiver.submit_frame(websocket_opcode::binary, event->message()->payload()) == websocket_frame_submit_status::accepted);
        const auto reply = drain(receiver);
        EXPECT_TRUE((static_cast<unsigned char>(reply[1]) & 0x80U) == 0);
        sender_input.append(reply);
        const auto echo = sender.next_event();
        EXPECT_TRUE(echo && echo->message());
        if (!echo || !echo->message()) {
            return;
        }
        EXPECT_EQ(echo->message()->payload(), payload_value);
    }
    EXPECT_EQ(source.calls_, 3U);
}

TEST(Ruvia, ws_public_client_fragmented_message_with_ping_and_close) {
    mask_source source;
    http_io_buffer input;
    auto connection = client(input, source);
    http_io_buffer server_input;
    websocket_connection server(server_input);
    // A fragmented binary message with a control frame between fragments.
    const std::string wire(
        "\x02\x02"
        "ab"
        "\x89\x01"
        "p"
        "\x80\x02"
        "cd",
        11);
    input.append(wire);
    const auto ping = connection.next_event();
    EXPECT_TRUE(ping && ping->ping());
    const auto pong = drain(connection);
    server_input.append(pong);
    const auto pong_event = server.next_event();
    EXPECT_TRUE(pong_event && pong_event->pong() && pong_event->pong()->payload() == "p");
    const auto message = connection.next_event();
    EXPECT_TRUE(message && message->message() && message->message()->payload() == "abcd");
    EXPECT_TRUE(server.submit_close(websocket_close_status::normal_closure, "done") == websocket_close_submit_status::accepted);
    const auto close = drain(server);
    input.append(close);
    const auto closed = connection.next_event();
    EXPECT_TRUE(closed && closed->close() && closed->close()->close_code() == 1000);
    // RFC 6455 section 7.1.1: the server closes the transport first, so the
    // client flushes its echo and keeps the transport open until server EOF.
    EXPECT_TRUE(connection.output_plan().disposition() == websocket_transport_disposition::keep_open);
    const auto response = drain(connection);
    EXPECT_TRUE(connection.output_plan().disposition() == websocket_transport_disposition::keep_open);
    EXPECT_TRUE(connection.liveness_mode() == websocket_liveness_mode::awaiting_peer_eof);
    server_input.append(response);
    const auto acknowledged = server.next_event();
    EXPECT_TRUE(acknowledged && acknowledged->close());
    // Bytes after the close handshake are discarded while waiting.
    EXPECT_TRUE(connection.feed("x") == websocket_feed_status::accepted);
    input.append("x");
    EXPECT_TRUE(!connection.next_event());
    EXPECT_TRUE(connection.submit_frame(websocket_opcode::text, "late") == websocket_frame_submit_status::not_open);
    connection.notify_transport_eof();
    const auto end = connection.next_event();
    EXPECT_TRUE(end && end->transport_end());
    EXPECT_TRUE(connection.output_plan().disposition() == websocket_transport_disposition::end_transport);
    connection.commit_transport_end();
    EXPECT_TRUE(connection.feed("x") == websocket_feed_status::inactive);
    EXPECT_EQ(source.calls_, 2U);
    EXPECT_TRUE(pong.substr(2, 4) != response.substr(2, 4));
}

TEST(Ruvia, ws_public_client_waits_for_server_eof_after_initiating_close) {
    mask_source source;
    http_io_buffer input;
    websocket_connection connection(input, {.role_ = websocket_connection_role::client,
                                               .mask_key_generator_ = &mask_source::generate,
                                               .mask_key_context_ = &source});
    http_io_buffer server_input;
    websocket_connection server(server_input);
    EXPECT_TRUE(connection.submit_close(websocket_close_status::normal_closure, "done") == websocket_close_submit_status::accepted);
    const auto request_bytes = drain(connection);
    server_input.append(request_bytes);
    const auto request = server.next_event();
    EXPECT_TRUE(request && request->close());
    const auto reply = drain(server);
    EXPECT_TRUE(server.output_plan().disposition() == websocket_transport_disposition::end_transport);

    // The server's Close reply, then bytes the client must ignore.
    input.append(reply);
    input.append(std::string_view("\x81\x01x", 3));
    const auto closed = connection.next_event();
    EXPECT_TRUE(closed && closed->close() && closed->close()->close_code() == 1000);
    const auto waiting = connection.output_plan();
    EXPECT_TRUE(waiting.bytes().empty());
    EXPECT_TRUE(waiting.disposition() == websocket_transport_disposition::keep_open);
    EXPECT_TRUE(connection.liveness_mode() == websocket_liveness_mode::awaiting_peer_eof);
    EXPECT_TRUE(!connection.next_event());
    EXPECT_EQ(input.size(), std::size_t{0});
    connection.notify_transport_eof();
    const auto end = connection.next_event();
    EXPECT_TRUE(end && end->transport_end());
    EXPECT_TRUE(connection.output_plan().disposition() == websocket_transport_disposition::end_transport);
    connection.commit_transport_end();
    EXPECT_TRUE(connection.liveness_mode() == websocket_liveness_mode::inactive);
}

// A borrowed payload is a distinct parameter form: it never binds an owning
// temporary, and a plain string never borrows implicitly.
static_assert(!std::is_constructible_v<websocket_borrowed_payload, std::string>);
static_assert(!std::is_constructible_v<websocket_borrowed_payload, std::string>);
static_assert(std::is_constructible_v<websocket_borrowed_payload, const std::string&>);
static_assert(!std::is_convertible_v<std::string_view, websocket_borrowed_payload>);

// RFC 6455 section 5.4: one entry sends whole messages and fragments; a frame
// is the final fragment of its message.
TEST(Ruvia, ws_public_submit_frame_sends_fragments_and_whole_messages) {
    http_io_buffer input;
    websocket_connection sender(input);
    EXPECT_EQ(sender.submit_frame(websocket_opcode::text, "ab", {.final_ = false}), websocket_frame_submit_status::accepted);
    EXPECT_EQ(sender.submit_frame(websocket_opcode::binary, "x"), websocket_frame_submit_status::message_in_progress);
    // Control frames are never fragmented (section 5.5) and may interleave.
    EXPECT_EQ(sender.submit_frame(websocket_opcode::ping, "p", {.final_ = false}), websocket_frame_submit_status::invalid_opcode);
    EXPECT_EQ(sender.submit_frame(websocket_opcode::ping, "p"), websocket_frame_submit_status::accepted);
    EXPECT_EQ(sender.submit_frame(websocket_opcode::text, "\xC3", {.final_ = false}), websocket_frame_submit_status::accepted);
    const std::string tail(
        "\xA9"
        "cd");
    EXPECT_EQ(sender.submit_frame(websocket_opcode::text, websocket_borrowed_payload(tail)), websocket_frame_submit_status::accepted);
    EXPECT_EQ(sender.output_plan().borrowed_bytes(), tail);
    // The next frame copies the borrowed payload ahead of itself.
    EXPECT_EQ(sender.submit_frame(websocket_opcode::binary, "z"), websocket_frame_submit_status::accepted);
    EXPECT_TRUE(sender.output_plan().borrowed_bytes().empty());
    const std::string_view expected(
        "\x01\x02"
        "ab"
        "\x89\x01"
        "p"
        "\x00\x01"
        "\xC3"
        "\x80\x03"
        "\xA9"
        "cd"
        "\x82\x01"
        "z",
        18);
    const auto wire = drain(sender);
    EXPECT_EQ(std::string_view(wire), expected);

    mask_source source;
    http_io_buffer client_input;
    auto receiver = client(client_input, source);
    client_input.append(wire);
    const auto ping = receiver.next_event();
    EXPECT_TRUE(ping && ping->ping() && ping->ping()->payload() == "p");
    const auto text = receiver.next_event();
    EXPECT_TRUE(text && text->message() && text->message()->payload() ==
                                               "ab\xC3\xA9"
                                               "cd");
    const auto binary = receiver.next_event();
    EXPECT_TRUE(binary && binary->message() && binary->message()->payload() == "z");
}

TEST(Ruvia, ws_public_client_rejects_masked_server_and_oversized_message) {
    mask_source source;
    http_io_buffer invalid_server_input;
    auto invalid_server = client(invalid_server_input, source);
    EXPECT_TRUE(invalid_server.submit_frame(websocket_opcode::text, "bad") == websocket_frame_submit_status::accepted);
    const auto masked = drain(invalid_server);
    http_io_buffer receiver_input;
    auto receiver = client(receiver_input, source);
    receiver_input.append(masked);
    const auto error = receiver.next_event();
    EXPECT_TRUE(error && error->protocol_error() && error->protocol_error()->close_code() == 1002);
    http_io_buffer limited_input;
    auto limited = client(limited_input, source, protocol_byte_limit::limited(2));
    limited_input.append(std::string_view(
        "\x82\x03"
        "abc",
        5));
    const auto large = limited.next_event();
    EXPECT_TRUE(large && large->protocol_error() && large->protocol_error()->close_code() == 1009);
    EXPECT_TRUE(limited.submit_frame(websocket_opcode::binary, "x") == websocket_frame_submit_status::not_open);
}

TEST(Ruvia, ws_public_client_requires_entropy_and_handles_transport_end) {
    EXPECT_TRUE(ruvia::test::throws_on([] {
        http_io_buffer input;
        websocket_connection connection(input, {.role_ = websocket_connection_role::client});
    }));
    EXPECT_TRUE(ruvia::test::throws_on([] {
        http_io_buffer input;
        websocket_connection connection(input, {.role_ = static_cast<websocket_connection_role>(255)});
    }));
    mask_source source;
    http_io_buffer input;
    auto connection = client(input, source);
    source.fail_ = true;
    EXPECT_TRUE(ruvia::test::throws_on([&] {
        (void)connection.submit_frame(websocket_opcode::binary, "x");
    }));
    EXPECT_TRUE(connection.output_plan().bytes().empty());
    EXPECT_TRUE(connection.abort() == websocket_abort_disposition::abort_transport);
    EXPECT_TRUE(connection.abort() == websocket_abort_disposition::no_transport_action);
    http_io_buffer eof_input;
    auto eof = client(eof_input, source);
    eof.notify_transport_eof();
    const auto event = eof.next_event();
    EXPECT_TRUE(event && event->transport_end());
    eof.commit_transport_end();
    EXPECT_TRUE(eof.liveness_mode() == websocket_liveness_mode::inactive);
}

TEST(Ruvia, ws_public_wire_bytes_count_whole_frames_and_close_roles_fix_termination) {
    mask_source source;
    http_io_buffer client_input;
    auto client_side = client(client_input, source);
    http_io_buffer server_input;
    websocket_connection server(server_input);
    EXPECT_TRUE(client_side.termination() == websocket_termination::none);
    EXPECT_TRUE(server.termination() == websocket_termination::none);

    // Masked text "hello": 2-byte header, 4-byte mask key and 5 payload bytes.
    EXPECT_TRUE(client_side.submit_frame(websocket_opcode::text, "hello") == websocket_frame_submit_status::accepted);
    EXPECT_EQ(client_side.bytes_sent(), 0U);
    const auto text = drain(client_side);
    EXPECT_EQ(text.size(), 11U);
    EXPECT_EQ(client_side.bytes_sent(), 11U);
    // A frame counts once it is parsed whole.
    server_input.append(std::string_view(text).substr(0, 4));
    EXPECT_TRUE(!server.next_event());
    EXPECT_EQ(server.bytes_received(), 0U);
    server_input.append(std::string_view(text).substr(4));
    const auto message = server.next_event();
    EXPECT_TRUE(message && message->message() && message->message()->payload() == "hello");
    EXPECT_EQ(server.bytes_received(), 11U);

    // Ping "abc" (5 bytes) and the client's automatic masked Pong (9 bytes).
    EXPECT_TRUE(server.submit_frame(websocket_opcode::ping, "abc") == websocket_frame_submit_status::accepted);
    client_input.append(drain(server));
    const auto ping = client_side.next_event();
    EXPECT_TRUE(ping && ping->ping());
    server_input.append(drain(client_side));
    const auto pong = server.next_event();
    EXPECT_TRUE(pong && pong->pong() && pong->pong()->payload() == "abc");

    // Client-initiated Close 1000 (8 bytes), then the server echo (4 bytes).
    EXPECT_TRUE(client_side.submit_close(websocket_close_status::normal_closure, {}) == websocket_close_submit_status::accepted);
    server_input.append(drain(client_side));
    const auto close = server.next_event();
    EXPECT_TRUE(close && close->close() && close->close()->close_code() == 1000);
    EXPECT_TRUE(server.termination() == websocket_termination::peer_close);
    EXPECT_TRUE(client_side.termination() == websocket_termination::none);
    client_input.append(drain(server));
    const auto echoed = client_side.next_event();
    EXPECT_TRUE(echoed && echoed->close());
    EXPECT_TRUE(client_side.termination() == websocket_termination::local_close);

    EXPECT_EQ(client_side.bytes_sent(), 11U + 9U + 8U);
    EXPECT_EQ(server.bytes_received(), client_side.bytes_sent());
    EXPECT_EQ(server.bytes_sent(), 5U + 4U);
    EXPECT_EQ(client_side.bytes_received(), server.bytes_sent());

    // The first cause survives the transport end and any later abort.
    server.commit_transport_end();
    EXPECT_TRUE(server.abort(websocket_termination::io_error) == websocket_abort_disposition::no_transport_action);
    EXPECT_TRUE(server.termination() == websocket_termination::peer_close);
    client_side.notify_transport_eof();
    EXPECT_TRUE(client_side.abort(websocket_termination::timeout) == websocket_abort_disposition::abort_transport);
    EXPECT_TRUE(client_side.termination() == websocket_termination::local_close);
}

TEST(Ruvia, ws_public_termination_reports_the_first_failure_cause) {
    // An unmasked client frame fails as 1002 and is not counted as received.
    http_io_buffer unmasked_input;
    websocket_connection unmasked(unmasked_input);
    unmasked_input.append(std::string_view("\x81\x01x", 3));
    const auto unmasked_error = unmasked.next_event();
    EXPECT_TRUE(unmasked_error && unmasked_error->protocol_error());
    EXPECT_TRUE(unmasked.termination() == websocket_termination::protocol_error);
    EXPECT_EQ(unmasked.bytes_received(), 0U);
    unmasked.notify_transport_eof();
    EXPECT_TRUE(unmasked.termination() == websocket_termination::protocol_error);

    mask_source source;
    http_io_buffer utf8_input;
    auto utf8 = client(utf8_input, source);
    utf8_input.append(std::string_view("\x81\x01\xff", 3));
    const auto utf8_error = utf8.next_event();
    EXPECT_TRUE(utf8_error && utf8_error->protocol_error() && utf8_error->protocol_error()->close_code() == 1007);
    EXPECT_TRUE(utf8.termination() == websocket_termination::invalid_payload_data);

    http_io_buffer limited_input;
    auto limited = client(limited_input, source, protocol_byte_limit::limited(2));
    limited_input.append(std::string_view(
        "\x82\x03"
        "abc",
        5));
    const auto large = limited.next_event();
    EXPECT_TRUE(large && large->protocol_error() && large->protocol_error()->close_code() == 1009);
    EXPECT_TRUE(limited.termination() == websocket_termination::message_too_large);

    http_io_buffer eof_input;
    auto eof = client(eof_input, source);
    eof.notify_transport_eof();
    EXPECT_TRUE(eof.termination() == websocket_termination::eof);
    (void)eof.abort(websocket_termination::timeout);
    EXPECT_TRUE(eof.termination() == websocket_termination::eof);

    for (const auto cause : {websocket_termination::none, websocket_termination::aborted,
             websocket_termination::timeout, websocket_termination::io_error}) {
        http_io_buffer input;
        websocket_connection connection(input);
        EXPECT_TRUE(connection.abort(cause) == websocket_abort_disposition::abort_transport);
        connection.notify_transport_eof();
        EXPECT_TRUE(connection.termination() == (cause == websocket_termination::none ? websocket_termination::aborted : cause));
    }
}

TEST(Ruvia, ws_public_wire_bytes_count_deflated_payloads) {
    mask_source source;
    const websocket_compression compression{.enabled_ = true};
    http_io_buffer server_input;
    websocket_connection server(server_input, {.compression_ = compression});
    http_io_buffer client_input;
    websocket_connection client_side(client_input, {.compression_ = compression, .role_ = websocket_connection_role::client, .mask_key_generator_ = &mask_source::generate, .mask_key_context_ = &source});
    const std::string payload_value(4096, 'x');
    EXPECT_TRUE(server.submit_frame(websocket_opcode::text, payload_value) == websocket_frame_submit_status::accepted);
    const auto wire = drain(server);
    EXPECT_TRUE(wire.size() < payload_value.size());
    EXPECT_EQ(server.bytes_sent(), wire.size());
    client_input.append(wire);
    const auto message = client_side.next_event();
    EXPECT_TRUE(message && message->message() && message->message()->payload() == payload_value);
    EXPECT_EQ(client_side.bytes_received(), wire.size());
}

TEST(Ruvia, ws_default_message_limit_rejects_oversized_declared_frame_before_payload) {
    // A masked binary frame declaring 16 MiB + 1 bytes requires no payload to
    // establish that it cannot fit the finite default message policy.
    const std::string header_value("\x82\xff\x00\x00\x00\x00\x01\x00\x00\x01\x00\x00\x00\x00", 14);
    websocket_connection connection;
    EXPECT_TRUE(connection.feed(header_value) == websocket_feed_status::accepted);
    const auto event = connection.next_event();
    EXPECT_TRUE(event && event->protocol_error());
    EXPECT_EQ(event->protocol_error()->close_code(), std::uint16_t{1009});
    http_io_buffer input;
    input.append(header_value);
    websocket_connection protocol(input);
    const auto borrowed_event = protocol.next_event();
    EXPECT_TRUE(borrowed_event && borrowed_event->protocol_error());
    EXPECT_EQ(borrowed_event->protocol_error()->close_code(), std::uint16_t{1009});
}

TEST(Ruvia, ws_owned_input_bound_excludes_parsed_prefix) {
    websocket_connection connection({.max_buffered_input_bytes_ = 32});
    const std::string ping("\x89\x81\x00\x00\x00\x00p", 7);
    std::string binary("\x82\x94\x00\x00\x00\x00", 6);
    binary.append(20, 'b');
    const auto first_chunk = ping + binary.substr(0, 10);
    EXPECT_TRUE(connection.feed(first_chunk) == websocket_feed_status::accepted);
    const auto ping_event = connection.next_event();
    EXPECT_TRUE(ping_event && ping_event->ping());
    (void)connection.consume_output(connection.output_plan().bytes().size());
    EXPECT_TRUE(!connection.next_event());
    // 16 pending + 10 buffered frame bytes fit the 32-byte bound once the
    // already-delivered Ping no longer occupies the buffer.
    EXPECT_TRUE(connection.feed(std::string_view(binary).substr(10)) == websocket_feed_status::accepted);
    const auto message_event = connection.next_event();
    EXPECT_TRUE(message_event && message_event->message());
    if (message_event && message_event->message()) {
        EXPECT_EQ(message_event->message()->payload(), std::string(20, 'b'));
    }
}

TEST(Ruvia, ws_owned_input_accepts_message_payload_during_compaction) {
    for (const bool following_frame : {false, true}) {
        const std::string nested_frame("\x82\x01x", 3);
        std::string wire("\x82\x03", 2);
        wire += nested_frame;
        if (following_frame) {
            wire.append(
                "\x82\x08"
                "abcdefgh",
                10);
        }
        mask_source mask;
        websocket_connection connection({.role_ = websocket_connection_role::client,
            .mask_key_generator_ = &mask_source::generate,
            .mask_key_context_ = &mask,
            .max_buffered_input_bytes_ = wire.size()});
        EXPECT_TRUE(connection.feed(wire) == websocket_feed_status::accepted);
        const auto wrapper = connection.next_event();
        EXPECT_TRUE(wrapper && wrapper->message());
        if (!wrapper || !wrapper->message()) {
            return;
        }
        EXPECT_EQ(wrapper->message()->payload(), nested_frame);

        // Feed bytes borrowed from the completed message while any following
        // frame is still buffered. The input bound requires reclaiming the
        // consumed wrapper before accepting the nested frame.
        EXPECT_TRUE(connection.feed(wrapper->message()->payload()) == websocket_feed_status::accepted);
        if (following_frame) {
            const auto following = connection.next_event();
            EXPECT_TRUE(following && following->message());
            if (!following || !following->message()) {
                return;
            }
            EXPECT_EQ(following->message()->payload(), "abcdefgh");
        }
        const auto nested = connection.next_event();
        EXPECT_TRUE(nested && nested->message());
        if (nested && nested->message()) {
            EXPECT_EQ(nested->message()->payload(), "x");
        }
        EXPECT_TRUE(!connection.next_event());
        EXPECT_TRUE(connection.liveness_mode() == websocket_liveness_mode::open);
    }
}

TEST(Ruvia, ws_input_accepts_borrowed_message_payload_during_compaction) {
    for (const bool following_frame : {false, true}) {
        const std::string nested_frame("\x82\x01x", 3);
        std::string wire("\x82\x03", 2);
        wire += nested_frame;
        if (following_frame) {
            wire.append(
                "\x82\x08"
                "abcdefgh",
                10);
        }
        mask_source mask;
        http_io_buffer input;
        websocket_connection connection(input,
            {.role_ = websocket_connection_role::client,
                .mask_key_generator_ = &mask_source::generate,
                .mask_key_context_ = &mask});
        input.append(wire);
        const auto wrapper = connection.next_event();
        EXPECT_TRUE(wrapper && wrapper->message());
        if (!wrapper || !wrapper->message()) {
            return;
        }
        EXPECT_EQ(wrapper->message()->payload(), nested_frame);

        // Append bytes borrowed from the completed message while any following
        // frame is still buffered. Making room must copy them before the
        // consumed wrapper is reclaimed.
        input.append(wrapper->message()->payload());
        if (following_frame) {
            const auto following = connection.next_event();
            EXPECT_TRUE(following && following->message());
            if (!following || !following->message()) {
                return;
            }
            EXPECT_EQ(following->message()->payload(), "abcdefgh");
        }
        const auto nested = connection.next_event();
        EXPECT_TRUE(nested && nested->message());
        if (nested && nested->message()) {
            EXPECT_EQ(nested->message()->payload(), "x");
        }
        EXPECT_TRUE(!connection.next_event());
        EXPECT_TRUE(connection.liveness_mode() == websocket_liveness_mode::open);
    }
}

TEST(Ruvia, ws_public_masking_round_trips_over_odd_lengths_and_offsets) {
    mask_source mask;
    http_io_buffer sender_input;
    auto sender = client(sender_input, mask, protocol_byte_limit::unlimited());
    http_io_buffer receiver_input;
    websocket_connection receiver(receiver_input, {.message_limit_ = protocol_byte_limit::unlimited()});
    for (const std::size_t length : {std::size_t{0}, std::size_t{1}, std::size_t{2}, std::size_t{3}, std::size_t{4},
             std::size_t{5}, std::size_t{7}, std::size_t{8}, std::size_t{9}, std::size_t{15}, std::size_t{16},
             std::size_t{17}, std::size_t{31}, std::size_t{33}, std::size_t{125}, std::size_t{126},
             std::size_t{127}, std::size_t{1021}, std::size_t{65535}, std::size_t{65537}}) {
        std::string payload_value(length, '\0');
        for (std::size_t i = 0; i < length; ++i) {
            payload_value[i] = static_cast<char>((i * 31U + length) & 0xFFU);
        }
        // A leading frame of `offset` bytes moves where the payload lands in
        // the receiver's input, so every alignment is unmasked in place.
        for (std::size_t offset = 0; offset < 8; ++offset) {
            const std::string leading(offset, 'L');
            EXPECT_TRUE(sender.submit_frame(websocket_opcode::binary, leading) == websocket_frame_submit_status::accepted);
            const auto leading_wire = drain(sender);
            EXPECT_TRUE(sender.submit_frame(websocket_opcode::binary, payload_value) == websocket_frame_submit_status::accepted);
            const auto wire = drain(sender);

            // Check the wire against RFC 6455 section 5.3 directly: a mask that
            // is only self-inverse would still round-trip.
            const std::size_t header_size = 2 + (length <= 125 ? 0 : (length <= 0xFFFF ? 2 : 8));
            EXPECT_EQ(wire.size(), header_size + 4 + length);
            if (wire.size() != header_size + 4 + length) {
                return;
            }
            const auto key = std::string_view(wire).substr(header_size, 4);
            bool masked_on_wire = true;
            for (std::size_t i = 0; i < length; ++i) {
                masked_on_wire = masked_on_wire && wire[header_size + 4 + i] == static_cast<char>(payload_value[i] ^ key[i % 4]);
            }
            EXPECT_TRUE(masked_on_wire);

            const auto frames = leading_wire + wire;
            receiver_input.append(frames);
            const auto leading_event = receiver.next_event();
            EXPECT_TRUE(leading_event && leading_event->message());
            if (leading_event && leading_event->message()) {
                EXPECT_EQ(leading_event->message()->payload(), leading);
            }
            const auto event = receiver.next_event();
            EXPECT_TRUE(event && event->message());
            if (event && event->message()) {
                EXPECT_TRUE(event->message()->payload() == payload_value);
            }
        }
    }
}

TEST(Ruvia, ws_server_borrowed_frame_follows_pending_output_until_drained) {
    http_io_buffer input;
    websocket_connection protocol(input, {.message_limit_ = protocol_byte_limit::unlimited()});
    const std::string payload_value(300, 'b');
    EXPECT_TRUE(protocol.submit_frame(websocket_opcode::binary, websocket_borrowed_payload(payload_value)) == websocket_frame_submit_status::accepted);
    auto plan = protocol.output_plan();
    EXPECT_EQ(plan.bytes().size(), std::size_t{4});
    EXPECT_TRUE(plan.borrowed_bytes().data() == payload_value.data());
    EXPECT_EQ(plan.size(), std::size_t{304});

    // A partial write may end inside the borrowed payload.
    EXPECT_TRUE(protocol.consume_output(104) == websocket_output_consume_status::pending);
    plan = protocol.output_plan();
    EXPECT_TRUE(plan.bytes().empty());
    EXPECT_TRUE(plan.borrowed_bytes().data() == payload_value.data() + 100);
    EXPECT_EQ(plan.borrowed_bytes().size(), std::size_t{200});
    EXPECT_TRUE(protocol.consume_output(201) == websocket_output_consume_status::out_of_range);
    EXPECT_TRUE(protocol.consume_output(200) == websocket_output_consume_status::drained);
    EXPECT_EQ(protocol.output_plan().size(), std::size_t{0});

    // Output queued behind a borrowed payload copies it first, so the wire
    // keeps submission order and no longer depends on the caller's bytes.
    std::string message_value("hello");
    EXPECT_TRUE(protocol.submit_frame(websocket_opcode::text, websocket_borrowed_payload(message_value)) == websocket_frame_submit_status::accepted);
    mask_source mask;
    http_io_buffer peer_input;
    auto peer = client(peer_input, mask);
    EXPECT_TRUE(peer.submit_frame(websocket_opcode::ping, "p") == websocket_frame_submit_status::accepted);
    input.append(drain(peer));
    const auto ping = protocol.next_event();
    EXPECT_TRUE(ping && ping->ping());
    message_value[0] = 'X';
    plan = protocol.output_plan();
    EXPECT_TRUE(plan.borrowed_bytes().empty());
    const std::string wire(plan.bytes());
    EXPECT_TRUE(protocol.consume_output(wire.size()) == websocket_output_consume_status::drained);
    peer_input.append(wire);
    const auto message = peer.next_event();
    EXPECT_TRUE(message && message->message());
    if (message && message->message()) {
        EXPECT_EQ(message->message()->payload(), "hello");
    }
    const auto pong = peer.next_event();
    EXPECT_TRUE(pong && pong->pong());
    if (pong && pong->pong()) {
        EXPECT_EQ(pong->pong()->payload(), "p");
    }
}

TEST(Ruvia, ws_server_borrowed_frame_owns_compressed_output) {
    http_io_buffer input;
    websocket_connection protocol(input, {.message_limit_ = protocol_byte_limit::unlimited(),
                                             .compression_ = websocket_compression{.enabled_ = true}});
    const std::string payload_value(4096, 'x');
    EXPECT_TRUE(protocol.submit_frame(websocket_opcode::binary, websocket_borrowed_payload(payload_value)) == websocket_frame_submit_status::accepted);
    const auto plan = protocol.output_plan();
    EXPECT_TRUE(plan.borrowed_bytes().empty());
    EXPECT_TRUE(!plan.bytes().empty() && (static_cast<unsigned char>(plan.bytes()[0]) & 0x40U) != 0);
    EXPECT_TRUE(protocol.submit_frame(websocket_opcode::binary, websocket_borrowed_payload(payload_value), {.compress_ = false}) == websocket_frame_submit_status::accepted);
    EXPECT_TRUE(protocol.output_plan().borrowed_bytes().data() == payload_value.data());
}

TEST(Ruvia, ws_public_eight_bit_receive_window_accepts_zlib_nine_bit_deflater_streams) {
    // zlib has no 8-bit raw deflate window, so a zlib peer that negotiated
    // window 8 compresses with window 9. Its deflater still never refers back
    // more than 250 bytes, which the 256-byte inflate window covers. The
    // inflater consults that window only for history produced by earlier
    // output passes, so the incompressible prefix ends just past the 8 KiB
    // inflate pass and the 300-byte repeat refers back across that boundary.
    std::string message(8192 + 10, '\0');
    std::uint64_t random = 0x9e3779b97f4a7c15ULL;
    for (auto& byte : message) {
        random ^= random << 13;
        random ^= random >> 7;
        random ^= random << 17;
        byte = static_cast<char>(random);
    }
    message += message.substr(message.size() - 300, 300);
    z_stream stream{};
    EXPECT_TRUE(deflateInit2(&stream, 6, Z_DEFLATED, -9, 8, Z_DEFAULT_STRATEGY) == Z_OK);
    std::string compressed(message.size() + 256, '\0');
    stream.next_in = reinterpret_cast<Bytef*>(message.data());
    stream.avail_in = static_cast<uInt>(message.size());
    stream.next_out = reinterpret_cast<Bytef*>(compressed.data());
    stream.avail_out = static_cast<uInt>(compressed.size());
    EXPECT_TRUE(deflate(&stream, Z_SYNC_FLUSH) == Z_OK);
    compressed.resize(compressed.size() - stream.avail_out);
    deflateEnd(&stream);
    EXPECT_TRUE(compressed.size() >= 4 && compressed.size() < 65536);
    compressed.resize(compressed.size() - 4);

    for (const bool no_takeover : {false, true}) {
        mask_source mask;
        http_io_buffer input;
        websocket_connection receiver(input, {.compression_ = (websocket_compression{.enabled_ = true, .server_no_context_takeover_ = no_takeover, .server_max_window_bits_ = 8}), .role_ = websocket_connection_role::client, .mask_key_generator_ = &mask_source::generate, .mask_key_context_ = &mask});
        std::string wire("\xc2\x7e", 2);
        wire.push_back(static_cast<char>(compressed.size() >> 8));
        wire.push_back(static_cast<char>(compressed.size() & 0xff));
        wire += compressed;
        input.append(wire);
        const auto event = receiver.next_event();
        EXPECT_TRUE(event && event->message() && event->message()->payload() == message);
    }
}

TEST(Ruvia, ws_public_receiver_accepts_non_minimal_payload_length_forms) {
    // RFC 6455 section 5.2 makes the minimal length form a sender obligation;
    // the longer forms still delimit the frame exactly, so a receiver reads
    // them instead of failing the connection.
    const std::string sixteen_bit("\x81\xfe\x00\x05\x00\x00\x00\x00hello", 13);
    const std::string sixty_four_bit("\x81\xff\x00\x00\x00\x00\x00\x00\x00\x05\x00\x00\x00\x00hello", 19);
    const std::string control_sixteen_bit("\x89\xfe\x00\x02\x00\x00\x00\x00hi", 10);
    for (const auto& wire : {sixteen_bit, sixty_four_bit}) {
        http_io_buffer input;
        websocket_connection server(input);
        input.append(wire);
        const auto event = server.next_event();
        EXPECT_TRUE(event && event->message() && event->message()->payload() == "hello");
    }
    http_io_buffer input;
    websocket_connection server(input);
    input.append(control_sixteen_bit);
    const auto event = server.next_event();
    EXPECT_TRUE(event && event->ping() && event->ping()->payload() == "hi");
}

TEST(Ruvia, ws_public_feed_reports_backpressure_when_initial_buffer_exceeds_limit) {
    http_io_buffer input;
    input.append("123456789");
    websocket_connection connection(input, {.max_buffered_input_bytes_ = 8});
    EXPECT_TRUE(connection.feed("x") == websocket_feed_status::backpressured);
    EXPECT_EQ(input.size(), std::size_t{9});
}

TEST(Ruvia, ws_receive_ceiling_is_early_directional_and_only_tightens) {
    for (const std::size_t receive_limit : {std::size_t{256}, std::size_t{2048}}) {
        const auto effective = (std::min)(receive_limit, std::size_t{1024});
        websocket_connection receiver({.message_limit_ = protocol_byte_limit::limited(1024),
            .receive_message_limit_ = protocol_byte_limit::limited(receive_limit)});
        EXPECT_EQ(receiver.submit_frame(websocket_opcode::binary, std::string(1024, 'r'), {.compress_ = false}),
            websocket_frame_submit_status::accepted);
        (void)drain(receiver);
        EXPECT_EQ(receiver.submit_frame(websocket_opcode::binary, std::string(1025, 'r'), {.compress_ = false}),
            websocket_frame_submit_status::message_too_large);
        // 仅发送帧头，无正文：在分配或等待完整消息前拒绝声明的超限长度。
        std::string header("\x82\xfe", 2);
        header.push_back(static_cast<char>((effective + 1) >> 8));
        header.push_back(static_cast<char>((effective + 1) & 0xff));
        header.append(4, '\0');
        EXPECT_EQ(receiver.feed(header), websocket_feed_status::accepted);
        const auto failure = receiver.next_event();
        ASSERT_TRUE(failure && failure->protocol_error());
        EXPECT_EQ(failure->protocol_error()->close_code(), 1009);
    }
    // 各分片单独合法，累计消息长度仍受同一个接收上限约束。
    websocket_connection receiver({.receive_message_limit_ = protocol_byte_limit::limited(4)});
    const std::string first("\x02\x83\0\0\0\0abc", 9);
    const std::string last("\x80\x82\0\0\0\0de", 8);
    EXPECT_EQ(receiver.feed(first), websocket_feed_status::accepted);
    EXPECT_FALSE(receiver.next_event());
    EXPECT_EQ(receiver.feed(last), websocket_feed_status::accepted);
    const auto failure = receiver.next_event();
    ASSERT_TRUE(failure && failure->protocol_error());
    EXPECT_EQ(failure->protocol_error()->close_code(), 1009);
}

TEST(Ruvia, ws_fragmented_deflate_enforces_uncompressed_message_limit) {
    {
        websocket_connection sender({.message_limit_ = protocol_byte_limit::limited(1000),
            .compression_ = websocket_compression{.enabled_ = true}});
        const std::string first_chunk(600, 'a');
        const std::string second_chunk(500, 'a');
        EXPECT_EQ(sender.submit_frame(websocket_opcode::binary, first_chunk, {.final_ = false, .compress_ = true}),
            websocket_frame_submit_status::accepted);
        EXPECT_EQ(sender.submit_frame(websocket_opcode::binary, second_chunk, {.final_ = true, .compress_ = true}),
            websocket_frame_submit_status::message_too_large);
    }
    {
        websocket_connection sender({.message_limit_ = protocol_byte_limit::limited(64),
            .compression_ = websocket_compression{.enabled_ = true}});
        std::string incompressible(64, '\0');
        std::uint64_t rng = 0x517cc1b727220a95ULL;
        for (char& ch : incompressible) {
            rng ^= rng << 13U;
            rng ^= rng >> 7U;
            rng ^= rng << 17U;
            ch = static_cast<char>(rng & 0xffU);
        }
        const std::string_view view(incompressible);
        EXPECT_EQ(sender.submit_frame(websocket_opcode::binary, view.substr(0, 32), {.final_ = false, .compress_ = true}),
            websocket_frame_submit_status::accepted);
        EXPECT_EQ(sender.submit_frame(websocket_opcode::binary, view.substr(32, 32), {.final_ = true, .compress_ = true}),
            websocket_frame_submit_status::accepted);
    }
}
