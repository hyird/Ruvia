#include "test_harness.h"

#include <string_view>

#include "ruvia/http/HttpCommon.h"

namespace {

using ruvia::HttpMethod;
using ruvia::isValidHttpHeaderName;
using ruvia::isValidHttpHeaderValue;
using ruvia::methodName;
using ruvia::parseMethod;

}  // namespace

RUVIA_TEST(http_header_name_validation) {
    RUVIA_CHECK(isValidHttpHeaderName("Content-Type"));
    RUVIA_CHECK(isValidHttpHeaderName("X-Custom-Header"));
    RUVIA_CHECK(isValidHttpHeaderName("a"));
    RUVIA_CHECK(!isValidHttpHeaderName(""));            // empty name is invalid
    RUVIA_CHECK(!isValidHttpHeaderName("X Y"));         // space is not a token char
    RUVIA_CHECK(!isValidHttpHeaderName("X:Y"));         // ':' is a separator
    RUVIA_CHECK(!isValidHttpHeaderName("(comment)"));   // '(' ')' are separators
    RUVIA_CHECK(!isValidHttpHeaderName("a@b"));         // '@' is a separator
    RUVIA_CHECK(!isValidHttpHeaderName(std::string_view("a\rb", 3)));  // control char
}

RUVIA_TEST(http_header_value_rejects_injection) {
    RUVIA_CHECK(isValidHttpHeaderValue("text/html; charset=utf-8"));  // spaces allowed
    RUVIA_CHECK(isValidHttpHeaderValue(""));                          // empty value is valid
    // CR, LF and NUL must be rejected -- these are response/header-injection bytes.
    RUVIA_CHECK(!isValidHttpHeaderValue(std::string_view("a\rb", 3)));
    RUVIA_CHECK(!isValidHttpHeaderValue(std::string_view("a\nb", 3)));
    RUVIA_CHECK(!isValidHttpHeaderValue(std::string_view("a\r\nInjected: x", 14)));
    RUVIA_CHECK(!isValidHttpHeaderValue(std::string_view("a\0b", 3)));
}

RUVIA_TEST(http_method_parsing_is_exact_and_case_sensitive) {
    RUVIA_CHECK(parseMethod("GET") == HttpMethod::kGet);
    RUVIA_CHECK(parseMethod("POST") == HttpMethod::kPost);
    RUVIA_CHECK(parseMethod("PUT") == HttpMethod::kPut);
    RUVIA_CHECK(parseMethod("DELETE") == HttpMethod::kDelete);
    RUVIA_CHECK(parseMethod("PATCH") == HttpMethod::kPatch);
    RUVIA_CHECK(parseMethod("HEAD") == HttpMethod::kHead);
    RUVIA_CHECK(parseMethod("OPTIONS") == HttpMethod::kOptions);
    RUVIA_CHECK(parseMethod("CONNECT") == HttpMethod::kConnect);
    // Methods are case-sensitive (RFC 7231 §4.1); anything else is unknown.
    RUVIA_CHECK(parseMethod("get") == HttpMethod::kUnknown);
    RUVIA_CHECK(parseMethod("Get") == HttpMethod::kUnknown);
    RUVIA_CHECK(parseMethod("FOO") == HttpMethod::kUnknown);
    RUVIA_CHECK(parseMethod("") == HttpMethod::kUnknown);
    RUVIA_CHECK(parseMethod("GETX") == HttpMethod::kUnknown);
}

RUVIA_TEST(http_method_name_round_trips) {
    const HttpMethod methods[] = {
        HttpMethod::kGet, HttpMethod::kPost, HttpMethod::kPut, HttpMethod::kDelete,
        HttpMethod::kPatch, HttpMethod::kHead, HttpMethod::kOptions, HttpMethod::kConnect};
    for (const auto method : methods) {
        RUVIA_CHECK(parseMethod(methodName(method)) == method);
    }
    RUVIA_CHECK_EQ(methodName(HttpMethod::kUnknown), std::string_view("UNKNOWN"));
}
