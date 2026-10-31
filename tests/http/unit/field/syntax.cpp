#include <string>
#include <string_view>

#include "ruvia/http/http_field_syntax.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_known_method.h"
#include "ruvia/http/http_protocol_error.h"
#include "ruvia/http/http_protocol_managed_fields.h"

#include "test_harness.h"

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

RUVIA_TEST(http_header_name_validation) {
    RUVIA_CHECK(is_valid_http_header_name("Content-Type"));
    RUVIA_CHECK(is_valid_http_header_name("X-Custom-Header"));
    RUVIA_CHECK(is_valid_http_header_name("a"));
    RUVIA_CHECK(!is_valid_http_header_name(""));                           // empty name is invalid
    RUVIA_CHECK(!is_valid_http_header_name("X Y"));                        // space is not a token char
    RUVIA_CHECK(!is_valid_http_header_name("X:Y"));                        // ':' is a separator
    RUVIA_CHECK(!is_valid_http_header_name("(comment)"));                  // '(' ')' are separators
    RUVIA_CHECK(!is_valid_http_header_name("a@b"));                        // '@' is a separator
    RUVIA_CHECK(!is_valid_http_header_name(std::string_view("a\rb", 3)));  // control char
}

RUVIA_TEST(http_header_value_rejects_injection) {
    RUVIA_CHECK(is_valid_http_header_value("text/html; charset=utf-8"));  // spaces allowed
    RUVIA_CHECK(is_valid_http_header_value("a b"));                       // inner spaces allowed
    RUVIA_CHECK(is_valid_http_header_value(""));                          // empty value is valid
    RUVIA_CHECK(!is_valid_http_header_value(" leading"));
    RUVIA_CHECK(!is_valid_http_header_value("trailing "));
    RUVIA_CHECK(!is_valid_http_header_value("\ttab"));
    RUVIA_CHECK(!is_valid_http_header_value("tab\t"));
    // CR, LF and NUL must be rejected -- these are response/header-injection bytes.
    RUVIA_CHECK(!is_valid_http_header_value(std::string_view("a\rb", 3)));
    RUVIA_CHECK(!is_valid_http_header_value(std::string_view("a\nb", 3)));
    RUVIA_CHECK(!is_valid_http_header_value(std::string_view("a\r\nInjected: x", 14)));
    RUVIA_CHECK(!is_valid_http_header_value(std::string_view("a\0b", 3)));
}

RUVIA_TEST(http_outbound_field_checks_normalized_syntax) {
    for (const std::string_view name : {"", "X Y", "X:Y", ":authority", "X-Name ", "\tX-Name"}) {
        RUVIA_CHECK(!is_valid_http_request_application_field(name, "value"));
        RUVIA_CHECK(!is_valid_http_response_field(name, "value"));
    }
    RUVIA_CHECK(!is_valid_http_request_application_field(std::string_view("X\0Name", 6), "value"));
    RUVIA_CHECK(!is_valid_http_response_field(std::string_view("X\0Name", 6), "value"));
    for (const std::string_view value : {" leading", "trailing ", "\tleading", "trailing\t", " ", "\t"}) {
        RUVIA_CHECK(!is_valid_http_request_application_field("X-Extension", value));
        RUVIA_CHECK(!is_valid_http_response_field("X-Extension", value));
    }
    // 同时覆盖短值与长值：内部 HTAB、SP、可打印 ASCII 和 obs-text 合法，
    // NUL、CR/LF、其余控制字节及 DEL 不得进入应用发送的字段值。
    for (const std::size_t size : {std::size_t{3}, std::size_t{40}}) {
        std::string value(size, 'a');
        for (unsigned byte = 0; byte < 256; ++byte) {
            value[size / 2] = static_cast<char>(byte);
            const bool allowed = byte == '\t' || (byte >= 0x20 && byte != 0x7f);
            RUVIA_CHECK_EQ(is_valid_http_request_application_field("X-Extension", value), allowed);
            RUVIA_CHECK_EQ(is_valid_http_response_field("X-Extension", value), allowed);
        }
    }
    RUVIA_CHECK(is_valid_http_request_application_field("X-Extension", ""));
    RUVIA_CHECK(is_valid_http_response_field("X-Extension", ""));
    RUVIA_CHECK(is_valid_http_request_application_field("X-!#$%&'*+-.^_`|~", "a b\tc"));
    RUVIA_CHECK(is_valid_http_response_field("X-!#$%&'*+-.^_`|~", "a b\tc"));
}

