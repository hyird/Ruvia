#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ruvia/http/http1_client_request_writer.h"
#include "ruvia/http/http1_client_response_parser.h"
#include "ruvia/http/http_limits.h"
#include "ruvia/http/http_request_target.h"

#include "test_harness.h"

namespace {

using ruvia::http1_client_request_prepare_error;
using ruvia::http1_client_request_wire_policy;
using ruvia::http1_client_request_writer;
using ruvia::http1_close_policy;
using ruvia::http_client_request_content_view;
using ruvia::http_client_request_view;
using ruvia::http_origin_view;
using ruvia::is_valid_http_origin_form_target;

class rejecting_character_storage_resource final : public std::pmr::memory_resource {
private:
    void* do_allocate(std::size_t bytes_value, std::size_t alignment) override {
        // MSVC's debug STL allocates an iterator proxy from the container's
        // resource before the string buffer. Let that bookkeeping through so
        // the injected failure lands on the Upgrade character storage itself.
        if (alignment == alignof(char)) {
            throw std::bad_alloc();
        }
        return std::pmr::new_delete_resource()->allocate(bytes_value, alignment);
    }
    void do_deallocate(void* value, std::size_t bytes_value, std::size_t alignment) override {
        std::pmr::new_delete_resource()->deallocate(value, bytes_value, alignment);
    }
    [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
};

RUVIA_TEST(http_client_origin_target_validation) {
    RUVIA_CHECK(is_valid_http_origin_form_target("/ok%2F?q=%7B%7D"));
    RUVIA_CHECK(!is_valid_http_origin_form_target("*"));
    RUVIA_CHECK(!is_valid_http_origin_form_target(""));
    RUVIA_CHECK(!is_valid_http_origin_form_target("relative"));
    RUVIA_CHECK(!is_valid_http_origin_form_target("/bad#fragment"));
    RUVIA_CHECK(!is_valid_http_origin_form_target("/bad\\path"));
    RUVIA_CHECK(!is_valid_http_origin_form_target("/bad%zz"));
    RUVIA_CHECK(!is_valid_http_origin_form_target("/bad%"));
    RUVIA_CHECK(!is_valid_http_origin_form_target("/bad%2"));
}

template <std::size_t n = 2048>
struct prepared_fixture final {
    std::array<char, n> buffer_{};
    ruvia::http1_client_request_prepare_result result_;

