#include "test_harness.h"

#include <string>
#include <string_view>

#include "ruvia/web/detail/CookieSignature.h"

namespace {

std::string sign(std::string_view secret, std::string_view name, std::string_view value) {
    std::string out(ruvia::detail::kCookieSignatureSize, '\0');
    ruvia::detail::writeCookieSignature(out.data(), secret, name, value);
    return out;
}

}  // namespace

RUVIA_TEST(cookie_signature_is_deterministic) {
    const std::string a = sign("s3cr3t", "user", "42");
    const std::string b = sign("s3cr3t", "user", "42");
    RUVIA_CHECK_EQ(a.size(), ruvia::detail::kCookieSignatureSize);
    RUVIA_CHECK_EQ(a, b);
}

RUVIA_TEST(cookie_signature_depends_on_name_value_and_secret) {
    const std::string base = sign("secret", "sid", "user=42");
    RUVIA_CHECK(base != sign("secret", "sid", "user=43"));   // value change
    RUVIA_CHECK(base != sign("secret2", "sid", "user=42"));  // secret change
    RUVIA_CHECK(base != sign("secret", "other", "user=42"));  // name change
}

RUVIA_TEST(cookie_signature_binds_name_no_collision) {
    // Length-framing binds the name so name||value pairs cannot collide: a value
    // signed for one cookie name must not validate for another.
    RUVIA_CHECK(sign("k", "a", "bc") != sign("k", "ab", "c"));
}

RUVIA_TEST(cookie_signature_known_vector) {
    // HMAC-SHA256(key="key", msg=<be32 len(name)> || name || value) base64:
    //   printf '\x00\x00\x00\x03sidvalue' | openssl dgst -sha256 -hmac key -binary | base64
    const std::string s = sign("key", "sid", "value");
    RUVIA_CHECK_EQ(s, std::string("Xq6alDcdYTT0HuiyunjjVNCmVDjn8+u4O78XJu5FObs="));
}

RUVIA_TEST(cookie_signature_equals_constant_time) {
    using ruvia::detail::cookieSignatureEquals;
    RUVIA_CHECK(cookieSignatureEquals("abcdef", "abcdef"));
    RUVIA_CHECK(!cookieSignatureEquals("abcdef", "abcdeg"));
    RUVIA_CHECK(!cookieSignatureEquals("abc", "abcd"));   // length mismatch
    RUVIA_CHECK(!cookieSignatureEquals("", "x"));
    RUVIA_CHECK(cookieSignatureEquals("", ""));
}

RUVIA_TEST(cookie_signature_large_value_spills_past_stack_arena) {
    // The message buffer uses a fixed stack arena with a heap fallback; a value
    // larger than the arena must still sign correctly (deterministic, and
    // sensitive to a single-byte change past the arena boundary).
    const std::string big(2000, 'x');
    std::string mutated = big;
    mutated.back() = 'y';
    const std::string a = sign("secret", "big", big);
    RUVIA_CHECK_EQ(a.size(), ruvia::detail::kCookieSignatureSize);
    RUVIA_CHECK_EQ(a, sign("secret", "big", big));   // deterministic across the fallback
    RUVIA_CHECK(a != sign("secret", "big", mutated));  // change beyond the stack arena matters
}

RUVIA_TEST(cookie_signature_roundtrip_verify) {
    const std::string secret = "topsecret";
    const std::string name = "session";
    const std::string value = "session=deadbeef; role=admin";
    const std::string good = sign(secret, name, value);
    const std::string recomputed = sign(secret, name, value);
    RUVIA_CHECK(ruvia::detail::cookieSignatureEquals(good, recomputed));
    // A tampered value must not verify.
    const std::string tampered = sign(secret, name, "session=deadbeef; role=user");
    RUVIA_CHECK(!ruvia::detail::cookieSignatureEquals(good, tampered));
}
