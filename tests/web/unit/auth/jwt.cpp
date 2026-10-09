#include "ruvia/web/auth/jwt.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>

#include "auth/jwt_primitives.h"
#include "memory_resource_fixture.h"
#include "test_harness.h"

namespace {

using ruvia::jwt_algorithm;
using ruvia::jwt_bearer_token;
using ruvia::jwt_claim;
using ruvia::jwt_expiration_claim_policy;
using ruvia::jwt_sign;
using ruvia::jwt_sign_options;
using ruvia::jwt_verify_options;
using ruvia::testing::throws_on;

// Deterministic test keys, not production secrets. Large enough for HS512.
constexpr std::string_view secret =
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
constexpr std::string_view other_secret =
    "fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210";

jwt_sign_options sign_options(std::string_view secret_value) {
    jwt_sign_options options;
    options.secret_ = secret_value;
    options.issuer_.assign("ruvia");
    options.subject_.assign("user-1");
    return options;
}

jwt_verify_options verify_options(std::string_view secret_value) {
    jwt_verify_options options;
    options.secret_ = secret_value;
    return options;
}

std::string sign(const jwt_sign_options& options) {
    const auto token = jwt_sign(options);
    return std::string(token.data(), token.size());
}

ruvia::jwt_payload verify_jwt(ruvia::borrowed_text token, jwt_verify_options options) {
    options.token_ = token.view();
    return ruvia::jwt_verify(options);
}

ruvia::jwt_payload decode_jwt_unverified(ruvia::borrowed_text token) {
    return ruvia::jwt_decode_unverified({.token_ = token});
}

std::string signed_token_with_header_and_payload(
    std::string_view secret_value, std::string_view header_json, std::string_view payload_json) {
    auto* const resource = std::pmr::get_default_resource();
    const auto header_value = ruvia::detail::jwt_base64_url_encode(header_json, resource);
    const auto payload_value = ruvia::detail::jwt_base64_url_encode(payload_json, resource);
    std::pmr::string signing_input(resource);
    signing_input.append(header_value);
    signing_input.push_back('.');
    signing_input.append(payload_value);
    const auto signature = ruvia::detail::jwt_hmac_sign(jwt_algorithm::hs256, secret_value,
        std::string_view(signing_input.data(), signing_input.size()), resource);
    signing_input.push_back('.');
    signing_input.append(signature);
    return std::string(signing_input.data(), signing_input.size());
}

std::string signed_token_with_payload(std::string_view secret_value, std::string_view payload_json) {
    return signed_token_with_header_and_payload(secret_value, R"({"alg":"HS256","typ":"JWT"})", payload_json);
}

}  // namespace

RUVIA_TEST(jwt_json_escape_preserves_exact_bytes) {
    std::string controls;
    for (unsigned byte = 0; byte < 32; ++byte) {
        controls.push_back(static_cast<char>(byte));
    }
    std::pmr::string output("prefix:");
    ruvia::detail::jwt_append_json_escaped(output, controls);
    RUVIA_CHECK_EQ(output, std::string_view(
                               R"(prefix:"\u0000\u0001\u0002\u0003\u0004\u0005\u0006\u0007\b\t\n\u000b\f\r\u000e\u000f\u0010\u0011\u0012\u0013\u0014\u0015\u0016\u0017\u0018\u0019\u001a\u001b\u001c\u001d\u001e\u001f")"));

    for (const std::size_t size : {0u, 15u, 16u, 17u, 4096u}) {
        const std::string plain(size, 'x');
        output.clear();
        ruvia::detail::jwt_append_json_escaped(output, plain);
        RUVIA_CHECK_EQ(output, std::string_view('"' + plain + '"'));
        output.clear();
        ruvia::detail::jwt_append_json_escaped(output, plain + "\"\\\n" + plain);
        RUVIA_CHECK_EQ(output, std::string_view('"' + plain + R"(\"\\\n)" + plain + '"'));
    }

    std::string high_bytes;
    for (unsigned byte = 0x80; byte <= 0xff; ++byte) {
        high_bytes.push_back(static_cast<char>(byte));
    }
    output.clear();
    ruvia::detail::jwt_append_json_escaped(output, high_bytes);
    RUVIA_CHECK_EQ(output, std::string_view('"' + high_bytes + '"'));
}

RUVIA_TEST(jwt_sign_verify_round_trip_preserves_claims) {
    auto options = sign_options(secret);
    options.claims_.push_back(
        jwt_claim({.name_ = std::string_view("role"), .value_ = std::string_view("admin")}));
    const auto token = sign(options);

    const auto payload_value = verify_jwt(token, verify_options(secret));
    RUVIA_CHECK_EQ(payload_value.issuer(), std::string_view("ruvia"));
    RUVIA_CHECK_EQ(payload_value.subject(), std::string_view("user-1"));
    const auto role = payload_value.claim("role");
    RUVIA_CHECK(role.has_value() && *role == std::string_view("admin"));
}