    prepared_fixture(const http_origin_view& origin, const http_client_request_view& request,
        http1_client_request_wire_policy policy = http1_client_request_wire_policy{})
        : result_(http1_client_request_writer().prepare(origin, request, buffer_, policy)) {}
};

[[nodiscard]] http1_client_request_prepare_error prepare_error(const http_client_request_view& request,
    http1_client_request_wire_policy policy = http1_client_request_wire_policy{}) {
    std::array<char, 512> buffer;
    const auto result_value = http1_client_request_writer().prepare(
        http_origin_view::https({.host_ = "example.test"}), request, buffer, policy);
    const auto* failure = result_value.failure();
    if (failure == nullptr) {
        throw std::runtime_error("test expected request preparation to fail");
    }
    return failure->error();
}

}  // namespace

RUVIA_TEST(http1_client_request_writer_emits_one_canonical_scatter_gather_plan) {
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
    const auto* prepared = fixture_value.result_.prepared();
    RUVIA_CHECK(prepared != nullptr);
    if (prepared == nullptr) {
        return;
    }
    RUVIA_CHECK(prepared->head() ==
                "POST /items?q=1 HTTP/1.1\r\n"
                "Host: example.test\r\n"
                "X-Test: one\r\n"
                "Content-Length: 7\r\n"
                "Expect: 100-continue\r\n\r\n");
    const auto* gated = prepared->content_plan().continue_gated();
    RUVIA_CHECK(gated != nullptr);
    RUVIA_CHECK(prepared->content_plan().without_content() == nullptr);
    RUVIA_CHECK(prepared->content_plan().immediate() == nullptr);
    if (gated != nullptr) {
        RUVIA_CHECK(gated->bytes() == "payload");
    }
}

RUVIA_TEST(http1_client_request_content_distinguishes_absent_from_explicit_empty) {
    http_client_request_view absent;
    absent.method_ = "GET";
    RUVIA_CHECK(absent.content_.without_content() != nullptr);
    RUVIA_CHECK(absent.content_.borrowed_bytes() == nullptr);
    prepared_fixture absent_fixture(http_origin_view::https({.host_ = "example.test"}), absent);
    const auto* absent_prepared = absent_fixture.result_.prepared();
    RUVIA_CHECK(absent_prepared != nullptr);
    if (absent_prepared != nullptr) {
        RUVIA_CHECK(absent_prepared->content_plan().without_content() != nullptr);
        RUVIA_CHECK(absent_prepared->content_plan().immediate() == nullptr);
        RUVIA_CHECK(absent_prepared->content_plan().continue_gated() == nullptr);
        RUVIA_CHECK(!(absent_prepared->head().find("Content-Length") != std::string_view::npos));
    }

    http_client_request_view empty;
    empty.method_ = "POST";
    empty.content_ = http_client_request_content_view::bytes("");
    RUVIA_CHECK(empty.content_.without_content() == nullptr);
    RUVIA_CHECK(empty.content_.borrowed_bytes() != nullptr);
    if (const auto* bytes = empty.content_.borrowed_bytes()) {
        RUVIA_CHECK(bytes->value().empty());
    }
    prepared_fixture empty_fixture(http_origin_view::https({.host_ = "example.test"}), empty);
    const auto* empty_prepared = empty_fixture.result_.prepared();
    RUVIA_CHECK(empty_prepared != nullptr);
    if (empty_prepared != nullptr) {
        const auto* immediate = empty_prepared->content_plan().immediate();
        RUVIA_CHECK(immediate != nullptr);
        RUVIA_CHECK(empty_prepared->content_plan().without_content() == nullptr);
        RUVIA_CHECK(empty_prepared->content_plan().continue_gated() == nullptr);
        if (immediate != nullptr) {
            RUVIA_CHECK(immediate->bytes().empty());
        }
        RUVIA_CHECK((empty_prepared->head().find("Content-Length: 0\r\n") != std::string_view::npos));
    }
}

RUVIA_TEST(http1_client_request_writer_owns_request_target_forms_and_host) {
    http_client_request_view extension;
    extension.method_ = "PROPFIND";
    extension.target_ = "/dav";
    prepared_fixture extension_fixture(
        http_origin_view::https({.host_ = "example.test", .port_ = 8443}), extension);
    const auto* extension_prepared = extension_fixture.result_.prepared();
    RUVIA_CHECK(extension_prepared != nullptr);
    if (extension_prepared != nullptr) {
        RUVIA_CHECK(extension_prepared->head().starts_with(
            "PROPFIND /dav HTTP/1.1\r\nHost: example.test:8443\r\n"));
    }

    http_client_request_view options;
    options.method_ = "OPTIONS";
    options.target_ = "*";
    prepared_fixture options_fixture(http_origin_view::http({.host_ = "example.test"}), options);
    RUVIA_CHECK(options_fixture.result_.prepared() != nullptr);

    http_client_request_view invalid_asterisk;
    invalid_asterisk.method_ = "GET";
    invalid_asterisk.target_ = "*";
    RUVIA_CHECK(prepare_error(invalid_asterisk) == http1_client_request_prepare_error::invalid_target);

    http_client_request_view absolute;
    absolute.target_ = "https://example.test/path";
    RUVIA_CHECK(prepare_error(absolute) == http1_client_request_prepare_error::invalid_target);

    http_client_request_view connect;
    connect.method_ = "CONNECT";
    connect.target_ = "example.test:443";
    RUVIA_CHECK(
        prepare_error(connect) == http1_client_request_prepare_error::connect_requires_dedicated_entry);
}

RUVIA_TEST(http1_client_connect_entry_generates_authority_form_atomically) {
    std::array<char, 512> buffer;
    const auto result_value = http1_client_request_writer().prepare_connect(
        http_origin_view::https({.host_ = "example.test"}), {}, buffer);
    const auto* prepared = result_value.prepared();
    RUVIA_CHECK(prepared != nullptr);
    if (prepared != nullptr) {
        RUVIA_CHECK(prepared->head() ==
                    "CONNECT example.test:443 HTTP/1.1\r\n"
                    "Host: example.test:443\r\n\r\n");
        RUVIA_CHECK(prepared->content_plan().without_content() != nullptr);
    }

    std::array<char, 512> ipv6_buffer;
    const auto ipv6 = http1_client_request_writer().prepare_connect(
        http_origin_view::https({.host_ = "[::1]"}), {}, ipv6_buffer);
    RUVIA_CHECK(ipv6.prepared() != nullptr);
    if (ipv6.prepared() != nullptr) {
        RUVIA_CHECK(
            ipv6.prepared()->head().starts_with("CONNECT [::1]:443 HTTP/1.1\r\nHost: [::1]:443\r\n"));
    }

    std::array<char, 128> invalid_buffer;
    const auto invalid = http1_client_request_writer().prepare_connect(
        http_origin_view::https({.host_ = "example.test", .port_ = 0}), {}, invalid_buffer);
    RUVIA_CHECK(invalid.failure() != nullptr);
    RUVIA_CHECK(
        invalid.failure()->error() == http1_client_request_prepare_error::invalid_connect_origin);
}

RUVIA_TEST(http1_client_request_writer_is_the_only_host_and_framing_owner) {
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
        {"Trailer", "Digest", http1_client_request_prepare_error::trailer_section_unsupported},
        {"Expect", "100-continue", http1_client_request_prepare_error::expect_header_managed_by_writer},
    };
    for (const auto& test : cases) {
        const ruvia::http_header_view header_value(test.name_, test.value_);
        http_client_request_view request;
        request.method_ = "POST";
        request.headers_ = std::span<const ruvia::http_header_view>(&header_value, 1);
        request.content_ = http_client_request_content_view::bytes("payload");
        RUVIA_CHECK(prepare_error(request) == test.error_);
    }
}

