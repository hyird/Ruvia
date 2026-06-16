#include <doctest/doctest.h>
#include "ruvia/http/HeaderUtils.h"

using namespace ruvia::detail;

// ─── httpTrimOws ───────────────────────────────────────────────────────────

TEST_SUITE("HeaderUtils / httpTrimOws") {

TEST_CASE("no whitespace unchanged") {
    CHECK(httpTrimOws("hello") == "hello");
}

TEST_CASE("leading and trailing spaces stripped") {
    CHECK(httpTrimOws("  hello  ") == "hello");
}

TEST_CASE("tabs stripped") {
    CHECK(httpTrimOws("\t\thello\t") == "hello");
}

TEST_CASE("mixed SP and HT") {
    CHECK(httpTrimOws(" \t hello \t ") == "hello");
}

TEST_CASE("all whitespace → empty") {
    CHECK(httpTrimOws("   ").empty());
}

TEST_CASE("empty input unchanged") {
    CHECK(httpTrimOws("").empty());
}

}  // TEST_SUITE

// ─── httpAsciiEqualsIgnoreCase ─────────────────────────────────────────────

TEST_SUITE("HeaderUtils / httpAsciiEqualsIgnoreCase") {

TEST_CASE("exact match") {
    CHECK(httpAsciiEqualsIgnoreCase("gzip", "gzip"));
}

TEST_CASE("case-insensitive match") {
    CHECK(httpAsciiEqualsIgnoreCase("GZIP", "gzip"));
    CHECK(httpAsciiEqualsIgnoreCase("gzip", "GZIP"));
    CHECK(httpAsciiEqualsIgnoreCase("Gzip", "gzip"));
}

TEST_CASE("different strings") {
    CHECK_FALSE(httpAsciiEqualsIgnoreCase("gzip", "deflate"));
}

TEST_CASE("different lengths") {
    CHECK_FALSE(httpAsciiEqualsIgnoreCase("gz", "gzip"));
}

TEST_CASE("empty strings equal") {
    CHECK(httpAsciiEqualsIgnoreCase("", ""));
}

TEST_CASE("one empty") {
    CHECK_FALSE(httpAsciiEqualsIgnoreCase("", "a"));
}

}  // TEST_SUITE

// ─── httpHasToken ──────────────────────────────────────────────────────────

TEST_SUITE("HeaderUtils / httpHasToken") {

TEST_CASE("single token match") {
    CHECK(httpHasToken("gzip", "gzip"));
}

TEST_CASE("single token case-insensitive") {
    CHECK(httpHasToken("GZIP", "gzip"));
}

TEST_CASE("token in list") {
    CHECK(httpHasToken("gzip, deflate, br", "deflate"));
}

TEST_CASE("token at end of list") {
    CHECK(httpHasToken("gzip, deflate", "deflate"));
}

TEST_CASE("token not present") {
    CHECK_FALSE(httpHasToken("gzip, deflate", "br"));
}

TEST_CASE("empty list") {
    CHECK_FALSE(httpHasToken("", "gzip"));
}

TEST_CASE("extra whitespace around tokens") {
    CHECK(httpHasToken("  gzip , deflate ", "gzip"));
}

TEST_CASE("partial match not confused with full token") {
    CHECK_FALSE(httpHasToken("keep-alive", "alive"));
}

}  // TEST_SUITE

// ─── httpQualityParameter ──────────────────────────────────────────────────

TEST_SUITE("HeaderUtils / httpQualityParameter") {

TEST_CASE("no q parameter → quality 1000") {
    CHECK(httpQualityParameter("gzip") == 1000);
}

TEST_CASE("q=1 → 1000") {
    CHECK(httpQualityParameter("gzip;q=1") == 1000);
}

TEST_CASE("q=0 → 0") {
    CHECK(httpQualityParameter("gzip;q=0") == 0);
}

TEST_CASE("q=0.5 → 500") {
    CHECK(httpQualityParameter("gzip;q=0.5") == 500);
}

TEST_CASE("q=0.001 → 1") {
    CHECK(httpQualityParameter("gzip;q=0.001") == 1);
}

TEST_CASE("q with spaces") {
    CHECK(httpQualityParameter("gzip ; q=0.5") == 500);
}

TEST_CASE("invalid q → 0") {
    CHECK(httpQualityParameter("gzip;q=abc") == 0);
}

}  // TEST_SUITE

// ─── httpAcceptsEncoding ───────────────────────────────────────────────────

TEST_SUITE("HeaderUtils / httpAcceptsEncoding") {

TEST_CASE("gzip in Accept-Encoding") {
    CHECK(httpAcceptsEncoding("gzip, deflate", "gzip"));
}

TEST_CASE("case-insensitive") {
    CHECK(httpAcceptsEncoding("GZIP", "gzip"));
}

TEST_CASE("not present") {
    CHECK_FALSE(httpAcceptsEncoding("deflate", "gzip"));
}

TEST_CASE("wildcard * accepts anything") {
    CHECK(httpAcceptsEncoding("*", "gzip"));
}

TEST_CASE("gzip;q=0 → not accepted") {
    CHECK_FALSE(httpAcceptsEncoding("gzip;q=0", "gzip"));
}

TEST_CASE("explicit q=0 overrides wildcard") {
    CHECK_FALSE(httpAcceptsEncoding("gzip;q=0, *", "gzip"));
}

TEST_CASE("wildcard;q=0 → not accepted") {
    CHECK_FALSE(httpAcceptsEncoding("*;q=0", "gzip"));
}

TEST_CASE("empty Accept-Encoding") {
    CHECK_FALSE(httpAcceptsEncoding("", "gzip"));
}

}  // TEST_SUITE

