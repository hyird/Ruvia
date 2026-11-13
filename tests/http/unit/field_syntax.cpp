#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ruvia/http/http_cache.h"
#include "ruvia/http/http_content_coding.h"
#include "ruvia/http/http_field_syntax.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_protocol_error.h"
#include "ruvia/http/http_request_target.h"

namespace {

using ruvia::classify_http_method;
using ruvia::http_known_method;
using ruvia::http_parse_error;
using ruvia::http_parse_protocol_error;
using ruvia::is_valid_http_header_name;
using ruvia::is_valid_http_header_value;
using ruvia::is_valid_http_method_token;
using ruvia::is_valid_http_request_application_field;
using ruvia::is_valid_http_response_field;
using ruvia::known_http_method_token;

}  // namespace

// What bytes a field name or field value (including a reason phrase) may carry.

TEST(Ruvia, http_header_name_validation) {
    EXPECT_TRUE(is_valid_http_header_name("Content-Type"));
    EXPECT_TRUE(is_valid_http_header_name("X-Custom-Header"));
    EXPECT_TRUE(is_valid_http_header_name("a"));
    EXPECT_TRUE(!is_valid_http_header_name(""));                           // empty name is invalid
    EXPECT_TRUE(!is_valid_http_header_name("X Y"));                        // space is not a token char
    EXPECT_TRUE(!is_valid_http_header_name("X:Y"));                        // ':' is a separator
    EXPECT_TRUE(!is_valid_http_header_name("(comment)"));                  // '(' ')' are separators
    EXPECT_TRUE(!is_valid_http_header_name("a@b"));                        // '@' is a separator
    EXPECT_TRUE(!is_valid_http_header_name(std::string_view("a\rb", 3)));  // control char
}

TEST(Ruvia, http_header_value_rejects_injection) {
    EXPECT_TRUE(is_valid_http_header_value("text/html; charset=utf-8"));  // spaces allowed
    EXPECT_TRUE(is_valid_http_header_value("a b"));                       // inner spaces allowed
    EXPECT_TRUE(is_valid_http_header_value(""));                          // empty value is valid
    EXPECT_TRUE(!is_valid_http_header_value(" leading"));
    EXPECT_TRUE(!is_valid_http_header_value("trailing "));
    EXPECT_TRUE(!is_valid_http_header_value("\ttab"));
    EXPECT_TRUE(!is_valid_http_header_value("tab\t"));
    // CR, LF and NUL must be rejected -- these are response/header-injection bytes.
    EXPECT_TRUE(!is_valid_http_header_value(std::string_view("a\rb", 3)));
    EXPECT_TRUE(!is_valid_http_header_value(std::string_view("a\nb", 3)));
    EXPECT_TRUE(!is_valid_http_header_value(std::string_view("a\r\nInjected: x", 14)));
    EXPECT_TRUE(!is_valid_http_header_value(std::string_view("a\0b", 3)));
}

TEST(Ruvia, http_outbound_field_checks_normalized_syntax) {
    for (const std::string_view name : {"", "X Y", "X:Y", ":authority", "X-Name ", "\tX-Name"}) {
        EXPECT_TRUE(!is_valid_http_request_application_field(name, "value"));
        EXPECT_TRUE(!is_valid_http_response_field(name, "value"));
    }
    EXPECT_TRUE(!is_valid_http_request_application_field(std::string_view("X\0Name", 6), "value"));
    EXPECT_TRUE(!is_valid_http_response_field(std::string_view("X\0Name", 6), "value"));
    for (const std::string_view value : {" leading", "trailing ", "\tleading", "trailing\t", " ", "\t"}) {
        EXPECT_TRUE(!is_valid_http_request_application_field("X-Extension", value));
        EXPECT_TRUE(!is_valid_http_response_field("X-Extension", value));
    }
    // 同时覆盖短值与长值：内部 HTAB、SP、可打印 ASCII 和 obs-text 合法，
    // NUL、CR/LF、其余控制字节及 DEL 不得进入应用发送的字段值。
    for (const std::size_t size : {std::size_t{3}, std::size_t{40}}) {
        std::string value(size, 'a');
        for (unsigned byte = 0; byte < 256; ++byte) {
            value[size / 2] = static_cast<char>(byte);
            const bool allowed = byte == '\t' || (byte >= 0x20 && byte != 0x7f);
            EXPECT_EQ(is_valid_http_request_application_field("X-Extension", value), allowed);
            EXPECT_EQ(is_valid_http_response_field("X-Extension", value), allowed);
        }
    }
    EXPECT_TRUE(is_valid_http_request_application_field("X-Extension", ""));
    EXPECT_TRUE(is_valid_http_response_field("X-Extension", ""));
    EXPECT_TRUE(is_valid_http_request_application_field("X-!#$%&'*+-.^_`|~", "a b\tc"));
    EXPECT_TRUE(is_valid_http_response_field("X-!#$%&'*+-.^_`|~", "a b\tc"));
}