RUVIA_TEST(jwt_sign_rejects_duplicate_custom_claim_names) {
    auto options = sign_options(secret);
    options.claims_.push_back(jwt_claim({.name_ = "role", .value_ = "admin"}));
    options.claims_.push_back(jwt_claim({.name_ = "role", .value_ = "operator"}));
    RUVIA_CHECK(throws_on([&] { (void)jwt_sign(options); }));

    auto reserved = sign_options(secret);
    reserved.claims_.push_back(jwt_claim({.name_ = "iss", .value_ = "forbidden"}));
    RUVIA_CHECK(throws_on([&] { (void)jwt_sign(reserved); }));
}

RUVIA_TEST(jwt_verify_rejects_wrong_secret) {
    const auto token = sign(sign_options(secret));
    RUVIA_CHECK(throws_on([&] { (void)verify_jwt(token, verify_options(other_secret)); }));
}

RUVIA_TEST(jwt_hmac_enforces_algorithm_key_lengths_for_signing_and_verification) {
    const struct {
        jwt_algorithm algorithm_;
        std::size_t minimum_bytes_;
    } cases[] = {
        {jwt_algorithm::hs256, 32},
        {jwt_algorithm::hs384, 48},
        {jwt_algorithm::hs512, 64},
    };
    for (const auto& test : cases) {
        // Include NUL bytes: this is a raw key, not a C string or encoded text.
        std::string key(257, '\0');
        key.back() = 'k';
        for (const auto size : {test.minimum_bytes_, test.minimum_bytes_ + 1, key.size()}) {
            const std::string_view valid_key(key.data(), size);
            auto signing = sign_options(valid_key);
            signing.algorithm_ = test.algorithm_;
            const auto token = sign(signing);
            auto verification = verify_options(valid_key);
            verification.algorithm_ = test.algorithm_;
            const auto verified = verify_jwt(token, verification);
            RUVIA_CHECK_EQ(verified.subject(), std::string_view("user-1"));

            for (const auto short_size : {std::size_t{0}, std::size_t{1}, test.minimum_bytes_ - 1}) {
                const std::string_view short_key(key.data(), short_size);
                signing.secret_ = short_key;
                bool signing_rejected = false;
                try {
                    (void)jwt_sign(signing);
                } catch (const std::invalid_argument&) {
                    signing_rejected = true;
                }
                RUVIA_CHECK(signing_rejected);
                verification.secret_ = short_key;
                bool verification_rejected = false;
                try {
                    (void)verify_jwt(token, verification);
                } catch (const std::invalid_argument&) {
                    verification_rejected = true;
                } catch (const std::runtime_error&) {
                    // A MAC mismatch is not configuration validation.
                }
                RUVIA_CHECK(verification_rejected);
            }
        }
    }
}

RUVIA_TEST(jwt_hmac_rejects_unrepresentable_secret_length) {
    if constexpr (sizeof(std::size_t) > sizeof(int)) {
        const auto oversized_secret_size =
            static_cast<std::size_t>((std::numeric_limits<int>::max)()) + 1;
        bool rejected = false;
        try {
            (void)ruvia::detail::jwt_hmac_sign(jwt_algorithm::hs256,
                std::string_view("x", oversized_secret_size), "data",
                std::pmr::get_default_resource());
        } catch (const std::length_error&) {
            rejected = true;
        }
        RUVIA_CHECK(rejected);

        const auto oversized_data_size =
            static_cast<std::size_t>((std::numeric_limits<int>::max)()) + 1;
        rejected = false;
        try {
            (void)ruvia::detail::jwt_hmac_sign(jwt_algorithm::hs256, secret,
                std::string_view("x", oversized_data_size), std::pmr::get_default_resource());
        } catch (const std::length_error&) {
            rejected = true;
        }
        RUVIA_CHECK(rejected);
    }
}

RUVIA_TEST(jwt_verify_rejects_tampered_payload) {
    auto token = sign(sign_options(secret));
    // Corrupt a byte inside the payload section (after the first '.').
    const auto first_dot = token.find('.');
    RUVIA_CHECK(first_dot != std::string::npos);
    const auto i = first_dot + 2;
    token[i] = token[i] == 'A' ? 'B' : 'A';
    RUVIA_CHECK(throws_on([&] { (void)verify_jwt(token, verify_options(secret)); }));
}

RUVIA_TEST(jwt_verify_rejects_algorithm_mismatch) {
    // Signed HS256; verifying as HS512 recomputes a different MAC and fails. The
    // server's configured algorithm is authoritative, so the token cannot dictate
    // the verification algorithm.
    auto options = sign_options(secret);
    options.algorithm_ = jwt_algorithm::hs256;
    const auto token = sign(options);

    auto verify = verify_options(secret);
    verify.algorithm_ = jwt_algorithm::hs512;
    RUVIA_CHECK(throws_on([&] { (void)verify_jwt(token, verify); }));
}

