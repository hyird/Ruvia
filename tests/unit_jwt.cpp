#include "test_harness.h"

#include <chrono>
#include <exception>
#include <memory_resource>
#include <string>
#include <string_view>
#include <type_traits>

#include "ruvia/auth/Jwt.h"
#include "auth/JwtInternal.h"

namespace {

using ruvia::JwtAlgorithm;
using ruvia::JwtClaim;
using ruvia::JwtSignOptions;
using ruvia::JwtVerifyOptions;
using ruvia::jwtBearerToken;
using ruvia::jwtSign;
using ruvia::jwtVerify;

static_assert(std::is_empty_v<ruvia::detail::JwtPayloadAccess>);

JwtSignOptions signOptions(std::string_view secret) {
    JwtSignOptions options;
    options.secret.assign(secret.data(), secret.size());
    options.issuer.assign("ruvia");
    options.subject.assign("user-1");
    return options;
}

JwtVerifyOptions verifyOptions(std::string_view secret) {
    JwtVerifyOptions options;
    options.secret.assign(secret.data(), secret.size());
    return options;
}

std::string sign(const JwtSignOptions& options) {
    const auto token = jwtSign(options);
    return std::string(token.data(), token.size());
}

std::string signedTokenWithPayload(std::string_view secret, std::string_view payloadJson) {
    auto* const resource = std::pmr::get_default_resource();
    const auto header = ruvia::detail::jwtBase64UrlEncode(R"({"alg":"HS256","typ":"JWT"})", resource);
    const auto payload = ruvia::detail::jwtBase64UrlEncode(payloadJson, resource);
    std::pmr::string signingInput(resource);
    signingInput.append(header);
    signingInput.push_back('.');
    signingInput.append(payload);
    const auto signature = ruvia::detail::jwtHmacSign(
        JwtAlgorithm::kHs256,
        secret,
        std::string_view(signingInput.data(), signingInput.size()),
        resource);
    signingInput.push_back('.');
    signingInput.append(signature);
    return std::string(signingInput.data(), signingInput.size());
}

template <typename Fn>
bool throwsOn(Fn&& fn) {
    try {
        fn();
        return false;
    } catch (const std::exception&) {
        return true;
    }
}

}  // namespace

RUVIA_TEST(jwt_sign_verify_round_trip_preserves_claims) {
    auto options = signOptions("supersecret");
    options.claims.push_back(JwtClaim(std::string_view("role"), std::string_view("admin")));
    const auto token = sign(options);

    const auto payload = jwtVerify(token, verifyOptions("supersecret"));
    RUVIA_CHECK_EQ(payload.issuer(), std::string_view("ruvia"));
    RUVIA_CHECK_EQ(payload.subject(), std::string_view("user-1"));
    const auto role = payload.claim("role");
    RUVIA_CHECK(role.has_value() && *role == std::string_view("admin"));
}

RUVIA_TEST(jwt_verify_rejects_wrong_secret) {
    const auto token = sign(signOptions("secretA"));
    RUVIA_CHECK(throwsOn([&] { (void)jwtVerify(token, verifyOptions("secretB")); }));
}

RUVIA_TEST(jwt_verify_rejects_tampered_payload) {
    auto token = sign(signOptions("secret"));
    // Corrupt a byte inside the payload section (after the first '.').
    const auto firstDot = token.find('.');
    RUVIA_CHECK(firstDot != std::string::npos);
    const auto i = firstDot + 2;
    token[i] = token[i] == 'A' ? 'B' : 'A';
    RUVIA_CHECK(throwsOn([&] { (void)jwtVerify(token, verifyOptions("secret")); }));
}

RUVIA_TEST(jwt_verify_rejects_algorithm_mismatch) {
    // Signed HS256; verifying as HS512 recomputes a different MAC and fails. The
    // server's configured algorithm is authoritative, so the token cannot dictate
    // the verification algorithm.
    auto options = signOptions("secret");
    options.algorithm = JwtAlgorithm::kHs256;
    const auto token = sign(options);

    auto verify = verifyOptions("secret");
    verify.algorithm = JwtAlgorithm::kHs512;
    RUVIA_CHECK(throwsOn([&] { (void)jwtVerify(token, verify); }));
}

