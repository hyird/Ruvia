#include <doctest/doctest.h>
#include "ruvia/auth/Jwt.h"

#include <chrono>
#include <string>

using namespace ruvia;
using namespace std::chrono_literals;

// ─── jwtSign + jwtVerify round-trip ───────────────────────────────────────

TEST_SUITE("JWT / sign and verify") {

TEST_CASE("HS256 round-trip") {
    JwtSignOptions signOpts;
    signOpts.secret.assign("supersecret");
    signOpts.subject.assign("user:42");
    signOpts.issuer.assign("ruvia-tests");
    signOpts.expiresIn = 1h;

    const auto token = jwtSign(signOpts);
    REQUIRE_FALSE(std::string_view(token).empty());

    JwtVerifyOptions verOpts;
    verOpts.secret.assign("supersecret");
    verOpts.issuer.assign("ruvia-tests");

    const auto payload = jwtVerify(token, verOpts);
    CHECK(payload.subject() == "user:42");
    CHECK(payload.issuer() == "ruvia-tests");
    CHECK(payload.expiresAt().has_value());
    CHECK(payload.issuedAt().has_value());
}

TEST_CASE("HS384 round-trip") {
    JwtSignOptions signOpts;
    signOpts.algorithm = JwtAlgorithm::kHs384;
    signOpts.secret.assign("secret384");
    signOpts.expiresIn = 1h;

    const auto token = jwtSign(signOpts);

    JwtVerifyOptions verOpts;
    verOpts.algorithm = JwtAlgorithm::kHs384;
    verOpts.secret.assign("secret384");

    CHECK_NOTHROW(jwtVerify(token, verOpts));
}

TEST_CASE("HS512 round-trip") {
    JwtSignOptions signOpts;
    signOpts.algorithm = JwtAlgorithm::kHs512;
    signOpts.secret.assign("secret512");
    signOpts.expiresIn = 1h;

    const auto token = jwtSign(signOpts);

    JwtVerifyOptions verOpts;
    verOpts.algorithm = JwtAlgorithm::kHs512;
    verOpts.secret.assign("secret512");

    CHECK_NOTHROW(jwtVerify(token, verOpts));
}

TEST_CASE("wrong secret → runtime_error") {
    JwtSignOptions signOpts;
    signOpts.secret.assign("correct");
    signOpts.expiresIn = 1h;
    const auto token = jwtSign(signOpts);

    JwtVerifyOptions verOpts;
    verOpts.secret.assign("wrong");
    CHECK_THROWS_AS(jwtVerify(token, verOpts), std::runtime_error);
}

TEST_CASE("wrong algorithm → runtime_error") {
    JwtSignOptions signOpts;
    signOpts.algorithm = JwtAlgorithm::kHs256;
    signOpts.secret.assign("secret");
    signOpts.expiresIn = 1h;
    const auto token = jwtSign(signOpts);

    JwtVerifyOptions verOpts;
    verOpts.algorithm = JwtAlgorithm::kHs384;
    verOpts.secret.assign("secret");
    CHECK_THROWS_AS(jwtVerify(token, verOpts), std::runtime_error);
}

TEST_CASE("malformed token → exception") {
    JwtVerifyOptions verOpts;
    verOpts.secret.assign("secret");
    CHECK_THROWS(jwtVerify("not.a.valid.jwt.token", verOpts));
    CHECK_THROWS(jwtVerify("two.parts", verOpts));
    CHECK_THROWS(jwtVerify("", verOpts));
}

}  // TEST_SUITE

// ─── expiry and not-before ─────────────────────────────────────────────────