RUVIA_TEST(jwt_verify_requires_unique_complete_json_objects) {
    const auto duplicate_algorithm = signed_token_with_header_and_payload(secret,
        R"({"alg":"HS256","alg":"HS256","typ":"JWT"})", R"({"sub":"user-1","exp":4102444800})");
    RUVIA_CHECK(throws_on([&] { (void)verify_jwt(duplicate_algorithm, verify_options(secret)); }));

    const auto duplicate_unknown_header = signed_token_with_header_and_payload(
        secret, R"({"alg":"HS256","kid":"a","kid":"b"})", R"({"sub":"user-1","exp":4102444800})");
    RUVIA_CHECK(
        throws_on([&] { (void)verify_jwt(duplicate_unknown_header, verify_options(secret)); }));

    const auto trailing_header = signed_token_with_header_and_payload(
        secret, R"({"alg":"HS256"}junk)", R"({"sub":"user-1","exp":4102444800})");
    RUVIA_CHECK(throws_on([&] { (void)verify_jwt(trailing_header, verify_options(secret)); }));

    const auto unsupported_critical_header = signed_token_with_header_and_payload(secret,
        R"({"alg":"HS256","crit":["custom"],"custom":true})",
        R"({"sub":"user-1","exp":4102444800})");
    RUVIA_CHECK(
        throws_on([&] { (void)verify_jwt(unsupported_critical_header, verify_options(secret)); }));

    for (const auto* payload : {R"({"sub":"first","sub":"second","exp":4102444800})",
             R"({"role":"first","role":"second","exp":4102444800})",
             R"({"role":"first","\u0072ole":"second","exp":4102444800})",
             R"({"sub":"user-1","exp":4102444800}junk)"}) {
        const auto token = signed_token_with_payload(secret, payload);
        RUVIA_CHECK(throws_on([&] { (void)verify_jwt(token, verify_options(secret)); }));
        RUVIA_CHECK(throws_on([&] { (void)decode_jwt_unverified(token); }));
    }
}

RUVIA_TEST(jwt_verify_rejects_unsupported_payload_encoding_headers) {
    constexpr std::string_view payload_value = R"({"sub":"user-1","exp":4102444800})";
    for (const auto header : {
             R"({"alg":"HS256","b64":false})",
             R"({"alg":"HS256","\u006264":false})",
             R"({"alg":"HS256","b64":false,"crit":["b64"]})",
             R"({"alg":"HS256","b64":true})",
             R"({"alg":"HS256","b64":true,"crit":["b64"]})",
             R"({"alg":"HS256","b64":"false"})",
             R"({"alg":"HS256","b64":null})"}) {
        // The MAC is valid. Reject the encoding extension itself, including
        // malformed uses that omit crit, rather than misinterpreting payloads.
        const auto token = signed_token_with_header_and_payload(secret, header, payload_value);
        bool rejected = false;
        try {
            (void)verify_jwt(token, verify_options(secret));
        } catch (const std::runtime_error& error) {
            rejected = std::string_view(error.what()) == "JWT JOSE header is invalid";
        }
        RUVIA_CHECK(rejected);
    }
    // Noncritical, unrelated extensions remain ignorable; names are case-sensitive.
    const auto ordinary = signed_token_with_header_and_payload(
        secret, R"({"alg":"HS256","kid":"key-1","B64":false})", payload_value);
    const auto verified = verify_jwt(ordinary, verify_options(secret));
    RUVIA_CHECK_EQ(verified.subject(), std::string_view("user-1"));
}

RUVIA_TEST(jwt_verify_rejects_malformed_registered_claim_values) {
    for (const auto* payload : {R"({"iss":1,"exp":4102444800})",
             R"({"sub":false,"exp":4102444800})", R"({"jti":{},"exp":4102444800})",
             R"({"aud":["api",2],"exp":4102444800})", R"({"exp":"4102444800"})",
             R"({"nbf":"0","exp":4102444800})", R"({"iat":null,"exp":4102444800})"}) {
        const auto token = signed_token_with_payload(secret, payload);
        RUVIA_CHECK(throws_on([&] { (void)verify_jwt(token, verify_options(secret)); }));
    }
}

RUVIA_TEST(jwt_verify_enforces_time_claims) {
    // A token minted without exp is rejected by default, and accepted only when
    // the caller opts out. Absence is explicit; zero means "expires now".
    auto no_exp = sign_options(secret);
    no_exp.expires_in_ = std::nullopt;
    const auto token_no_exp = sign(no_exp);
    RUVIA_CHECK(throws_on([&] { (void)verify_jwt(token_no_exp, verify_options(secret)); }));
    auto allow_no_exp = verify_options(secret);
    allow_no_exp.expiration_claim_ = jwt_expiration_claim_policy::allow_missing;
    const auto no_exp_payload = verify_jwt(token_no_exp, allow_no_exp);
    RUVIA_CHECK_EQ(no_exp_payload.subject(), std::string_view("user-1"));

    auto expires_now = sign_options(secret);
    expires_now.expires_in_ = std::chrono::seconds(0);
    const auto token_expires_now = sign(expires_now);
    RUVIA_CHECK(throws_on([&] { (void)verify_jwt(token_expires_now, verify_options(secret)); }));

    auto valid_now = sign_options(secret);
    valid_now.not_before_delay_ = std::chrono::seconds(0);
    const auto valid_now_token = sign(valid_now);
    const auto valid_now_payload = decode_jwt_unverified(valid_now_token);
    RUVIA_CHECK(valid_now_payload.not_before().has_value());

    // not_before: a token whose nbf is in the future is not yet valid, unless the
    // configured leeway covers the gap.
    auto future = sign_options(secret);
    future.not_before_delay_ = std::chrono::seconds{3600};
    const auto token_future = sign(future);
    RUVIA_CHECK(throws_on([&] { (void)verify_jwt(token_future, verify_options(secret)); }));
    auto lenient = verify_options(secret);
    lenient.leeway_ = std::chrono::seconds{7200};
    const auto future_payload = verify_jwt(token_future, lenient);
    RUVIA_CHECK_EQ(future_payload.subject(), std::string_view("user-1"));
}

