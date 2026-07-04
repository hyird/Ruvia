#include "test_harness.h"

#include <stdexcept>
#include <string_view>

#include "ruvia/http/Cookies.h"
#include "http/CookieValidation.h"

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

RUVIA_TEST(cookie_samesite_accepts_canonical) {
    ruvia::CookieOptions strict;
    strict.sameSite = "Strict";
    RUVIA_CHECK(!rejects(strict));

    ruvia::CookieOptions lax;
    lax.sameSite = "Lax";
    RUVIA_CHECK(!rejects(lax));

    ruvia::CookieOptions none;
    none.sameSite = "None";
    none.secure = true;
    RUVIA_CHECK(!rejects(none));
}

RUVIA_TEST(cookie_samesite_empty_is_allowed) {
    ruvia::CookieOptions options;  // no SameSite attribute at all
    RUVIA_CHECK(!rejects(options));
}

RUVIA_TEST(cookie_samesite_is_case_insensitive) {
    ruvia::CookieOptions lower;
    lower.sameSite = "lax";
    RUVIA_CHECK(!rejects(lower));
    // The serialized form is canonicalized regardless of input case.
    RUVIA_CHECK_EQ(ruvia::detail::cookieSameSiteToken("lax"), std::string_view("Lax"));
    RUVIA_CHECK_EQ(ruvia::detail::cookieSameSiteToken("STRICT"), std::string_view("Strict"));
    RUVIA_CHECK_EQ(ruvia::detail::cookieSameSiteToken("nOnE"), std::string_view("None"));
    RUVIA_CHECK(ruvia::detail::cookieSameSiteToken("laxx").empty());
}

RUVIA_TEST(cookie_samesite_rejects_typos) {
    ruvia::CookieOptions typo;
    typo.sameSite = "Laxx";
    RUVIA_CHECK(rejects(typo));

    ruvia::CookieOptions bogus;
    bogus.sameSite = "bogus";
    RUVIA_CHECK(rejects(bogus));
}

RUVIA_TEST(cookie_samesite_none_requires_secure) {
    ruvia::CookieOptions insecureNone;
    insecureNone.sameSite = "None";
    insecureNone.secure = false;
    RUVIA_CHECK(rejects(insecureNone));  // RFC 6265bis §5.5

    ruvia::CookieOptions secureNone;
    secureNone.sameSite = "None";
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
}

RUVIA_TEST(cookie_priority_token_canonicalizes) {
    using ruvia::detail::cookiePriorityToken;
    RUVIA_CHECK_EQ(cookiePriorityToken("Low"), std::string_view("Low"));
    RUVIA_CHECK_EQ(cookiePriorityToken("medium"), std::string_view("Medium"));  // case-insensitive
    RUVIA_CHECK_EQ(cookiePriorityToken("HIGH"), std::string_view("High"));
    RUVIA_CHECK(cookiePriorityToken("").empty());
    RUVIA_CHECK(cookiePriorityToken("Urgent").empty());  // not a valid priority
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
    badPriority.priority = "Urgent";
    RUVIA_CHECK(rejectsCookie("sid", "value", badPriority));
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

RUVIA_TEST(cookie_max_age_capped_at_400_days) {
    ruvia::CookieOptions atCap;
    atCap.maxAge = 34560000;  // exactly 400 days is allowed
    RUVIA_CHECK(!rejects(atCap));

    ruvia::CookieOptions overCap;
    overCap.maxAge = 34560001;
    RUVIA_CHECK(rejects(overCap));
}
