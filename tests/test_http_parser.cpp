#include <doctest/doctest.h>
#include "ruvia/http/HttpParser.h"

using namespace ruvia;

namespace {

HttpParseResult parse(std::string_view raw) {
    HttpParser p;
    return p.parse(raw);
}

}  // namespace

// ─── Request-line ──────────────────────────────────────────────────────────

TEST_SUITE("HttpParser / request-line") {

TEST_CASE("GET / HTTP/1.1 parses correctly") {
    auto r = parse("GET / HTTP/1.1\r\nHost: h\r\n\r\n");
    REQUIRE(r.status == HttpParseStatus::kComplete);
    CHECK(r.request.method() == HttpMethod::kGet);
    CHECK(r.request.path() == "/");
    CHECK(r.request.httpVersion() == "HTTP/1.1");
    CHECK(r.request.queryString() == "");
    CHECK(r.contentLength == 0);
    CHECK(!r.chunked);
}

TEST_CASE("path and query string split") {
    auto r = parse("GET /items?page=2&limit=10 HTTP/1.1\r\nHost: h\r\n\r\n");
    REQUIRE(r.status == HttpParseStatus::kComplete);
    CHECK(r.request.path() == "/items");
    CHECK(r.request.queryString() == "page=2&limit=10");
}

TEST_CASE("POST with Content-Length body") {
    const std::string raw =
        "POST /submit HTTP/1.1\r\n"
        "Host: h\r\n"
        "Content-Length: 5\r\n"
        "\r\n"
        "hello";
    auto r = parse(raw);
    REQUIRE(r.status == HttpParseStatus::kComplete);
    CHECK(r.request.method() == HttpMethod::kPost);
    CHECK(r.contentLength == 5);
    CHECK(r.request.header("Content-Length") == "5");
}

TEST_CASE("all supported methods") {
    for (auto [s, m] : std::initializer_list<std::pair<std::string_view, HttpMethod>>{
            {"GET", HttpMethod::kGet}, {"POST", HttpMethod::kPost}, {"PUT", HttpMethod::kPut},
            {"DELETE", HttpMethod::kDelete}, {"PATCH", HttpMethod::kPatch},
            {"HEAD", HttpMethod::kHead}, {"OPTIONS", HttpMethod::kOptions}}) {
        std::string raw = std::string(s) + " / HTTP/1.1\r\nHost: h\r\n\r\n";
        auto r = parse(raw);
        REQUIRE_MESSAGE(r.status == HttpParseStatus::kComplete, "method: ", s);
        CHECK_MESSAGE(r.request.method() == m, "method: ", s);
    }
}

TEST_CASE("unsupported method → 501") {
    auto r = parse("FOOBAR / HTTP/1.1\r\nHost: h\r\n\r\n");
    CHECK(r.status == HttpParseStatus::kError);
    CHECK(r.error == HttpParseError::kUnsupportedMethod);
    CHECK(httpParseErrorStatus(r.error) == 501);
}

TEST_CASE("HTTP/1.0 accepted without Host") {
    auto r = parse("GET / HTTP/1.0\r\n\r\n");
    REQUIRE(r.status == HttpParseStatus::kComplete);
    CHECK(r.request.httpVersion() == "HTTP/1.0");
}

TEST_CASE("HTTP/2.0 rejected → 505") {
    auto r = parse("GET / HTTP/2.0\r\nHost: h\r\n\r\n");
    CHECK(r.status == HttpParseStatus::kError);
    CHECK(r.error == HttpParseError::kUnsupportedHttpVersion);
    CHECK(httpParseErrorStatus(r.error) == 505);
}

TEST_CASE("invalid version string") {
    auto r = parse("GET / HTTP/x.y\r\nHost: h\r\n\r\n");
    CHECK(r.status == HttpParseStatus::kError);
}

TEST_CASE("OPTIONS * target") {
    auto r = parse("OPTIONS * HTTP/1.1\r\nHost: h\r\n\r\n");
    REQUIRE(r.status == HttpParseStatus::kComplete);
    CHECK(r.request.path() == "*");
}

TEST_CASE("* target on non-OPTIONS rejected") {
    auto r = parse("GET * HTTP/1.1\r\nHost: h\r\n\r\n");
    CHECK(r.status == HttpParseStatus::kError);
    CHECK(r.error == HttpParseError::kInvalidRequestTarget);
}

TEST_CASE("absolute-form target") {
    auto r = parse("GET http://example.com/path?q=1 HTTP/1.1\r\nHost: example.com\r\n\r\n");
    REQUIRE(r.status == HttpParseStatus::kComplete);
    CHECK(r.request.path() == "/path");
    CHECK(r.request.queryString() == "q=1");
}

TEST_CASE("absolute-form with mismatched Host rejected") {
    auto r = parse("GET http://example.com/ HTTP/1.1\r\nHost: other.com\r\n\r\n");
    CHECK(r.status == HttpParseStatus::kError);
    CHECK(r.error == HttpParseError::kInvalidHost);
}

}  // TEST_SUITE