RUVIA_TEST(http1_client_request_writer_rejects_repeated_singleton_fields) {
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

        RUVIA_CHECK(prepare_error(request) == http1_client_request_prepare_error::invalid_header);
    }
}

RUVIA_TEST(http1_client_request_writer_validates_cors_fields) {
    const ruvia::http_header_view invalid_headers[] = {
        {"Origin", "https://example.test/path"},
        {"Access-Control-Request-Method", "GET, POST"},
        {"Access-Control-Request-Headers", "X-Good, Bad Header"},
    };
    for (const auto& header : invalid_headers) {
        http_client_request_view request;
        request.headers_ = std::span<const ruvia::http_header_view>(&header, 1);
        RUVIA_CHECK(prepare_error(request) == http1_client_request_prepare_error::invalid_header);
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
    RUVIA_CHECK(fixture_value.result_.prepared() != nullptr);
}

RUVIA_TEST(http1_client_request_writer_owns_hop_by_hop_field_contracts) {
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
        RUVIA_CHECK(prepare_error(request) == test.error_);
    }

    for (const std::string_view connection_options :
        {"Host", "close, content-length", "EXPECT, keep-alive", "Cache-Control", "Trailer",
            "Authorization", "Cookie", "Range"}) {
        const ruvia::http_header_view connection("Connection", connection_options);
        http_client_request_view request;
        request.method_ = "POST";
        request.headers_ = std::span<const ruvia::http_header_view>(&connection, 1);
        request.content_ = http_client_request_content_view::bytes("payload");
        RUVIA_CHECK(prepare_error(request) == http1_client_request_prepare_error::invalid_connection);
    }

    const ruvia::http_header_view valid_headers[] = {
        {"Connection", "keep-alive"},
        {"Connection", "Upgrade, TE, X-Hop"},
        {"Upgrade", "custom/1, websocket"},
        {"TE", "trailers"},
        {"X-Hop", "value"},
    };
    http_client_request_view valid;
    valid.headers_ = valid_headers;
    prepared_fixture fixture_value(http_origin_view::https({.host_ = "example.test"}), valid);
    RUVIA_CHECK(fixture_value.result_.prepared() != nullptr);
}

RUVIA_TEST(http1_client_request_writer_validates_te_capabilities_and_weights) {
    const auto prepare_with_te =
        [&ruvia_ctx](std::string_view value) -> std::optional<http1_client_request_prepare_error> {
        const std::array headers{
            ruvia::http_header_view("Connection", "TE"),
            ruvia::http_header_view("TE", value),
        };
        http_client_request_view request;
        request.headers_ = headers;
        std::array<char, 512> buffer;
        const auto result_value = http1_client_request_writer().prepare(
            http_origin_view::https({.host_ = "example.test"}), request, buffer);
        if (const auto* failure = result_value.failure()) {
            return failure->error();
        }
        RUVIA_CHECK(result_value.prepared() != nullptr);
        return std::nullopt;
    };

    for (const std::string_view valid : {"", "trailers", "gzip", "deflate;q=0.5", "deflate;Q=0.5",
             "x-gzip ; q=1.000", "gzip;q=0, trailers", "trailers , TRAILERS"}) {
        RUVIA_CHECK(!prepare_with_te(valid).has_value());
    }

    for (const std::string_view invalid : {",trailers", "trailers,", "trailers,,gzip", "chunked",
             "br", "trailers;q=0.5", "gzip;q=1.001", "gzip;q=\"0.5\"", "gzip;q =0.5", "gzip;q= 0.5",
             "gzip; q = 0.5", "gzip;level=1", "gzip;q=0.5;level=1", "gzip; q", "gzip;q="}) {
        RUVIA_CHECK(prepare_with_te(invalid) == http1_client_request_prepare_error::invalid_header);
    }
}

RUVIA_TEST(http1_client_request_writer_enforces_expect_content_semantics) {
    const ruvia::http_header_view expect("Expect", "100-Continue");
    http_client_request_view raw_expectation;
    raw_expectation.method_ = "POST";
    raw_expectation.headers_ = std::span<const ruvia::http_header_view>(&expect, 1);
    raw_expectation.content_ = http_client_request_content_view::bytes("x");
    RUVIA_CHECK(prepare_error(raw_expectation) ==
                http1_client_request_prepare_error::expect_header_managed_by_writer);

    http_client_request_view absent;
    absent.method_ = "POST";
    prepared_fixture absent_fixture(http_origin_view::https({.host_ = "example.test"}), absent,
        http1_client_request_wire_policy{
            .expectation_ = ruvia::http_client_request_expectation::continue_value});
    RUVIA_CHECK(absent_fixture.result_.failure() != nullptr);
    if (absent_fixture.result_.failure() != nullptr) {
        RUVIA_CHECK(absent_fixture.result_.failure()->error() ==
                    http1_client_request_prepare_error::expectation_without_content);
    }

    http_client_request_view empty = absent;
    empty.content_ = http_client_request_content_view::bytes("");
    prepared_fixture empty_fixture(http_origin_view::https({.host_ = "example.test"}), empty,
        http1_client_request_wire_policy{
            .expectation_ = ruvia::http_client_request_expectation::continue_value});
    RUVIA_CHECK(empty_fixture.result_.failure() != nullptr);
    if (empty_fixture.result_.failure() != nullptr) {
        RUVIA_CHECK(empty_fixture.result_.failure()->error() ==
                    http1_client_request_prepare_error::expectation_without_content);
    }

    http_client_request_view nonempty;
    nonempty.method_ = "POST";
    nonempty.content_ = http_client_request_content_view::bytes("x");
    prepared_fixture fixture_value(http_origin_view::https({.host_ = "example.test"}), nonempty,
        http1_client_request_wire_policy{
            .expectation_ = ruvia::http_client_request_expectation::continue_value});
    RUVIA_CHECK(fixture_value.result_.prepared() != nullptr);
    if (fixture_value.result_.prepared() != nullptr) {
        RUVIA_CHECK(fixture_value.result_.prepared()->content_plan().continue_gated() != nullptr);
        RUVIA_CHECK(fixture_value.result_.prepared()->head().find("Expect: 100-continue\r\n") !=
                    std::string_view::npos);
    }
}

