#include <array>
#include <cstddef>
#include <span>

#include "ruvia/http/http_content_coding.h"
#include "ruvia/http/http_header.h"

#include "test_harness.h"

RUVIA_TEST(response_content_encoding_folds_header_lines_in_application_order) {
    const std::array unrelated{ruvia::http_header("Content-Type", "text/plain")};
    const auto missing = ruvia::parse_http_content_coding_headers(unrelated);
    RUVIA_CHECK(missing.codings().empty());
    RUVIA_CHECK(missing.invalid() == nullptr);
    RUVIA_CHECK(missing.unsupported() == nullptr);

    const std::array lines{ruvia::http_header("Content-Encoding", "gzip"),
        ruvia::http_header("Content-Type", "text/plain"), ruvia::http_header("content-encoding", "deflate"),
        ruvia::http_header("Content-Encoding", "br")};
    const auto stacked = ruvia::parse_http_content_coding_headers(lines);
    constexpr std::array expected{ruvia::http_content_coding::gzip,
        ruvia::http_content_coding::deflate, ruvia::http_content_coding::brotli};
    RUVIA_CHECK(stacked.invalid() == nullptr);
    RUVIA_CHECK(stacked.unsupported() == nullptr);
    RUVIA_CHECK_EQ(stacked.codings().size(), expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        RUVIA_CHECK(stacked.codings()[i] == expected[i]);
    }
}
