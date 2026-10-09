#include <string_view>

#include "http1/http1_cleartext_input.h"
#include "test_harness.h"

namespace {

using ruvia::detail::http1_server_request_parse_failure_source;
using ruvia::detail::should_drop_invalid_cleartext_http1_input;

}  // namespace

RUVIA_TEST(drop_invalid_cleartext_only_for_request_line_errors) {
    RUVIA_CHECK(!should_drop_invalid_cleartext_http1_input(
        "blah blah\r\n", http1_server_request_parse_failure_source::message));
}

RUVIA_TEST(drop_invalid_cleartext_by_version_token) {
    RUVIA_CHECK(should_drop_invalid_cleartext_http1_input(
        "FOO /path GARBAGE\r\n", http1_server_request_parse_failure_source::request_line));
    RUVIA_CHECK(should_drop_invalid_cleartext_http1_input(
        "random bytes here\r\n", http1_server_request_parse_failure_source::request_line));

    RUVIA_CHECK(!should_drop_invalid_cleartext_http1_input(
        "XX /p HTTP/1.1\r\n", http1_server_request_parse_failure_source::request_line));
    RUVIA_CHECK(!should_drop_invalid_cleartext_http1_input(
        "PRI * HTTP/2.0\r\n", http1_server_request_parse_failure_source::request_line));

    RUVIA_CHECK(!should_drop_invalid_cleartext_http1_input(
        "no-line-break", http1_server_request_parse_failure_source::request_line));
    RUVIA_CHECK(!should_drop_invalid_cleartext_http1_input(
        "singletoken\r\n", http1_server_request_parse_failure_source::request_line));
}
