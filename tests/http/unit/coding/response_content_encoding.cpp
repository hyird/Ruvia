#include <array>
#include <cstddef>

#include "ruvia/http/http_content_coding.h"
#include "ruvia/http/http_response.h"

#include "test_harness.h"

RUVIA_TEST(response_content_encoding_folds_header_lines_in_application_order) {
    ruvia::http_response response;
    const auto missing = ruvia::parse_http_content_coding_headers(response.headers());
    RUVIA_CHECK(missing.codings().empty());
    RUVIA_CHECK(missing.invalid() == nullptr);
    RUVIA_CHECK(missing.unsupported() == nullptr);

    response.header("Content-Encoding", "gzip");
    response.header("Content-Encoding", "deflate",
        {.mode_ = ruvia::http_response_header_mode::append});
    response.header("Content-Encoding", "br",
        {.mode_ = ruvia::http_response_header_mode::append});
    const auto stacked = ruvia::parse_http_content_coding_headers(response.headers());
    constexpr std::array expected{ruvia::http_content_coding::gzip,
        ruvia::http_content_coding::deflate, ruvia::http_content_coding::brotli};
    RUVIA_CHECK(stacked.invalid() == nullptr);
    RUVIA_CHECK(stacked.unsupported() == nullptr);
    RUVIA_CHECK_EQ(stacked.codings().size(), expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        RUVIA_CHECK(stacked.codings()[i] == expected[i]);
    }
}