RUVIA_TEST(http_request_application_field_rejects_protocol_owned_names) {
    for (const std::string_view name : {"Host", "hOsT", "Content-Length", "cOnTeNt-LeNgTh",
             "Transfer-Encoding", "tRaNsFeR-EnCoDiNg", "Trailer", "tRaIlEr",
             "Expect", "eXpEcT", "Connection", "cOnNeCtIoN", "Keep-Alive", "kEeP-aLiVe",
             "Proxy-Connection", "pRoXy-CoNnEcTiOn", "TE", "tE", "Upgrade", "uPgRaDe"}) {
        RUVIA_CHECK(!is_valid_http_request_application_field(name, "value"));
        RUVIA_CHECK(!is_valid_http_request_application_field(name, ""));
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
        RUVIA_CHECK(!is_valid_http_request_application_field(field.name(), field.value()));
    }
    RUVIA_CHECK(is_valid_http_request_application_field("X-Host", "example.test"));
    RUVIA_CHECK(is_valid_http_request_application_field("Expect-Extension", "100-continue"));
}

RUVIA_TEST(http_request_application_field_uses_known_sender_semantics) {
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
        RUVIA_CHECK(!is_valid_http_request_application_field(field.name(), field.value()));
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
        RUVIA_CHECK(is_valid_http_request_application_field(field.name(), field.value()));
    }
}

RUVIA_TEST(http_response_field_uses_known_sender_semantics) {
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
        RUVIA_CHECK(!is_valid_http_response_field(field.name(), field.value()));
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
        RUVIA_CHECK(is_valid_http_response_field(field.name(), field.value()));
    }
}

RUVIA_TEST(http_response_field_does_not_apply_request_admission_rules) {
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
        RUVIA_CHECK(is_valid_http_response_field(field.name(), field.value()));
        RUVIA_CHECK(!is_valid_http_request_application_field(field.name(), field.value()));
    }
}

RUVIA_TEST(http_parse_error_messages_and_codes) {
    const auto missing_host = http_parse_protocol_error(http_parse_error::missing_host);
    const auto header_too_large = http_parse_protocol_error(http_parse_error::header_too_large);
    const auto unsupported_version = http_parse_protocol_error(http_parse_error::unsupported_http_version);
    RUVIA_CHECK_EQ(std::string_view(missing_host.what()), std::string_view("missing Host header"));
    RUVIA_CHECK_EQ(missing_host.code(), std::string_view("missing_host"));
    RUVIA_CHECK_EQ(
        std::string_view(header_too_large.what()), std::string_view("request header is too large"));
    RUVIA_CHECK_EQ(header_too_large.code(), std::string_view("header_too_large"));
    RUVIA_CHECK_EQ(
        std::string_view(unsupported_version.what()), std::string_view("unsupported HTTP version"));
    RUVIA_CHECK_EQ(unsupported_version.code(), std::string_view("unsupported_http_version"));
    // The two Content-Length faults carry distinct machine codes.
    const auto invalid_length = http_parse_protocol_error(http_parse_error::invalid_content_length);
    const auto conflicting_length =
        http_parse_protocol_error(http_parse_error::conflicting_content_length);
    RUVIA_CHECK_EQ(invalid_length.code(), std::string_view("invalid_content_length"));
    RUVIA_CHECK_EQ(conflicting_length.code(), std::string_view("conflicting_content_length"));
    RUVIA_CHECK(invalid_length.status() == conflicting_length.status());
    // Reachable errors all map to a non-empty message.
    RUVIA_CHECK(
        std::string_view(http_parse_protocol_error(http_parse_error::invalid_transfer_encoding).what())
            .size() != 0);
    RUVIA_CHECK(std::string_view(http_parse_protocol_error(http_parse_error::invalid_connection).what())
                    .size() != 0);
    RUVIA_CHECK(
        std::string_view(http_parse_protocol_error(http_parse_error::invalid_upgrade).what()).size() !=
        0);
}
