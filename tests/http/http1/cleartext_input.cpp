#include <string_view>

#include "ruvia/http/detail/http1/Http1CleartextInput.h"

#include "test_harness.h"

namespace {

using ruvia::detail::Http1ServerRequestParseFailureSource;
using ruvia::detail::shouldDropInvalidCleartextHttp1Input;

}  // namespace

RUVIA_TEST(drop_invalid_cleartext_only_for_request_line_errors) {
    RUVIA_CHECK(!shouldDropInvalidCleartextHttp1Input(
        "blah blah\r\n", Http1ServerRequestParseFailureSource::kMessage));
}

RUVIA_TEST(drop_invalid_cleartext_by_version_token) {
    RUVIA_CHECK(shouldDropInvalidCleartextHttp1Input(
        "FOO /path GARBAGE\r\n", Http1ServerRequestParseFailureSource::kRequestLine));
    RUVIA_CHECK(shouldDropInvalidCleartextHttp1Input(
        "random bytes here\r\n", Http1ServerRequestParseFailureSource::kRequestLine));

    RUVIA_CHECK(!shouldDropInvalidCleartextHttp1Input(
        "XX /p HTTP/1.1\r\n", Http1ServerRequestParseFailureSource::kRequestLine));
    RUVIA_CHECK(!shouldDropInvalidCleartextHttp1Input(
        "PRI * HTTP/2.0\r\n", Http1ServerRequestParseFailureSource::kRequestLine));

    RUVIA_CHECK(!shouldDropInvalidCleartextHttp1Input(
        "no-line-break", Http1ServerRequestParseFailureSource::kRequestLine));
    RUVIA_CHECK(!shouldDropInvalidCleartextHttp1Input(
        "singletoken\r\n", Http1ServerRequestParseFailureSource::kRequestLine));
}
