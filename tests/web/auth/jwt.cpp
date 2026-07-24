#include "test_harness.h"

#include <chrono>
#include <exception>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "ruvia/web/auth/Jwt.h"
#include "ruvia/web/detail/auth/JwtPrimitives.h"

namespace {

using ruvia::JwtAlgorithm;
using ruvia::jwtBearerToken;
using ruvia::JwtClaim;
using ruvia::jwtSign;
using ruvia::JwtSignOptions;
using ruvia::jwtVerify;
using ruvia::JwtVerifyOptions;

static_assert(std::is_empty_v<ruvia::detail::JwtPayloadAccess>);
static_assert(std::is_same_v<decltype(JwtSignOptions{}.expiresIn), std::optional<std::chrono::seconds>>);
static_assert(std::is_same_v<decltype(JwtSignOptions{}.notBeforeDelay), std::optional<std::chrono::seconds>>);

template <typename T>
concept ExposesAnyRvalueJwtOwnedView = requires(T&& value) { std::move(value).name(); } || requires(T&& value) { std::move(value).value(); } || requires(T&& value) { std::move(value).issuer(); } || requires(T&& value) { std::move(value).subject(); } || requires(T&& value) { std::move(value).audience(); } || requires(T&& value) { std::move(value).id(); } || requires(T&& value) { std::move(value).claims(); } || requires(T&& value) { std::move(value).claim(std::string_view{}); };

static_assert(!ExposesAnyRvalueJwtOwnedView<ruvia::JwtClaim>);
static_assert(!ExposesAnyRvalueJwtOwnedView<ruvia::JwtPayload>);

template <typename Token>
concept AcceptsJwtTokenSplit = requires(Token&& token) { ruvia::detail::jwtSplitToken(std::forward<Token>(token)); };

template <typename Authorization>
concept AcceptsJwtBearerToken = requires(Authorization&& authorization) { ruvia::jwtBearerToken(std::forward<Authorization>(authorization)); };

static_assert(!AcceptsJwtTokenSplit<std::string>);
static_assert(!AcceptsJwtTokenSplit<const std::string>);
static_assert(!AcceptsJwtTokenSplit<std::pmr::string>);
static_assert(AcceptsJwtTokenSplit<std::string&>);
static_assert(AcceptsJwtTokenSplit<std::pmr::string&>);
static_assert(AcceptsJwtTokenSplit<std::string_view>);
static_assert(!AcceptsJwtBearerToken<std::string>);
static_assert(!AcceptsJwtBearerToken<const std::string>);
static_assert(!AcceptsJwtBearerToken<std::pmr::string>);
static_assert(AcceptsJwtBearerToken<std::string&>);
static_assert(AcceptsJwtBearerToken<std::pmr::string&>);
static_assert(AcceptsJwtBearerToken<std::string_view>);

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

std::string signedTokenWithHeaderAndPayload(std::string_view secret, std::string_view headerJson, std::string_view payloadJson) {
    auto* const resource = std::pmr::get_default_resource();
    const auto header = ruvia::detail::jwtBase64UrlEncode(headerJson, resource);
    const auto payload = ruvia::detail::jwtBase64UrlEncode(payloadJson, resource);
    std::pmr::string signingInput(resource);
    signingInput.append(header);
    signingInput.push_back('.');
    signingInput.append(payload);
    const auto signature = ruvia::detail::jwtHmacSign(JwtAlgorithm::kHs256, secret, std::string_view(signingInput.data(), signingInput.size()), resource);
    signingInput.push_back('.');
    signingInput.append(signature);
    return std::string(signingInput.data(), signingInput.size());
}

std::string signedTokenWithPayload(std::string_view secret, std::string_view payloadJson) {
    return signedTokenWithHeaderAndPayload(secret, R"({"alg":"HS256","typ":"JWT"})", payloadJson);
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

RUVIA_TEST(jwt_sign_rejects_duplicate_custom_claim_names) {
    auto options = signOptions("secret");
    options.claims.push_back(JwtClaim("role", "admin"));
    options.claims.push_back(JwtClaim("role", "operator"));
    RUVIA_CHECK(throwsOn([&] { (void)jwtSign(options); }));

    auto reserved = signOptions("secret");
    reserved.claims.push_back(JwtClaim("iss", "forbidden"));
    RUVIA_CHECK(throwsOn([&] { (void)jwtSign(reserved); }));
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

RUVIA_TEST(jwt_verify_requires_unique_complete_json_objects) {
    const auto duplicateAlgorithm = signedTokenWithHeaderAndPayload("secret", R"({"alg":"HS256","alg":"HS256","typ":"JWT"})", R"({"sub":"user-1","exp":4102444800})");
    RUVIA_CHECK(throwsOn([&] { (void)jwtVerify(duplicateAlgorithm, verifyOptions("secret")); }));

    const auto duplicateUnknownHeader = signedTokenWithHeaderAndPayload("secret", R"({"alg":"HS256","kid":"a","kid":"b"})", R"({"sub":"user-1","exp":4102444800})");
    RUVIA_CHECK(throwsOn([&] { (void)jwtVerify(duplicateUnknownHeader, verifyOptions("secret")); }));

    const auto trailingHeader = signedTokenWithHeaderAndPayload("secret", R"({"alg":"HS256"}junk)", R"({"sub":"user-1","exp":4102444800})");
    RUVIA_CHECK(throwsOn([&] { (void)jwtVerify(trailingHeader, verifyOptions("secret")); }));

    const auto unsupportedCriticalHeader = signedTokenWithHeaderAndPayload("secret", R"({"alg":"HS256","crit":["custom"],"custom":true})", R"({"sub":"user-1","exp":4102444800})");
    RUVIA_CHECK(throwsOn([&] { (void)jwtVerify(unsupportedCriticalHeader, verifyOptions("secret")); }));

    for (const auto* payload : {R"({"sub":"first","sub":"second","exp":4102444800})", R"({"role":"first","role":"second","exp":4102444800})", R"({"role":"first","\u0072ole":"second","exp":4102444800})", R"({"sub":"user-1","exp":4102444800}junk)"}) {
        const auto token = signedTokenWithPayload("secret", payload);
        RUVIA_CHECK(throwsOn([&] { (void)jwtVerify(token, verifyOptions("secret")); }));
        RUVIA_CHECK(throwsOn([&] { (void)ruvia::jwtDecodeUnverified(token); }));
    }
}

RUVIA_TEST(jwt_verify_rejects_malformed_registered_claim_values) {
    for (const auto* payload : {R"({"iss":1,"exp":4102444800})", R"({"sub":false,"exp":4102444800})", R"({"jti":{},"exp":4102444800})", R"({"aud":["api",2],"exp":4102444800})", R"({"exp":"4102444800"})", R"({"nbf":"0","exp":4102444800})", R"({"iat":null,"exp":4102444800})"}) {
        const auto token = signedTokenWithPayload("secret", payload);
        RUVIA_CHECK(throwsOn([&] { (void)jwtVerify(token, verifyOptions("secret")); }));
    }
}

RUVIA_TEST(jwt_verify_enforces_time_claims) {
    // A token minted without exp is rejected by default, and accepted only when
    // the caller opts out. Absence is explicit; zero means "expires now".
    auto noExp = signOptions("secret");
    noExp.expiresIn = std::nullopt;
    const auto tokenNoExp = sign(noExp);
    RUVIA_CHECK(throwsOn([&] { (void)jwtVerify(tokenNoExp, verifyOptions("secret")); }));
    auto allowNoExp = verifyOptions("secret");
    allowNoExp.requireExpiration = false;
    const auto noExpPayload = jwtVerify(tokenNoExp, allowNoExp);
    RUVIA_CHECK_EQ(noExpPayload.subject(), std::string_view("user-1"));

    auto expiresNow = signOptions("secret");
    expiresNow.expiresIn = std::chrono::seconds(0);
    const auto tokenExpiresNow = sign(expiresNow);
    RUVIA_CHECK(throwsOn([&] { (void)jwtVerify(tokenExpiresNow, verifyOptions("secret")); }));

    auto validNow = signOptions("secret");
    validNow.notBeforeDelay = std::chrono::seconds(0);
    const auto validNowPayload = ruvia::jwtDecodeUnverified(sign(validNow));
    RUVIA_CHECK(validNowPayload.notBefore().has_value());

    // notBefore: a token whose nbf is in the future is not yet valid, unless the
    // configured leeway covers the gap.
    auto future = signOptions("secret");
    future.notBeforeDelay = std::chrono::seconds{3600};
    const auto tokenFuture = sign(future);
    RUVIA_CHECK(throwsOn([&] { (void)jwtVerify(tokenFuture, verifyOptions("secret")); }));
    auto lenient = verifyOptions("secret");
    lenient.leeway = std::chrono::seconds{7200};
    const auto futurePayload = jwtVerify(tokenFuture, lenient);
    RUVIA_CHECK_EQ(futurePayload.subject(), std::string_view("user-1"));
}

RUVIA_TEST(jwt_time_options_reject_negative_offsets) {
    auto negativeExpiration = signOptions("secret");
    negativeExpiration.expiresIn = std::chrono::seconds(-1);
    RUVIA_CHECK(throwsOn([&] { (void)jwtSign(negativeExpiration); }));

    auto negativeNotBefore = signOptions("secret");
    negativeNotBefore.notBeforeDelay = std::chrono::seconds(-1);
    RUVIA_CHECK(throwsOn([&] { (void)jwtSign(negativeNotBefore); }));

    auto negativeLeeway = verifyOptions("secret");
    negativeLeeway.leeway = std::chrono::seconds(-1);
    const auto token = sign(signOptions("secret"));
    RUVIA_CHECK(throwsOn([&] { (void)jwtVerify(token, negativeLeeway); }));
}

RUVIA_TEST(jwt_verify_enforces_registered_claims) {
    auto options = signOptions("secret");
    options.audience.assign("api");
    const auto token = sign(options);

    // Matching issuer/audience passes.
    auto ok = verifyOptions("secret");
    ok.issuer.assign("ruvia");
    ok.audience.assign("api");
    const auto verified = jwtVerify(token, ok);
    RUVIA_CHECK_EQ(verified.audience(), std::string_view("api"));

    // A wrong expected issuer is rejected.
    auto badIssuer = verifyOptions("secret");
    badIssuer.issuer.assign("evil");
    RUVIA_CHECK(throwsOn([&] { (void)jwtVerify(token, badIssuer); }));

    // A wrong expected audience is rejected.
    auto badAudience = verifyOptions("secret");
    badAudience.audience.assign("other");
    RUVIA_CHECK(throwsOn([&] { (void)jwtVerify(token, badAudience); }));
}

RUVIA_TEST(jwt_verify_supports_audience_array) {
    // RFC 7519 §4.1.3: aud may be a single string OR an array of strings. A
    // configured audience must be accepted iff it is one of the token's values.
    const auto multi = signedTokenWithPayload("secret", R"({"sub":"u","exp":4102444800,"aud":["api","web"]})");

    auto forApi = verifyOptions("secret");
    forApi.audience.assign("api");
    const auto apiPayload = jwtVerify(multi, forApi);
    RUVIA_CHECK_EQ(apiPayload.subject(), std::string_view("u"));
    auto forWeb = verifyOptions("secret");
    forWeb.audience.assign("web");
    RUVIA_CHECK(jwtVerify(multi, forWeb).hasAudience("web"));

    const auto spaced = signedTokenWithPayload("secret", R"({"sub":"u","exp":4102444800,"aud":[ "api" , "web" ]})");
    RUVIA_CHECK(jwtVerify(spaced, forWeb).hasAudience("web"));

    // The critical negative: an audience NOT in the array must be rejected --
    // array support must not become a fail-open path.
    auto forMobile = verifyOptions("secret");
    forMobile.audience.assign("mobile");
    RUVIA_CHECK(throwsOn([&] { (void)jwtVerify(multi, forMobile); }));

    // Accessors: audience() reports the first entry; hasAudience covers the set.
    const auto decoded = ruvia::jwtDecodeUnverified(multi);
    RUVIA_CHECK_EQ(decoded.audience(), std::string_view("api"));
    RUVIA_CHECK(decoded.hasAudience("api"));
    RUVIA_CHECK(decoded.hasAudience("web"));
    RUVIA_CHECK(!decoded.hasAudience("mobile"));

    // An escaped array element is decoded before matching.
    const auto escaped = signedTokenWithPayload("secret", R"({"sub":"u","exp":4102444800,"aud":["a\"b"]})");
    auto forEscaped = verifyOptions("secret");
    forEscaped.audience.assign("a\"b");
    RUVIA_CHECK(jwtVerify(escaped, forEscaped).hasAudience("a\"b"));

    // Malformed / non-string members yield an empty set -> fail closed.
    for (const auto* payload : {R"({"sub":"u","exp":4102444800,"aud":[]})", R"({"sub":"u","exp":4102444800,"aud":[1]})", R"({"sub":"u","exp":4102444800,"aud":["api",2]})"}) {
        const auto bad = signedTokenWithPayload("secret", payload);
        auto wantApi = verifyOptions("secret");
        wantApi.audience.assign("api");
        RUVIA_CHECK(throwsOn([&] { (void)jwtVerify(bad, wantApi); }));
    }

    // The single-string form is unchanged (regression guard).
    const auto single = signedTokenWithPayload("secret", R"({"sub":"u","exp":4102444800,"aud":"api"})");
    auto wantApiSingle = verifyOptions("secret");
    wantApiSingle.audience.assign("api");
    const auto singlePayload = jwtVerify(single, wantApiSingle);
    RUVIA_CHECK_EQ(singlePayload.audience(), std::string_view("api"));
    auto wantOtherSingle = verifyOptions("secret");
    wantOtherSingle.audience.assign("other");
    RUVIA_CHECK(throwsOn([&] { (void)jwtVerify(single, wantOtherSingle); }));
}

RUVIA_TEST(jwt_epoch_seconds_saturates_instead_of_overflowing) {
    // exp/nbf beyond the clock's representable range (~year 2262) would overflow
    // int64 nanoseconds when converted to a time_point (UB on attacker-controlled
    // claims); jwtFromEpochSeconds must saturate instead. A huge exp then reads as
    // far-future (not expired) and a huge nbf as far-future (not yet valid).
    const auto farExp = signedTokenWithPayload("secret", R"({"sub":"u","exp":99999999999})");
    const auto farExpPayload = jwtVerify(farExp, verifyOptions("secret"));
    RUVIA_CHECK_EQ(farExpPayload.subject(), std::string_view("u"));

    // int64 max must not overflow the saturating conversion either.
    const auto maxExp = signedTokenWithPayload("secret", R"({"sub":"u","exp":9223372036854775807})");
    const auto maxExpPayload = jwtVerify(maxExp, verifyOptions("secret"));
    RUVIA_CHECK_EQ(maxExpPayload.subject(), std::string_view("u"));

    using Clock = std::chrono::system_clock;
    RUVIA_CHECK(ruvia::detail::jwtTimeWithOffset(Clock::time_point::max(), std::chrono::seconds(1)) == Clock::time_point::max());
    RUVIA_CHECK(ruvia::detail::jwtTimeWithOffset(Clock::time_point::min(), std::chrono::seconds(-1)) == Clock::time_point::min());

    const auto fractional = signedTokenWithPayload("secret", R"({"sub":"u","iat":1.5,"exp":4102444800.5})");
    const auto fractionalPayload = jwtVerify(fractional, verifyOptions("secret"));
    RUVIA_CHECK(fractionalPayload.issuedAt().has_value());
    const auto issuedSeconds = std::chrono::duration<long double>(fractionalPayload.issuedAt()->time_since_epoch()).count();
    RUVIA_CHECK(issuedSeconds > 1.49L && issuedSeconds < 1.51L);

    auto allowNoExp = verifyOptions("secret");
    allowNoExp.requireExpiration = false;
    const auto farNbf = signedTokenWithPayload("secret", R"({"sub":"u","nbf":99999999999})");
    RUVIA_CHECK(throwsOn([&] { (void)jwtVerify(farNbf, allowNoExp); }));
}

RUVIA_TEST(jwt_verify_rejects_expired_token) {
    // exp is a Unix timestamp; 1 (1970) is far in the past, so this token is
    // expired regardless of the current clock and must be rejected -- the core
    // reason exp exists. jwtSign can only mint future exp, so craft it directly.
    const auto expired = signedTokenWithPayload("secret", R"({"sub":"user-1","exp":1})");
    RUVIA_CHECK(throwsOn([&] { (void)jwtVerify(expired, verifyOptions("secret")); }));

    // leeway applies to exp as well as nbf: a token that expired a few seconds
    // ago is rejected by default but accepted when leeway covers the gap.
    const auto nowSeconds = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    const std::string recentPayload = R"({"sub":"user-1","exp":)" + std::to_string(nowSeconds - 10) + "}";
    const auto recentlyExpired = signedTokenWithPayload("secret", recentPayload);
    RUVIA_CHECK(throwsOn([&] { (void)jwtVerify(recentlyExpired, verifyOptions("secret")); }));
    auto lenient = verifyOptions("secret");
    lenient.leeway = std::chrono::seconds{3600};
    const auto recentlyExpiredPayload = jwtVerify(recentlyExpired, lenient);
    RUVIA_CHECK_EQ(recentlyExpiredPayload.subject(), std::string_view("user-1"));
}

RUVIA_TEST(jwt_exp_nbf_boundaries_follow_rfc7519) {
    // The exact exp/nbf boundary can't be pinned against the live clock inside
    // jwtVerify, so exercise the pure predicates it delegates to. RFC 7519 §4.1.4:
    // a token is rejected at now == exp (no leeway); §4.1.5: it is valid at
    // now == nbf. leeway widens each window (later for exp, earlier for nbf).
    using std::chrono::seconds;
    const auto t = std::chrono::system_clock::from_time_t(1'000'000'000);

    RUVIA_CHECK(ruvia::detail::jwtTokenExpired(t, t, seconds{0}));                 // now == exp -> expired
    RUVIA_CHECK(ruvia::detail::jwtTokenExpired(t + seconds{1}, t, seconds{0}));    // after exp
    RUVIA_CHECK(!ruvia::detail::jwtTokenExpired(t - seconds{1}, t, seconds{0}));   // before exp
    RUVIA_CHECK(!ruvia::detail::jwtTokenExpired(t + seconds{5}, t, seconds{10}));  // inside leeway grace
    RUVIA_CHECK(ruvia::detail::jwtTokenExpired(t + seconds{10}, t, seconds{10}));  // now == exp+leeway -> expired

    RUVIA_CHECK(!ruvia::detail::jwtTokenNotYetValid(t, t, seconds{0}));                // now == nbf -> valid
    RUVIA_CHECK(ruvia::detail::jwtTokenNotYetValid(t - seconds{1}, t, seconds{0}));    // before nbf
    RUVIA_CHECK(!ruvia::detail::jwtTokenNotYetValid(t + seconds{1}, t, seconds{0}));   // after nbf
    RUVIA_CHECK(!ruvia::detail::jwtTokenNotYetValid(t - seconds{5}, t, seconds{10}));  // inside leeway grace
    RUVIA_CHECK(ruvia::detail::jwtTokenNotYetValid(t - seconds{11}, t, seconds{10}));  // before nbf-leeway
}

RUVIA_TEST(jwt_decode_unverified_reads_claims_without_authenticating) {
    // jwtDecodeUnverified reads the payload WITHOUT checking the signature -- it
    // provides no authentication. Pin that contract: it returns the claims even
    // for a token whose signature has been corrupted, while jwtVerify rejects the
    // same token. Callers must never treat the unverified payload as trusted.
    const auto token = sign(signOptions("secret"));
    const auto decoded = ruvia::jwtDecodeUnverified(token);
    RUVIA_CHECK_EQ(decoded.subject(), std::string_view("user-1"));

    std::string forged = token.substr(0, token.rfind('.') + 1) + "corruptedsignature";
    const auto decodedForged = ruvia::jwtDecodeUnverified(forged);
    RUVIA_CHECK_EQ(decodedForged.subject(), std::string_view("user-1"));
    RUVIA_CHECK(throwsOn([&] { (void)jwtVerify(forged, verifyOptions("secret")); }));
}

RUVIA_TEST(jwt_verify_rejects_malformed_token) {
    const auto verify = verifyOptions("secret");
    RUVIA_CHECK(throwsOn([&] { (void)jwtVerify("not-a-jwt", verify); }));
    RUVIA_CHECK(throwsOn([&] { (void)jwtVerify("only.two", verify); }));  // two sections
    RUVIA_CHECK(throwsOn([&] { (void)jwtVerify("a.b.c.d", verify); }));   // four sections
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
    for (const std::string_view sample : {std::string_view(""), std::string_view("f"), std::string_view("fo"), std::string_view("foo"), std::string_view("\xfb\xff\xbf")}) {
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