// ─── Host header ──────────────────────────────────────────────────────────

TEST_SUITE("HttpParser / Host header") {

TEST_CASE("missing Host in HTTP/1.1 → 400") {
    auto r = parse("GET / HTTP/1.1\r\n\r\n");
    CHECK(r.status == HttpParseStatus::kError);
    CHECK(r.error == HttpParseError::kMissingHost);
    CHECK(httpParseErrorStatus(r.error) == 400);
}

TEST_CASE("duplicate Host rejected") {
    auto r = parse("GET / HTTP/1.1\r\nHost: a.com\r\nHost: b.com\r\n\r\n");
    CHECK(r.status == HttpParseStatus::kError);
    CHECK(r.error == HttpParseError::kInvalidHost);
}

TEST_CASE("IPv6 Host accepted") {
    auto r = parse("GET / HTTP/1.1\r\nHost: [::1]\r\n\r\n");
    REQUIRE(r.status == HttpParseStatus::kComplete);
}

TEST_CASE("IPv6 Host with port accepted") {
    auto r = parse("GET / HTTP/1.1\r\nHost: [::1]:8080\r\n\r\n");
    REQUIRE(r.status == HttpParseStatus::kComplete);
}

TEST_CASE("IPv6 Host with invalid char rejected (our RFC 3986 fix)") {
    // '[::1!]' contains '!' which is not a valid IPv6 character
    auto r = parse("GET / HTTP/1.1\r\nHost: [::1!]\r\n\r\n");
    CHECK(r.status == HttpParseStatus::kError);
    CHECK(r.error == HttpParseError::kInvalidHost);
}

TEST_CASE("IPv6 Host with sub-delimiter rejected") {
    // Old code accepted '[fe80::1$]' ($ is a reg-name sub-delim); new code rejects it
    auto r = parse("GET / HTTP/1.1\r\nHost: [fe80::1$]\r\n\r\n");
    CHECK(r.status == HttpParseStatus::kError);
}

TEST_CASE("Host with port accepted") {
    auto r = parse("GET / HTTP/1.1\r\nHost: example.com:8080\r\n\r\n");
    REQUIRE(r.status == HttpParseStatus::kComplete);
    CHECK(r.request.header("Host") == "example.com:8080");
}

TEST_CASE("empty Host rejected") {
    auto r = parse("GET / HTTP/1.1\r\nHost: \r\n\r\n");
    CHECK(r.status == HttpParseStatus::kError);
}

}  // TEST_SUITE

// ─── Content-Length ────────────────────────────────────────────────────────