RUVIA_TEST(jwt_not_before_delay_never_starts_before_the_requested_time) {
    using clock_type = std::chrono::system_clock;
    using namespace std::chrono;

    auto before = clock_type::now();
    while (before - floor<seconds>(before) < milliseconds(150) ||
           before - floor<seconds>(before) > milliseconds(750)) {
        std::this_thread::sleep_for(milliseconds(10));
        before = clock_type::now();
    }
    auto options = sign_options(secret);
    options.not_before_delay_ = seconds(1);
    const auto token = sign(options);
    const auto after = clock_type::now();
    const auto payload_value = decode_jwt_unverified(token);
    RUVIA_CHECK(payload_value.not_before().has_value());
    RUVIA_CHECK(*payload_value.not_before() >= before + seconds(1));
    RUVIA_CHECK(*payload_value.not_before() < after + seconds(2));

    options.not_before_delay_ = seconds(0);
    const auto immediate = sign(options);
    const auto immediate_payload = decode_jwt_unverified(immediate);
    RUVIA_CHECK(immediate_payload.not_before().has_value());
    RUVIA_CHECK(*immediate_payload.not_before() <= clock_type::now());
}

RUVIA_TEST(jwt_time_options_reject_negative_offsets) {
    auto negative_expiration = sign_options(secret);
    negative_expiration.expires_in_ = std::chrono::seconds(-1);
    RUVIA_CHECK(throws_on([&] { (void)jwt_sign(negative_expiration); }));

    auto negative_not_before = sign_options(secret);
    negative_not_before.not_before_delay_ = std::chrono::seconds(-1);
    RUVIA_CHECK(throws_on([&] { (void)jwt_sign(negative_not_before); }));

    auto negative_leeway = verify_options(secret);
    negative_leeway.leeway_ = std::chrono::seconds(-1);
    const auto token = sign(sign_options(secret));
    RUVIA_CHECK(throws_on([&] { (void)verify_jwt(token, negative_leeway); }));

    auto invalid_expiration_claim = verify_options(secret);
    invalid_expiration_claim.expiration_claim_ = static_cast<jwt_expiration_claim_policy>(0xFF);
    RUVIA_CHECK(throws_on([&] { (void)verify_jwt(token, invalid_expiration_claim); }));
}

RUVIA_TEST(jwt_verify_enforces_registered_claims) {
    auto options = sign_options(secret);
    options.audience_.assign("api");
    const auto token = sign(options);

    // Matching issuer/audience passes.
    auto ok = verify_options(secret);
    ok.issuer_.assign("ruvia");
    ok.audience_.assign("api");
    const auto verified = verify_jwt(token, ok);
    RUVIA_CHECK_EQ(verified.audience(), std::string_view("api"));

    // A wrong expected issuer is rejected.
    auto bad_issuer = verify_options(secret);
    bad_issuer.issuer_.assign("evil");
    RUVIA_CHECK(throws_on([&] { (void)verify_jwt(token, bad_issuer); }));

    // A wrong expected audience is rejected.
    auto bad_audience = verify_options(secret);
    bad_audience.audience_.assign("other");
    RUVIA_CHECK(throws_on([&] { (void)verify_jwt(token, bad_audience); }));
}

