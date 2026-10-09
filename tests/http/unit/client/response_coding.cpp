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

RUVIA_TEST(http_client_content_decode_rejects_invalid_deflate_content) {
    auto parsed_value = parse_response("GET",
        "HTTP/1.1 200 OK\r\nContent-Encoding: deflate\r\n"
        "Content-Length: 7");
    const std::string_view encoded_content = "encoded";

    const auto decoded = ruvia::detail::decode_http_client_response_content_encoding(
        parsed_value.head_, encoded_content, 1024, std::pmr::get_default_resource());
    RUVIA_CHECK(decoded.decoded() == nullptr);
    RUVIA_CHECK(decoded.failure() != nullptr);
    if (decoded.failure() != nullptr) {
        RUVIA_CHECK(
            decoded.failure()->error() == ruvia::http_content_decode_error::invalid_content);
    }
}

RUVIA_TEST(http_client_identity_content_decode_accepts_a_null_resource) {
    auto parsed_value = parse_response("GET", "HTTP/1.1 200 OK\r\nContent-Length: 1024");
    const std::string content(1024, 'i');

    auto decoded = ruvia::detail::decode_http_client_response_content_encoding(
        parsed_value.head_, content, content.size(), nullptr);
    RUVIA_CHECK(decoded.decoded() != nullptr);
    if (decoded.decoded() != nullptr) {
        auto bytes_value = std::move(*decoded.decoded()).take_bytes();
        RUVIA_CHECK_EQ(std::string_view(bytes_value), std::string_view(content));
        RUVIA_CHECK(bytes_value.get_allocator().resource() == std::pmr::get_default_resource());
    }
}

RUVIA_TEST(http_client_content_decode_consumes_concatenated_gzip_members) {
    auto first_encoding = ruvia::encode_http_content(ruvia::http_content_coding::gzip, "first-",
        {.max_encoded_bytes_ = 1024, .resource_ = std::pmr::get_default_resource()});
    auto second_encoding = ruvia::encode_http_content(ruvia::http_content_coding::gzip, "second",
        {.max_encoded_bytes_ = 1024, .resource_ = std::pmr::get_default_resource()});
    RUVIA_CHECK(first_encoding.encoded() != nullptr);
    RUVIA_CHECK(second_encoding.encoded() != nullptr);
    if (first_encoding.encoded() == nullptr || second_encoding.encoded() == nullptr) {
        return;
    }
    auto first = std::move(*first_encoding.encoded()).take_bytes();
    auto second = std::move(*second_encoding.encoded()).take_bytes();

    auto parsed_value = parse_response("GET",
        "HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\n"
        "Content-Length: 1");
    std::string encoded_content(first);
    encoded_content.append(second);
    auto decoded = ruvia::detail::decode_http_client_response_content_encoding(
        parsed_value.head_, encoded_content, 1024, std::pmr::get_default_resource());
    RUVIA_CHECK(decoded.decoded() != nullptr);
    if (const auto* content = decoded.decoded()) {
        RUVIA_CHECK_EQ(content->bytes(), std::string_view("first-second"));
    }
    // Decoding is a separate representation; the sans-I/O driver's encoded
    // content remains independent from the immutable parsed response head.
    RUVIA_CHECK(!encoded_content.empty());
    std::pmr::monotonic_buffer_resource resource;
    const auto coding = ruvia::parse_http_content_coding_headers(parsed_value.head_.headers(), &resource);
    RUVIA_CHECK_EQ(coding.codings().size(), 1U);
    if (!coding.codings().empty()) {
        RUVIA_CHECK(coding.codings().front() == ruvia::http_content_coding::gzip);
    }
}

RUVIA_TEST(http_client_decodes_content_coding_stacks_in_reverse_order) {
    constexpr std::array codings{ruvia::http_content_coding::gzip,
        ruvia::http_content_coding::deflate};
    const std::string plain(8192, 'r');
    auto encoded = ruvia::encode_http_content(codings, plain, {.max_encoded_bytes_ = plain.size()});
    RUVIA_CHECK(encoded.encoded() != nullptr);
    if (encoded.encoded() == nullptr) {
        return;
    }
    auto parsed_value = parse_response("GET",
        "HTTP/1.1 200 OK\r\nContent-Encoding: gzip, deflate\r\n"
        "Content-Length: 1");
    std::pmr::monotonic_buffer_resource resource;
    auto decoded = ruvia::detail::decode_http_client_response_content_encoding(
        parsed_value.head_, encoded.encoded()->bytes(), plain.size(), &resource);
    RUVIA_CHECK(decoded.decoded() != nullptr);
    if (const auto* content = decoded.decoded()) {
        RUVIA_CHECK_EQ(content->bytes(), plain);
        RUVIA_CHECK(content->bytes().data() != encoded.encoded()->bytes().data());
    }
}

RUVIA_TEST(http_client_content_decode_failure_preserves_encoded_body) {
    auto parsed_value = parse_response("GET",
        "HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\n"
        "Content-Length: 1");
    const std::string_view encoded_content = "not-gzip";
    const auto decoded = ruvia::detail::decode_http_client_response_content_encoding(
        parsed_value.head_, encoded_content, 1024, std::pmr::get_default_resource());
    RUVIA_CHECK(decoded.decoded() == nullptr);
    RUVIA_CHECK(decoded.failure() != nullptr);
    RUVIA_CHECK(decoded.failure()->error() == ruvia::http_content_decode_error::invalid_content);
}
