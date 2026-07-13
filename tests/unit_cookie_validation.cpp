#include "test_harness.h"

#include <chrono>
#include <concepts>
#include <optional>
#include <stdexcept>
#include <string_view>

#include "ruvia/http/Cookies.h"
#include "ruvia/http/detail/CookieValidation.h"

namespace {

// True if validateCookie rejects the options (throws invalid_argument).
bool rejects(const ruvia::CookieOptions& options) {
    try {
        ruvia::detail::validateCookie("sid", "value", options);
        return false;
    } catch (const std::invalid_argument&) {
        return true;
    }
}

}  // namespace

static_assert(std::same_as<
    decltype(ruvia::CookieOptions{}.sameSite),
    std::optional<ruvia::CookieSameSite>>);
static_assert(std::same_as<
    decltype(ruvia::CookieOptions{}.priority),
    std::optional<ruvia::CookiePriority>>);
static_assert(std::same_as<
    decltype(ruvia::CookieOptions{}.prefix),
    std::optional<ruvia::CookiePrefix>>);
static_assert(std::same_as<
    decltype(ruvia::CookieOptions{}.maxAge),
    std::optional<std::chrono::seconds>>);

RUVIA_TEST(cookie_samesite_enum_maps_to_wire_tokens) {
    ruvia::CookieOptions strict;
    strict.sameSite = ruvia::CookieSameSite::kStrict;
    RUVIA_CHECK(!rejects(strict));

    ruvia::CookieOptions lax;
    lax.sameSite = ruvia::CookieSameSite::kLax;
    RUVIA_CHECK(!rejects(lax));

    ruvia::CookieOptions none;
    none.sameSite = ruvia::CookieSameSite::kNone;
    none.secure = true;
    RUVIA_CHECK(!rejects(none));

    RUVIA_CHECK(!ruvia::CookieOptions{}.sameSite.has_value());
    RUVIA_CHECK_EQ(
        ruvia::detail::cookieSameSiteToken(ruvia::CookieSameSite::kStrict),
        std::string_view("Strict"));
    RUVIA_CHECK_EQ(
        ruvia::detail::cookieSameSiteToken(ruvia::CookieSameSite::kLax),
        std::string_view("Lax"));
    RUVIA_CHECK_EQ(
        ruvia::detail::cookieSameSiteToken(ruvia::CookieSameSite::kNone),
        std::string_view("None"));
}

RUVIA_TEST(cookie_samesite_none_requires_secure) {
    ruvia::CookieOptions insecureNone;
    insecureNone.sameSite = ruvia::CookieSameSite::kNone;
    insecureNone.secure = false;
    RUVIA_CHECK(rejects(insecureNone));  // RFC 6265bis §5.5

    ruvia::CookieOptions secureNone;
    secureNone.sameSite = ruvia::CookieSameSite::kNone;
    secureNone.secure = true;
    RUVIA_CHECK(!rejects(secureNone));
}

RUVIA_TEST(cookie_value_char_validation) {
    using ruvia::detail::isValidCookieValue;
    RUVIA_CHECK(isValidCookieValue("abc123"));
    RUVIA_CHECK(isValidCookieValue("a-b_c.d~e"));
    RUVIA_CHECK(isValidCookieValue(""));  // an empty value is valid
    RUVIA_CHECK(!isValidCookieValue("a b"));   // space
    RUVIA_CHECK(!isValidCookieValue("a;b"));   // ';' would inject an attribute
    RUVIA_CHECK(!isValidCookieValue("a,b"));   // ','
    RUVIA_CHECK(!isValidCookieValue("a\"b"));  // '"'
    RUVIA_CHECK(!isValidCookieValue("a\\b"));  // backslash
    RUVIA_CHECK(!isValidCookieValue(std::string_view("a\rb", 3)));    // CR
    RUVIA_CHECK(!isValidCookieValue(std::string_view("a\x7f" "b", 3)));  // DEL
}