TEST(Ruvia, http_request_application_field_rejects_protocol_owned_names) {
    for (const std::string_view name : {"Host", "hOsT", "Content-Length", "cOnTeNt-LeNgTh",
             "Transfer-Encoding", "tRaNsFeR-EnCoDiNg", "Trailer", "tRaIlEr",
             "Expect", "eXpEcT", "Connection", "cOnNeCtIoN", "Keep-Alive", "kEeP-aLiVe",
             "Proxy-Connection", "pRoXy-CoNnEcTiOn", "TE", "tE", "Upgrade", "uPgRaDe"}) {
        EXPECT_TRUE(!is_valid_http_request_application_field(name, "value"));
        EXPECT_TRUE(!is_valid_http_request_application_field(name, ""));
    }
    // 即使协议字段值本身合法，也不能从应用首部入口注入。
    const ruvia::http_header_view managed[] = {
        {"Host", "example.test"},
        {"Content-Length", "0"},
        {"Transfer-Encoding", "chunked"},
        {"Trailer", "Digest"},
        {"Expect", "100-continue"},
        {"Connection", "close"},
        {"Keep-Alive", "timeout=5"},
        {"Proxy-Connection", "keep-alive"},
        {"TE", "trailers"},
        {"Upgrade", "websocket"},
    };
    for (const auto& field : managed) {
        EXPECT_TRUE(!is_valid_http_request_application_field(field.name(), field.value()));
    }
    EXPECT_TRUE(is_valid_http_request_application_field("X-Host", "example.test"));
    EXPECT_TRUE(is_valid_http_request_application_field("Expect-Extension", "100-continue"));
}

TEST(Ruvia, http_request_application_field_uses_known_sender_semantics) {
    const ruvia::http_header_view invalid[] = {
        {"Content-Type", ""},
        {"cOnTeNt-TyPe", "text plain"},
        {"Content-Type", "*/plain"},
        {"Content-Type", "text/plain; charset"},
        {"Content-Type", "text/plain; charset="},
        {"Content-Type", "text/plain; charset =utf-8"},
        {"Content-Type", "text/plain; charset=utf-8; CHARSET=latin1"},
        {"Content-Type", "text/plain; charset=\"unfinished"},
        {"Origin", ""},
        {"oRiGiN", "https://example.test/path"},
        {"Origin", "null https://example.test"},
        {"Origin", "https://first.test\thttps://second.test"},
        {"Access-Control-Request-Method", ""},
        {"aCcEsS-cOnTrOl-ReQuEsT-mEtHoD", "GET, POST"},
        {"Access-Control-Request-Headers", ""},
        {"aCcEsS-cOnTrOl-ReQuEsT-hEaDeRs", "X-Good, Bad Header"},
        {"Content-Encoding", ""},
        {"cOnTeNt-EnCoDiNg", "gzip;level=9"},
        {"Content-Encoding", ",gzip"},
        {"Content-Encoding", "gzip,"},
    };
    for (const auto& field : invalid) {
        EXPECT_TRUE(!is_valid_http_request_application_field(field.name(), field.value()));
    }
    const ruvia::http_header_view valid[] = {
        {"Content-Type", "text/plain; charset=\"utf-8\""},
        {"content-type", "application/x-example"},
        {"Content-Type", "application/example; extension=\"\""},
        {"Origin", "null"},
        {"origin", "https://first.test https://second.test"},
        {"Access-Control-Request-Method", "CUSTOM"},
        {"Access-Control-Request-Headers", "X-First, X-Second"},
        {"Content-Encoding", "gzip, br"},
        {"content-encoding", "example-extension"},
        {"X-Extension", "not a media type"},
    };
    for (const auto& field : valid) {
        EXPECT_TRUE(is_valid_http_request_application_field(field.name(), field.value()));
    }
}

