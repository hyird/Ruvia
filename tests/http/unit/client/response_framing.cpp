#include "http_client_response_fixture.h"

// HTTP/1 client responses: what the head says about the body.

RUVIA_TEST(http_client_response_plan_alternatives_are_exclusive) {
    const ruvia::HttpHeaderView upgradeHeaders[] = {
        {"Connection", "Upgrade"},
        {"Upgrade", "websocket"},
    };
    const auto informational = parseHead("GET", "HTTP/1.1 103 Early Hints");
    const auto withoutContent = parseHead("GET", "HTTP/1.1 204 No Content");
    const auto knownLength = parseHead("GET", "HTTP/1.1 200 OK\r\nContent-Length: 1");
    const auto chunked = parseHead("GET", "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked");
    const auto closeDelimited = parseHead("GET", "HTTP/1.1 200 OK");
    const auto tunnel = parseHead("CONNECT", "HTTP/1.1 200 Connection Established");
    const auto upgrade = parseHead("GET",
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Connection: Upgrade\r\nUpgrade: websocket",
        Http1ClosePolicy::kAllowReuse, upgradeHeaders);

    for (const auto* plan : {&informational.plan(), &withoutContent.plan(), &knownLength.plan(), &chunked.plan(), &closeDelimited.plan(), &tunnel.plan(), &upgrade.plan()}) {
        RUVIA_CHECK_EQ(activePlanAlternativeCount(*plan), std::size_t{1});
    }
}

RUVIA_TEST(http_client_response_plan_owns_content_length_framing) {
    constexpr std::string_view header = "HTTP/1.1 200 OK\r\nContent-Length: 5";
    const auto head = parseHead("GET", header);
    const auto& knownLength = requireKnownLength(head.plan());
    RUVIA_CHECK_EQ(knownLength.contentLength(), std::size_t{5});
    RUVIA_CHECK(knownLength.requiresBodyConsumption());
    RUVIA_CHECK(knownLength.persistence() == Http1ClosePolicy::kAllowReuse);
    RUVIA_CHECK_EQ(head.consumedBytes(), header.size() + 4);

    const auto empty = parseHead("GET", "HTTP/1.1 200 OK\r\nContent-Length: 0");
    RUVIA_CHECK(!requireKnownLength(empty.plan()).requiresBodyConsumption());
}

RUVIA_TEST(http_client_content_length_combined_and_repeated_equal_values) {
    const auto combined = parseHead("GET", "HTTP/1.1 200 OK\r\nContent-Length: 5, 5");
    RUVIA_CHECK_EQ(requireKnownLength(combined.plan()).contentLength(), std::size_t{5});

    const auto repeated = parseHead("GET", "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Length: 5");
    RUVIA_CHECK_EQ(requireKnownLength(repeated.plan()).contentLength(), std::size_t{5});

    RUVIA_CHECK(parseFails("GET", "HTTP/1.1 200 OK\r\nContent-Length: 5, 6"));
    RUVIA_CHECK(parseFails("GET", "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Length: 6"));
}

RUVIA_TEST(http_client_response_plan_owns_chunked_framing_and_reuse) {
    const auto head = parseHead("GET", "HTTP/1.1 200 OK\r\nTransfer-Encoding: Chunked");
    const auto& chunked = requireChunked(head.plan());
    RUVIA_CHECK(chunked.transferCodings().empty());
    RUVIA_CHECK(chunked.persistence() == Http1ClosePolicy::kAllowReuse);
}

RUVIA_TEST(http_client_transfer_coding_before_final_chunked_is_typed) {
    const auto combined = parseHead("GET", "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, chunked");
    const auto& combinedChunked = requireChunked(combined.plan());
    RUVIA_CHECK_EQ(combinedChunked.transferCodings().count, std::size_t{1});
    RUVIA_CHECK(combinedChunked.transferCodings().values[0] == ruvia::HttpTransferCoding::kGzip);
    RUVIA_CHECK(combinedChunked.persistence() == Http1ClosePolicy::kAllowReuse);

    // Transfer-Encoding is list-based: split field lines retain wire order.
    const auto split = parseHead("GET",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: deflate\r\n"
        "Transfer-Encoding: chunked");
    const auto& splitChunked = requireChunked(split.plan());
    RUVIA_CHECK(splitChunked.transferCodings().values[0] == ruvia::HttpTransferCoding::kDeflate);
}

RUVIA_TEST(http_client_non_chunked_transfer_coding_is_close_delimited) {
    const auto head = parseHead("GET", "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip");
    const auto& closeDelimited = requireCloseDelimited(head.plan());
    RUVIA_CHECK_EQ(closeDelimited.transferCodings().count, std::size_t{1});
}