TEST_SUITE("HttpParser / Content-Length") {

TEST_CASE("valid Content-Length") {
    auto r = parse("POST / HTTP/1.1\r\nHost: h\r\nContent-Length: 0\r\n\r\n");
    REQUIRE(r.status == HttpParseStatus::kComplete);
    CHECK(r.contentLength == 0);
}

TEST_CASE("duplicate identical Content-Length accepted") {
    auto r = parse("POST / HTTP/1.1\r\nHost: h\r\nContent-Length: 5\r\nContent-Length: 5\r\n\r\nhello");
    REQUIRE(r.status == HttpParseStatus::kComplete);
    CHECK(r.contentLength == 5);
}

TEST_CASE("duplicate differing Content-Length rejected") {
    auto r = parse("POST / HTTP/1.1\r\nHost: h\r\nContent-Length: 5\r\nContent-Length: 6\r\n\r\n");
    CHECK(r.status == HttpParseStatus::kError);
    CHECK(r.error == HttpParseError::kConflictingContentLength);
}

TEST_CASE("non-numeric Content-Length rejected") {
    auto r = parse("POST / HTTP/1.1\r\nHost: h\r\nContent-Length: abc\r\n\r\n");
    CHECK(r.status == HttpParseStatus::kError);
    CHECK(r.error == HttpParseError::kInvalidContentLength);
}

TEST_CASE("negative Content-Length rejected") {
    auto r = parse("POST / HTTP/1.1\r\nHost: h\r\nContent-Length: -1\r\n\r\n");
    CHECK(r.status == HttpParseStatus::kError);
}

TEST_CASE("Transfer-Encoding + Content-Length conflict rejected") {
    auto r = parse("POST / HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\nContent-Length: 5\r\n\r\n");
    CHECK(r.status == HttpParseStatus::kError);
    CHECK(r.error == HttpParseError::kInvalidTransferEncoding);
}

}  // TEST_SUITE

// ─── Transfer-Encoding ─────────────────────────────────────────────────────

TEST_SUITE("HttpParser / Transfer-Encoding") {

TEST_CASE("chunked Transfer-Encoding") {
    auto r = parse("POST / HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n");
    REQUIRE(r.status == HttpParseStatus::kComplete);
    CHECK(r.chunked);
    CHECK(r.decodedBodyBytes == 5);
}

TEST_CASE("chunked with extension") {
    auto r = parse("POST / HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\n5;ext=val\r\nhello\r\n0\r\n\r\n");
    REQUIRE(r.status == HttpParseStatus::kComplete);
    CHECK(r.chunked);
}

TEST_CASE("chunked with trailers") {
    auto r = parse("POST / HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\nX-Checksum: abc\r\n\r\n");
    REQUIRE(r.status == HttpParseStatus::kComplete);
    CHECK(r.chunked);
}

TEST_CASE("unsupported transfer encoding") {
    auto r = parse("POST / HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: deflate\r\n\r\n");
    // deflate without chunked at the end is unsupported
    CHECK(r.status == HttpParseStatus::kError);
}

TEST_CASE("Transfer-Encoding on HTTP/1.0 rejected") {
    auto r = parse("POST / HTTP/1.0\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n");
    CHECK(r.status == HttpParseStatus::kError);
    CHECK(r.error == HttpParseError::kInvalidTransferEncoding);
}

TEST_CASE("Expect: 100-continue parsed") {
    auto r = parse("POST / HTTP/1.1\r\nHost: h\r\nContent-Length: 5\r\nExpect: 100-continue\r\n\r\nhello");
    REQUIRE(r.status == HttpParseStatus::kComplete);
    CHECK(r.flags.expectContinue);
}

TEST_CASE("unknown Expect value → 417") {
    auto r = parse("POST / HTTP/1.1\r\nHost: h\r\nExpect: 200-ok\r\n\r\n");
    CHECK(r.status == HttpParseStatus::kError);
    CHECK(r.error == HttpParseError::kExpectationFailed);
    CHECK(httpParseErrorStatus(r.error) == 417);
}

}  // TEST_SUITE

// ─── Header limits ─────────────────────────────────────────────────────────

TEST_SUITE("HttpParser / header limits") {

TEST_CASE("too many headers → 431") {
    std::string raw = "GET / HTTP/1.1\r\nHost: h\r\n";
    for (int i = 0; i < 64; ++i) {
        raw += "X-H" + std::to_string(i) + ": v\r\n";
    }
    raw += "\r\n";
    auto r = parse(raw);
    CHECK(r.status == HttpParseStatus::kError);
    CHECK(r.error == HttpParseError::kTooManyHeaders);
    CHECK(httpParseErrorStatus(r.error) == 431);
}

TEST_CASE("incomplete header returns Incomplete") {
    auto r = parse("GET / HTTP/1.1\r\nHost: h\r\n");
    CHECK(r.status == HttpParseStatus::kIncomplete);
}

TEST_CASE("empty input returns Incomplete") {
    auto r = parse("");
    CHECK(r.status == HttpParseStatus::kIncomplete);
}

}  // TEST_SUITE

// ─── Chunked body scan ─────────────────────────────────────────────────────