RUVIA_TEST(jwt_verify_enforces_time_claims) {
    // requireExpiration: a token minted without an exp (expiresIn <= 0 emits none)
    // is rejected by default, and accepted only when the caller opts out.
    auto noExp = signOptions("secret");
    noExp.expiresIn = std::chrono::seconds{0};
    const auto tokenNoExp = sign(noExp);
    RUVIA_CHECK(throwsOn([&] { (void)jwtVerify(tokenNoExp, verifyOptions("secret")); }));
    auto allowNoExp = verifyOptions("secret");
    allowNoExp.requireExpiration = false;
    RUVIA_CHECK_EQ(jwtVerify(tokenNoExp, allowNoExp).subject(), std::string_view("user-1"));

    // notBefore: a token whose nbf is in the future is not yet valid, unless the
    // configured leeway covers the gap.
    auto future = signOptions("secret");
    future.notBeforeDelay = std::chrono::seconds{3600};
    const auto tokenFuture = sign(future);
    RUVIA_CHECK(throwsOn([&] { (void)jwtVerify(tokenFuture, verifyOptions("secret")); }));
    auto lenient = verifyOptions("secret");
    lenient.leeway = std::chrono::seconds{7200};
    RUVIA_CHECK_EQ(jwtVerify(tokenFuture, lenient).subject(), std::string_view("user-1"));
}

RUVIA_TEST(jwt_verify_enforces_registered_claims) {
    auto options = signOptions("secret");
    options.audience.assign("api");
    const auto token = sign(options);

    // Matching issuer/audience passes.
    auto ok = verifyOptions("secret");
    ok.issuer.assign("ruvia");
    ok.audience.assign("api");
    RUVIA_CHECK_EQ(jwtVerify(token, ok).audience(), std::string_view("api"));

    // A wrong expected issuer is rejected.
    auto badIssuer = verifyOptions("secret");
    badIssuer.issuer.assign("evil");
    RUVIA_CHECK(throwsOn([&] { (void)jwtVerify(token, badIssuer); }));

    // A wrong expected audience is rejected.
    auto badAudience = verifyOptions("secret");
    badAudience.audience.assign("other");
    RUVIA_CHECK(throwsOn([&] { (void)jwtVerify(token, badAudience); }));
}

RUVIA_TEST(jwt_verify_rejects_expired_token) {
    // exp is a Unix timestamp; 1 (1970) is far in the past, so this token is
    // expired regardless of the current clock and must be rejected -- the core
    // reason exp exists. jwtSign can only mint future exp, so craft it directly.
    const auto expired = signedTokenWithPayload("secret", R"({"sub":"user-1","exp":1})");
    RUVIA_CHECK(throwsOn([&] { (void)jwtVerify(expired, verifyOptions("secret")); }));

    // leeway applies to exp as well as nbf: a token that expired a few seconds
    // ago is rejected by default but accepted when leeway covers the gap.
    const auto nowSeconds = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const std::string recentPayload =
        R"({"sub":"user-1","exp":)" + std::to_string(nowSeconds - 10) + "}";
    const auto recentlyExpired = signedTokenWithPayload("secret", recentPayload);
    RUVIA_CHECK(throwsOn([&] { (void)jwtVerify(recentlyExpired, verifyOptions("secret")); }));
    auto lenient = verifyOptions("secret");
    lenient.leeway = std::chrono::seconds{3600};
    RUVIA_CHECK_EQ(jwtVerify(recentlyExpired, lenient).subject(), std::string_view("user-1"));
}

RUVIA_TEST(jwt_decode_unverified_reads_claims_without_authenticating) {
    // jwtDecodeUnverified reads the payload WITHOUT checking the signature -- it
    // provides no authentication. Pin that contract: it returns the claims even
    // for a token whose signature has been corrupted, while jwtVerify rejects the
    // same token. Callers must never treat the unverified payload as trusted.
    const auto token = sign(signOptions("secret"));
    RUVIA_CHECK_EQ(ruvia::jwtDecodeUnverified(token).subject(), std::string_view("user-1"));

    std::string forged = token.substr(0, token.rfind('.') + 1) + "corruptedsignature";
    RUVIA_CHECK_EQ(ruvia::jwtDecodeUnverified(forged).subject(), std::string_view("user-1"));
    RUVIA_CHECK(throwsOn([&] { (void)jwtVerify(forged, verifyOptions("secret")); }));
}

RUVIA_TEST(jwt_verify_rejects_malformed_token) {
    const auto verify = verifyOptions("secret");
    RUVIA_CHECK(throwsOn([&] { (void)jwtVerify("not-a-jwt", verify); }));
    RUVIA_CHECK(throwsOn([&] { (void)jwtVerify("only.two", verify); }));       // two sections
    RUVIA_CHECK(throwsOn([&] { (void)jwtVerify("a.b.c.d", verify); }));        // four sections
}

