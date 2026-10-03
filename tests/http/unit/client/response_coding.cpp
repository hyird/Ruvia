#include <array>
#include <cstddef>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ruvia/http/HttpContentCoding.h"

#include "http_client_response_fixture.h"

// HTTP/1 client responses: Content-Encoding and decoding the body.

RUVIA_TEST(http_client_content_encoding_has_one_authoritative_path) {
    using ruvia::HttpContentCoding;

    struct Case final {
        std::string_view headers;
        std::vector<HttpContentCoding> expected;
        bool unsupported{false};
    };
    const Case cases[] = {
        {"HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\nContent-Length: 0",
            {HttpContentCoding::kGzip}},
        {"HTTP/1.1 200 OK\r\nContent-Encoding: x-gzip\r\nContent-Length: 0",
            {HttpContentCoding::kGzip}},
        {"HTTP/1.1 200 OK\r\nContent-Encoding: GZIP\r\nContent-Length: 0",
            {HttpContentCoding::kGzip}},
        {"HTTP/1.1 200 OK\r\nContent-Encoding: br\r\nContent-Length: 0",
            {HttpContentCoding::kBrotli}},
        {"HTTP/1.1 200 OK\r\nContent-Encoding: zstd\r\nContent-Length: 0",
            {HttpContentCoding::kZstd}},
        {"HTTP/1.1 200 OK\r\nContent-Encoding: deflate\r\nContent-Length: 0",
            {HttpContentCoding::deflate}},
        {"HTTP/1.1 200 OK\r\nContent-Encoding: identity\r\nContent-Length: 0",
            {HttpContentCoding::kIdentity}},
        {"HTTP/1.1 200 OK\r\nContent-Encoding: gzip, br\r\nContent-Length: 0",
            {HttpContentCoding::kGzip, HttpContentCoding::kBrotli}},
        {"HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\n"
         "Content-Encoding: br\r\nContent-Length: 0",
            {HttpContentCoding::kGzip, HttpContentCoding::kBrotli}},
        {"HTTP/1.1 200 OK\r\nContent-Encoding: compress\r\nContent-Length: 0", {}, true},
        {"HTTP/1.1 200 OK\r\nContent-Length: 0", {}},
    };

    for (const auto& test : cases) {
        auto parsed = parseResponse("GET", test.headers);
        RUVIA_CHECK_EQ(parsed.head.status(), ruvia::http_status::kOk);
        std::pmr::monotonic_buffer_resource resource;
        const auto coding = ruvia::parseHttpContentCodingHeaders(parsed.head.headers(), &resource);
        RUVIA_CHECK(coding.invalid() == nullptr);
        RUVIA_CHECK((coding.unsupported() != nullptr) == test.unsupported);
        RUVIA_CHECK_EQ(coding.codings().size(), test.expected.size());
        for (std::size_t i = 0; i < test.expected.size(); ++i) {
            RUVIA_CHECK(coding.codings()[i] == test.expected[i]);
        }
    }
}

RUVIA_TEST(http_client_rejects_invalid_content_encoding_syntax) {
    for (const std::string_view value : {"gzip;level=9", "bad coding", "gzip/deflate"}) {
        std::string response = "HTTP/1.1 200 OK\r\nContent-Encoding: ";
        response.append(value);
        response.append("\r\nContent-Length: 0");
        RUVIA_CHECK(
            parseFailureError("GET", response) == Http1ClientResponseParseError::kInvalidHeader);
    }

    const auto tolerant = parseResponse("GET",
        "HTTP/1.1 200 OK\r\n"
        "Content-Encoding: , gzip,,\r\n"
        "Content-Length: 0");
    std::pmr::monotonic_buffer_resource resource;
    const auto coding = ruvia::parseHttpContentCodingHeaders(tolerant.head.headers(), &resource);
    RUVIA_CHECK(coding.unsupported() == nullptr);
    RUVIA_CHECK_EQ(coding.codings().size(), 1U);
    if (!coding.codings().empty()) {
        RUVIA_CHECK(coding.codings().front() == ruvia::HttpContentCoding::kGzip);
    }
}

RUVIA_TEST(http_client_content_decode_rejects_invalid_deflate_content) {
    auto parsed = parseResponse("GET",
        "HTTP/1.1 200 OK\r\nContent-Encoding: deflate\r\n"
        "Content-Length: 7");
    const std::string_view encodedContent = "encoded";

    const auto decoded = ruvia::detail::decodeHttpClientResponseContentEncoding(
        parsed.head, encodedContent, 1024, std::pmr::get_default_resource());
    RUVIA_CHECK(decoded.decoded() == nullptr);
    RUVIA_CHECK(decoded.failure() != nullptr);
    if (decoded.failure() != nullptr) {
        RUVIA_CHECK(
            decoded.failure()->error() == ruvia::HttpContentDecodeError::kInvalidContent);
    }
}

