#include <doctest/doctest.h>
#include "ruvia/http/HttpTypes.h"
#include "ruvia/http/Error.h"

using namespace ruvia;

// ─── isValidHttpHeaderName ─────────────────────────────────────────────────

TEST_SUITE("HttpTypes / isValidHttpHeaderName") {

TEST_CASE("valid header names") {
    CHECK(isValidHttpHeaderName("Content-Type"));
    CHECK(isValidHttpHeaderName("X-Custom-Header"));
    CHECK(isValidHttpHeaderName("accept"));
    CHECK(isValidHttpHeaderName("ETag"));
    CHECK(isValidHttpHeaderName("x"));
}

TEST_CASE("empty name rejected") {
    CHECK_FALSE(isValidHttpHeaderName(""));
}

TEST_CASE("space in name rejected") {
    CHECK_FALSE(isValidHttpHeaderName("Content Type"));
    CHECK_FALSE(isValidHttpHeaderName(" Content-Type"));
}

TEST_CASE("colon rejected (separates name from value)") {
    CHECK_FALSE(isValidHttpHeaderName("X:Header"));
}

TEST_CASE("special token chars accepted") {
    CHECK(isValidHttpHeaderName("X!Header"));
    CHECK(isValidHttpHeaderName("X#Header"));
    CHECK(isValidHttpHeaderName("X~Header"));
}

}  // TEST_SUITE

// ─── isValidHttpHeaderValue ────────────────────────────────────────────────

TEST_SUITE("HttpTypes / isValidHttpHeaderValue (RFC 9110 §5.5 fix)") {

TEST_CASE("plain ASCII accepted") {
    CHECK(isValidHttpHeaderValue("Hello, World!"));
    CHECK(isValidHttpHeaderValue("application/json; charset=utf-8"));
}

TEST_CASE("empty value accepted") {
    CHECK(isValidHttpHeaderValue(""));
}

TEST_CASE("obs-text bytes (0x80-0xFF) accepted") {
    std::string v = "x";
    v[0] = static_cast<char>(0x80);
    CHECK(isValidHttpHeaderValue(v));
}

TEST_CASE("HTAB (0x09) accepted") {
    std::string v = "\t";
    CHECK(isValidHttpHeaderValue(v));
}

TEST_CASE("CR (0x0D) rejected") {
    CHECK_FALSE(isValidHttpHeaderValue("foo\rbar"));
}

TEST_CASE("LF (0x0A) rejected") {
    CHECK_FALSE(isValidHttpHeaderValue("foo\nbar"));
}

TEST_CASE("NUL (0x00) rejected") {
    std::string v = "foo";
    v.push_back('\0');
    v += "bar";
    CHECK_FALSE(isValidHttpHeaderValue(v));
}

TEST_CASE("DEL (0x7F) rejected — our RFC 9110 fix") {
    std::string v = "foo";
    v.push_back(static_cast<char>(0x7F));
    CHECK_FALSE(isValidHttpHeaderValue(v));
}

TEST_CASE("control char 0x01 rejected") {
    std::string v = "foo";
    v.push_back('\x01');
    CHECK_FALSE(isValidHttpHeaderValue(v));
}

TEST_CASE("control char 0x1F rejected") {
    std::string v = "foo";
    v.push_back('\x1F');
    CHECK_FALSE(isValidHttpHeaderValue(v));
}

TEST_CASE("vertical tab 0x0B rejected") {
    std::string v = "foo";
    v.push_back('\x0B');
    CHECK_FALSE(isValidHttpHeaderValue(v));
}

}  // TEST_SUITE

// ─── HttpRequest::query ────────────────────────────────────────────────────

TEST_SUITE("HttpTypes / HttpRequest::query") {

HttpParseResult parseRequest(std::string_view raw) {
    ruvia::HttpParser parser;
    auto result = parser.parse(raw);
    result.request.setResource(std::pmr::get_default_resource());
    return result;
}

TEST_CASE("simple query param") {
    auto r = parseRequest("GET /path?name=alice HTTP/1.1\r\nHost: h\r\n\r\n");
    REQUIRE(r.status == HttpParseStatus::kComplete);
    auto v = r.request.query("name");
    REQUIRE(v.exists());
    CHECK(v.toStringView() == "alice");
}

TEST_CASE("multiple query params") {
    auto r = parseRequest("GET /?a=1&b=2&c=3 HTTP/1.1\r\nHost: h\r\n\r\n");
    REQUIRE(r.status == HttpParseStatus::kComplete);
    CHECK(r.request.query("a").toStringView() == "1");
    CHECK(r.request.query("b").toStringView() == "2");
    CHECK(r.request.query("c").toStringView() == "3");
}

TEST_CASE("missing param returns nullopt") {
    auto r = parseRequest("GET /?a=1 HTTP/1.1\r\nHost: h\r\n\r\n");
    REQUIRE(r.status == HttpParseStatus::kComplete);
    CHECK_FALSE(r.request.query("b").exists());
}

TEST_CASE("encoded query param decoded") {
    auto r = parseRequest("GET /?name=hello%20world HTTP/1.1\r\nHost: h\r\n\r\n");
    REQUIRE(r.status == HttpParseStatus::kComplete);
    auto v = r.request.query("name");
    REQUIRE(v.exists());
    auto s = v.toString();
    REQUIRE(s.has_value());
    CHECK(*s == "hello world");
}

TEST_CASE("integer conversion") {
    auto r = parseRequest("GET /?count=42 HTTP/1.1\r\nHost: h\r\n\r\n");
    REQUIRE(r.status == HttpParseStatus::kComplete);
    auto v = r.request.query("count");
    REQUIRE(v.exists());
    CHECK(v.toInt() == 42);
    CHECK(v.toInt32() == 42);
    CHECK(v.toUInt32() == 42u);
    CHECK(v.toInt64() == 42);
}

TEST_CASE("bool conversion") {
    auto r = parseRequest("GET /?flag=true HTTP/1.1\r\nHost: h\r\n\r\n");
    REQUIRE(r.status == HttpParseStatus::kComplete);
    auto v = r.request.query("flag");
    REQUIRE(v.exists());
    CHECK(v.toBool() == true);
}

}  // TEST_SUITE

