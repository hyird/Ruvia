#include <array>
#include <cstddef>

#include "ruvia/http/HttpContentCoding.h"
#include "ruvia/http/HttpResponse.h"

#include "test_harness.h"

RUVIA_TEST(response_content_encoding_folds_header_lines_in_application_order) {
    ruvia::HttpResponse response;
    const auto missing = ruvia::parseHttpContentCodingHeaders(response.headers());
    RUVIA_CHECK(missing.codings().empty());
    RUVIA_CHECK(missing.invalid() == nullptr);
    RUVIA_CHECK(missing.unsupported() == nullptr);

    response.header("Content-Encoding", "gzip");
    response.header("Content-Encoding", "deflate",
        {.mode = ruvia::HttpResponseHeaderMode::kAppend});
    response.header("Content-Encoding", "br",
        {.mode = ruvia::HttpResponseHeaderMode::kAppend});
    const auto stacked = ruvia::parseHttpContentCodingHeaders(response.headers());
    constexpr std::array expected{ruvia::HttpContentCoding::kGzip,
        ruvia::HttpContentCoding::deflate, ruvia::HttpContentCoding::kBrotli};
    RUVIA_CHECK(stacked.invalid() == nullptr);
    RUVIA_CHECK(stacked.unsupported() == nullptr);
    RUVIA_CHECK_EQ(stacked.codings().size(), expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        RUVIA_CHECK(stacked.codings()[i] == expected[i]);
    }
}
