#include <array>
#include <cstddef>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/http/http_content_coding.h"
#include "ruvia/http/http_header.h"
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

RUVIA_TEST(content_encoding_field_lines_fold_in_order_ignoring_other_fields) {
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

RUVIA_TEST(response_content_encoding_rewrites_validators_and_range_support) {
    const struct {
        std::string_view etag_;
        std::optional<std::string_view> expected_;
    } cases[] = {
        {"\"v1\"", std::string_view("W/\"v1\"")},
        {"W/\"v2\"", std::string_view("W/\"v2\"")},
        {"abc", std::nullopt},
        {"\"unterminated", std::nullopt},
        {"W/abc", std::nullopt},
    };
    for (const bool replace_body : {false, true}) {
        for (const auto& test : cases) {
            ruvia::http_response response;
            response.body("identity representation");
            response.header("ETag", test.etag_);
            response.header("Accept-Ranges", "bytes");
            if (replace_body) {
                std::pmr::string coded("coded", response.memory_resource());
                response.replace_body_with_content_encoding(std::move(coded), "gzip");
            } else {
                response.apply_content_encoding("gzip");
            }
            RUVIA_CHECK(response.header("Content-Encoding") == std::optional<std::string_view>("gzip"));
            RUVIA_CHECK(response.header("ETag") == test.expected_);
            RUVIA_CHECK(!response.header("Accept-Ranges").has_value());
        }
    }
}