TEST(Ruvia, http_response_field_uses_known_sender_semantics) {
    const ruvia::http_header_view invalid[] = {
        {"Content-Type", ""},
        {"cOnTeNt-TyPe", "text plain"},
        {"Content-Type", "*/plain"},
        {"Content-Type", "text/plain; charset="},
        {"Content-Type", "text/plain; charset=utf-8; CHARSET=latin1"},
        {"Content-Type", "text/plain; charset=\"unfinished"},
        {"Content-Encoding", ""},
        {"cOnTeNt-EnCoDiNg", "gzip;level=9"},
        {"Content-Encoding", ",gzip"},
        {"Content-Encoding", "gzip,"},
        {"Trailer", "Content-Length"},
        {"tRaIlEr", "bad name"},
    };
    for (const auto& field : invalid) {
        EXPECT_TRUE(!is_valid_http_response_field(field.name(), field.value()));
    }
    const ruvia::http_header_view valid[] = {
        {"Content-Type", "text/plain; charset=\"utf-8\""},
        {"content-type", "application/x-example"},
        {"Content-Type", "application/example; extension=\"\""},
        {"Content-Encoding", "gzip, br"},
        {"content-encoding", "example-extension"},
        {"Trailer", "ETag, X-Checksum"},
        {"X-Extension", "not a media type"},
    };
    for (const auto& field : valid) {
        EXPECT_TRUE(is_valid_http_response_field(field.name(), field.value()));
    }
}

TEST(Ruvia, http_response_field_does_not_apply_request_admission_rules) {
    const ruvia::http_header_view fields[] = {
        {"Origin", "https://example.test/path"},
        {"Access-Control-Request-Method", "GET, POST"},
        {"Access-Control-Request-Headers", "X-Good, Bad Header"},
        {"Expect", "extension"},
        {"Origin", ""},
        {"Content-Length", "0"},
        {"Connection", "close"},
        {"Upgrade", "websocket"},
    };
    for (const auto& field : fields) {
        EXPECT_TRUE(is_valid_http_response_field(field.name(), field.value()));
        EXPECT_TRUE(!is_valid_http_request_application_field(field.name(), field.value()));
    }
}

TEST(Ruvia, http_parse_error_messages_and_codes) {
    const auto missing_host = http_parse_protocol_error(http_parse_error::missing_host);
    const auto header_too_large = http_parse_protocol_error(http_parse_error::header_too_large);
    const auto unsupported_version = http_parse_protocol_error(http_parse_error::unsupported_http_version);
    EXPECT_EQ(std::string_view(missing_host.what()), std::string_view("missing Host header"));
    EXPECT_EQ(missing_host.code(), std::string_view("missing_host"));
    EXPECT_EQ(std::string_view(header_too_large.what()), std::string_view("request header is too large"));
    EXPECT_EQ(header_too_large.code(), std::string_view("header_too_large"));
    EXPECT_EQ(std::string_view(unsupported_version.what()), std::string_view("unsupported HTTP version"));
    EXPECT_EQ(unsupported_version.code(), std::string_view("unsupported_http_version"));
    // The two Content-Length faults carry distinct machine codes.
    const auto invalid_length = http_parse_protocol_error(http_parse_error::invalid_content_length);
    const auto conflicting_length =
        http_parse_protocol_error(http_parse_error::conflicting_content_length);
    EXPECT_EQ(invalid_length.code(), std::string_view("invalid_content_length"));
    EXPECT_EQ(conflicting_length.code(), std::string_view("conflicting_content_length"));
    EXPECT_TRUE(invalid_length.status() == conflicting_length.status());
    // Reachable errors all map to a non-empty message.
    EXPECT_TRUE(std::string_view(http_parse_protocol_error(http_parse_error::invalid_transfer_encoding).what())
                    .size() != 0);
    EXPECT_TRUE(std::string_view(http_parse_protocol_error(http_parse_error::invalid_connection).what())
                    .size() != 0);
    EXPECT_TRUE(std::string_view(http_parse_protocol_error(http_parse_error::invalid_upgrade).what()).size() !=
                0);
}