RUVIA_TEST(cookie_attribute_char_validation) {
    using ruvia::detail::isValidCookieAttribute;
    RUVIA_CHECK(isValidCookieAttribute("/path/to"));
    RUVIA_CHECK(isValidCookieAttribute("example.com"));
    RUVIA_CHECK(isValidCookieAttribute(""));
    RUVIA_CHECK(!isValidCookieAttribute("a;b"));  // ';' would inject another attribute
    RUVIA_CHECK(!isValidCookieAttribute(std::string_view("a\rb", 3)));  // CR (header injection)
    RUVIA_CHECK(!isValidCookieAttribute(std::string_view("a\nb", 3)));  // LF
    RUVIA_CHECK(!isValidCookieAttribute(std::string_view("a\0b", 3)));  // NUL
    // Non-CR/LF control bytes are also forbidden HTTP field-value octets (RFC 9110
    // 5.5) and previously slipped through into the raw Set-Cookie value.
    RUVIA_CHECK(!isValidCookieAttribute("a\x0b" "b"));  // vertical tab
    RUVIA_CHECK(!isValidCookieAttribute("a\x0c" "b"));  // form feed
    RUVIA_CHECK(!isValidCookieAttribute("a\x01" "b"));  // SOH
    RUVIA_CHECK(!isValidCookieAttribute("a\x7f" "b"));  // DEL
    // But the legitimate field-value octets a path/domain may need stay allowed:
    // SP, HTAB, and obs-text (0x80-0xFF) are valid HTTP field-value bytes.
    RUVIA_CHECK(isValidCookieAttribute("/a path"));         // SP
    RUVIA_CHECK(isValidCookieAttribute("a\tb"));            // HTAB
    RUVIA_CHECK(isValidCookieAttribute("caf\xc3\xa9.com"));  // UTF-8 obs-text
}

RUVIA_TEST(cookie_priority_enum_maps_to_wire_tokens) {
    using ruvia::detail::cookiePriorityToken;
    RUVIA_CHECK(!ruvia::CookieOptions{}.priority.has_value());
    RUVIA_CHECK_EQ(
        cookiePriorityToken(ruvia::CookiePriority::kLow),
        std::string_view("Low"));
    RUVIA_CHECK_EQ(
        cookiePriorityToken(ruvia::CookiePriority::kMedium),
        std::string_view("Medium"));
    RUVIA_CHECK_EQ(
        cookiePriorityToken(ruvia::CookiePriority::kHigh),
        std::string_view("High"));
}

RUVIA_TEST(cookie_validation_rejects_injection_and_bad_options) {
    const auto rejectsCookie =
        [](std::string_view name, std::string_view value, const ruvia::CookieOptions& options) {
            try {
                ruvia::detail::validateCookie(name, value, options);
                return false;
            } catch (const std::invalid_argument&) {
                return true;
            }
        };
    const ruvia::CookieOptions clean;
    RUVIA_CHECK(!rejectsCookie("sid", "value", clean));
    RUVIA_CHECK(rejectsCookie("bad name", "value", clean));  // space -> not a token name
    RUVIA_CHECK(rejectsCookie("sid", "a;b", clean));         // ';' in value
    ruvia::CookieOptions badPath;
    badPath.path = "a;b";
    RUVIA_CHECK(rejectsCookie("sid", "value", badPath));     // ';' in path attribute
    ruvia::CookieOptions badPriority;
    badPriority.priority = static_cast<ruvia::CookiePriority>(255);
    RUVIA_CHECK(rejectsCookie("sid", "value", badPriority));
    ruvia::CookieOptions badSameSite;
    badSameSite.sameSite = static_cast<ruvia::CookieSameSite>(255);
    RUVIA_CHECK(rejectsCookie("sid", "value", badSameSite));
    ruvia::CookieOptions badPrefix;
    badPrefix.prefix = static_cast<ruvia::CookiePrefix>(255);
    RUVIA_CHECK(rejectsCookie("sid", "value", badPrefix));
    ruvia::CookieOptions partitioned;
    partitioned.partitioned = true;  // partitioned requires Secure
    RUVIA_CHECK(rejectsCookie("sid", "value", partitioned));
}

