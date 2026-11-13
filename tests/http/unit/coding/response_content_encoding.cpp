#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/http/http_content_coding.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_response.h"

TEST(Ruvia, response_content_encoding_folds_header_lines_in_application_order) {
    ruvia::http_response response;
    const auto missing = ruvia::parse_http_content_coding_headers(response.headers());
    EXPECT_TRUE(missing.codings().empty());
    EXPECT_TRUE(missing.invalid() == nullptr);
    EXPECT_TRUE(missing.unsupported() == nullptr);

    response.header("Content-Encoding", "gzip");
    response.header("Content-Encoding", "deflate",
        {.mode_ = ruvia::http_response_header_mode::append});
    response.header("Content-Encoding", "br",
        {.mode_ = ruvia::http_response_header_mode::append});
    const auto stacked = ruvia::parse_http_content_coding_headers(response.headers());
    constexpr std::array expected{ruvia::http_content_coding::gzip,
        ruvia::http_content_coding::deflate, ruvia::http_content_coding::brotli};
    EXPECT_TRUE(stacked.invalid() == nullptr);
    EXPECT_TRUE(stacked.unsupported() == nullptr);
    EXPECT_EQ(stacked.codings().size(), expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        EXPECT_TRUE(stacked.codings()[i] == expected[i]);
    }
}

TEST(Ruvia, content_encoding_field_lines_fold_in_order_ignoring_other_fields) {
    const std::array unrelated{ruvia::http_header("Content-Type", "text/plain")};
    const auto missing = ruvia::parse_http_content_coding_headers(unrelated);
    EXPECT_TRUE(missing.codings().empty());
    EXPECT_TRUE(missing.invalid() == nullptr);
    EXPECT_TRUE(missing.unsupported() == nullptr);

    const std::array lines{ruvia::http_header("Content-Encoding", "gzip"),
        ruvia::http_header("Content-Type", "text/plain"), ruvia::http_header("content-encoding", "deflate"),
        ruvia::http_header("Content-Encoding", "br")};
    const auto stacked = ruvia::parse_http_content_coding_headers(lines);
    constexpr std::array expected{ruvia::http_content_coding::gzip,
        ruvia::http_content_coding::deflate, ruvia::http_content_coding::brotli};
    EXPECT_TRUE(stacked.invalid() == nullptr);
    EXPECT_TRUE(stacked.unsupported() == nullptr);
    EXPECT_EQ(stacked.codings().size(), expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        EXPECT_TRUE(stacked.codings()[i] == expected[i]);
    }
}