RUVIA_TEST(http_client_identity_content_decode_accepts_a_null_resource) {
    auto parsed = parseResponse("GET", "HTTP/1.1 200 OK\r\nContent-Length: 1024");
    const std::string content(1024, 'i');

    auto decoded = ruvia::detail::decodeHttpClientResponseContentEncoding(
        parsed.head, content, content.size(), nullptr);
    RUVIA_CHECK(decoded.decoded() != nullptr);
    if (decoded.decoded() != nullptr) {
        auto bytes = std::move(*decoded.decoded()).takeBytes();
        RUVIA_CHECK_EQ(std::string_view(bytes), std::string_view(content));
        RUVIA_CHECK(bytes.get_allocator().resource() == std::pmr::get_default_resource());
    }
}

RUVIA_TEST(http_client_content_decode_consumes_concatenated_gzip_members) {
    auto firstEncoding = ruvia::encodeHttpContent(ruvia::HttpContentCoding::kGzip, "first-",
        {.maxEncodedBytes = 1024, .resource = std::pmr::get_default_resource()});
    auto secondEncoding = ruvia::encodeHttpContent(ruvia::HttpContentCoding::kGzip, "second",
        {.maxEncodedBytes = 1024, .resource = std::pmr::get_default_resource()});
    RUVIA_CHECK(firstEncoding.encoded() != nullptr);
    RUVIA_CHECK(secondEncoding.encoded() != nullptr);
    if (firstEncoding.encoded() == nullptr || secondEncoding.encoded() == nullptr) {
        return;
    }
    auto first = std::move(*firstEncoding.encoded()).takeBytes();
    auto second = std::move(*secondEncoding.encoded()).takeBytes();

    auto parsed = parseResponse("GET",
        "HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\n"
        "Content-Length: 1");
    std::string encodedContent(first);
    encodedContent.append(second);
    auto decoded = ruvia::detail::decodeHttpClientResponseContentEncoding(
        parsed.head, encodedContent, 1024, std::pmr::get_default_resource());
    RUVIA_CHECK(decoded.decoded() != nullptr);
    if (const auto* content = decoded.decoded()) {
        RUVIA_CHECK_EQ(content->bytes(), std::string_view("first-second"));
    }
    // Decoding is a separate representation; the sans-I/O driver's encoded
    // content remains independent from the immutable parsed response head.
    RUVIA_CHECK(!encodedContent.empty());
    std::pmr::monotonic_buffer_resource resource;
    const auto coding = ruvia::parseHttpContentCodingHeaders(parsed.head.headers(), &resource);
    RUVIA_CHECK_EQ(coding.codings().size(), 1U);
    if (!coding.codings().empty()) {
        RUVIA_CHECK(coding.codings().front() == ruvia::HttpContentCoding::kGzip);
    }
}

RUVIA_TEST(http_client_decodes_content_coding_stacks_in_reverse_order) {
    constexpr std::array codings{ruvia::HttpContentCoding::kGzip,
        ruvia::HttpContentCoding::deflate};
    const std::string plain(8192, 'r');
    auto encoded = ruvia::encodeHttpContent(codings, plain, {.maxEncodedBytes = plain.size()});
    RUVIA_CHECK(encoded.encoded() != nullptr);
    if (encoded.encoded() == nullptr) {
        return;
    }
    auto parsed = parseResponse("GET",
        "HTTP/1.1 200 OK\r\nContent-Encoding: gzip, deflate\r\n"
        "Content-Length: 1");
    std::pmr::monotonic_buffer_resource resource;
    auto decoded = ruvia::detail::decodeHttpClientResponseContentEncoding(
        parsed.head, encoded.encoded()->bytes(), plain.size(), &resource);
    RUVIA_CHECK(decoded.decoded() != nullptr);
    if (const auto* content = decoded.decoded()) {
        RUVIA_CHECK_EQ(content->bytes(), plain);
        RUVIA_CHECK(content->bytes().data() != encoded.encoded()->bytes().data());
    }
}

RUVIA_TEST(http_client_content_decode_failure_preserves_encoded_body) {
    auto parsed = parseResponse("GET",
        "HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\n"
        "Content-Length: 1");
    const std::string_view encodedContent = "not-gzip";
    const auto decoded = ruvia::detail::decodeHttpClientResponseContentEncoding(
        parsed.head, encodedContent, 1024, std::pmr::get_default_resource());
    RUVIA_CHECK(decoded.decoded() == nullptr);
    RUVIA_CHECK(decoded.failure() != nullptr);
    RUVIA_CHECK(decoded.failure()->error() == ruvia::HttpContentDecodeError::kInvalidContent);
}