RUVIA_TEST(http_client_rejects_invalid_or_unsupported_transfer_coding) {
    RUVIA_CHECK(parseFails("GET", "HTTP/1.1 200 OK\r\nTransfer-Encoding: , chunked"));
    RUVIA_CHECK(parseFails("GET", "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked, gzip"));
    RUVIA_CHECK(parseFails("GET", "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked;foo=bar"));
    RUVIA_CHECK(parseFails("GET", "HTTP/1.1 200 OK\r\nTransfer-Encoding: compress, chunked"));
    RUVIA_CHECK(parseFails("GET", "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, deflate, chunked"));
}

RUVIA_TEST(http_client_content_length_and_transfer_encoding_rejected_for_body) {
    RUVIA_CHECK(parseFails("GET",
        "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n"
        "Transfer-Encoding: chunked"));
    RUVIA_CHECK(parseFails("GET",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n"
        "Content-Length: 5"));
}

RUVIA_TEST(http_client_no_body_precedence_ignores_framing_fields) {
    const auto head = parseHead("HEAD",
        "HTTP/1.1 200 OK\r\nContent-Length: invalid\r\n"
        "Transfer-Encoding: custom-coding");
    const auto& withoutContent = requireWithoutContent(head.plan());
    RUVIA_CHECK(withoutContent.persistence() == Http1ClosePolicy::kAllowReuse);

    const auto notModified = parseHead("GET",
        "HTTP/1.1 304 Not Modified\r\nContent-Length: invalid\r\n"
        "Transfer-Encoding: custom-coding");
    RUVIA_CHECK(notModified.plan().withoutContent() != nullptr);

    const auto noContent = parseHead("GET", "HTTP/1.1 204 No Content");
    RUVIA_CHECK(noContent.plan().withoutContent() != nullptr);
}

RUVIA_TEST(http_client_204_rejects_framing_fields) {
    RUVIA_CHECK(parseFails("GET", "HTTP/1.1 204 No Content\r\nContent-Length: 0"));
    RUVIA_CHECK(parseFails("GET", "HTTP/1.1 204 No Content\r\nContent-Length: invalid"));
    RUVIA_CHECK(parseFails("GET", "HTTP/1.1 204 No Content\r\nTransfer-Encoding: chunked"));
    RUVIA_CHECK(parseFails("GET", "HTTP/1.1 204 No Content\r\nTransfer-Encoding: custom-coding"));
}

RUVIA_TEST(http_client_205_owns_zero_content_framing) {
    const auto zeroLength = parseHead("GET", "HTTP/1.1 205 Reset Content\r\nContent-Length: 0");
    const auto* zeroLengthBody = requireZeroContent(zeroLength.plan()).knownLength();
    RUVIA_CHECK(zeroLengthBody != nullptr);
    if (zeroLengthBody == nullptr) {
        return;
    }
    RUVIA_CHECK(!zeroLengthBody->requiresBodyConsumption());
    RUVIA_CHECK(zeroLengthBody->persistence() == Http1ClosePolicy::kAllowReuse);
    RUVIA_CHECK_EQ(activePlanAlternativeCount(zeroLength.plan()), std::size_t{1});

    RUVIA_CHECK(parseFails("GET", "HTTP/1.1 205 Reset Content\r\nContent-Length: 3"));
    RUVIA_CHECK(parseFails("HEAD", "HTTP/1.1 205 Reset Content\r\nContent-Length: 3"));

    const auto chunked = parseHead("GET",
        "HTTP/1.1 205 Reset Content\r\n"
        "Transfer-Encoding: gzip, chunked");
    const auto& chunkedZero = requireZeroContent(chunked.plan());
    RUVIA_CHECK(chunkedZero.chunked() != nullptr);
    RUVIA_CHECK(chunkedZero.closeDelimited() == nullptr);
    if (chunkedZero.chunked() != nullptr) {
        RUVIA_CHECK_EQ(chunkedZero.chunked()->transferCodings().count, std::size_t{1});
        RUVIA_CHECK(chunkedZero.chunked()->transferCodings().values[0] == ruvia::HttpTransferCoding::kGzip);
    }
    RUVIA_CHECK_EQ(activePlanAlternativeCount(chunked.plan()), std::size_t{1});

    const auto transferCoded = parseHead("GET", "HTTP/1.1 205 Reset Content\r\nTransfer-Encoding: gzip");
    const auto& transferCodedZero = requireZeroContent(transferCoded.plan());
    RUVIA_CHECK(transferCodedZero.closeDelimited() != nullptr);
    if (transferCodedZero.closeDelimited() != nullptr) {
        RUVIA_CHECK_EQ(transferCodedZero.closeDelimited()->transferCodings().count, std::size_t{1});
        RUVIA_CHECK(transferCodedZero.closeDelimited()->transferCodings().values[0] == ruvia::HttpTransferCoding::kGzip);
    }

    const auto unframed = parseHead("GET", "HTTP/1.1 205 Reset Content");
    const auto& closeZero = requireZeroContent(unframed.plan());
    RUVIA_CHECK(closeZero.closeDelimited() != nullptr);
    RUVIA_CHECK(closeZero.chunked() == nullptr);
    RUVIA_CHECK_EQ(activePlanAlternativeCount(unframed.plan()), std::size_t{1});

    const auto connect = parseHead("CONNECT", "HTTP/1.1 205 Reset Content\r\nContent-Length: invalid");
    RUVIA_CHECK(connect.plan().connectTunnel() != nullptr);
    RUVIA_CHECK(connect.plan().zeroContent() == nullptr);
}

