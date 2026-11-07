#include <gtest/gtest.h>

#include <string_view>

#include "ruvia/http/http_header.h"
#include "ruvia/http/http_known_method.h"
#include "ruvia/http/http_protocol_error.h"
#include "ruvia/http/http_request_content_semantics.h"

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
