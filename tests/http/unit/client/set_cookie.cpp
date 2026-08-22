#include <chrono>
#include <concepts>
#include <cstdint>
#include <ctime>
#include <limits>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "ruvia/http/HttpCache.h"
#include "ruvia/http/HttpSetCookie.h"
#include "test_harness.h"

namespace {

template <typename Input>
concept CanParseSetCookie = requires(Input&& input) {
    ruvia::parseSetCookie(std::forward<Input>(input));
};

template <typename T>
concept HasSetCookiePublicField = requires(T& cookie) { cookie.name = std::string_view{}; } ||
    requires(T& cookie) { cookie.value = std::string_view{}; } ||
    requires(T& cookie) { cookie.path = std::string_view{}; } ||
    requires(T& cookie) { cookie.domain = std::string_view{}; } ||
    requires(T& cookie) { cookie.expires = std::optional<std::time_t>{}; } ||
    requires(T& cookie) { cookie.maxAgeSeconds = std::optional<std::int64_t>{}; } ||
    requires(T& cookie) { cookie.secure = true; } ||
    requires(T& cookie) { cookie.hasPathAttribute = true; } ||
    requires(T& cookie) { cookie.sameSiteNone = true; };

static_assert(CanParseSetCookie<std::string&>);
static_assert(CanParseSetCookie<const std::string&>);
static_assert(CanParseSetCookie<std::pmr::string&>);
static_assert(CanParseSetCookie<std::string_view>);
static_assert(!CanParseSetCookie<std::string>);
static_assert(!CanParseSetCookie<const std::string>);
static_assert(!CanParseSetCookie<std::pmr::string>);
static_assert(std::same_as<std::underlying_type_t<ruvia::HttpSetCookieAttribute>, std::uint8_t>);
static_assert(std::same_as<decltype(std::declval<const ruvia::HttpSetCookieView&>().name()), std::string_view>);
static_assert(std::same_as<decltype(std::declval<const ruvia::HttpSetCookieView&>().maxAgeSeconds()), std::optional<std::int64_t>>);
static_assert(std::same_as<decltype(std::declval<const ruvia::HttpSetCookieView&>().has(ruvia::HttpSetCookieAttribute::kSecure)), bool>);
static_assert(!HasSetCookiePublicField<ruvia::HttpSetCookieView>);

}  // namespace

RUVIA_TEST(set_cookie_parser_exposes_client_storage_fields) {
    const auto parsed = ruvia::parseSetCookie("sid=abc; Path=/api; Domain=.example.com; Max-Age=60; Secure; HttpOnly");
    RUVIA_CHECK(parsed.has_value());
    RUVIA_CHECK(parsed->name() == "sid");
    RUVIA_CHECK(parsed->value() == "abc");
    RUVIA_CHECK(parsed->path() == "/api");
    RUVIA_CHECK(parsed->domain() == "example.com");
    RUVIA_CHECK(parsed->maxAgeSeconds() == 60);
    RUVIA_CHECK(parsed->has(ruvia::HttpSetCookieAttribute::kSecure));
    RUVIA_CHECK(!parsed->has(static_cast<ruvia::HttpSetCookieAttribute>(
        static_cast<std::uint8_t>(ruvia::HttpSetCookieAttribute::kSecure) |
        static_cast<std::uint8_t>(ruvia::HttpSetCookieAttribute::kPath))));
}

RUVIA_TEST(set_cookie_parser_accepts_user_agent_cookie_pair_grammar) {
    const auto nameless = ruvia::parseSetCookie("sid; Path=/");
    RUVIA_CHECK(nameless.has_value());
    if (!nameless) return;
    RUVIA_CHECK(nameless->name().empty());
    RUVIA_CHECK(nameless->value() == "sid");

    const auto spacedName = ruvia::parseSetCookie("bad name=value");
    RUVIA_CHECK(spacedName.has_value());
    if (!spacedName) return;
    RUVIA_CHECK(spacedName->name() == "bad name");

    const auto spacedValue = ruvia::parseSetCookie("name=bad value");
    RUVIA_CHECK(spacedValue.has_value());
    if (!spacedValue) return;
    RUVIA_CHECK(spacedValue->value() == "bad value");

    const auto emptyName = ruvia::parseSetCookie("=value");
    RUVIA_CHECK(emptyName.has_value());
    if (!emptyName) return;
    RUVIA_CHECK(emptyName->name().empty());
    RUVIA_CHECK(emptyName->value() == "value");
}

