#include <string_view>

#include "ruvia/http/http_header.h"
#include "ruvia/http/http_known_method.h"
#include "ruvia/http/http_parse_error.h"

#include "test_harness.h"

namespace {

using ruvia::classify_http_method;
using ruvia::http_known_method;
using ruvia::http_parse_error;
using ruvia::http_parse_protocol_error;
using ruvia::is_valid_http_header_name;
using ruvia::is_valid_http_header_value;
using ruvia::is_valid_http_method_token;
using ruvia::is_valid_http_status_text;
using ruvia::known_http_method_token;

}  // namespace

// What bytes a field name, field value or reason phrase may carry.

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

RUVIA_TEST(http_status_text_validation) {
    // The status reason phrase is validated by the header-value rules, so
    // response-splitting bytes on the status line are rejected.
    RUVIA_CHECK(is_valid_http_status_text("OK"));
    RUVIA_CHECK(is_valid_http_status_text("Not Found"));  // space is allowed
    RUVIA_CHECK(is_valid_http_status_text(""));           // an empty reason phrase is valid
    RUVIA_CHECK(!is_valid_http_status_text(std::string_view("a\r\nb", 4)));
    RUVIA_CHECK(!is_valid_http_status_text(std::string_view("a\nb", 3)));
    RUVIA_CHECK(!is_valid_http_status_text(std::string_view("a\0b", 3)));
}

RUVIA_TEST(http_parse_error_messages) {
    const auto missing_host = http_parse_protocol_error(http_parse_error::missing_host);
    const auto header_too_large = http_parse_protocol_error(http_parse_error::header_too_large);
    const auto unsupported_version = http_parse_protocol_error(http_parse_error::unsupported_http_version);
    RUVIA_CHECK_EQ(std::string_view(missing_host.what()), std::string_view("missing Host header"));
    RUVIA_CHECK_EQ(
        std::string_view(header_too_large.what()), std::string_view("request header is too large"));
    RUVIA_CHECK_EQ(
        std::string_view(unsupported_version.what()), std::string_view("unsupported HTTP version"));
    // The two Content-Length faults intentionally share one message.
    const auto invalid_length = http_parse_protocol_error(http_parse_error::invalid_content_length);
    const auto conflicting_length =
        http_parse_protocol_error(http_parse_error::conflicting_content_length);
    RUVIA_CHECK_EQ(
        std::string_view(invalid_length.what()), std::string_view(conflicting_length.what()));
    // Reachable errors all map to a non-empty message.
    RUVIA_CHECK(std::string_view(http_parse_protocol_error(http_parse_error::chunk_size_overflow).what())
                    .size() != 0);
    RUVIA_CHECK(
        std::string_view(http_parse_protocol_error(http_parse_error::invalid_transfer_encoding).what())
            .size() != 0);
    RUVIA_CHECK(std::string_view(http_parse_protocol_error(http_parse_error::invalid_connection).what())
                    .size() != 0);
    RUVIA_CHECK(
        std::string_view(http_parse_protocol_error(http_parse_error::invalid_upgrade).what()).size() !=
        0);
}
