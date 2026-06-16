#include <doctest/doctest.h>
#include "ruvia/http/FileResponse.h"
#include "ruvia/memory/MemoryPool.h"

using namespace ruvia::detail;

// ─── httpParseImfFixdate ───────────────────────────────────────────────────

TEST_SUITE("FileResponse / httpParseImfFixdate") {

TEST_CASE("valid RFC 7231 date") {
    // Wed, 21 Oct 2015 07:28:00 GMT
    auto t = httpParseImfFixdate("Wed, 21 Oct 2015 07:28:00 GMT");
    REQUIRE(t.has_value());
    CHECK(*t > 0);
}

TEST_CASE("epoch date") {
    auto t = httpParseImfFixdate("Thu, 01 Jan 1970 00:00:00 GMT");
    REQUIRE(t.has_value());
    CHECK(*t == 0);
}

TEST_CASE("wrong length → nullopt") {
    CHECK_FALSE(httpParseImfFixdate("Wed, 21 Oct 2015 07:28:00 GMT ").has_value());
    CHECK_FALSE(httpParseImfFixdate("Wed, 21 Oct 2015 07:28:00").has_value());
}

TEST_CASE("invalid month → nullopt") {
    CHECK_FALSE(httpParseImfFixdate("Wed, 21 Xxx 2015 07:28:00 GMT").has_value());
}

TEST_CASE("invalid day → nullopt") {
    CHECK_FALSE(httpParseImfFixdate("Wed, 00 Oct 2015 07:28:00 GMT").has_value());
    CHECK_FALSE(httpParseImfFixdate("Wed, 32 Oct 2015 07:28:00 GMT").has_value());
}

TEST_CASE("invalid hour → nullopt") {
    CHECK_FALSE(httpParseImfFixdate("Wed, 21 Oct 2015 24:28:00 GMT").has_value());
}

TEST_CASE("invalid minute → nullopt") {
    CHECK_FALSE(httpParseImfFixdate("Wed, 21 Oct 2015 07:60:00 GMT").has_value());
}

TEST_CASE("second=60 accepted (leap second)") {
    // Leap second is allowed per RFC
    auto t = httpParseImfFixdate("Wed, 21 Oct 2015 07:28:60 GMT");
    REQUIRE(t.has_value());
}

TEST_CASE("non-GMT timezone → nullopt") {
    CHECK_FALSE(httpParseImfFixdate("Wed, 21 Oct 2015 07:28:00 UTC").has_value());
}

TEST_CASE("monotone: later date > earlier date") {
    auto t1 = httpParseImfFixdate("Mon, 01 Jan 2000 00:00:00 GMT");
    auto t2 = httpParseImfFixdate("Tue, 02 Jan 2000 00:00:00 GMT");
    REQUIRE(t1.has_value());
    REQUIRE(t2.has_value());
    CHECK(*t2 > *t1);
    CHECK(*t2 - *t1 == 86400);
}

}  // TEST_SUITE

// ─── httpParseByteRange ────────────────────────────────────────────────────

TEST_SUITE("FileResponse / httpParseByteRange") {

TEST_CASE("first-last range") {
    auto r = httpParseByteRange("bytes=0-499", 1000);
    REQUIRE(r.has_value());
    CHECK(r->offset == 0);
    CHECK(r->length == 500);
}

TEST_CASE("suffix range") {
    auto r = httpParseByteRange("bytes=-500", 1000);
    REQUIRE(r.has_value());
    CHECK(r->offset == 500);
    CHECK(r->length == 500);
}

TEST_CASE("suffix larger than file clamps to whole file") {
    auto r = httpParseByteRange("bytes=-2000", 1000);
    REQUIRE(r.has_value());
    CHECK(r->offset == 0);
    CHECK(r->length == 1000);
}

TEST_CASE("open-ended range") {
    auto r = httpParseByteRange("bytes=500-", 1000);
    REQUIRE(r.has_value());
    CHECK(r->offset == 500);
    CHECK(r->length == 500);
}

TEST_CASE("last byte of file") {
    auto r = httpParseByteRange("bytes=999-999", 1000);
    REQUIRE(r.has_value());
    CHECK(r->offset == 999);
    CHECK(r->length == 1);
}

TEST_CASE("start beyond file size → nullopt") {
    auto r = httpParseByteRange("bytes=1000-1500", 1000);
    CHECK_FALSE(r.has_value());
}

TEST_CASE("end clamped to file size") {
    auto r = httpParseByteRange("bytes=0-9999", 1000);
    REQUIRE(r.has_value());
    CHECK(r->offset == 0);
    CHECK(r->length == 1000);
}

TEST_CASE("multiple ranges → nullopt (no multipart)") {
    auto r = httpParseByteRange("bytes=0-499,500-999", 1000);
    CHECK_FALSE(r.has_value());
}

TEST_CASE("missing bytes= prefix → nullopt") {
    auto r = httpParseByteRange("0-499", 1000);
    CHECK_FALSE(r.has_value());
}

TEST_CASE("empty file (size=0) → nullopt") {
    auto r = httpParseByteRange("bytes=0-", 0);
    CHECK_FALSE(r.has_value());
}

TEST_CASE("inverted range (first > last) → nullopt") {
    auto r = httpParseByteRange("bytes=500-100", 1000);
    CHECK_FALSE(r.has_value());
}

}  // TEST_SUITE

