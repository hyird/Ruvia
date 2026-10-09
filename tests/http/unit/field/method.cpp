#include <string_view>

#include "ruvia/http/http_header.h"
#include "ruvia/http/http_known_method.h"
#include "ruvia/http/http_parse_error.h"
#include "ruvia/http/http_request_content_semantics.h"

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

// The method vocabulary: recognising tokens, spelling them back, and the
// safe and idempotent properties a recipient acts on.

RUVIA_TEST(http_request_method_content_semantics_are_shared_by_client_and_server) {
    using ruvia::http_request_content_semantics;
    RUVIA_CHECK(ruvia::http_request_content_semantics("CONNECT") ==
                http_request_content_semantics::forbidden);
    RUVIA_CHECK(ruvia::http_request_content_semantics("TRACE") ==
                http_request_content_semantics::forbidden);
    RUVIA_CHECK(ruvia::http_request_content_semantics("OPTIONS") ==
                http_request_content_semantics::content_type_required);
    RUVIA_CHECK(ruvia::http_request_content_semantics("POST") ==
                http_request_content_semantics::no_additional_requirements);
}

RUVIA_TEST(http_method_parsing_is_exact_and_case_sensitive) {
    RUVIA_CHECK(classify_http_method("GET") == http_known_method::get);
    RUVIA_CHECK(classify_http_method("POST") == http_known_method::post);
    RUVIA_CHECK(classify_http_method("PUT") == http_known_method::put);
    RUVIA_CHECK(classify_http_method("DELETE") == http_known_method::delete_value);
    RUVIA_CHECK(classify_http_method("PATCH") == http_known_method::patch);
    RUVIA_CHECK(classify_http_method("HEAD") == http_known_method::head);
    RUVIA_CHECK(classify_http_method("OPTIONS") == http_known_method::options);
    RUVIA_CHECK(classify_http_method("CONNECT") == http_known_method::connect);
    // Methods are case-sensitive (RFC 9110 section 9.1); anything outside the
    // framework's fixed semantic set remains an unknown classification.
    RUVIA_CHECK(classify_http_method("get") == http_known_method::unknown);
    RUVIA_CHECK(classify_http_method("Get") == http_known_method::unknown);
    RUVIA_CHECK(classify_http_method("FOO") == http_known_method::unknown);
    RUVIA_CHECK(classify_http_method("") == http_known_method::unknown);
    RUVIA_CHECK(classify_http_method("GETX") == http_known_method::unknown);
}

RUVIA_TEST(http_method_token_validation_is_separate_from_classification) {
    RUVIA_CHECK(is_valid_http_method_token("PROPFIND"));
    RUVIA_CHECK(is_valid_http_method_token("get"));
    RUVIA_CHECK(is_valid_http_method_token("M-SEARCH"));
    RUVIA_CHECK(!is_valid_http_method_token(""));
    RUVIA_CHECK(!is_valid_http_method_token("BAD METHOD"));
    RUVIA_CHECK(!is_valid_http_method_token("BAD(METHOD"));
    RUVIA_CHECK(!is_valid_http_method_token(std::string_view("BAD\x01METHOD", 10)));
}

RUVIA_TEST(http_method_safety_and_idempotency_follow_wire_semantics) {
    for (const std::string_view method : {"GET", "HEAD", "OPTIONS", "TRACE"}) {
        RUVIA_CHECK(ruvia::is_http_method_safe(method));
        RUVIA_CHECK(ruvia::is_http_method_idempotent(method));
    }
    for (const std::string_view method : {"PUT", "DELETE"}) {
        RUVIA_CHECK(!ruvia::is_http_method_safe(method));
        RUVIA_CHECK(ruvia::is_http_method_idempotent(method));
    }
    for (const std::string_view method : {"POST", "PATCH", "CONNECT", "CUSTOM", "get"}) {
        RUVIA_CHECK(!ruvia::is_http_method_safe(method));
        RUVIA_CHECK(!ruvia::is_http_method_idempotent(method));
    }
}

RUVIA_TEST(http_known_method_token_round_trips) {
    const http_known_method methods[] = {http_known_method::get, http_known_method::post,
        http_known_method::put, http_known_method::delete_value, http_known_method::patch,
        http_known_method::head, http_known_method::options, http_known_method::connect};
    for (const auto method : methods) {
        RUVIA_CHECK(classify_http_method(known_http_method_token(method)) == method);
    }
    // An unknown classification has no canonical wire spelling. Callers that
    // need it must retain the exact token instead of manufacturing "UNKNOWN".
    RUVIA_CHECK(known_http_method_token(http_known_method::unknown).empty());
}
