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

RUVIA_TEST(http_client_content_encoding_has_one_authoritative_path) {
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
        {"HTTP/1.1 200 OK\r\nContent-Encoding: identity\r\nContent-Length: 0",
            {http_content_coding::identity}},
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
        RUVIA_CHECK_EQ(parsed_value.head_.status(), ruvia::http_status::ok);
        std::pmr::monotonic_buffer_resource resource;
        const auto coding = ruvia::parse_http_content_coding_headers(parsed_value.head_.headers(), &resource);
        RUVIA_CHECK(coding.invalid() == nullptr);
        RUVIA_CHECK((coding.unsupported() != nullptr) == test.unsupported_);
        RUVIA_CHECK_EQ(coding.codings().size(), test.expected_.size());
        for (std::size_t i = 0; i < test.expected_.size(); ++i) {
            RUVIA_CHECK(coding.codings()[i] == test.expected_[i]);
        }
    }
}

RUVIA_TEST(http_client_rejects_invalid_content_encoding_syntax) {
    for (const std::string_view value : {"gzip;level=9", "bad coding", "gzip/deflate"}) {
        std::string response = "HTTP/1.1 200 OK\r\nContent-Encoding: ";
        response.append(value);
        response.append("\r\nContent-Length: 0");
        RUVIA_CHECK(
            parse_failure_error("GET", response) == http1_client_response_parse_error::invalid_header);
    }

    const auto tolerant = parse_response("GET",
        "HTTP/1.1 200 OK\r\n"
        "Content-Encoding: , gzip,,\r\n"
        "Content-Length: 0");
    std::pmr::monotonic_buffer_resource resource;
    const auto coding = ruvia::parse_http_content_coding_headers(tolerant.head_.headers(), &resource);
    RUVIA_CHECK(coding.unsupported() == nullptr);
    RUVIA_CHECK_EQ(coding.codings().size(), 1U);
    if (!coding.codings().empty()) {
        RUVIA_CHECK(coding.codings().front() == ruvia::http_content_coding::gzip);
    }
}