TEST(Ruvia, http_field_ows_trims_only_space_and_tab) {
    EXPECT_EQ(ruvia::http_trim_ows(" \t text/plain \t"), std::string_view("text/plain"));
    EXPECT_EQ(ruvia::http_trim_ows("\ntext/plain\n"), std::string_view("\ntext/plain\n"));
}

TEST(Ruvia, http_serialized_origin_rejects_opaque_origin_and_origin_lists) {
    // A field can contain null or several origins; neither is a single
    // serialized origin usable in configuration or connection advertisements.
    EXPECT_TRUE(!ruvia::is_valid_http_serialized_origin("null"));
    EXPECT_TRUE(!ruvia::is_valid_http_serialized_origin("https://example.com http://other.example"));
}

TEST(Ruvia, http_field_name_list_preserves_fields_and_skips_empty_members) {
    ruvia::http_field_name_list names(" , X-One,\tX-Two\t, ");
    EXPECT_EQ(names.next().value(), std::string_view("X-One"));
    EXPECT_EQ(names.next().value(), std::string_view("X-Two"));
    EXPECT_TRUE(!names.next());
    EXPECT_TRUE(names.valid());
}

TEST(Ruvia, http_field_name_list_borrows_input_and_keeps_terminal_state) {
    constexpr std::string_view field = "\tX-One , X-Two";
    ruvia::http_field_name_list names(field);
    EXPECT_TRUE(!names.valid());
    const auto first = names.next();
    EXPECT_TRUE(first.has_value());
    EXPECT_EQ(first->data(), field.data() + 1);
    EXPECT_TRUE(!names.valid());
    const auto second = names.next();
    EXPECT_TRUE(second.has_value());
    EXPECT_EQ(second->data(), field.data() + 9);
    EXPECT_TRUE(names.valid());
    EXPECT_TRUE(!names.next());
    EXPECT_TRUE(!names.next());
    EXPECT_TRUE(names.valid());
}

TEST(Ruvia, http_field_name_list_accepts_empty_lists_and_rejects_invalid_names) {
    ruvia::http_field_name_list empty(" , \t, ");
    EXPECT_TRUE(!empty.next());
    EXPECT_TRUE(empty.valid());

    ruvia::http_field_name_list invalid("X-Ok, bad name");
    EXPECT_EQ(invalid.next().value(), std::string_view("X-Ok"));
    EXPECT_TRUE(!invalid.next());
    EXPECT_TRUE(!invalid.valid());
    EXPECT_TRUE(!invalid.next());
    EXPECT_TRUE(!invalid.valid());
}

namespace {

using ruvia::http_media_type_only;

}  // namespace

TEST(Ruvia, http_media_type_only_strips_parameters_and_ows) {
    EXPECT_EQ(http_media_type_only("application/json"), std::string_view("application/json"));
    EXPECT_EQ(http_media_type_only("application/json; charset=utf-8"), std::string_view("application/json"));
    EXPECT_EQ(http_media_type_only("application/json ; charset=utf-8"), std::string_view("application/json"));
    EXPECT_EQ(http_media_type_only(""), std::string_view(""));
}

TEST(Ruvia, http_content_type_field_validation_checks_syntax) {
    EXPECT_TRUE(ruvia::is_valid_http_content_type_field_value("text/plain; charset=utf-8"));
    EXPECT_TRUE(!ruvia::is_valid_http_content_type_field_value("text/plain; charset="));
}

TEST(Ruvia, http_content_type_allows_empty_parameter_slots) {
    for (const std::string_view field : {
             "text/plain;",
             "text/plain;;",
             "text/plain; \t; charset=utf-8; ;",
             R"(text/plain;;note="a;b";;charset="utf-8";)",
             R"(text/plain;;note="";)",
             "text/plain;q=invalid",
             R"(text/plain;q="0.5")"}) {
        EXPECT_TRUE(ruvia::is_valid_http_content_type_field_value(field));
    }
}

TEST(Ruvia, http_content_type_empty_slots_do_not_relax_nonempty_parameter_syntax) {
    for (const std::string_view field : {
             "text/plain;;charset;",
             "text/plain;;charset=;",
             "text/plain;;=utf-8;",
             "text/plain;;charset =utf-8;",
             "text/plain;;charset= utf-8;",
             "text/plain;;charset=utf-8;;CHARSET=utf-8;",
             "text/plain;;note=\"unterminated;",
             R"(text/plain;note="closed"suffix)",
             "text/plain;note=a/b"}) {
        EXPECT_TRUE(!ruvia::is_valid_http_content_type_field_value(field));
    }
}

