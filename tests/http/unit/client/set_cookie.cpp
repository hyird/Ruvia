#include <chrono>
#include <limits>

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

RUVIA_TEST(set_cookie_parser_preserves_quoted_cookie_value) {
    const auto parsed = ruvia::parseSetCookie("name=\"value\"; Path=/");
    RUVIA_CHECK(parsed.has_value());
    RUVIA_CHECK(parsed->value == "\"value\"");
}

RUVIA_TEST(set_cookie_parser_treats_empty_domain_as_host_only) {
    const auto parsed = ruvia::parseSetCookie("sid=abc; Domain=; Path=/api");
    RUVIA_CHECK(parsed.has_value());
    RUVIA_CHECK(parsed->name == "sid");
    RUVIA_CHECK(parsed->domain.empty());
    RUVIA_CHECK(parsed->path == "/api");

    const auto overridesEarlierDomain = ruvia::parseSetCookie(
        "sid=abc; Domain=example.com; Domain=");
    RUVIA_CHECK(overridesEarlierDomain.has_value());
    RUVIA_CHECK(overridesEarlierDomain->domain.empty());
}

RUVIA_TEST(set_cookie_parser_saturates_max_age_overflow) {
    const auto positive = ruvia::parseSetCookie(
        "sid=abc; Max-Age=999999999999999999999999999999999999");
    RUVIA_CHECK(positive.has_value());
    RUVIA_CHECK(positive->maxAgeSeconds ==
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::days(400)).count());

    const auto negative = ruvia::parseSetCookie(
        "sid=abc; Max-Age=-999999999999999999999999999999999999");
    RUVIA_CHECK(negative.has_value());
    RUVIA_CHECK(negative->maxAgeSeconds == std::numeric_limits<std::int64_t>::min());
}

RUVIA_TEST(set_cookie_parser_ignores_invalid_later_expires_attribute) {
    const auto parsed = ruvia::parseSetCookie(
        "sid=abc; Expires=Sun, 06 Nov 1994 08:49:37 GMT; Expires=not-a-date");
    RUVIA_CHECK(parsed.has_value());
    RUVIA_CHECK(parsed->expires.has_value());
}
