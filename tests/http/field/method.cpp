#include "test_harness.h"

#include <string_view>

#include "ruvia/http/HttpHeader.h"
#include "ruvia/http/HttpKnownMethod.h"
#include "ruvia/http/HttpParseError.h"

namespace {

using ruvia::classifyHttpMethod;
using ruvia::HttpKnownMethod;
using ruvia::HttpParseError;
using ruvia::httpParseProtocolError;
using ruvia::isValidHttpHeaderName;
using ruvia::isValidHttpHeaderValue;
using ruvia::isValidHttpMethodToken;
using ruvia::isValidHttpStatusText;
using ruvia::knownHttpMethodToken;

}  // namespace

// The method vocabulary: recognising tokens, spelling them back, and the
// safe and idempotent properties a recipient acts on.

RUVIA_TEST(http_method_parsing_is_exact_and_case_sensitive) {
    RUVIA_CHECK(classifyHttpMethod("GET") == HttpKnownMethod::kGet);
    RUVIA_CHECK(classifyHttpMethod("POST") == HttpKnownMethod::kPost);
    RUVIA_CHECK(classifyHttpMethod("PUT") == HttpKnownMethod::kPut);
    RUVIA_CHECK(classifyHttpMethod("DELETE") == HttpKnownMethod::kDelete);
    RUVIA_CHECK(classifyHttpMethod("PATCH") == HttpKnownMethod::kPatch);
    RUVIA_CHECK(classifyHttpMethod("HEAD") == HttpKnownMethod::kHead);
    RUVIA_CHECK(classifyHttpMethod("OPTIONS") == HttpKnownMethod::kOptions);
    RUVIA_CHECK(classifyHttpMethod("CONNECT") == HttpKnownMethod::kConnect);
    // Methods are case-sensitive (RFC 9110 section 9.1); anything outside the
    // framework's fixed semantic set remains an unknown classification.
    RUVIA_CHECK(classifyHttpMethod("get") == HttpKnownMethod::kUnknown);
    RUVIA_CHECK(classifyHttpMethod("Get") == HttpKnownMethod::kUnknown);
    RUVIA_CHECK(classifyHttpMethod("FOO") == HttpKnownMethod::kUnknown);
    RUVIA_CHECK(classifyHttpMethod("") == HttpKnownMethod::kUnknown);
    RUVIA_CHECK(classifyHttpMethod("GETX") == HttpKnownMethod::kUnknown);
}

RUVIA_TEST(http_method_token_validation_is_separate_from_classification) {
    RUVIA_CHECK(isValidHttpMethodToken("PROPFIND"));
    RUVIA_CHECK(isValidHttpMethodToken("get"));
    RUVIA_CHECK(isValidHttpMethodToken("M-SEARCH"));
    RUVIA_CHECK(!isValidHttpMethodToken(""));
    RUVIA_CHECK(!isValidHttpMethodToken("BAD METHOD"));
    RUVIA_CHECK(!isValidHttpMethodToken("BAD(METHOD"));
    RUVIA_CHECK(!isValidHttpMethodToken(std::string_view("BAD\x01METHOD", 10)));
}

RUVIA_TEST(http_method_safety_and_idempotency_follow_wire_semantics) {
    for (const std::string_view method : {"GET", "HEAD", "OPTIONS", "TRACE"}) {
        RUVIA_CHECK(ruvia::isHttpMethodSafe(method));
        RUVIA_CHECK(ruvia::isHttpMethodIdempotent(method));
    }
    for (const std::string_view method : {"PUT", "DELETE"}) {
        RUVIA_CHECK(!ruvia::isHttpMethodSafe(method));
        RUVIA_CHECK(ruvia::isHttpMethodIdempotent(method));
    }
    for (const std::string_view method : {"POST", "PATCH", "CONNECT", "CUSTOM", "get"}) {
        RUVIA_CHECK(!ruvia::isHttpMethodSafe(method));
        RUVIA_CHECK(!ruvia::isHttpMethodIdempotent(method));
    }
}

RUVIA_TEST(http_known_method_token_round_trips) {
    const HttpKnownMethod methods[] = {HttpKnownMethod::kGet, HttpKnownMethod::kPost, HttpKnownMethod::kPut, HttpKnownMethod::kDelete, HttpKnownMethod::kPatch, HttpKnownMethod::kHead, HttpKnownMethod::kOptions, HttpKnownMethod::kConnect};
    for (const auto method : methods) {
        RUVIA_CHECK(classifyHttpMethod(knownHttpMethodToken(method)) == method);
    }
    // An unknown classification has no canonical wire spelling. Callers that
    // need it must retain the exact token instead of manufacturing "UNKNOWN".
    RUVIA_CHECK(knownHttpMethodToken(HttpKnownMethod::kUnknown).empty());
}