// ─── httpContentRange ──────────────────────────────────────────────────────

TEST_SUITE("FileResponse / httpContentRange") {

TEST_CASE("standard content-range header") {
    auto* r = ruvia::ProcessMemory::instance().upstreamResource();
    auto cr = httpContentRange(r, 0, 500, 1000);
    CHECK(std::string_view(cr) == "bytes 0-499/1000");
}

TEST_CASE("single byte range") {
    auto* r = ruvia::ProcessMemory::instance().upstreamResource();
    auto cr = httpContentRange(r, 999, 1, 1000);
    CHECK(std::string_view(cr) == "bytes 999-999/1000");
}

TEST_CASE("unsatisfied range") {
    auto* r = ruvia::ProcessMemory::instance().upstreamResource();
    auto cr = httpContentRangeUnsatisfied(r, 1000);
    CHECK(std::string_view(cr) == "bytes */1000");
}

}  // TEST_SUITE

// ─── ETag comparison ───────────────────────────────────────────────────────

TEST_SUITE("FileResponse / etag comparison") {

TEST_CASE("strong etag equal") {
    CHECK(httpStrongEtagEquals("\"abc\"", "\"abc\""));
}

TEST_CASE("strong etag different") {
    CHECK_FALSE(httpStrongEtagEquals("\"abc\"", "\"def\""));
}

TEST_CASE("strong vs weak always false") {
    CHECK_FALSE(httpStrongEtagEquals("W/\"abc\"", "W/\"abc\""));
    CHECK_FALSE(httpStrongEtagEquals("\"abc\"", "W/\"abc\""));
}

TEST_CASE("weak etag equal") {
    CHECK(httpWeakEtagEquals("W/\"abc\"", "W/\"abc\""));
}

TEST_CASE("weak etag compares without W/ prefix") {
    CHECK(httpWeakEtagEquals("W/\"abc\"", "\"abc\""));
}

TEST_CASE("weak etag different") {
    CHECK_FALSE(httpWeakEtagEquals("W/\"abc\"", "W/\"def\""));
}

}  // TEST_SUITE

// ─── httpGuessContentType ──────────────────────────────────────────────────

TEST_SUITE("FileResponse / httpGuessContentType") {

TEST_CASE("html extension") {
    CHECK(httpGuessContentType("/index.html") == "text/html; charset=utf-8");
    CHECK(httpGuessContentType("/index.htm") == "text/html; charset=utf-8");
}

TEST_CASE("css extension") {
    CHECK(httpGuessContentType("/style.css") == "text/css; charset=utf-8");
}

TEST_CASE("js extension") {
    CHECK(httpGuessContentType("/app.js") == "text/javascript; charset=utf-8");
}

TEST_CASE("json extension") {
    CHECK(httpGuessContentType("/data.json") == "application/json; charset=utf-8");
}

TEST_CASE("png extension") {
    CHECK(httpGuessContentType("/logo.png") == "image/png");
}

TEST_CASE("jpeg extension") {
    CHECK(httpGuessContentType("/photo.jpg") == "image/jpeg");
    CHECK(httpGuessContentType("/photo.jpeg") == "image/jpeg");
}

TEST_CASE("svg extension") {
    CHECK(httpGuessContentType("/icon.svg") == "image/svg+xml");
}

TEST_CASE("wasm extension") {
    CHECK(httpGuessContentType("/app.wasm") == "application/wasm");
}

TEST_CASE("unknown extension → octet-stream") {
    CHECK(httpGuessContentType("/file.xyz") == "application/octet-stream");
}

TEST_CASE("case-insensitive extension") {
    CHECK(httpGuessContentType("/logo.PNG") == "image/png");
    CHECK(httpGuessContentType("/style.CSS") == "text/css; charset=utf-8");
}

}  // TEST_SUITE