RUVIA_TEST(http_client_final_after_continue_does_not_cancel_released_content) {
    ruvia::HttpClientRequestView request;
    request.method = "POST";
    request.content = ruvia::HttpClientRequestContentView::bytes("payload");
    std::array<char, 512> requestHead;
    const auto prepared = ruvia::Http1ClientRequestWriter().prepare(ruvia::HttpOriginView::https("example.test"), request, requestHead, Http1ClientRequestWirePolicy(Http1ClosePolicy::kAllowReuse, ruvia::HttpClientRequestExpectation::kContinue));
    RUVIA_CHECK(prepared.prepared() != nullptr);
    if (prepared.prepared() == nullptr) {
        return;
    }

    Http1ClientResponseParser parser(prepared.prepared()->exchangeState());
    const auto continueResponse = parser.parse("HTTP/1.1 100 Continue\r\n\r\n");
    RUVIA_CHECK(continueResponse.parsed() != nullptr);
    if (continueResponse.parsed() != nullptr) {
        RUVIA_CHECK(continueResponse.parsed()->plan().requestContentSignal() == HttpClientRequestContentSignal::kContinue);
    }

    const auto finalResponse = parser.parse("HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n");
    RUVIA_CHECK(finalResponse.parsed() != nullptr);
    if (finalResponse.parsed() != nullptr) {
        RUVIA_CHECK(!finalResponse.parsed()->plan().requestContentSignal());
        RUVIA_CHECK(requireKnownLength(finalResponse.parsed()->plan()).persistence() == Http1ClosePolicy::kAllowReuse);
    }
}

RUVIA_TEST(http_client_closing_final_stops_unfinished_request_content) {
    ruvia::HttpClientRequestView request;
    request.method = "POST";
    request.content = ruvia::HttpClientRequestContentView::bytes("payload");
    std::array<char, 512> requestHead;
    const auto prepared = ruvia::Http1ClientRequestWriter().prepare(ruvia::HttpOriginView::https("example.test"), request, requestHead, Http1ClientRequestWirePolicy(Http1ClosePolicy::kAllowReuse, ruvia::HttpClientRequestExpectation::kContinue));
    RUVIA_CHECK(prepared.prepared() != nullptr);
    if (prepared.prepared() == nullptr) {
        return;
    }

    Http1ClientResponseParser parser(prepared.prepared()->exchangeState());
    const auto continueResponse = parser.parse("HTTP/1.1 100 Continue\r\n\r\n");
    RUVIA_CHECK(continueResponse.parsed() != nullptr);

    const auto closingFinal = parser.parse(
        "HTTP/1.1 413 Content Too Large\r\n"
        "Connection: close\r\nContent-Length: 0\r\n\r\n");
    RUVIA_CHECK(closingFinal.parsed() != nullptr);
    if (closingFinal.parsed() != nullptr) {
        RUVIA_CHECK(closingFinal.parsed()->plan().requestContentSignal() == HttpClientRequestContentSignal::kExchangeComplete);
        RUVIA_CHECK(requireKnownLength(closingFinal.parsed()->plan()).persistence() == Http1ClosePolicy::kCloseAfterResponse);
    }
}

RUVIA_TEST(http_client_response_preserves_typed_protocol_version) {
    const auto http10 = parseHead("GET", "HTTP/1.0 204 No Content");
    const auto http11 = parseHead("GET", "HTTP/1.1 204 No Content");

    RUVIA_CHECK(http10.head().protocolVersion() == HttpProtocolVersion::kHttp10);
    RUVIA_CHECK(http11.head().protocolVersion() == HttpProtocolVersion::kHttp11);
}

RUVIA_TEST(http_client_unframed_body_response_is_close_delimited) {
    const auto head = parseHead("GET", "HTTP/1.1 200 OK");
    RUVIA_CHECK(head.plan().closeDelimited() != nullptr);
}

RUVIA_TEST(http_client_http10_transfer_encoding_is_faulty_framing) {
    RUVIA_CHECK(parseFails("GET", "HTTP/1.0 200 OK\r\nTransfer-Encoding: chunked"));
    RUVIA_CHECK(parseFails("HEAD", "HTTP/1.0 200 OK\r\nTransfer-Encoding: gzip"));
}

RUVIA_TEST(http_client_head_method_is_case_sensitive) {
    const auto head = parseHead("HEAD", "HTTP/1.1 200 OK");
    const auto lowercase = parseHead("head", "HTTP/1.1 200 OK");
    RUVIA_CHECK(head.plan().withoutContent() != nullptr);
    RUVIA_CHECK(lowercase.plan().closeDelimited() != nullptr);
}