RUVIA_TEST(http1_client_request_writer_rejects_invalid_wire_policy) {
    http_client_request_view request;
    const auto invalid_without_expectation =
        http1_client_request_wire_policy{.close_policy_ = static_cast<http1_close_policy>(0xFF)};
    const auto invalid_expect_continue =
        http1_client_request_wire_policy{.close_policy_ = static_cast<http1_close_policy>(0xFF),
            .expectation_ = ruvia::http_client_request_expectation::continue_value};
    const auto invalid_expectation =
        http1_client_request_wire_policy{
            .expectation_ = static_cast<ruvia::http_client_request_expectation>(0xFF)};
    RUVIA_CHECK(prepare_error(request, invalid_without_expectation) ==
                http1_client_request_prepare_error::invalid_close_policy);
    RUVIA_CHECK(prepare_error(request, invalid_expect_continue) ==
                http1_client_request_prepare_error::invalid_close_policy);
    RUVIA_CHECK(prepare_error(request, invalid_expectation) ==
                http1_client_request_prepare_error::invalid_expectation);

    std::array<char, 512> buffer{};
    const auto connect =
        http1_client_request_writer().prepare_connect(http_origin_view::https({.host_ = "example.test"}),
            std::span<const ruvia::http_header_view>{}, buffer, invalid_without_expectation);
    RUVIA_CHECK(connect.failure() != nullptr);
    if (connect.failure() != nullptr) {
        RUVIA_CHECK(
            connect.failure()->error() == http1_client_request_prepare_error::invalid_close_policy);
    }

    const auto invalid_connect_expectation =
        http1_client_request_writer().prepare_connect(http_origin_view::https({.host_ = "example.test"}),
            std::span<const ruvia::http_header_view>{}, buffer, invalid_expectation);
    RUVIA_CHECK(invalid_connect_expectation.failure() != nullptr);
    if (invalid_connect_expectation.failure() != nullptr) {
        RUVIA_CHECK(invalid_connect_expectation.failure()->error() ==
                    http1_client_request_prepare_error::invalid_expectation);
    }
}

RUVIA_TEST(http1_client_request_writer_enforces_method_content_semantics) {
    http_client_request_view trace;
    trace.method_ = "TRACE";
    trace.content_ = http_client_request_content_view::bytes("trace body");
    RUVIA_CHECK(prepare_error(trace) == http1_client_request_prepare_error::content_forbidden_for_method);

    http_client_request_view options;
    options.method_ = "OPTIONS";
    options.content_ = http_client_request_content_view::bytes("options body");
    RUVIA_CHECK(
        prepare_error(options) == http1_client_request_prepare_error::options_content_type_required);

    const ruvia::http_header_view content_type_value("Content-Type", "application/json");
    options.headers_ = std::span<const ruvia::http_header_view>(&content_type_value, 1);
    prepared_fixture fixture_value(http_origin_view::https({.host_ = "example.test"}), options);
    RUVIA_CHECK(fixture_value.result_.prepared() != nullptr);
    if (const auto* prepared = fixture_value.result_.prepared()) {
        RUVIA_CHECK(prepared->content_plan().immediate() != nullptr);
        if (const auto* content = prepared->content_plan().immediate()) {
            RUVIA_CHECK_EQ(content->bytes(), std::string_view("options body"));
        }
    }

    const ruvia::http_header_view invalid_content_type("Content-Type", "invalid");
    options.headers_ = std::span<const ruvia::http_header_view>(&invalid_content_type, 1);
    RUVIA_CHECK(prepare_error(options) == http1_client_request_prepare_error::invalid_header);
}