RUVIA_TEST(jwt_verify_supports_audience_array) {
    // RFC 7519 §4.1.3: aud may be a single string OR an array of strings. A
    // configured audience must be accepted iff it is one of the token's values.
    const auto multi =
        signed_token_with_payload(secret, R"({"sub":"u","exp":4102444800,"aud":["api","web"]})");

    auto for_api = verify_options(secret);
    for_api.audience_.assign("api");
    const auto api_payload = verify_jwt(multi, for_api);
    RUVIA_CHECK_EQ(api_payload.subject(), std::string_view("u"));
    auto for_web = verify_options(secret);
    for_web.audience_.assign("web");
    RUVIA_CHECK(verify_jwt(multi, for_web).has_audience("web"));

    const auto spaced =
        signed_token_with_payload(secret, R"({"sub":"u","exp":4102444800,"aud":[ "api" , "web" ]})");
    RUVIA_CHECK(verify_jwt(spaced, for_web).has_audience("web"));

    // The critical negative: an audience NOT in the array must be rejected --
    // array support must not become a fail-open path.
    auto for_mobile = verify_options(secret);
    for_mobile.audience_.assign("mobile");
    RUVIA_CHECK(throws_on([&] { (void)verify_jwt(multi, for_mobile); }));

    // Accessors: audience() reports the first entry; has_audience covers the set.
    const auto decoded = decode_jwt_unverified(multi);
    RUVIA_CHECK_EQ(decoded.audience(), std::string_view("api"));
    RUVIA_CHECK(decoded.has_audience("api"));
    RUVIA_CHECK(decoded.has_audience("web"));
    RUVIA_CHECK(!decoded.has_audience("mobile"));

    // An escaped array element is decoded before matching.
    const auto escaped =
        signed_token_with_payload(secret, R"({"sub":"u","exp":4102444800,"aud":["a\"b"]})");
    auto for_escaped = verify_options(secret);
    for_escaped.audience_.assign("a\"b");
    RUVIA_CHECK(verify_jwt(escaped, for_escaped).has_audience("a\"b"));

    // Malformed / non-string members yield an empty set -> fail closed.
    for (const auto* payload :
        {R"({"sub":"u","exp":4102444800,"aud":[]})", R"({"sub":"u","exp":4102444800,"aud":[1]})",
            R"({"sub":"u","exp":4102444800,"aud":["api",2]})"}) {
        const auto bad = signed_token_with_payload(secret, payload);
        auto want_api = verify_options(secret);
        want_api.audience_.assign("api");
        RUVIA_CHECK(throws_on([&] { (void)verify_jwt(bad, want_api); }));
    }

    // The single-string form is unchanged (regression guard).
    const auto single =
        signed_token_with_payload(secret, R"({"sub":"u","exp":4102444800,"aud":"api"})");
    auto want_api_single = verify_options(secret);
    want_api_single.audience_.assign("api");
    const auto single_payload = verify_jwt(single, want_api_single);
    RUVIA_CHECK_EQ(single_payload.audience(), std::string_view("api"));
    auto want_other_single = verify_options(secret);
    want_other_single.audience_.assign("other");
    RUVIA_CHECK(throws_on([&] { (void)verify_jwt(single, want_other_single); }));
}

RUVIA_TEST(jwt_epoch_seconds_saturates_instead_of_overflowing) {
    // exp/nbf beyond the clock's representable range (~year 2262) would overflow
    // int64 nanoseconds when converted to a time_point (UB on attacker-controlled
    // claims); jwt_from_epoch_seconds must saturate instead. A huge exp then reads as
    // far-future (not expired) and a huge nbf as far-future (not yet valid).
    const auto far_exp = signed_token_with_payload(secret, R"({"sub":"u","exp":99999999999})");
    const auto far_exp_payload = verify_jwt(far_exp, verify_options(secret));
    RUVIA_CHECK_EQ(far_exp_payload.subject(), std::string_view("u"));

    // int64 max must not overflow the saturating conversion either.
    const auto max_exp =
        signed_token_with_payload(secret, R"({"sub":"u","exp":9223372036854775807})");
    const auto max_exp_payload = verify_jwt(max_exp, verify_options(secret));
    RUVIA_CHECK_EQ(max_exp_payload.subject(), std::string_view("u"));

    using clock_type = std::chrono::system_clock;
    RUVIA_CHECK(ruvia::detail::jwt_time_with_offset(
                    clock_type::time_point::max(), std::chrono::seconds(1)) == clock_type::time_point::max());
    RUVIA_CHECK(ruvia::detail::jwt_time_with_offset(clock_type::time_point::min(),
                    std::chrono::seconds(-1)) == clock_type::time_point::min());

    const auto fractional =
        signed_token_with_payload(secret, R"({"sub":"u","iat":1.5,"exp":4102444800.5})");
    const auto fractional_payload = verify_jwt(fractional, verify_options(secret));
    RUVIA_CHECK(fractional_payload.issued_at().has_value());
    const auto issued_seconds =
        std::chrono::duration<long double>(fractional_payload.issued_at()->time_since_epoch())
            .count();
    RUVIA_CHECK(issued_seconds > 1.49L && issued_seconds < 1.51L);

    auto allow_no_exp = verify_options(secret);
    allow_no_exp.expiration_claim_ = jwt_expiration_claim_policy::allow_missing;
    const auto far_nbf = signed_token_with_payload(secret, R"({"sub":"u","nbf":99999999999})");
    RUVIA_CHECK(throws_on([&] { (void)verify_jwt(far_nbf, allow_no_exp); }));
}

RUVIA_TEST(jwt_verify_rejects_expired_token) {
    // exp is a Unix timestamp; 1 (1970) is far in the past, so this token is
    // expired regardless of the current clock and must be rejected -- the core
    // reason exp exists. jwt_sign can only mint future exp, so craft it directly.
    const auto expired = signed_token_with_payload(secret, R"({"sub":"user-1","exp":1})");
    RUVIA_CHECK(throws_on([&] { (void)verify_jwt(expired, verify_options(secret)); }));

    // leeway applies to exp as well as nbf: a token that expired a few seconds
    // ago is rejected by default but accepted when leeway covers the gap.
    const auto now_seconds = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch())
                                 .count();
    const std::string recent_payload =
        R"({"sub":"user-1","exp":)" + std::to_string(now_seconds - 10) + "}";
    const auto recently_expired = signed_token_with_payload(secret, recent_payload);
    RUVIA_CHECK(throws_on([&] { (void)verify_jwt(recently_expired, verify_options(secret)); }));
    auto lenient = verify_options(secret);
    lenient.leeway_ = std::chrono::seconds{3600};
    const auto recently_expired_payload = verify_jwt(recently_expired, lenient);
    RUVIA_CHECK_EQ(recently_expired_payload.subject(), std::string_view("user-1"));
}

