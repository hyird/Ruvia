#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ruvia/http/http_content_coding.h"

#include "http_client_response_fixture.h"

// HTTP/1 client responses: Content-Encoding and decoding the body.

TEST(Ruvia, http_client_content_encoding_has_one_authoritative_path) {
    using ruvia::http_content_coding;

    struct case_value final {
        std::string_view headers_;
        std::vector<http_content_coding> expected_;
        bool unsupported_{false};
    };
    const case_value cases[] = {
        {"HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\nContent-Length: 0",
            {http_content_coding::gzip}},
        {"HTTP/1.1 200 OK\r\nContent-Encoding: x-gzip\r\nContent-Length: 0",
            {http_content_coding::gzip}},
        {"HTTP/1.1 200 OK\r\nContent-Encoding: GZIP\r\nContent-Length: 0",
            {http_content_coding::gzip}},
        {"HTTP/1.1 200 OK\r\nContent-Encoding: br\r\nContent-Length: 0",
            {http_content_coding::brotli}},
        {"HTTP/1.1 200 OK\r\nContent-Encoding: zstd\r\nContent-Length: 0",
            {http_content_coding::zstd}},
        {"HTTP/1.1 200 OK\r\nContent-Encoding: deflate\r\nContent-Length: 0",
            {http_content_coding::deflate}},
        {"HTTP/1.1 200 OK\r\nContent-Encoding: identity\r\nContent-Length: 0", {}},
        {"HTTP/1.1 200 OK\r\nContent-Encoding: identity, identity, gzip, identity\r\nContent-Length: 0",
            {http_content_coding::gzip}},
        {"HTTP/1.1 200 OK\r\nContent-Encoding: gzip, br\r\nContent-Length: 0",
            {http_content_coding::gzip, http_content_coding::brotli}},
        {"HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\n"
         "Content-Encoding: br\r\nContent-Length: 0",
            {http_content_coding::gzip, http_content_coding::brotli}},
        {"HTTP/1.1 200 OK\r\nContent-Encoding: compress\r\nContent-Length: 0", {}, true},
        {"HTTP/1.1 200 OK\r\nContent-Length: 0", {}},
    };

    for (const auto& test : cases) {
        auto parsed_value = parse_response("GET", test.headers_);
        EXPECT_EQ(parsed_value.head_.status(), ruvia::http_status::ok);
        const auto coding = ruvia::parse_http_content_coding_headers(parsed_value.head_.headers());
        EXPECT_TRUE(coding.invalid() == nullptr);
        EXPECT_TRUE((coding.unsupported() != nullptr) == test.unsupported_);
        EXPECT_EQ(coding.codings().size(), test.expected_.size());
        for (std::size_t i = 0; i < test.expected_.size(); ++i) {
            EXPECT_TRUE(coding.codings()[i] == test.expected_[i]);
        }
    }
}

TEST(Ruvia, http_client_content_encoding_ignores_empty_list_members) {
    const auto tolerant = parse_response("GET",
        "HTTP/1.1 200 OK\r\n"
        "Content-Encoding: , gzip,,\r\n"
        "Content-Length: 0");
    const auto coding = ruvia::parse_http_content_coding_headers(tolerant.head_.headers());
    EXPECT_TRUE(coding.unsupported() == nullptr);
    EXPECT_EQ(coding.codings().size(), 1U);
    if (!coding.codings().empty()) {
        EXPECT_TRUE(coding.codings().front() == ruvia::http_content_coding::gzip);
    }
}

TEST(Ruvia, http_client_coding_metadata_preserves_framing_without_decoder_fallback) {
    for (const std::string_view value : {"gzip;level=9", "bad coding", "gzip/deflate", "future-coding"}) {
        std::string wire = "HTTP/1.1 200 OK\r\nContent-Encoding: ";
        wire.append(value);
        wire.append("\r\nContent-Encoding: identity\r\nContent-Length: 3\r\n\r\nabcNEXT");
        const auto result = parse_wire("GET", wire);
        const auto* parsed = result.parsed();
        EXPECT_TRUE(parsed != nullptr);
        if (parsed == nullptr) {
            continue;
        }
        EXPECT_EQ(parsed->head().headers().front().value(), value);
        const auto length = require_known_length(parsed->plan()).content_length();
        EXPECT_EQ(length, std::size_t{3});
        EXPECT_EQ(std::string_view(wire).substr(parsed->consumed_bytes(), length), "abc");
        EXPECT_EQ(std::string_view(wire).substr(parsed->consumed_bytes() + length), "NEXT");
        const auto coding = ruvia::parse_http_content_coding_headers(parsed->head().headers());
        const bool unsupported = value == "future-coding";
        EXPECT_TRUE((coding.unsupported() != nullptr) == unsupported);
        EXPECT_TRUE((coding.invalid() != nullptr) == !unsupported);
    }
}