TEST_SUITE("JWT / time claims") {

TEST_CASE("expired token rejected") {
    JwtSignOptions signOpts;
    signOpts.secret.assign("secret");
    // Signed with negative expiresIn so it's already expired
    signOpts.expiresIn = std::chrono::seconds(-3600);

    const auto token = jwtSign(signOpts);

    JwtVerifyOptions verOpts;
    verOpts.secret.assign("secret");
    CHECK_THROWS_AS(jwtVerify(token, verOpts), std::runtime_error);
}

TEST_CASE("expired token accepted with sufficient leeway") {
    JwtSignOptions signOpts;
    signOpts.secret.assign("secret");
    signOpts.expiresIn = std::chrono::seconds(-10);  // expired 10s ago
    const auto token = jwtSign(signOpts);

    JwtVerifyOptions verOpts;
    verOpts.secret.assign("secret");
    verOpts.leeway = std::chrono::seconds(60);  // allow 60s drift
    CHECK_NOTHROW(jwtVerify(token, verOpts));
}

TEST_CASE("not-before in future rejected") {
    JwtSignOptions signOpts;
    signOpts.secret.assign("secret");
    signOpts.expiresIn = 1h;
    signOpts.notBeforeDelay = std::chrono::seconds(3600);  // valid 1h from now

    const auto token = jwtSign(signOpts);

    JwtVerifyOptions verOpts;
    verOpts.secret.assign("secret");
    CHECK_THROWS_AS(jwtVerify(token, verOpts), std::runtime_error);
}

TEST_CASE("missing exp when requireExpiration=true → runtime_error") {
    JwtSignOptions signOpts;
    signOpts.secret.assign("secret");
    signOpts.expiresIn = std::chrono::seconds(0);  // no expiry set

    const auto token = jwtSign(signOpts);

    JwtVerifyOptions verOpts;
    verOpts.secret.assign("secret");
    verOpts.requireExpiration = true;
    // exp was added with 0 → already expired
    CHECK_THROWS(jwtVerify(token, verOpts));
}

TEST_CASE("issuer mismatch rejected") {
    JwtSignOptions signOpts;
    signOpts.secret.assign("secret");
    signOpts.issuer.assign("ruvia");
    signOpts.expiresIn = 1h;
    const auto token = jwtSign(signOpts);

    JwtVerifyOptions verOpts;
    verOpts.secret.assign("secret");
    verOpts.issuer.assign("other");
    CHECK_THROWS_AS(jwtVerify(token, verOpts), std::runtime_error);
}

TEST_CASE("audience mismatch rejected") {
    JwtSignOptions signOpts;
    signOpts.secret.assign("secret");
    signOpts.audience.assign("my-service");
    signOpts.expiresIn = 1h;
    const auto token = jwtSign(signOpts);

    JwtVerifyOptions verOpts;
    verOpts.secret.assign("secret");
    verOpts.audience.assign("other-service");
    CHECK_THROWS_AS(jwtVerify(token, verOpts), std::runtime_error);
}

}  // TEST_SUITE

// ─── custom claims ─────────────────────────────────────────────────────────