namespace {

std::string format_date(std::time_t time) {
    const auto out = ruvia::format_http_date(time).value();
    return std::string(out.data(), out.size());
}

}  // namespace

TEST(Ruvia, http_format_date_known_vectors) {
    EXPECT_EQ(format_date(0), std::string("Thu, 01 Jan 1970 00:00:00 GMT"));
    EXPECT_EQ(format_date(86400), std::string("Fri, 02 Jan 1970 00:00:00 GMT"));
    EXPECT_EQ(format_date(784111777), std::string("Sun, 06 Nov 1994 08:49:37 GMT"));
    EXPECT_EQ(format_date(172800), std::string("Sat, 03 Jan 1970 00:00:00 GMT"));
    EXPECT_EQ(format_date(345600), std::string("Mon, 05 Jan 1970 00:00:00 GMT"));
    EXPECT_EQ(format_date(432000), std::string("Tue, 06 Jan 1970 00:00:00 GMT"));
    EXPECT_EQ(format_date(518400), std::string("Wed, 07 Jan 1970 00:00:00 GMT"));
}

TEST(Ruvia, http_format_date_round_trips_through_public_parser) {
    for (const auto time : std::array<std::int64_t, 5>{
             -315619200, -1, 0, 784111777, 253402300799}) {
        if (!std::in_range<std::time_t>(time)) {
            continue;
        }
        const auto parsed = ruvia::parse_http_date(format_date(static_cast<std::time_t>(time)));
        EXPECT_TRUE(parsed.has_value());
        EXPECT_EQ(static_cast<std::int64_t>(*parsed), time);
    }
}

TEST(Ruvia, http_date_conversion_rejects_unrepresentable_years) {
    for (const auto time : {std::int64_t{-62167219201LL}, std::int64_t{253402300800LL},
             std::numeric_limits<std::int64_t>::min(), std::numeric_limits<std::int64_t>::max()}) {
        if (!std::in_range<std::time_t>(time)) {
            continue;
        }
        EXPECT_TRUE(!ruvia::format_http_date(static_cast<std::time_t>(time)));
    }
    EXPECT_EQ(format_date(-315619200), std::string("Fri, 01 Jan 1960 00:00:00 GMT"));
    EXPECT_EQ(format_date(-1), std::string("Wed, 31 Dec 1969 23:59:59 GMT"));
    if (std::in_range<std::time_t>(253402300799LL)) {
        EXPECT_EQ(format_date(static_cast<std::time_t>(253402300799LL)), std::string("Fri, 31 Dec 9999 23:59:59 GMT"));
    }
}

namespace {

using ruvia::http_client_expectation_is_valid;
using ruvia::http_request_content_indication;
using ruvia::http_request_expectations;
using ruvia::http_unsupported_expectation_policy;

}  // namespace

// The Expect field and what a client that sends it must follow with.

TEST(Ruvia, client_expectation_requires_following_content) {
    EXPECT_TRUE(http_client_expectation_is_valid(false, http_request_content_indication::no_content));
    EXPECT_TRUE(!http_client_expectation_is_valid(true, http_request_content_indication::no_content));
    EXPECT_TRUE(http_client_expectation_is_valid(true, http_request_content_indication::will_follow));
}

TEST(Ruvia, expectations_parse_one_logical_recipient_list) {
    http_request_expectations expectations;
    expectations.parse_field(" , 100-continue, , 100-Continue, ");
    expectations.parse_field(" 100-CONTINUE ");

    EXPECT_TRUE(expectations.has_continue());
    EXPECT_TRUE(!expectations.has_unsupported());
    const auto no_content = expectations.server_plan(
        http_request_content_indication::no_content, http_unsupported_expectation_policy::reject);
    EXPECT_TRUE(no_content == ruvia::http_server_expectation_plan::no_action);
    const auto with_content = expectations.server_plan(
        http_request_content_indication::will_follow, http_unsupported_expectation_policy::reject);
    EXPECT_TRUE(with_content == ruvia::http_server_expectation_plan::send_continue);
}