RUVIA_TEST(jwt_exp_nbf_boundaries_follow_rfc7519) {
    // The exact exp/nbf boundary can't be pinned against the live clock inside
    // jwt_verify, so exercise the pure predicates it delegates to. RFC 7519 §4.1.4:
    // a token is rejected at now == exp (no leeway); §4.1.5: it is valid at
    // now == nbf. leeway widens each window (later for exp, earlier for nbf).
    using std::chrono::seconds;
    const auto t = std::chrono::system_clock::from_time_t(1'000'000'000);

    RUVIA_CHECK(ruvia::detail::jwt_token_expired(t, t, seconds{0}));                // now == exp -> expired
    RUVIA_CHECK(ruvia::detail::jwt_token_expired(t + seconds{1}, t, seconds{0}));   // after exp
    RUVIA_CHECK(!ruvia::detail::jwt_token_expired(t - seconds{1}, t, seconds{0}));  // before exp
    RUVIA_CHECK(
        !ruvia::detail::jwt_token_expired(t + seconds{5}, t, seconds{10}));  // inside leeway grace
    RUVIA_CHECK(ruvia::detail::jwt_token_expired(
        t + seconds{10}, t, seconds{10}));  // now == exp+leeway -> expired

    RUVIA_CHECK(!ruvia::detail::jwt_token_not_yet_valid(t, t, seconds{0}));               // now == nbf -> valid
    RUVIA_CHECK(ruvia::detail::jwt_token_not_yet_valid(t - seconds{1}, t, seconds{0}));   // before nbf
    RUVIA_CHECK(!ruvia::detail::jwt_token_not_yet_valid(t + seconds{1}, t, seconds{0}));  // after nbf
    RUVIA_CHECK(!ruvia::detail::jwt_token_not_yet_valid(
        t - seconds{5}, t, seconds{10}));  // inside leeway grace
    RUVIA_CHECK(
        ruvia::detail::jwt_token_not_yet_valid(t - seconds{11}, t, seconds{10}));  // before nbf-leeway
}

RUVIA_TEST(jwt_time_offset_preserves_clock_ticks) {
    using clock_type = std::chrono::system_clock;
    using std::chrono::seconds;
    for (const auto base : {clock_type::time_point::max() - seconds{2},
             clock_type::time_point::min() + seconds{2}, clock_type::time_point{seconds{2'000'000'000}}}) {
        for (int index = 0; index < 4096; ++index) {
            const auto time = base + clock_type::duration{index};
            const auto unchanged = ruvia::detail::jwt_time_with_offset(time, seconds{0});
            RUVIA_CHECK(unchanged == time);
            if (unchanged != time) {
                return;
            }
            RUVIA_CHECK(ruvia::detail::jwt_time_with_offset(time, seconds{1}) == time + seconds{1});
            RUVIA_CHECK(ruvia::detail::jwt_time_with_offset(time, seconds{-1}) == time - seconds{1});
        }
    }
}

RUVIA_TEST(jwt_time_offset_saturates_extreme_offsets_and_partial_seconds) {
    using clock_type = std::chrono::system_clock;
    using std::chrono::seconds;
    const auto tick = clock_type::duration{1};
    RUVIA_CHECK(ruvia::detail::jwt_time_with_offset(clock_type::time_point::min(), seconds::max()) == clock_type::time_point::max());
    RUVIA_CHECK(ruvia::detail::jwt_time_with_offset(clock_type::time_point::max(), seconds::min()) == clock_type::time_point::min());
    RUVIA_CHECK(ruvia::detail::jwt_time_with_offset(clock_type::time_point::min() + seconds{1}, seconds{-1}) == clock_type::time_point::min());
    RUVIA_CHECK(ruvia::detail::jwt_time_with_offset(clock_type::time_point::max() - seconds{1}, seconds{1}) == clock_type::time_point::max());
    RUVIA_CHECK(ruvia::detail::jwt_from_epoch_seconds(std::numeric_limits<std::int64_t>::max()) == clock_type::time_point::max());
    RUVIA_CHECK(ruvia::detail::jwt_from_epoch_seconds(std::numeric_limits<std::int64_t>::min()) == clock_type::time_point::min());
    RUVIA_CHECK(ruvia::detail::jwt_from_epoch_seconds(-1) == clock_type::time_point{seconds{-1}});
    const auto high_seconds = std::chrono::duration_cast<seconds>(clock_type::duration::max());
    const auto high = clock_type::time_point{high_seconds};
    const auto fraction = (clock_type::time_point::max() - high) / 2;
    RUVIA_CHECK(ruvia::detail::jwt_time_with_offset(clock_type::time_point{fraction - seconds{1}}, high_seconds + seconds{1}) == high + fraction);
    RUVIA_CHECK(ruvia::detail::jwt_time_with_offset(clock_type::time_point{-tick}, high_seconds) == high - tick);
}

RUVIA_TEST(jwt_time_predicates_distinguish_adjacent_clock_ticks) {
    using clock_type = std::chrono::system_clock;
    using std::chrono::seconds;
    const auto tick = clock_type::duration{1};
    for (const auto time : {clock_type::time_point{seconds{2'147'483'648}},
             clock_type::time_point::max() - seconds{20}, clock_type::time_point::min() + seconds{20}}) {
        RUVIA_CHECK(!ruvia::detail::jwt_token_expired(time - tick, time, seconds{0}));
        RUVIA_CHECK(ruvia::detail::jwt_token_not_yet_valid(time - tick, time, seconds{0}));
        RUVIA_CHECK(!ruvia::detail::jwt_token_expired(time + seconds{10} - tick, time, seconds{10}));
        RUVIA_CHECK(ruvia::detail::jwt_token_expired(time + seconds{10}, time, seconds{10}));
        RUVIA_CHECK(ruvia::detail::jwt_token_not_yet_valid(time - seconds{10} - tick, time, seconds{10}));
        RUVIA_CHECK(!ruvia::detail::jwt_token_not_yet_valid(time - seconds{10}, time, seconds{10}));
    }
    RUVIA_CHECK(!ruvia::detail::jwt_token_expired(clock_type::time_point::max(), clock_type::time_point::min(), seconds::max()));
    RUVIA_CHECK(!ruvia::detail::jwt_token_not_yet_valid(clock_type::time_point::min(), clock_type::time_point::max(), seconds::max()));
}

RUVIA_TEST(jwt_decode_unverified_reads_claims_without_authenticating) {
    // jwt_decode_unverified reads the payload WITHOUT checking the signature -- it
    // provides no authentication. Pin that contract: it returns the claims even
    // for a token whose signature has been corrupted, while jwt_verify rejects the
    // same token. Callers must never treat the unverified payload as trusted.
    const auto token = sign(sign_options(secret));
    const auto decoded = decode_jwt_unverified(token);
    RUVIA_CHECK_EQ(decoded.subject(), std::string_view("user-1"));

    std::string forged = token.substr(0, token.rfind('.') + 1) + "corruptedsignature";
    const auto decoded_forged = decode_jwt_unverified(forged);
    RUVIA_CHECK_EQ(decoded_forged.subject(), std::string_view("user-1"));
    RUVIA_CHECK(throws_on([&] { (void)verify_jwt(forged, verify_options(secret)); }));
}

RUVIA_TEST(jwt_verify_rejects_malformed_token) {
    const auto verify = verify_options(secret);
    RUVIA_CHECK(throws_on([&] { (void)verify_jwt("not-a-jwt", verify); }));
    RUVIA_CHECK(throws_on([&] { (void)verify_jwt("only.two", verify); }));  // two sections
    RUVIA_CHECK(throws_on([&] { (void)verify_jwt("a.b.c.d", verify); }));   // four sections
}

RUVIA_TEST(jwt_verify_rejects_none_algorithm_downgrade) {
    // The canonical JWT forgery (the "alg:none" attack): an attacker crafts a
    // header claiming no signature algorithm and strips the signature, hoping the
    // verifier trusts the token's own alg field and skips authentication. Ruvia
    // recomputes the MAC with the server-configured algorithm and compares it to
    // the token's signature, so the forgery fails the signature gate before the
    // alg field is even inspected. The exp requirement is disabled here so the
    // rejection can only come from the signature/alg gates, never a missing exp.
    auto* const resource = std::pmr::get_default_resource();
    const auto header_value =
        ruvia::detail::jwt_base64_url_encode(R"({"alg":"none","typ":"JWT"})", resource);
    const auto payload_value = ruvia::detail::jwt_base64_url_encode(R"({"sub":"admin"})", resource);

    // "<header>.<payload>." -- an empty third section, as an alg:none token has.
    std::string forged;
    forged.append(header_value.data(), header_value.size());
    forged.push_back('.');
    forged.append(payload_value.data(), payload_value.size());
    forged.push_back('.');

    auto verify = verify_options(secret);
    verify.expiration_claim_ = jwt_expiration_claim_policy::allow_missing;
    RUVIA_CHECK(throws_on([&] { (void)verify_jwt(forged, verify); }));

    // The same forgery with attacker-supplied junk in the signature slot is also
    // rejected: no chosen string equals HMAC(secret, signing_input).
    const std::string forged_junk = forged + "AAAA";
    RUVIA_CHECK(throws_on([&] { (void)verify_jwt(forged_junk, verify); }));
}

RUVIA_TEST(jwt_verify_rejects_signed_non_object_payload) {
    auto verify = verify_options(secret);
    verify.expiration_claim_ = jwt_expiration_claim_policy::allow_missing;
    const auto token = signed_token_with_payload(secret, "not-json");
    RUVIA_CHECK(throws_on([&] { (void)verify_jwt(token, verify); }));
}

RUVIA_TEST(jwt_base64url_round_trip_and_strict_decode) {
    using ruvia::detail::jwt_base64_url_decode;
    using ruvia::detail::jwt_base64_url_encode;
    auto* res = std::pmr::get_default_resource();

    // Round trip over every remainder length, including bytes that need the -/_
    // alphabet (0xFB 0xFF 0xBF -> "-_-_").
    for (const std::string_view sample : {std::string_view(""), std::string_view("f"),
             std::string_view("fo"), std::string_view("foo"), std::string_view("\xfb\xff\xbf")}) {
        const auto encoded = jwt_base64_url_encode(sample, res);
        const auto decoded =
            jwt_base64_url_decode(std::string_view(encoded.data(), encoded.size()), res);
        RUVIA_CHECK_EQ(std::string_view(decoded.data(), decoded.size()), sample);
    }

    // "QQ" is the canonical encoding of the single byte 'A'. "QR" would decode to
    // the same byte but leaves non-zero trailing bits, so it must be rejected --
    // otherwise a token part would have multiple valid spellings (malleability).
    const auto canonical = jwt_base64_url_decode("QQ", res);
    RUVIA_CHECK(canonical.size() == 1 && canonical[0] == 'A');
    RUVIA_CHECK(throws_on([&] { (void)jwt_base64_url_decode("QR", res); }));

    // '=' padding and the standard-base64 '+' '/' are not part of base64url.
    RUVIA_CHECK(throws_on([&] { (void)jwt_base64_url_decode("QQ==", res); }));
    RUVIA_CHECK(throws_on([&] { (void)jwt_base64_url_decode("a+/b", res); }));

    // A length of 1 (mod 4) cannot encode a whole byte group.
    RUVIA_CHECK(throws_on([&] { (void)jwt_base64_url_decode("abcde", res); }));
}

RUVIA_TEST(jwt_base64url_preserves_binary_values_and_releases_output_storage) {
    ruvia::test::counting_memory_resource resource;
    std::string input;
    for (std::size_t size = 0; size <= 257; ++size) {
        {
            const auto encoded = ruvia::detail::jwt_base64_url_encode(input, &resource);
            const auto remainder = input.size() % 3;
            RUVIA_CHECK_EQ(encoded.size(), (input.size() / 3) * 4 + (remainder == 0 ? 0 : remainder + 1));
            const auto decoded = ruvia::detail::jwt_base64_url_decode(encoded, &resource);
            RUVIA_CHECK_EQ(std::string_view(decoded), std::string_view(input));
        }
        RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
        input.push_back(static_cast<char>(size));
    }
    {
        const auto encoded = ruvia::detail::jwt_base64_url_encode("\xfb\xff\xbf", &resource);
        RUVIA_CHECK_EQ(std::string_view(encoded), std::string_view("-_-_"));
    }
    const std::string invalid = std::string(128, 'A') + '?';
    RUVIA_CHECK(throws_on([&] { (void)ruvia::detail::jwt_base64_url_decode(invalid, &resource); }));
    RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
}

RUVIA_TEST(jwt_base64url_decode_preserves_error_categories) {
    const struct {
        std::string_view input_;
        std::string_view message_;
    } cases[] = {
        {"AA=", "JWT base64url value is invalid"},
        {"A", "JWT base64url has invalid length"},
        {"QR", "JWT base64url is not canonical"},
    };
    for (const auto& test : cases) {
        bool rejected = false;
        try {
            (void)ruvia::detail::jwt_base64_url_decode(test.input_, std::pmr::get_default_resource());
        } catch (const std::invalid_argument& error) {
            rejected = true;
            RUVIA_CHECK_EQ(std::string_view(error.what()), test.message_);
        }
        RUVIA_CHECK(rejected);
    }
}

RUVIA_TEST(jwt_base64url_rejects_unrepresentable_capacity_hint) {
    using ruvia::detail::jwt_base64_url_encode;
    const auto max_value = std::numeric_limits<std::size_t>::max();
    const auto too_large = (max_value / 4) * 3 + 1;
    const std::string_view fake_input("x", too_large);

    bool length_error = false;
    try {
        (void)jwt_base64_url_encode(fake_input, std::pmr::get_default_resource());
    } catch (const std::length_error&) {
        length_error = true;
    } catch (...) {
    }
    RUVIA_CHECK(length_error);
}

RUVIA_TEST(jwt_bearer_token_extraction) {
    RUVIA_CHECK(jwt_bearer_token("Bearer abc.def.ghi").value() == std::string_view("abc.def.ghi"));
    RUVIA_CHECK(jwt_bearer_token("Bearer    abc.def.ghi").value() == std::string_view("abc.def.ghi"));
    RUVIA_CHECK(jwt_bearer_token("bearer xyz").value() ==
                std::string_view("xyz"));  // scheme is case-insensitive
    RUVIA_CHECK(!jwt_bearer_token("Basic abc").has_value());
    RUVIA_CHECK(!jwt_bearer_token("Bearer").has_value());  // scheme only, no token
    RUVIA_CHECK(!jwt_bearer_token("Bearer    ").has_value());
    RUVIA_CHECK(
        !jwt_bearer_token("Bearer\tabc").has_value());  // auth-param separator is SP, not HTAB
}