TEST_SUITE("HttpParser / scanHttpChunkedBody") {

TEST_CASE("simple chunked body") {
    auto r = scanHttpChunkedBody("5\r\nhello\r\n0\r\n\r\n");
    CHECK(r.status == HttpChunkScanStatus::kComplete);
    CHECK(r.decodedBytes == 5);
    CHECK(r.consumedBytes == 16);
}

TEST_CASE("multiple chunks") {
    auto r = scanHttpChunkedBody("3\r\nabc\r\n4\r\ndefg\r\n0\r\n\r\n");
    CHECK(r.status == HttpChunkScanStatus::kComplete);
    CHECK(r.decodedBytes == 7);
}

TEST_CASE("empty body (zero chunk only)") {
    auto r = scanHttpChunkedBody("0\r\n\r\n");
    CHECK(r.status == HttpChunkScanStatus::kComplete);
    CHECK(r.decodedBytes == 0);
    CHECK(r.consumedBytes == 5);
}

TEST_CASE("incomplete chunk data returns Incomplete") {
    auto r = scanHttpChunkedBody("5\r\nhel");
    CHECK(r.status == HttpChunkScanStatus::kIncomplete);
}

TEST_CASE("incomplete chunk header returns Incomplete") {
    auto r = scanHttpChunkedBody("5");
    CHECK(r.status == HttpChunkScanStatus::kIncomplete);
}

TEST_CASE("invalid chunk size") {
    auto r = scanHttpChunkedBody("xyz\r\n\r\n");
    CHECK(r.status == HttpChunkScanStatus::kInvalidSize);
}

TEST_CASE("chunk CRLF missing") {
    auto r = scanHttpChunkedBody("3\r\nabcXX");
    // Not enough data for CRLF after chunk
    CHECK(r.status == HttpChunkScanStatus::kIncomplete);
}

TEST_CASE("invalid CRLF after chunk data") {
    auto r = scanHttpChunkedBody("3\r\nabc\n\n");
    CHECK(r.status == HttpChunkScanStatus::kInvalidCrlf);
}

TEST_CASE("chunk with valid trailer") {
    auto r = scanHttpChunkedBody("3\r\nabc\r\n0\r\nX-Custom: value\r\n\r\n");
    CHECK(r.status == HttpChunkScanStatus::kComplete);
    CHECK(r.decodedBytes == 3);
}

TEST_CASE("chunk with forbidden trailer (Content-Length) rejected") {
    auto r = scanHttpChunkedBody("3\r\nabc\r\n0\r\nContent-Length: 3\r\n\r\n");
    CHECK(r.status == HttpChunkScanStatus::kInvalidTrailer);
}

TEST_CASE("chunk size overflow") {
    // A very large hex value that overflows size_t
    auto r = scanHttpChunkedBody("ffffffffffffffffffffffff\r\n");
    CHECK(r.status == HttpChunkScanStatus::kSizeOverflow);
}

}  // TEST_SUITE

// ─── keep-alive flags ──────────────────────────────────────────────────────

TEST_SUITE("HttpParser / connection flags") {

TEST_CASE("HTTP/1.1 defaults to keep-alive") {
    auto r = parse("GET / HTTP/1.1\r\nHost: h\r\n\r\n");
    REQUIRE(r.status == HttpParseStatus::kComplete);
    CHECK(!r.flags.connectionClose);
    CHECK(!r.flags.connectionKeepAlive);
}

TEST_CASE("Connection: close sets flag") {
    auto r = parse("GET / HTTP/1.1\r\nHost: h\r\nConnection: close\r\n\r\n");
    REQUIRE(r.status == HttpParseStatus::kComplete);
    CHECK(r.flags.connectionClose);
}

TEST_CASE("Connection: keep-alive sets flag") {
    auto r = parse("GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\n");
    REQUIRE(r.status == HttpParseStatus::kComplete);
    CHECK(r.flags.connectionKeepAlive);
}

TEST_CASE("Connection: upgrade sets flag") {
    auto r = parse("GET / HTTP/1.1\r\nHost: h\r\nConnection: Upgrade\r\nUpgrade: websocket\r\n\r\n");
    REQUIRE(r.status == HttpParseStatus::kComplete);
    CHECK(r.flags.upgrade);
}

}  // TEST_SUITE