TEST(Ruvia, expectations_preserve_unsupported_extensions_as_semantics) {
    http_request_expectations expectations;
    expectations.parse_field("100-continue");
    expectations.parse_field(R"(custom="a,b")");

    EXPECT_TRUE(expectations.has_continue());
    EXPECT_TRUE(expectations.has_unsupported());
    const auto rejected = expectations.server_plan(
        http_request_content_indication::will_follow, http_unsupported_expectation_policy::reject);
    EXPECT_TRUE(rejected == ruvia::http_server_expectation_plan::reject_unsupported);
    EXPECT_EQ(ruvia::http_unsupported_expectation_error().status(), ruvia::http_status::expectation_failed);
    const auto ignored = expectations.server_plan(
        http_request_content_indication::will_follow, http_unsupported_expectation_policy::ignore);
    EXPECT_TRUE(ignored == ruvia::http_server_expectation_plan::send_continue);

    expectations.ignore_continue();
    EXPECT_TRUE(!expectations.has_continue());
    EXPECT_TRUE(expectations.has_unsupported());
}

namespace {

using ruvia::classify_http_method;
using ruvia::http_known_method;
using ruvia::http_parse_error;
using ruvia::http_parse_protocol_error;
using ruvia::is_valid_http_header_name;
using ruvia::is_valid_http_header_value;
using ruvia::is_valid_http_method_token;
using ruvia::known_http_method_token;

}  // namespace

// The method vocabulary: recognising tokens, spelling them back, and the
// safe and idempotent properties a recipient acts on.

TEST(Ruvia, http_request_method_content_semantics_are_shared_by_client_and_server) {
    using ruvia::classify_http_request_content_semantics;
    using ruvia::http_request_content_semantics;
    EXPECT_TRUE(classify_http_request_content_semantics("CONNECT") ==
                http_request_content_semantics::forbidden);
    EXPECT_TRUE(classify_http_request_content_semantics("TRACE") ==
                http_request_content_semantics::forbidden);
    EXPECT_TRUE(classify_http_request_content_semantics("OPTIONS") ==
                http_request_content_semantics::content_type_required);
    EXPECT_TRUE(classify_http_request_content_semantics("POST") ==
                http_request_content_semantics::no_additional_requirements);
}

TEST(Ruvia, http_method_parsing_is_exact_and_case_sensitive) {
    EXPECT_TRUE(classify_http_method("GET") == http_known_method::get);
    EXPECT_TRUE(classify_http_method("POST") == http_known_method::post);
    EXPECT_TRUE(classify_http_method("PUT") == http_known_method::put);
    EXPECT_TRUE(classify_http_method("DELETE") == http_known_method::delete_value);
    EXPECT_TRUE(classify_http_method("PATCH") == http_known_method::patch);
    EXPECT_TRUE(classify_http_method("HEAD") == http_known_method::head);
    EXPECT_TRUE(classify_http_method("OPTIONS") == http_known_method::options);
    EXPECT_TRUE(classify_http_method("CONNECT") == http_known_method::connect);
    // Methods are case-sensitive (RFC 9110 section 9.1); anything outside the
    // framework's fixed semantic set remains an unknown classification.
    EXPECT_TRUE(classify_http_method("get") == http_known_method::unknown);
    EXPECT_TRUE(classify_http_method("Get") == http_known_method::unknown);
    EXPECT_TRUE(classify_http_method("FOO") == http_known_method::unknown);
    EXPECT_TRUE(classify_http_method("") == http_known_method::unknown);
    EXPECT_TRUE(classify_http_method("GETX") == http_known_method::unknown);
}

TEST(Ruvia, http_method_token_validation_is_separate_from_classification) {
    EXPECT_TRUE(is_valid_http_method_token("PROPFIND"));
    EXPECT_TRUE(is_valid_http_method_token("get"));
    EXPECT_TRUE(is_valid_http_method_token("M-SEARCH"));
    EXPECT_TRUE(!is_valid_http_method_token(""));
    EXPECT_TRUE(!is_valid_http_method_token("BAD METHOD"));
    EXPECT_TRUE(!is_valid_http_method_token("BAD(METHOD"));
    EXPECT_TRUE(!is_valid_http_method_token(std::string_view("BAD\x01METHOD", 10)));
}

