#include <chrono>

#include "ruvia/http/HttpSetCookie.h"
#include "test_harness.h"

RUVIA_TEST(set_cookie_parser_exposes_client_storage_fields) {
    const auto parsed = ruvia::parseSetCookie("sid=abc; Path=/api; Domain=.example.com; Max-Age=60; Secure; HttpOnly");
    RUVIA_CHECK(parsed.has_value());
    RUVIA_CHECK(parsed->name == "sid");
    RUVIA_CHECK(parsed->value == "abc");
    RUVIA_CHECK(parsed->path == "/api");
    RUVIA_CHECK(parsed->domain == "example.com");
    RUVIA_CHECK(parsed->maxAgeSeconds == 60);
    RUVIA_CHECK(parsed->secure);
}

RUVIA_TEST(set_cookie_parser_rejects_invalid_cookie_pair) {
    RUVIA_CHECK(!ruvia::parseSetCookie("sid; Path=/").has_value());
    RUVIA_CHECK(!ruvia::parseSetCookie("bad name=value").has_value());
    RUVIA_CHECK(!ruvia::parseSetCookie("name=bad value").has_value());
    RUVIA_CHECK(!ruvia::parseSetCookie("=value").has_value());
    RUVIA_CHECK(!ruvia::parseSetCookie("name=value; Domain=bad_domain").has_value());
}

RUVIA_TEST(set_cookie_parser_accepts_quoted_cookie_value) {
    const auto parsed = ruvia::parseSetCookie("name=\"value\"; Path=/");
    RUVIA_CHECK(parsed.has_value());
    RUVIA_CHECK(parsed->value == "value");
}
