#include "http_client_response_fixture.h"

// HTTP/1 client responses: Content-Encoding and decoding the body.

RUVIA_TEST(http_client_content_encoding_has_one_authoritative_path) {
    using ruvia::detail::httpClientResponseContentCoding;
    using ruvia::HttpContentCoding;

    struct Case final {
        std::string_view headers;
        std::optional<HttpContentCoding> expected;
    };
    const Case cases[] = {
        {"HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\nContent-Length: 0", HttpContentCoding::kGzip},
        {"HTTP/1.1 200 OK\r\nContent-Encoding: x-gzip\r\nContent-Length: 0", HttpContentCoding::kGzip},
        {"HTTP/1.1 200 OK\r\nContent-Encoding: GZIP\r\nContent-Length: 0", HttpContentCoding::kGzip},
        {"HTTP/1.1 200 OK\r\nContent-Encoding: br\r\nContent-Length: 0", HttpContentCoding::kBrotli},
        {"HTTP/1.1 200 OK\r\nContent-Encoding: zstd\r\nContent-Length: 0", HttpContentCoding::kZstd},
        {"HTTP/1.1 200 OK\r\nContent-Encoding: identity\r\nContent-Length: 0", HttpContentCoding::kIdentity},
        {"HTTP/1.1 200 OK\r\nContent-Encoding: deflate\r\nContent-Length: 0", std::nullopt},
        {"HTTP/1.1 200 OK\r\nContent-Encoding: gzip, br\r\nContent-Length: 0", std::nullopt},
        {"HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\n"
         "Content-Encoding: br\r\nContent-Length: 0",
            std::nullopt},
        {"HTTP/1.1 200 OK\r\nContent-Length: 0", HttpContentCoding::kIdentity},
    };

    for (const auto& test : cases) {
        auto parsed = parseResponse("GET", test.headers);
        RUVIA_CHECK_EQ(parsed.head.status(), ruvia::http_status::kOk);
        const auto coding = httpClientResponseContentCoding(parsed.head);
        RUVIA_CHECK(coding.invalid() == nullptr);
        RUVIA_CHECK((coding.coding() != nullptr) == test.expected.has_value());
        RUVIA_CHECK((coding.unsupported() != nullptr) == !test.expected.has_value());
        if (coding.coding() != nullptr && test.expected.has_value()) {
            RUVIA_CHECK(*coding.coding() == *test.expected);
        }
    }
}

RUVIA_TEST(http_client_rejects_invalid_content_encoding_syntax) {
    for (const std::string_view value : {"gzip;level=9", "bad coding", "gzip/deflate"}) {
        std::string response = "HTTP/1.1 200 OK\r\nContent-Encoding: ";
        response.append(value);
        response.append("\r\nContent-Length: 0");
        RUVIA_CHECK(parseFailureError("GET", response) == Http1ClientResponseParseError::kInvalidHeader);
    }

    const auto tolerant = parseResponse("GET",
        "HTTP/1.1 200 OK\r\n"
        "Content-Encoding: , gzip,,\r\n"
        "Content-Length: 0");
    const auto coding = ruvia::detail::httpClientResponseContentCoding(tolerant.head);
    RUVIA_CHECK(coding.unsupported() == nullptr);
    RUVIA_CHECK(coding.coding() != nullptr);
    if (coding.coding() != nullptr) {
        RUVIA_CHECK(*coding.coding() == ruvia::HttpContentCoding::kGzip);
    }
}

RUVIA_TEST(http_client_content_decode_reports_unsupported_wire_coding) {
    auto parsed = parseResponse("GET",
        "HTTP/1.1 200 OK\r\nContent-Encoding: deflate\r\n"
        "Content-Length: 7");
    const std::string_view encodedContent = "encoded";

    const auto decoded = ruvia::detail::decodeHttpClientResponseContentEncoding(parsed.head, encodedContent, 1024, std::pmr::get_default_resource());
    RUVIA_CHECK(decoded.decoded() == nullptr);
    RUVIA_CHECK(decoded.failure() != nullptr);
    if (decoded.failure() != nullptr) {
        RUVIA_CHECK(decoded.failure()->error() == ruvia::HttpContentDecodeError::kUnsupportedCoding);
    }
}

RUVIA_TEST(http_client_identity_content_decode_accepts_a_null_resource) {
    auto parsed = parseResponse("GET", "HTTP/1.1 200 OK\r\nContent-Length: 1024");
    const std::string content(1024, 'i');

    auto decoded = ruvia::detail::decodeHttpClientResponseContentEncoding(parsed.head, content, content.size(), nullptr);
    RUVIA_CHECK(decoded.decoded() != nullptr);
    if (decoded.decoded() != nullptr) {
        auto bytes = std::move(*decoded.decoded()).takeBytes();
        RUVIA_CHECK_EQ(std::string_view(bytes), std::string_view(content));
        RUVIA_CHECK(bytes.get_allocator().resource() == std::pmr::get_default_resource());
    }
}

RUVIA_TEST(http_client_content_decode_consumes_concatenated_gzip_members) {
    auto firstEncoding = ruvia::encodeHttpContent(ruvia::HttpContentCoding::kGzip, "first-", 1024, std::pmr::get_default_resource());
    auto secondEncoding = ruvia::encodeHttpContent(ruvia::HttpContentCoding::kGzip, "second", 1024, std::pmr::get_default_resource());
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
    auto decoded = ruvia::detail::decodeHttpClientResponseContentEncoding(parsed.head, encodedContent, 1024, std::pmr::get_default_resource());
    RUVIA_CHECK(decoded.decoded() != nullptr);
    if (const auto* content = decoded.decoded()) {
        RUVIA_CHECK_EQ(content->bytes(), std::string_view("first-second"));
    }
    // Decoding is a separate representation; the sans-I/O driver's encoded
    // content remains independent from the immutable parsed response head.
    RUVIA_CHECK(!encodedContent.empty());
    const auto coding = ruvia::detail::httpClientResponseContentCoding(parsed.head);
    RUVIA_CHECK(coding.coding() != nullptr);
    if (coding.coding() != nullptr) {
        RUVIA_CHECK(*coding.coding() == ruvia::HttpContentCoding::kGzip);
    }
}

RUVIA_TEST(http_client_content_decode_failure_preserves_encoded_body) {
    auto parsed = parseResponse("GET",
        "HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\n"
        "Content-Length: 1");
    const std::string_view encodedContent = "not-gzip";
    const auto decoded = ruvia::detail::decodeHttpClientResponseContentEncoding(parsed.head, encodedContent, 1024, std::pmr::get_default_resource());
    RUVIA_CHECK(decoded.decoded() == nullptr);
    RUVIA_CHECK(decoded.failure() != nullptr);
    RUVIA_CHECK(decoded.failure()->error() == ruvia::HttpContentDecodeError::kInvalidContent);
}