TEST(Ruvia, http_method_safety_and_idempotency_follow_wire_semantics) {
    for (const std::string_view method : {"GET", "HEAD", "OPTIONS", "TRACE"}) {
        EXPECT_TRUE(ruvia::is_http_method_safe(method));
        EXPECT_TRUE(ruvia::is_http_method_idempotent(method));
    }
    for (const std::string_view method : {"PUT", "DELETE"}) {
        EXPECT_TRUE(!ruvia::is_http_method_safe(method));
        EXPECT_TRUE(ruvia::is_http_method_idempotent(method));
    }
    for (const std::string_view method : {"POST", "PATCH", "CONNECT", "CUSTOM", "get"}) {
        EXPECT_TRUE(!ruvia::is_http_method_safe(method));
        EXPECT_TRUE(!ruvia::is_http_method_idempotent(method));
    }
}

TEST(Ruvia, http_known_method_token_round_trips) {
    const http_known_method methods[] = {http_known_method::get, http_known_method::post,
        http_known_method::put, http_known_method::delete_value, http_known_method::patch,
        http_known_method::head, http_known_method::options, http_known_method::connect};
    for (const auto method : methods) {
        EXPECT_TRUE(classify_http_method(known_http_method_token(method)) == method);
    }
    // An unknown classification has no canonical wire spelling. Callers that
    // need it must retain the exact token instead of manufacturing "UNKNOWN".
    EXPECT_TRUE(known_http_method_token(http_known_method::unknown).empty());
}

// A quoted-string in a field value is opaque: a delimiter inside it never splits the field.

TEST(Ruvia, public_quoted_field_visitors_preserve_quoted_delimiters) {
    std::string_view parameter_name;
    std::string_view parameter_value;
    ruvia::http_visit_semicolon_parameters_quoted_field(
        R"(for="[2001:db8::1];zone"; proto=https)",
        [&](std::string_view name, std::string_view value) {
            parameter_name = name;
            parameter_value = ruvia::http_trim_quoted_field_value(value);
            return true;
        });
    EXPECT_EQ(parameter_name, std::string_view("proto"));
    EXPECT_EQ(parameter_value, std::string_view("https"));

    std::size_t item_count = 0;
    ruvia::http_visit_comma_separated_quoted_field_items(
        R"(for="192.0.2.1,192.0.2.2", proto=https)",
        [&](std::string_view) {
            ++item_count;
            return true;
        });
    EXPECT_EQ(item_count, std::size_t{2});
}

