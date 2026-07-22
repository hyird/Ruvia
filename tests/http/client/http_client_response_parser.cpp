#include "http_client_response_fixture.h"

// HTTP/1 client responses: reading a response head off the wire.

RUVIA_TEST(http_client_response_head_commits_status_and_version_at_construction) {
    auto head = ruvia::detail::HttpClientResponseHeadAccess::make(
        ruvia::http_status::kMultiStatus,
        HttpProtocolVersion::kHttp10,
        std::pmr::get_default_resource());
    RUVIA_CHECK_EQ(head.status(), ruvia::http_status::kMultiStatus);
    RUVIA_CHECK(head.protocolVersion() == HttpProtocolVersion::kHttp10);
}

RUVIA_TEST(http_client_origin_target_validation) {
    RUVIA_CHECK(isValidHttpClientOriginTarget("/ok%2F?q=%7B%7D"));
    RUVIA_CHECK(!isValidHttpClientOriginTarget("*"));
    RUVIA_CHECK(!isValidHttpClientOriginTarget(""));
    RUVIA_CHECK(!isValidHttpClientOriginTarget("relative"));
    RUVIA_CHECK(!isValidHttpClientOriginTarget("/bad#fragment"));
    RUVIA_CHECK(!isValidHttpClientOriginTarget("/bad\\path"));
    RUVIA_CHECK(!isValidHttpClientOriginTarget("/bad%zz"));
    RUVIA_CHECK(!isValidHttpClientOriginTarget("/bad%"));
    RUVIA_CHECK(!isValidHttpClientOriginTarget("/bad%2"));
}

RUVIA_TEST(http_client_rejects_malformed_status_and_length_fields) {
    const auto upperBoundary = parseHead(
        "GET", "HTTP/1.1 599 Extension Status\r\nContent-Length: 0");
    RUVIA_CHECK_EQ(upperBoundary.head().status(), ruvia::HttpStatusCode::fromValue(599));
    RUVIA_CHECK(
        parseFailureError("GET", "HTTP/2 200 OK") ==
        Http1ClientResponseParseError::kUnsupportedHttpVersion);
    RUVIA_CHECK(
        parseFailureError("GET", "HTTP/1.1 99 Too Small") ==
        Http1ClientResponseParseError::kInvalidStatusCode);
    RUVIA_CHECK(
        parseFailureError("GET", "HTTP/1.1 abc Bad") ==
        Http1ClientResponseParseError::kInvalidStatusCode);
    for (const std::string_view invalid : {
             "HTTP/1.1 600 Invalid",
             "HTTP/1.1 999 Invalid"}) {
        RUVIA_CHECK(
            parseFailureError("GET", invalid) ==
            Http1ClientResponseParseError::kInvalidStatusCode);
    }
    RUVIA_CHECK(
        parseFailureError("GET", "HTTP/1.1 200") ==
        Http1ClientResponseParseError::kInvalidStatusCode);
    std::string invalidReason("HTTP/1.1 200 ");
    invalidReason.push_back('\x01');
    RUVIA_CHECK(
        parseFailureError("GET", invalidReason) ==
        Http1ClientResponseParseError::kInvalidReasonPhrase);
    RUVIA_CHECK(parseFails(
        "GET", "HTTP/1.1 200 OK\r\nContent-Length: notanumber"));
    RUVIA_CHECK(parseFails(
        "GET", "HTTP/1.1 200 OK\r\nContent-Length: 5,"));
    RUVIA_CHECK(!ruvia::http1ClientResponseParseErrorMessage(
        Http1ClientResponseParseError::kInvalidStatusCode).empty());
}