RUVIA_TEST(http1_client_request_writer_allows_empty_method_content) {
    for (const char* method : {"OPTIONS", "TRACE"}) {
        http_client_request_view request;
        request.method_ = method;
        request.content_ = http_client_request_content_view::bytes("");
        prepared_fixture fixture(http_origin_view::http({.host_ = "example.test"}), request);
        const auto* prepared = fixture.result_.prepared();
        RUVIA_CHECK(prepared != nullptr);
        if (prepared == nullptr) {
            continue;
        }
        RUVIA_CHECK(prepared->head().find("Content-Length: 0\r\n") != std::string_view::npos);
        const auto* content = prepared->content_plan().immediate();
        RUVIA_CHECK(content != nullptr);
        if (content != nullptr) {
            RUVIA_CHECK(content->bytes().empty());
        }

        std::array<char, 1024> head_buffer;
        ruvia::http1_client_request_head_view streaming;
        streaming.method_ = method;
        streaming.content_length_ = 0;
        const auto result = http1_client_request_writer().prepare_streaming(
            http_origin_view::http({.host_ = "example.test"}), streaming, head_buffer);
        const auto* streaming_prepared = result.prepared();
        RUVIA_CHECK(streaming_prepared != nullptr);
        if (streaming_prepared != nullptr) {
            RUVIA_CHECK(streaming_prepared->head().find("Content-Length: 0\r\n") != std::string_view::npos);
            const auto* streaming_content = streaming_prepared->content_plan().streaming();
            RUVIA_CHECK(streaming_content != nullptr);
            if (streaming_content != nullptr) {
                RUVIA_CHECK_EQ(streaming_content->content_length(), std::optional<std::uint64_t>{0});
            }
        }
    }
}

RUVIA_TEST(http1_client_request_writer_rejects_invalid_content_type_parameters) {
    for (const std::string_view value :
        {"text/plain; charset", "text/plain; charset=", "text/plain; charset =utf-8",
            "text/plain; charset=utf-8; CHARSET=latin1", "text/plain; charset=\"unterminated"}) {
        const ruvia::http_header_view content_type_value("Content-Type", value);
        http_client_request_view request;
        request.method_ = "POST";
        request.headers_ = std::span<const ruvia::http_header_view>(&content_type_value, 1);
        request.content_ = http_client_request_content_view::bytes("body");
        RUVIA_CHECK(prepare_error(request) == http1_client_request_prepare_error::invalid_header);
    }

    const ruvia::http_header_view valid_content_type("Content-Type", "text/plain; charset=\"utf-8\"");
    http_client_request_view valid;
    valid.method_ = "POST";
    valid.headers_ = std::span<const ruvia::http_header_view>(&valid_content_type, 1);
    valid.content_ = http_client_request_content_view::bytes("body");
    prepared_fixture fixture_value(http_origin_view::https({.host_ = "example.test"}), valid);
    RUVIA_CHECK(fixture_value.result_.prepared() != nullptr);
}

RUVIA_TEST(http1_client_request_writer_rejects_invalid_content_encoding_syntax) {
    for (const std::string_view value : {"gzip;level=9", "bad coding", "", ",gzip", "gzip,"}) {
        const ruvia::http_header_view content_encoding("Content-Encoding", value);
        http_client_request_view request;
        request.method_ = "POST";
        request.headers_ = std::span<const ruvia::http_header_view>(&content_encoding, 1);
        request.content_ = http_client_request_content_view::bytes("body");
        RUVIA_CHECK(prepare_error(request) == http1_client_request_prepare_error::invalid_header);
    }

    for (const std::string_view value : {"deflate", "gzip, br"}) {
        const ruvia::http_header_view content_encoding("Content-Encoding", value);
        http_client_request_view request;
        request.method_ = "POST";
        request.headers_ = std::span<const ruvia::http_header_view>(&content_encoding, 1);
        request.content_ = http_client_request_content_view::bytes("body");
        prepared_fixture fixture_value(http_origin_view::https({.host_ = "example.test"}), request);
        RUVIA_CHECK(fixture_value.result_.prepared() != nullptr);
    }
}

RUVIA_TEST(http1_client_request_writer_returns_exact_buffer_requirement_without_partial_output) {
    http_client_request_view request;
    request.method_ = "POST";
    request.target_ = "/upload";
    request.content_ = http_client_request_content_view::bytes("body");

    std::array<char, 8> small;
    small.fill('z');
    const auto too_small = http1_client_request_writer().prepare(
        http_origin_view::https({.host_ = "example.test"}), request, small);
    RUVIA_CHECK(too_small.buffer_too_small() != nullptr);
    RUVIA_CHECK(std::ranges::all_of(small, [](char value) { return value == 'z'; }));

    std::array<char, 512> enough;
    const auto prepared = http1_client_request_writer().prepare(
        http_origin_view::https({.host_ = "example.test"}), request, enough);
    RUVIA_CHECK(prepared.prepared() != nullptr);
    if (prepared.prepared() != nullptr && too_small.buffer_too_small() != nullptr) {
        RUVIA_CHECK_EQ(
            too_small.buffer_too_small()->required_head_bytes(), prepared.prepared()->head().size());
    }
}

