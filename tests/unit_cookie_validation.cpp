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