RUVIA_TEST(set_cookie_parser_rejects_invalid_received_cookie) {
    RUVIA_CHECK(!ruvia::parseSetCookie("=").has_value());
    RUVIA_CHECK(!ruvia::parseSetCookie("name=bad\x01value").has_value());
    RUVIA_CHECK(!ruvia::parseSetCookie("name=value; Domain=bad_domain").has_value());
    RUVIA_CHECK(!ruvia::parseSetCookie("name=value; Path=/bad\tpath").has_value());

    std::string oversized(4097, 'v');
    RUVIA_CHECK(!ruvia::parseSetCookie(oversized).has_value());
}

RUVIA_TEST(set_cookie_parser_ignores_oversized_attribute_value) {
    std::string value = "sid=abc; Path=";
    value.append(1025, 'p');
    const auto parsed = ruvia::parseSetCookie(value);
    RUVIA_CHECK(parsed.has_value());
    if (!parsed) return;
    RUVIA_CHECK(parsed->path().empty());
    RUVIA_CHECK(!parsed->has(ruvia::HttpSetCookieAttribute::kPath));
}

RUVIA_TEST(set_cookie_parser_tracks_storage_security_attributes) {
    const auto parsed = ruvia::parseSetCookie(
        "sid=abc; Path=relative; SameSite=None");
    RUVIA_CHECK(parsed.has_value());
    if (!parsed) return;
    RUVIA_CHECK(parsed->has(ruvia::HttpSetCookieAttribute::kPath));
    RUVIA_CHECK(parsed->path() == "relative");
    RUVIA_CHECK(parsed->has(ruvia::HttpSetCookieAttribute::kSameSiteNone));

    const auto overridden = ruvia::parseSetCookie(
        "sid=abc; SameSite=None; SameSite=Lax");
    RUVIA_CHECK(overridden.has_value());
    if (!overridden) return;
    RUVIA_CHECK(!overridden->has(ruvia::HttpSetCookieAttribute::kSameSiteNone));
}

RUVIA_TEST(set_cookie_parser_preserves_quoted_cookie_value) {
    const auto parsed = ruvia::parseSetCookie("name=\"value\"; Path=/");
    RUVIA_CHECK(parsed.has_value());
    RUVIA_CHECK(parsed->value() == "\"value\"");
}

RUVIA_TEST(set_cookie_parser_treats_empty_domain_as_host_only) {
    const auto parsed = ruvia::parseSetCookie("sid=abc; Domain=; Path=/api");
    RUVIA_CHECK(parsed.has_value());
    RUVIA_CHECK(parsed->name() == "sid");
    RUVIA_CHECK(parsed->domain().empty());
    RUVIA_CHECK(parsed->path() == "/api");

    const auto overridesEarlierDomain = ruvia::parseSetCookie(
        "sid=abc; Domain=example.com; Domain=");
    RUVIA_CHECK(overridesEarlierDomain.has_value());
    RUVIA_CHECK(overridesEarlierDomain->domain().empty());
}

RUVIA_TEST(set_cookie_parser_saturates_max_age_overflow) {
    const auto positive = ruvia::parseSetCookie(
        "sid=abc; Max-Age=999999999999999999999999999999999999");
    RUVIA_CHECK(positive.has_value());
    RUVIA_CHECK(positive->maxAgeSeconds() ==
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::days(400)).count());

    const auto negative = ruvia::parseSetCookie(
        "sid=abc; Max-Age=-999999999999999999999999999999999999");
    RUVIA_CHECK(negative.has_value());
    RUVIA_CHECK(negative->maxAgeSeconds() == std::numeric_limits<std::int64_t>::min());
}

RUVIA_TEST(set_cookie_parser_ignores_invalid_later_expires_attribute) {
    const auto parsed = ruvia::parseSetCookie(
        "sid=abc; Expires=Sun, 06 Nov 1994 08:49:37 GMT; Expires=not-a-date");
    RUVIA_CHECK(parsed.has_value());
    RUVIA_CHECK(parsed->expires().has_value());
}

RUVIA_TEST(set_cookie_parser_uses_cookie_date_token_grammar) {
    const auto hyphenated = ruvia::parseSetCookie(
        "sid=abc; Expires=Wed, 09-Jun-2021 10:18:14 GMT");
    RUVIA_CHECK(hyphenated.has_value());
    if (!hyphenated) return;
    RUVIA_CHECK(hyphenated->expires() ==
        ruvia::parseHttpDate("Wed, 09 Jun 2021 10:18:14 GMT"));

    const auto shortYear = ruvia::parseSetCookie(
        "sid=abc; Expires=Thursday, 01-Jan-70 00:00:00 GMT");
    RUVIA_CHECK(shortYear.has_value());
    if (!shortYear) return;
    RUVIA_CHECK(shortYear->expires() ==
        ruvia::parseHttpDate("Thu, 01 Jan 1970 00:00:00 GMT"));
}