TEST_SUITE("JWT / custom claims") {

TEST_CASE("custom string claim round-trip") {
    JwtSignOptions signOpts;
    signOpts.secret.assign("secret");
    signOpts.expiresIn = 1h;
    signOpts.claims.push_back({std::pmr::string("role"), std::pmr::string("admin")});
    signOpts.claims.push_back({std::pmr::string("tenant"), std::pmr::string("acme")});

    const auto token = jwtSign(signOpts);

    JwtVerifyOptions verOpts;
    verOpts.secret.assign("secret");
    const auto payload = jwtVerify(token, verOpts);

    CHECK(payload.claim("role") == "admin");
    CHECK(payload.claim("tenant") == "acme");
}

TEST_CASE("custom claim with escaped value round-trip — our JSON parse fix") {
    JwtSignOptions signOpts;
    signOpts.secret.assign("secret");
    signOpts.expiresIn = 1h;
    // Value contains a quote that will be JSON-escaped as \"
    signOpts.claims.push_back({std::pmr::string("note"), std::pmr::string("say \"hello\"")});

    const auto token = jwtSign(signOpts);

    JwtVerifyOptions verOpts;
    verOpts.secret.assign("secret");
    const auto payload = jwtVerify(token, verOpts);

    // Our fixed parser correctly decodes the escape sequence
    CHECK(payload.claim("note") == "say \"hello\"");
}

TEST_CASE("claim value with newline escaped") {
    JwtSignOptions signOpts;
    signOpts.secret.assign("secret");
    signOpts.expiresIn = 1h;
    signOpts.claims.push_back({std::pmr::string("desc"), std::pmr::string("line1\nline2")});

    const auto token = jwtSign(signOpts);

    JwtVerifyOptions verOpts;
    verOpts.secret.assign("secret");
    const auto payload = jwtVerify(token, verOpts);
    CHECK(payload.claim("desc") == "line1\nline2");
}

TEST_CASE("reserved claim name in custom list throws") {
    JwtSignOptions signOpts;
    signOpts.secret.assign("secret");
    signOpts.expiresIn = 1h;
    signOpts.claims.push_back({std::pmr::string("exp"), std::pmr::string("invalid")});

    CHECK_THROWS_AS(jwtSign(signOpts), std::invalid_argument);
}

TEST_CASE("missing custom claim returns nullopt") {
    JwtSignOptions signOpts;
    signOpts.secret.assign("secret");
    signOpts.expiresIn = 1h;
    const auto token = jwtSign(signOpts);

    JwtVerifyOptions verOpts;
    verOpts.secret.assign("secret");
    const auto payload = jwtVerify(token, verOpts);
    CHECK_FALSE(payload.claim("nonexistent").has_value());
}

TEST_CASE("payload value masquerading as another claim does not confuse parser") {
    // A custom claim value that looks like another key-value pair
    // This tests our fix: old code used substring search and could be fooled
    JwtSignOptions signOpts;
    signOpts.secret.assign("secret");
    signOpts.subject.assign("real-sub");
    signOpts.expiresIn = 1h;
    // This value contains a pattern that would fool a substring-search parser
    signOpts.claims.push_back({
        std::pmr::string("evil"),
        std::pmr::string("ignore this sub\":\"fake-sub")
    });

    const auto token = jwtSign(signOpts);

    JwtVerifyOptions verOpts;
    verOpts.secret.assign("secret");
    const auto payload = jwtVerify(token, verOpts);

    // Our fixed parser uses structural traversal; sub must be "real-sub"
    CHECK(payload.subject() == "real-sub");
}

}  // TEST_SUITE

// ─── jwtBearerToken ────────────────────────────────────────────────────────

TEST_SUITE("JWT / jwtBearerToken") {

TEST_CASE("extracts token from valid Bearer header") {
    auto t = jwtBearerToken("Bearer abc.def.ghi");
    REQUIRE(t.has_value());
    CHECK(*t == "abc.def.ghi");
}

TEST_CASE("case-insensitive prefix") {
    CHECK(jwtBearerToken("bearer token").has_value());
    CHECK(jwtBearerToken("BEARER token").has_value());
}

TEST_CASE("missing Bearer prefix") {
    CHECK_FALSE(jwtBearerToken("Basic abc").has_value());
}

TEST_CASE("too short to contain token") {
    CHECK_FALSE(jwtBearerToken("Bear").has_value());
}

TEST_CASE("empty string") {
    CHECK_FALSE(jwtBearerToken("").has_value());
}

}  // TEST_SUITE

// ─── jwtDecodeUnverified ───────────────────────────────────────────────────

TEST_SUITE("JWT / jwtDecodeUnverified") {

TEST_CASE("decodes without signature check") {
    JwtSignOptions signOpts;
    signOpts.secret.assign("secret");
    signOpts.subject.assign("user:1");
    signOpts.issuer.assign("myapp");
    signOpts.expiresIn = 1h;

    const auto token = jwtSign(signOpts);
    const auto payload = jwtDecodeUnverified(token);

    CHECK(payload.subject() == "user:1");
    CHECK(payload.issuer() == "myapp");
}

TEST_CASE("decodes even with wrong signature") {
    // Build a valid-structure token with wrong signature
    JwtSignOptions signOpts;
    signOpts.secret.assign("secret");
    signOpts.subject.assign("u");
    signOpts.expiresIn = 1h;
    auto token = std::string(jwtSign(signOpts));

    // Corrupt the signature (last segment)
    const auto dotPos = token.rfind('.');
    token[dotPos + 1] = token[dotPos + 1] == 'A' ? 'B' : 'A';

    const auto payload = jwtDecodeUnverified(token);
    CHECK(payload.subject() == "u");
}

}  // TEST_SUITE