RUVIA_TEST(http1_client_request_writer_preserves_metadata_borrowed_from_output) {
    enum class alias_kind { field_name,
        field_value,
        method,
        target,
        host };
    for (const bool streaming : {false, true}) {
        for (const auto kind : {alias_kind::field_name, alias_kind::field_value,
                 alias_kind::method, alias_kind::target, alias_kind::host}) {
            std::array<char, 128> buffer;
            buffer.fill('#');
            const auto store = [&](std::size_t offset, std::string_view text) {
                std::copy(text.begin(), text.end(), buffer.begin() + static_cast<std::ptrdiff_t>(offset));
                return std::string_view(buffer.data() + offset, text.size());
            };
            std::string_view method = "HEAD";
            std::string_view target = "/";
            std::string_view host = "example.test";
            std::array fields{ruvia::http_header_view{"X-Test", "retained"}};
            switch (kind) {
                case alias_kind::field_name:
                    fields[0] = {store(0, "X-Test"), "retained"};
                    break;
                case alias_kind::field_value:
                    fields[0] = {"X-Test", store(0, "retained")};
                    break;
                case alias_kind::method:
                    // The method is read before Host overwrites these bytes;
                    // the exchange must still retain HEAD response semantics.
                    method = store(24, "HEAD");
                    break;
                case alias_kind::target:
                    target = store(0, "/");
                    break;
                case alias_kind::host:
                    host = store(0, "example.test");
                    break;
            }
            const auto original = buffer;
            const auto origin = http_origin_view::http({.host_ = host});
            rejecting_character_storage_resource resource;
            const http1_client_request_writer writer({.resource_ = &resource});
            const auto prepare = [&](std::span<char> output) {
                return streaming
                           ? writer.prepare_streaming(origin,
                                 {.method_ = method, .target_ = target, .headers_ = fields, .content_length_ = 0}, output)
                           : writer.prepare(origin,
                                 {.method_ = method, .target_ = target, .headers_ = fields}, output);
            };
            std::string expected = "HEAD / HTTP/1.1\r\nHost: example.test\r\nX-Test: retained\r\n";
            if (streaming) {
                expected += "Content-Length: 0\r\n";
            }
            expected += "\r\n";
            const auto too_small = prepare(std::span(buffer).first(expected.size() - 1));
            RUVIA_CHECK(too_small.buffer_too_small() != nullptr);
            if (too_small.buffer_too_small() != nullptr) {
                RUVIA_CHECK_EQ(too_small.buffer_too_small()->required_head_bytes(), expected.size());
            }
            RUVIA_CHECK(buffer == original);

            const auto result = prepare(buffer);
            const auto* prepared = result.prepared();
            RUVIA_CHECK(prepared != nullptr);
            if (prepared != nullptr) {
                RUVIA_CHECK_EQ(prepared->head(), std::string_view(expected));
                auto parser = ruvia::http1_client_response_parser(prepared->exchange_state());
                const auto response = parser.parse("HTTP/1.1 200 OK\r\nContent-Length: 7\r\n\r\n");
                RUVIA_CHECK(response.parsed() != nullptr);
                if (response.parsed() != nullptr) {
                    RUVIA_CHECK(response.parsed()->plan().without_content() != nullptr);
                }
            }
            RUVIA_CHECK(std::equal(buffer.begin() + static_cast<std::ptrdiff_t>(expected.size()), buffer.end(),
                original.begin() + static_cast<std::ptrdiff_t>(expected.size())));
        }
    }
}

RUVIA_TEST(http1_client_request_writer_preserves_borrowed_fields_at_head_size_limits) {
    for (const bool streaming : {false, true}) {
        for (const auto head_bytes : {ruvia::max_http_header_bytes - 1, ruvia::max_http_header_bytes,
                 ruvia::max_http_header_bytes + 1}) {
            constexpr std::string_view prefix = "GET / HTTP/1.1\r\nHost: example.test\r\nX-Test: ";
            const std::string_view suffix = streaming ? "\r\nContent-Length: 0\r\n\r\n" : "\r\n\r\n";
            const std::string value(head_bytes - prefix.size() - suffix.size(), 'v');
            const std::string expected = std::string(prefix) + value + std::string(suffix);
            std::array<char, ruvia::max_http_header_bytes + 8> buffer;
            buffer.fill('#');
            constexpr std::string_view field_name = "X-Test";
            std::copy(field_name.begin(), field_name.end(), buffer.begin());
            const auto original = buffer;
            const std::array fields{ruvia::http_header_view{
                std::string_view(buffer.data(), field_name.size()), value}};
            const auto origin = http_origin_view::http({.host_ = "example.test"});
            rejecting_character_storage_resource resource;
            const http1_client_request_writer writer({.resource_ = &resource});
            const auto prepare = [&](std::span<char> output) {
                return streaming
                           ? writer.prepare_streaming(origin, {.headers_ = fields, .content_length_ = 0}, output)
                           : writer.prepare(origin, {.headers_ = fields}, output);
            };
            if (head_bytes > ruvia::max_http_header_bytes) {
                const auto invalid = prepare(buffer);
                RUVIA_CHECK(invalid.failure() != nullptr);
                if (invalid.failure() != nullptr) {
                    RUVIA_CHECK(invalid.failure()->error() == http1_client_request_prepare_error::header_too_large);
                }
                RUVIA_CHECK(buffer == original);
                continue;
            }
            const auto too_small = prepare(std::span(buffer).first(head_bytes - 1));
            RUVIA_CHECK(too_small.buffer_too_small() != nullptr);
            if (too_small.buffer_too_small() != nullptr) {
                RUVIA_CHECK_EQ(too_small.buffer_too_small()->required_head_bytes(), head_bytes);
            }
            RUVIA_CHECK(buffer == original);
            const auto result = prepare(buffer);
            RUVIA_CHECK(result.prepared() != nullptr);
            if (result.prepared() != nullptr) {
                RUVIA_CHECK_EQ(result.prepared()->head(), std::string_view(expected));
            }
            RUVIA_CHECK(std::equal(buffer.begin() + static_cast<std::ptrdiff_t>(head_bytes), buffer.end(),
                original.begin() + static_cast<std::ptrdiff_t>(head_bytes)));
        }
    }
}