RUVIA_TEST(http_client_rejects_invalid_or_repeated_content_type) {
    const auto invalid = parseResult(
        "GET",
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: not a media type\r\n"
        "Content-Length: 0");
    RUVIA_CHECK(invalid.failure() != nullptr);
    if (invalid.failure() != nullptr) {
        RUVIA_CHECK(
            invalid.failure()->error() ==
            Http1ClientResponseParseError::kInvalidHeader);
    }

    const auto repeated = parseResult(
        "GET",
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: 0");
    RUVIA_CHECK(repeated.failure() != nullptr);
    if (repeated.failure() != nullptr) {
        RUVIA_CHECK(
            repeated.failure()->error() ==
            Http1ClientResponseParseError::kInvalidHeader);
    }

    const auto valid = parseResponse(
        "GET",
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json; charset=utf-8\r\n"
        "Content-Length: 0");
    RUVIA_CHECK_EQ(valid.head.headers().size(), std::size_t{2});
    RUVIA_CHECK_EQ(
        valid.head.headers().front().value(),
        std::string_view("application/json; charset=utf-8"));
}

RUVIA_TEST(http_client_response_parser_need_more_is_distinct) {
    const auto result = parseWire(
        "GET", "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n");
    RUVIA_CHECK(result.needMore() != nullptr);
    RUVIA_CHECK(result.parsed() == nullptr);
    RUVIA_CHECK(result.failure() == nullptr);
}

RUVIA_TEST(http_client_response_parser_owns_exact_head_boundary) {
    std::string wire =
        "HTTP/1.1 200 OK\r\nX-Owner: response\r\nContent-Length: 4\r\n\r\n"
        "bodyHTTP/1.1 500 ignored\r\n\r\n";
    const auto expectedConsumed = wire.find("\r\n\r\n") + 4;
    auto result = parseWire("GET", wire);
    auto* parsed = result.parsed();
    RUVIA_CHECK(parsed != nullptr);
    if (parsed == nullptr) {
        return;
    }
    RUVIA_CHECK_EQ(parsed->consumedBytes(), expectedConsumed);
    RUVIA_CHECK_EQ(parsed->head().status(), ruvia::http_status::kOk);
    RUVIA_CHECK(
        parsed->head().protocolVersion() ==
        HttpProtocolVersion::kHttp11);
    RUVIA_CHECK_EQ(parsed->head().headers().size(), std::size_t{2});

    wire.assign(wire.size(), 'x');
    const auto headers = parsed->head().headers();
    RUVIA_CHECK(headers[0].name() == "X-Owner");
    RUVIA_CHECK(headers[0].value() == "response");
    RUVIA_CHECK(headers[1].value() == "4");
}

RUVIA_TEST(http_client_response_parser_failure_is_typed_and_allocation_free) {
    CountingMemoryResource counting;
    ruvia::HttpClientRequest request;
    request.method = "GET";
    std::array<char, 512> requestHead;
    const auto preparedResult = ruvia::Http1ClientRequestWriter().prepare(
        ruvia::HttpOrigin::https("example.test"), request, requestHead);
    const auto* prepared = preparedResult.prepared();
    RUVIA_CHECK(prepared != nullptr);
    if (prepared == nullptr) {
        return;
    }

    auto failureParser = Http1ClientResponseParser(*prepared, &counting);
    const auto failure = failureParser.parse("HTTP/2 200 OK\r\n\r\n");
    RUVIA_CHECK(failure.failure() != nullptr);
    RUVIA_CHECK(
        failure.failure()->error() ==
        Http1ClientResponseParseError::kUnsupportedHttpVersion);
    RUVIA_CHECK_EQ(counting.allocationCount(), std::size_t{0});

    auto successParser = Http1ClientResponseParser(*prepared, &counting);
    const auto success = successParser.parse(
        "HTTP/1.1 200 OK\r\n"
        "X-Requires-Ownership: a-long-enough-value-to-require-storage\r\n"
        "Content-Length: 0\r\n\r\n");
    RUVIA_CHECK(success.parsed() != nullptr);
    RUVIA_CHECK(counting.allocationCount() > 0);
}

RUVIA_TEST(http_client_response_parser_enforces_the_complete_head_limit) {
    std::string oversized(ruvia::kMaxHttpHeaderBytes, 'x');
    const auto result = parseWire("GET", oversized);
    RUVIA_CHECK(result.failure() != nullptr);
    RUVIA_CHECK(
        result.failure()->error() ==
        Http1ClientResponseParseError::kHeaderTooLarge);
}