TEST(Ruvia, public_quoted_string_cursor_validates_and_decodes) {
    std::string decoded;
    ruvia::http_quoted_string_cursor cursor(R"("a\"b\\c";rest)");
    unsigned char byte = 0;
    while (cursor.next(byte)) {
        decoded.push_back(static_cast<char>(byte));
    }
    EXPECT_TRUE(cursor.closed());
    EXPECT_EQ(decoded, std::string_view(R"(a"b\c)"));
    EXPECT_EQ(cursor.consumed(), std::size_t{9});

    EXPECT_EQ(ruvia::http_quoted_string_size(R"("x" tail)"), std::size_t{3});
    EXPECT_TRUE(ruvia::http_is_quoted_string(R"("")"));
    EXPECT_TRUE(ruvia::http_is_quoted_string("\"\t obs\x80\""));
    EXPECT_TRUE(!ruvia::http_is_quoted_string(""));
    EXPECT_TRUE(!ruvia::http_is_quoted_string("token"));
    EXPECT_TRUE(!ruvia::http_is_quoted_string(R"("open)"));
    EXPECT_TRUE(!ruvia::http_is_quoted_string(R"("a"b")"));
    EXPECT_TRUE(!ruvia::http_is_quoted_string(R"("trailing\")"));
    EXPECT_TRUE(!ruvia::http_is_quoted_string("\"ctl\x01\""));
    EXPECT_TRUE(!ruvia::http_is_quoted_string("\"del\x7f\""));

    EXPECT_EQ(ruvia::http_find_unquoted_delimiter(R"(a="x,\",y",b)", 0, ','), std::size_t{10});
}

TEST(Ruvia, public_unquoted_delimiter_preserves_quote_state_before_start) {
    constexpr std::string_view field = R"(p="a;b";q=x)";
    for (std::size_t start = 0; start <= 7; ++start) {
        EXPECT_EQ(ruvia::http_find_unquoted_delimiter(field, start, ';'), std::size_t{7});
    }
    EXPECT_EQ(ruvia::http_find_unquoted_delimiter(field, 8, ';'), field.size());
    EXPECT_EQ(ruvia::http_find_unquoted_delimiter(field, field.size() + 1, ';'), field.size());

    constexpr std::string_view escaped = R"(p="a\";b";q=x)";
    for (std::size_t start = 4; start <= 9; ++start) {
        EXPECT_EQ(ruvia::http_find_unquoted_delimiter(escaped, start, ';'), std::size_t{9});
    }
    EXPECT_EQ(ruvia::http_find_unquoted_delimiter(R"(p="a;b)", 4, ';'), std::size_t{6});
}

TEST(Ruvia, accept_encoding_quality_unquoted_unchanged) {
    using ruvia::http_accepts_encoding;
    EXPECT_TRUE(http_accepts_encoding("", "identity"));
    EXPECT_TRUE(!http_accepts_encoding("", "gzip"));
    EXPECT_TRUE(http_accepts_encoding("gzip;q=0.5, br", "br"));
    EXPECT_TRUE(http_accepts_encoding("gzip;q=0.5, br", "gzip"));
    EXPECT_TRUE(!http_accepts_encoding("gzip;q=0", "gzip"));
}

TEST(Ruvia, accept_quality_quoted_comma_does_not_split_item) {
    using ruvia::http_accepts_encoding;
    EXPECT_TRUE(!http_accepts_encoding(R"(gzip;note="a,b";q=0)", "gzip"));
}

static_assert(std::is_constructible_v<ruvia::http_quoted_string_cursor, const std::string&>);
static_assert(std::is_constructible_v<ruvia::http_quoted_string_cursor, std::string_view>);
static_assert(!std::is_constructible_v<ruvia::http_quoted_string_cursor, std::string>);
static_assert(!std::is_constructible_v<ruvia::http_quoted_string_cursor, std::string>);

TEST(Ruvia, quoted_field_members_keep_escaped_quotes_delimiters_and_empty_items) {
    constexpr std::string_view input = R"( first , "second,\"still quoted",, last, )";
    std::vector<std::string_view> items;
    ruvia::http_visit_comma_separated_quoted_field_items(input, [&](std::string_view item) {
        items.push_back(item);
        return true;
    });
    EXPECT_EQ(items, (std::vector<std::string_view>{"first", R"("second,\"still quoted")", "", "last", ""}));
    EXPECT_EQ(items.front().data(), input.data() + 1);
    int empty_visits{};
    ruvia::http_visit_comma_separated_quoted_field_items({}, [&](std::string_view item) {
        ++empty_visits;
        EXPECT_TRUE(item.empty());
        return true;
    });
    EXPECT_EQ(empty_visits, 1);
}

TEST(Ruvia, quoted_field_parameters_preserve_quotes_and_skip_non_parameters) {
    constexpr std::string_view input = R"( text/plain ; name = "a;\"still quoted" ; ignored ; empty= ;=value; )";
    std::vector<std::pair<std::string_view, std::string_view>> parameters;
    ruvia::http_visit_semicolon_parameters_quoted_field(input, [&](std::string_view name, std::string_view value) {
        parameters.emplace_back(name, value);
        return true;
    });
    EXPECT_EQ(parameters, (std::vector<std::pair<std::string_view, std::string_view>>{
                              {"name", R"("a;\"still quoted")"}, {"empty", ""}, {"", "value"}}));
}

TEST(Ruvia, quoted_field_visitors_stop_without_visiting_remaining_segments) {
    std::vector<std::string_view> members;
    ruvia::http_visit_comma_separated_quoted_field_items("first,second,third", [&](std::string_view item) {
        members.push_back(item);
        return members.size() != 2;
    });
    EXPECT_EQ(members, (std::vector<std::string_view>{"first", "second"}));
    std::vector<std::string_view> names;
    ruvia::http_visit_semicolon_parameters_quoted_field("ignored;a=1;b=2;c=3", [&](std::string_view name, std::string_view) {
        names.push_back(name);
        return false;
    });
    EXPECT_EQ(names, (std::vector<std::string_view>{"a"}));
}