RUVIA_TEST(http1_client_request_writer_consumes_header_descriptors_before_overwriting_output) {
    for (const bool streaming : {false, true}) {
        for (const auto offset : {std::size_t{0}, sizeof(ruvia::http_header_view), std::size_t{192}}) {
            alignas(ruvia::http_header_view) std::array<char, 256> buffer;
            buffer.fill('#');
            auto* const field = ::new (static_cast<void*>(buffer.data() + offset)) ruvia::http_header_view{"X-Test", "retained"};
            const auto original = buffer;
            const std::span<const ruvia::http_header_view> fields(field, 1);
            const auto origin = http_origin_view::http({.host_ = "example.test"});
            rejecting_character_storage_resource resource;
            const http1_client_request_writer writer({.resource_ = &resource});
            const auto prepare = [&](std::span<char> output) {
                return streaming
                           ? writer.prepare_streaming(origin, {.headers_ = fields, .content_length_ = 0}, output)
                           : writer.prepare(origin, {.headers_ = fields}, output);
            };
            std::string expected = "GET / HTTP/1.1\r\nHost: example.test\r\nX-Test: retained\r\n";
            if (streaming) {
                expected += "Content-Length: 0\r\n";
            }
            expected += "\r\n";
            const auto too_small = prepare(std::span(buffer).first(expected.size() - 1));
            RUVIA_CHECK(too_small.buffer_too_small() != nullptr);
            RUVIA_CHECK(buffer == original);
            const auto result = prepare(buffer);
            RUVIA_CHECK(result.prepared() != nullptr);
            if (result.prepared() != nullptr) {
                RUVIA_CHECK_EQ(result.prepared()->head(), std::string_view(expected));
            }
            RUVIA_CHECK(std::equal(buffer.begin() + static_cast<std::ptrdiff_t>(expected.size()), buffer.end(),
                original.begin() + static_cast<std::ptrdiff_t>(expected.size())));
            std::destroy_at(field);
        }
    }
}

RUVIA_TEST(http1_client_request_writer_preserves_borrowed_content_beyond_emitted_head) {
    for (const auto expectation : {ruvia::http_client_request_expectation::none,
             ruvia::http_client_request_expectation::continue_value}) {
        std::array<char, 512> buffer;
        buffer.fill('#');
        constexpr std::string_view value = "retained";
        constexpr std::string_view content = "body";
        std::copy(value.begin(), value.end(), buffer.begin());
        std::copy(content.begin(), content.end(), buffer.begin() + 256);
        const auto original = buffer;
        const std::array fields{ruvia::http_header_view{"X-Test", std::string_view(buffer.data(), value.size())}};
        const auto bytes = std::string_view(buffer.data() + 256, content.size());
        const http_client_request_view request{.method_ = "POST", .headers_ = fields, .content_ = http_client_request_content_view::bytes(bytes)};
        rejecting_character_storage_resource resource;
        const http1_client_request_writer writer({.resource_ = &resource});
        const auto result = writer.prepare(http_origin_view::http({.host_ = "example.test"}), request, buffer,
            {.expectation_ = expectation});
        const auto* prepared = result.prepared();
        RUVIA_CHECK(prepared != nullptr);
        if (prepared == nullptr) {
            continue;
        }
        std::string expected = "POST / HTTP/1.1\r\nHost: example.test\r\nX-Test: retained\r\nContent-Length: 4\r\n";
        if (expectation == ruvia::http_client_request_expectation::continue_value) {
            expected += "Expect: 100-continue\r\n";
            const auto* gated = prepared->content_plan().continue_gated();
            RUVIA_CHECK(gated != nullptr);
            if (gated != nullptr) {
                RUVIA_CHECK_EQ(gated->bytes(), content);
                RUVIA_CHECK(gated->bytes().data() == bytes.data());
            }
        } else {
            const auto* immediate = prepared->content_plan().immediate();
            RUVIA_CHECK(immediate != nullptr);
            if (immediate != nullptr) {
                RUVIA_CHECK_EQ(immediate->bytes(), content);
                RUVIA_CHECK(immediate->bytes().data() == bytes.data());
            }
        }
        expected += "\r\n";
        RUVIA_CHECK_EQ(prepared->head(), std::string_view(expected));
        RUVIA_CHECK(std::equal(buffer.begin() + static_cast<std::ptrdiff_t>(expected.size()), buffer.end(),
            original.begin() + static_cast<std::ptrdiff_t>(expected.size())));
    }
}