RUVIA_TEST(cookie_secure_prefix_requires_secure) {
    ruvia::CookieOptions secured;
    secured.prefix = ruvia::CookiePrefix::kSecure;
    secured.secure = true;
    RUVIA_CHECK(!rejects(secured));

    ruvia::CookieOptions insecure;
    insecure.prefix = ruvia::CookiePrefix::kSecure;
    insecure.secure = false;
    RUVIA_CHECK(rejects(insecure));  // __Secure- requires Secure
}

RUVIA_TEST(cookie_host_prefix_requires_secure_root_path_no_domain) {
    // __Host- is the strictest prefix (RFC 6265bis 4.1.3.2).
    ruvia::CookieOptions valid;
    valid.prefix = ruvia::CookiePrefix::kHost;
    valid.secure = true;  // path defaults to "/", domain is empty
    RUVIA_CHECK(!rejects(valid));

    ruvia::CookieOptions notSecure = valid;
    notSecure.secure = false;
    RUVIA_CHECK(rejects(notSecure));

    ruvia::CookieOptions subPath = valid;
    subPath.path = "/sub";
    RUVIA_CHECK(rejects(subPath));  // Path must be exactly "/"

    ruvia::CookieOptions withDomain = valid;
    withDomain.domain = "example.com";
    RUVIA_CHECK(rejects(withDomain));  // Domain must be absent
}

RUVIA_TEST(cookie_literal_prefix_name_enforces_requirements) {
    const auto rejectsWithName = [](std::string_view name, const ruvia::CookieOptions& options) {
        try {
            ruvia::detail::validateCookie(name, "value", options);
            return false;
        } catch (const std::exception&) {
            return true;
        }
    };

    // RFC 6265bis §4.1.3: the prefix rules apply to the cookie's actual wire name,
    // so a hand-typed "__Host-"/"__Secure-" name (with no CookiePrefix) must be
    // enforced like the enum, case-insensitively -- not silently shipped for the
    // browser to drop. Keying only on the enum let these through.
    ruvia::CookieOptions insecure;  // secure defaults to false
    RUVIA_CHECK(rejectsWithName("__Secure-tok", insecure));    // __Secure- requires Secure
    RUVIA_CHECK(rejectsWithName("__secure-tok", insecure));    // matched case-insensitively

    ruvia::CookieOptions hostBadDomain;
    hostBadDomain.secure = true;
    hostBadDomain.domain = "example.com";  // __Host- forbids Domain
    RUVIA_CHECK(rejectsWithName("__Host-sid", hostBadDomain));
    RUVIA_CHECK(rejectsWithName("__HOST-sid", hostBadDomain));  // case-insensitive

    // A literal-prefixed name that meets the constraints is accepted.
    ruvia::CookieOptions okSecure;
    okSecure.secure = true;
    RUVIA_CHECK(!rejectsWithName("__Secure-tok", okSecure));
    ruvia::CookieOptions okHost;
    okHost.secure = true;  // path defaults to "/", domain empty
    RUVIA_CHECK(!rejectsWithName("__Host-sid", okHost));

    // A name that only resembles a prefix (missing the trailing '-') is unaffected.
    ruvia::CookieOptions plain;
    RUVIA_CHECK(!rejectsWithName("__Secured", plain));
    RUVIA_CHECK(!rejectsWithName("__Hostname", plain));
}

RUVIA_TEST(cookie_max_age_capped_at_400_days) {
    ruvia::CookieOptions atCap;
    atCap.maxAge = std::chrono::seconds(34560000);  // exactly 400 days is allowed
    RUVIA_CHECK(!rejects(atCap));

    ruvia::CookieOptions overCap;
    overCap.maxAge = std::chrono::seconds(34560001);
    RUVIA_CHECK(rejects(overCap));

    ruvia::CookieOptions deletion;
    deletion.maxAge = std::chrono::seconds(0);
    RUVIA_CHECK(!rejects(deletion));

    ruvia::CookieOptions negative;
    negative.maxAge = std::chrono::seconds(-1);
    RUVIA_CHECK(rejects(negative));
}
