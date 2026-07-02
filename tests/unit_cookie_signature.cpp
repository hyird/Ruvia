#include "test_harness.h"

#include <string>
#include <string_view>

#include "http/CookieSignature.h"

namespace {

std::string sign(std::string_view secret, std::string_view value) {
    std::string out(ruvia::detail::kCookieSignatureSize, '\0');
    ruvia::detail::writeCookieSignature(out.data(), secret, value);
    return out;
}

}  // namespace

RUVIA_TEST(cookie_signature_is_deterministic) {
    const std::string a = sign("s3cr3t", "user=42");
    const std::string b = sign("s3cr3t", "user=42");
    RUVIA_CHECK_EQ(a.size(), ruvia::detail::kCookieSignatureSize);
    RUVIA_CHECK_EQ(a, b);
}

RUVIA_TEST(cookie_signature_depends_on_value_and_secret) {
    const std::string base = sign("secret", "user=42");
    RUVIA_CHECK(base != sign("secret", "user=43"));   // value change
    RUVIA_CHECK(base != sign("secret2", "user=42"));  // secret change
}

RUVIA_TEST(cookie_signature_known_vector) {
    // HMAC-SHA256(key="key", msg="value") base64, verified via:
    //   printf value | openssl dgst -sha256 -hmac key -binary | base64
    const std::string s = sign("key", "value");
    RUVIA_CHECK_EQ(s, std::string("kPv88V50o2uJ29sqch2a7P/f3dxcg+J/dZJZT3GTJIE="));
}

RUVIA_TEST(cookie_signature_equals_constant_time) {
    using ruvia::detail::cookieSignatureEquals;
    RUVIA_CHECK(cookieSignatureEquals("abcdef", "abcdef"));
    RUVIA_CHECK(!cookieSignatureEquals("abcdef", "abcdeg"));
    RUVIA_CHECK(!cookieSignatureEquals("abc", "abcd"));   // length mismatch
    RUVIA_CHECK(!cookieSignatureEquals("", "x"));
    RUVIA_CHECK(cookieSignatureEquals("", ""));
}

RUVIA_TEST(cookie_signature_roundtrip_verify) {
    const std::string secret = "topsecret";
    const std::string value = "session=deadbeef; role=admin";
    const std::string good = sign(secret, value);
    const std::string recomputed = sign(secret, value);
    RUVIA_CHECK(ruvia::detail::cookieSignatureEquals(good, recomputed));
    // A tampered value must not verify.
    const std::string tampered = sign(secret, "session=deadbeef; role=user");
    RUVIA_CHECK(!ruvia::detail::cookieSignatureEquals(good, tampered));
}