// ─── httpAcceptsMediaType ──────────────────────────────────────────────────

TEST_SUITE("HeaderUtils / httpAcceptsMediaType") {

TEST_CASE("empty accept matches anything") {
    CHECK(httpAcceptsMediaType("", "application/json"));
}

TEST_CASE("exact match") {
    CHECK(httpAcceptsMediaType("application/json", "application/json"));
}

TEST_CASE("type/* wildcard") {
    CHECK(httpAcceptsMediaType("application/*", "application/json"));
}

TEST_CASE("*/* wildcard") {
    CHECK(httpAcceptsMediaType("*/*", "text/html"));
}

TEST_CASE("different type rejected") {
    CHECK_FALSE(httpAcceptsMediaType("text/html", "application/json"));
}

TEST_CASE("q=0 rejects") {
    CHECK_FALSE(httpAcceptsMediaType("application/json;q=0", "application/json"));
}

TEST_CASE("specific overrides wildcard q=0") {
    CHECK(httpAcceptsMediaType("application/json, */*;q=0", "application/json"));
}

TEST_CASE("case-insensitive media type") {
    CHECK(httpAcceptsMediaType("Application/JSON", "application/json"));
}

}  // TEST_SUITE

// ─── httpContentTypeParameter ──────────────────────────────────────────────

TEST_SUITE("HeaderUtils / httpContentTypeParameter") {

TEST_CASE("boundary extracted from multipart") {
    auto b = httpContentTypeParameter("multipart/form-data; boundary=abc123", "boundary");
    REQUIRE(b.has_value());
    CHECK(*b == "abc123");
}

TEST_CASE("quoted boundary") {
    auto b = httpContentTypeParameter("multipart/form-data; boundary=\"--abc\"", "boundary");
    REQUIRE(b.has_value());
    CHECK(*b == "--abc");
}

TEST_CASE("charset from Content-Type") {
    auto cs = httpContentTypeParameter("text/html; charset=utf-8", "charset");
    REQUIRE(cs.has_value());
    CHECK(*cs == "utf-8");
}

TEST_CASE("missing parameter returns nullopt") {
    auto b = httpContentTypeParameter("application/json", "boundary");
    CHECK_FALSE(b.has_value());
}

TEST_CASE("case-insensitive parameter name") {
    auto b = httpContentTypeParameter("multipart/form-data; BOUNDARY=x", "boundary");
    REQUIRE(b.has_value());
    CHECK(*b == "x");
}

}  // TEST_SUITE

// ─── httpUpdateConnectionFlags ─────────────────────────────────────────────

TEST_SUITE("HeaderUtils / httpUpdateConnectionFlags") {

TEST_CASE("close token sets close flag") {
    bool close = false, keepAlive = false, upgrade = false;
    httpUpdateConnectionFlags("close", close, keepAlive, upgrade);
    CHECK(close);
    CHECK_FALSE(keepAlive);
}

TEST_CASE("keep-alive token") {
    bool close = false, keepAlive = false, upgrade = false;
    httpUpdateConnectionFlags("keep-alive", close, keepAlive, upgrade);
    CHECK(keepAlive);
}

TEST_CASE("Upgrade token (case-insensitive)") {
    bool close = false, keepAlive = false, upgrade = false;
    httpUpdateConnectionFlags("upgrade", close, keepAlive, upgrade);
    CHECK(upgrade);
}

TEST_CASE("multiple tokens") {
    bool close = false, keepAlive = false, upgrade = false;
    httpUpdateConnectionFlags("keep-alive, Upgrade", close, keepAlive, upgrade);
    CHECK(keepAlive);
    CHECK(upgrade);
    CHECK_FALSE(close);
}

TEST_CASE("flags not reset (only set)") {
    bool close = true, keepAlive = false, upgrade = false;
    httpUpdateConnectionFlags("keep-alive", close, keepAlive, upgrade);
    CHECK(close);   // still true from before
    CHECK(keepAlive);
}

}  // TEST_SUITE

// ─── httpUpdateExpectContinueFlag ──────────────────────────────────────────

TEST_SUITE("HeaderUtils / httpUpdateExpectContinueFlag") {

TEST_CASE("100-continue sets flag") {
    bool flag = false;
    CHECK(httpUpdateExpectContinueFlag("100-continue", flag));
    CHECK(flag);
}

TEST_CASE("case-insensitive") {
    bool flag = false;
    CHECK(httpUpdateExpectContinueFlag("100-Continue", flag));
    CHECK(flag);
}

TEST_CASE("unknown expect value returns false") {
    bool flag = false;
    CHECK_FALSE(httpUpdateExpectContinueFlag("200-ok", flag));
    CHECK_FALSE(flag);
}

TEST_CASE("empty expect value returns false") {
    bool flag = false;
    CHECK_FALSE(httpUpdateExpectContinueFlag("", flag));
}

}  // TEST_SUITE