// ─── HttpRequest::cookie ───────────────────────────────────────────────────

TEST_SUITE("HttpTypes / HttpRequest::cookie") {

HttpParseResult parseRequest(std::string_view raw) {
    ruvia::HttpParser parser;
    auto result = parser.parse(raw);
    result.request.setResource(std::pmr::get_default_resource());
    return result;
}

TEST_CASE("single cookie") {
    auto r = parseRequest("GET / HTTP/1.1\r\nHost: h\r\nCookie: session=abc123\r\n\r\n");
    REQUIRE(r.status == HttpParseStatus::kComplete);
    auto v = r.request.cookie("session");
    REQUIRE(v.has_value());
    CHECK(*v == "abc123");
}

TEST_CASE("multiple cookies") {
    auto r = parseRequest("GET / HTTP/1.1\r\nHost: h\r\nCookie: a=1; b=2; c=3\r\n\r\n");
    REQUIRE(r.status == HttpParseStatus::kComplete);
    CHECK(r.request.cookie("a") == "1");
    CHECK(r.request.cookie("b") == "2");
    CHECK(r.request.cookie("c") == "3");
}

TEST_CASE("missing cookie returns nullopt") {
    auto r = parseRequest("GET / HTTP/1.1\r\nHost: h\r\nCookie: a=1\r\n\r\n");
    REQUIRE(r.status == HttpParseStatus::kComplete);
    CHECK_FALSE(r.request.cookie("b").has_value());
}

TEST_CASE("no Cookie header returns nullopt") {
    auto r = parseRequest("GET / HTTP/1.1\r\nHost: h\r\n\r\n");
    REQUIRE(r.status == HttpParseStatus::kComplete);
    CHECK_FALSE(r.request.cookie("session").has_value());
}

TEST_CASE("cookie value with spaces stripped") {
    auto r = parseRequest("GET / HTTP/1.1\r\nHost: h\r\nCookie: key = value \r\n\r\n");
    REQUIRE(r.status == HttpParseStatus::kComplete);
    auto v = r.request.cookie("key");
    REQUIRE(v.has_value());
    CHECK(*v == "value");
}

}  // TEST_SUITE

// ─── HttpResponse header manipulation ─────────────────────────────────────

TEST_SUITE("HttpTypes / HttpResponse header manipulation") {

TEST_CASE("setHeader then read back") {
    HttpResponse resp(std::pmr::get_default_resource());
    resp.setHeader("Content-Type", "application/json");
    CHECK(resp.header("Content-Type") == "application/json");
    CHECK(resp.hasKnownHeader(HttpResponse::kKnownHeaderContentType));
}

TEST_CASE("setHeader overwrites same-name header") {
    HttpResponse resp(std::pmr::get_default_resource());
    resp.setHeader("Content-Type", "text/plain");
    resp.setHeader("Content-Type", "application/json");
    CHECK(resp.header("Content-Type") == "application/json");
    // Only one Content-Type header should exist
    std::size_t count = 0;
    for (const auto& h : resp.headers()) {
        if (h.name() == "Content-Type") ++count;
    }
    CHECK(count == 1);
}

TEST_CASE("appendHeader allows duplicate Set-Cookie") {
    HttpResponse resp(std::pmr::get_default_resource());
    resp.appendHeader("Set-Cookie", "a=1");
    resp.appendHeader("Set-Cookie", "b=2");
    std::size_t count = 0;
    for (const auto& h : resp.headers()) {
        if (h.name() == "Set-Cookie") ++count;
    }
    CHECK(count == 2);
}

TEST_CASE("setStatus sets code and text") {
    HttpResponse resp(std::pmr::get_default_resource());
    resp.setStatus(404, "Not Found");
    CHECK(resp.statusCode() == 404);
    CHECK(resp.statusText() == "Not Found");
}

TEST_CASE("setBody owns value") {
    HttpResponse resp(std::pmr::get_default_resource());
    std::pmr::string body(std::pmr::get_default_resource());
    body.assign("hello");
    resp.setBody(std::move(body));
    CHECK(resp.bodyBytes() == "hello");
    CHECK(resp.bodySize() == 5);
}

TEST_CASE("invalid header name throws") {
    HttpResponse resp(std::pmr::get_default_resource());
    CHECK_THROWS(resp.setHeader("X Header", "value"));  // space in name
}

TEST_CASE("header value with DEL throws — our fix") {
    HttpResponse resp(std::pmr::get_default_resource());
    std::string bad = "value";
    bad.push_back(static_cast<char>(0x7F));
    CHECK_THROWS(resp.setHeader("X-Test", bad));
}

}  // TEST_SUITE