RUVIA_TEST(http1_client_upgrade_allocation_failure_preserves_metadata_borrowed_from_output) {
    for (const bool streaming : {false, true}) {
        constexpr std::string_view protocol = "a-valid-protocol-token-that-exceeds-small-string-storage";
        std::array<char, 512> buffer;
        buffer.fill('#');
        std::copy(protocol.begin(), protocol.end(), buffer.begin());
        const auto original = buffer;
        const std::array fields{ruvia::http_header_view{"Connection", "Upgrade"},
            ruvia::http_header_view{"Upgrade", std::string_view(buffer.data(), protocol.size())}};
        const auto origin = http_origin_view::http({.host_ = "example.test"});
        rejecting_character_storage_resource resource;
        const http1_client_request_writer writer({.resource_ = &resource});
        bool failed = false;
        try {
            if (streaming) {
                (void)writer.prepare_streaming(origin, {.headers_ = fields, .content_length_ = 0}, buffer);
            } else {
                (void)writer.prepare(origin, {.headers_ = fields}, buffer);
            }
        } catch (const std::bad_alloc&) {
            failed = true;
        }
        RUVIA_CHECK(failed);
        RUVIA_CHECK(buffer == original);
    }
}

RUVIA_TEST(http1_client_upgrade_state_allocation_failure_leaves_head_buffer_untouched) {
    rejecting_character_storage_resource rejecting;
    const std::array headers{
        ruvia::http_header_view{"Connection", "Upgrade"},
        ruvia::http_header_view{
            "Upgrade", "a-valid-protocol-token-that-exceeds-small-string-storage"},
    };
    http_client_request_view request{.headers_ = headers};
    std::array<char, 512> output;
    output.fill('u');

    bool allocation_failed = false;
    try {
        (void)http1_client_request_writer({.resource_ = &rejecting})
            .prepare(http_origin_view::https({.host_ = "example.test"}), request, output);
    } catch (const std::bad_alloc&) {
        allocation_failed = true;
    }
    RUVIA_CHECK(allocation_failed);
    RUVIA_CHECK(std::ranges::all_of(output, [](char value) { return value == 'u'; }));
}

RUVIA_TEST(http1_client_request_writer_enforces_header_count_and_size_transactionally) {
    std::array<ruvia::http_header_view, ruvia::max_http_header_fields> headers;
    headers.fill(ruvia::http_header_view("X-Test", "x"));
    http_client_request_view too_many;
    too_many.headers_ = headers;
    RUVIA_CHECK(prepare_error(too_many) == http1_client_request_prepare_error::too_many_headers);

    http_client_request_view oversized;
    std::string target(ruvia::max_http_header_bytes, 'a');
    target.front() = '/';
    oversized.target_ = target;
    RUVIA_CHECK(prepare_error(oversized) == http1_client_request_prepare_error::header_too_large);

    http_client_request_view invalid_method;
    invalid_method.method_ = "GET\r";
    std::array<char, 128> untouched;
    untouched.fill('q');
    const auto invalid = http1_client_request_writer().prepare(
        http_origin_view::https({.host_ = "example.test"}), invalid_method, untouched);
    RUVIA_CHECK(invalid.failure() != nullptr);
    RUVIA_CHECK(invalid.failure()->error() == http1_client_request_prepare_error::invalid_method);
    RUVIA_CHECK(std::ranges::all_of(untouched, [](char value) { return value == 'q'; }));
    RUVIA_CHECK(!ruvia::http1_client_request_prepare_error_message(
        http1_client_request_prepare_error::invalid_method)
            .empty());
}

RUVIA_TEST(http1_client_request_context_binds_the_actual_close_signal) {
    const ruvia::http_header_view close_header("Connection", "close");
    http_client_request_view request;
    request.headers_ = std::span<const ruvia::http_header_view>(&close_header, 1);
    prepared_fixture explicit_close(http_origin_view::https({.host_ = "example.test"}), request);
    const auto* explicit_prepared = explicit_close.result_.prepared();
    RUVIA_CHECK(explicit_prepared != nullptr);
    if (explicit_prepared != nullptr) {
        auto parser = ruvia::http1_client_response_parser(explicit_prepared->exchange_state());
        const auto response = parser.parse("HTTP/1.1 204 No Content\r\n\r\n");
        RUVIA_CHECK(response.parsed() != nullptr);
        if (response.parsed() != nullptr) {
            const auto* without_content = response.parsed()->plan().without_content();
            RUVIA_CHECK(without_content != nullptr);
            if (without_content != nullptr) {
                RUVIA_CHECK(
                    without_content->persistence() == ruvia::http1_close_policy::close_after_response);
            }
        }
    }

    http_client_request_view generated_request;
    prepared_fixture generated_close(http_origin_view::https({.host_ = "example.test"}),
        generated_request,
        http1_client_request_wire_policy{.close_policy_ = http1_close_policy::close_after_response});
    RUVIA_CHECK(generated_close.result_.prepared() != nullptr);
    if (generated_close.result_.prepared() != nullptr) {
        RUVIA_CHECK_EQ(generated_close.result_.prepared()->head(),
            std::string_view("GET / HTTP/1.1\r\nHost: example.test\r\nConnection: close\r\n\r\n"));
    }
}
