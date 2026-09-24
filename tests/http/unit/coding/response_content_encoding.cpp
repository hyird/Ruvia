#include "ruvia/http/HttpContentCoding.h"
#include "ruvia/http/HttpResponse.h"

#include "test_harness.h"

RUVIA_TEST(response_content_encoding_folds_header_lines) {
    ruvia::HttpResponse response;
    auto missing = ruvia::parseHttpContentCodingHeaders(response.headers());
    RUVIA_CHECK(missing.coding() != nullptr);
    if (missing.coding() != nullptr) {
        RUVIA_CHECK(*missing.coding() == ruvia::HttpContentCoding::kIdentity);
    }

    response.header("Content-Encoding", "gzip");
    const auto single = ruvia::parseHttpContentCodingHeaders(response.headers());
    RUVIA_CHECK(single.coding() != nullptr);
    if (single.coding() != nullptr) {
        RUVIA_CHECK(*single.coding() == ruvia::HttpContentCoding::kGzip);
    }

    response.header("Content-Encoding", "br", {.mode = ruvia::HttpResponseHeaderMode::kAppend});
    const auto stacked = ruvia::parseHttpContentCodingHeaders(response.headers());
    RUVIA_CHECK(stacked.unsupported() != nullptr);
}