RUVIA_TEST(jwt_verify_rejects_none_algorithm_downgrade) {
    // The canonical JWT forgery (the "alg:none" attack): an attacker crafts a
    // header claiming no signature algorithm and strips the signature, hoping the
    // verifier trusts the token's own alg field and skips authentication. Ruvia
    // recomputes the MAC with the server-configured algorithm and compares it to
    // the token's signature, so the forgery fails the signature gate before the
    // alg field is even inspected. requireExpiration is disabled here so the
    // rejection can only come from the signature/alg gates, never a missing exp.
    auto* const resource = std::pmr::get_default_resource();
    const auto header = ruvia::detail::jwtBase64UrlEncode(R"({"alg":"none","typ":"JWT"})", resource);
    const auto payload = ruvia::detail::jwtBase64UrlEncode(R"({"sub":"admin"})", resource);

    // "<header>.<payload>." -- an empty third section, as an alg:none token has.
    std::string forged;
    forged.append(header.data(), header.size());
    forged.push_back('.');
    forged.append(payload.data(), payload.size());
    forged.push_back('.');

    auto verify = verifyOptions("secret");
    verify.requireExpiration = false;
    RUVIA_CHECK(throwsOn([&] { (void)jwtVerify(forged, verify); }));

    // The same forgery with attacker-supplied junk in the signature slot is also
    // rejected: no chosen string equals HMAC(secret, signingInput).
    const std::string forgedJunk = forged + "AAAA";
    RUVIA_CHECK(throwsOn([&] { (void)jwtVerify(forgedJunk, verify); }));
}

RUVIA_TEST(jwt_verify_rejects_signed_non_object_payload) {
    auto verify = verifyOptions("secret");
    verify.requireExpiration = false;
    const auto token = signedTokenWithPayload("secret", "not-json");
    RUVIA_CHECK(throwsOn([&] { (void)jwtVerify(token, verify); }));
}

RUVIA_TEST(jwt_base64url_round_trip_and_strict_decode) {
    using ruvia::detail::jwtBase64UrlDecode;
    using ruvia::detail::jwtBase64UrlEncode;
    auto* res = std::pmr::get_default_resource();

    // Round trip over every remainder length, including bytes that need the -/_
    // alphabet (0xFB 0xFF 0xBF -> "-_-_").
    for (const std::string_view sample : {std::string_view(""), std::string_view("f"),
                                          std::string_view("fo"), std::string_view("foo"),
                                          std::string_view("\xfb\xff\xbf")}) {
        const auto encoded = jwtBase64UrlEncode(sample, res);
        const auto decoded = jwtBase64UrlDecode(std::string_view(encoded.data(), encoded.size()), res);
        RUVIA_CHECK_EQ(std::string_view(decoded.data(), decoded.size()), sample);
    }

    // "QQ" is the canonical encoding of the single byte 'A'. "QR" would decode to
    // the same byte but leaves non-zero trailing bits, so it must be rejected --
    // otherwise a token part would have multiple valid spellings (malleability).
    const auto canonical = jwtBase64UrlDecode("QQ", res);
    RUVIA_CHECK(canonical.size() == 1 && canonical[0] == 'A');
    RUVIA_CHECK(throwsOn([&] { (void)jwtBase64UrlDecode("QR", res); }));

    // '=' padding and the standard-base64 '+' '/' are not part of base64url.
    RUVIA_CHECK(throwsOn([&] { (void)jwtBase64UrlDecode("QQ==", res); }));
    RUVIA_CHECK(throwsOn([&] { (void)jwtBase64UrlDecode("a+/b", res); }));

    // A length of 1 (mod 4) cannot encode a whole byte group.
    RUVIA_CHECK(throwsOn([&] { (void)jwtBase64UrlDecode("abcde", res); }));
}

RUVIA_TEST(jwt_bearer_token_extraction) {
    RUVIA_CHECK(jwtBearerToken("Bearer abc.def.ghi").value() == std::string_view("abc.def.ghi"));
    RUVIA_CHECK(jwtBearerToken("bearer xyz").value() == std::string_view("xyz"));  // scheme is case-insensitive
    RUVIA_CHECK(!jwtBearerToken("Basic abc").has_value());
    RUVIA_CHECK(!jwtBearerToken("Bearer").has_value());  // scheme only, no token
}
