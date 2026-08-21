#include "content_decoding_fixture.h"

// Decoding a request body: what each coding accepts and what it refuses.

RUVIA_TEST(request_body_failures_own_cross_runtime_http_errors) {
    const auto tooLarge = ruvia::detail::httpRequestBodySizeFailure(5, ProtocolByteLimit::limited(4));
    RUVIA_CHECK(tooLarge.has_value());
    if (tooLarge) {
        const auto error = tooLarge->protocolError();
        RUVIA_CHECK_EQ(error.status(), ruvia::http_status::kContentTooLarge);
        RUVIA_CHECK_EQ(std::string_view(error.what()), std::string_view("request body is too large"));
    }
    RUVIA_CHECK(!ruvia::detail::httpRequestBodyAdditionFailure(2, 2, ProtocolByteLimit::limited(4)));
    RUVIA_CHECK(ruvia::detail::httpRequestBodyAdditionFailure(2, 3, ProtocolByteLimit::limited(4)).has_value());

    const auto incomplete = ruvia::detail::HttpRequestBodyFailure::incomplete().protocolError();
    RUVIA_CHECK_EQ(incomplete.status(), ruvia::http_status::kBadRequest);
    RUVIA_CHECK_EQ(std::string_view(incomplete.what()), std::string_view("incomplete request body"));
}

RUVIA_TEST(http1_request_body_plan_has_one_framing_truth) {
    Http1ServerRequestParser parser;
    const auto noneState = parser.parseMessage("GET / HTTP/1.1\r\nHost: x\r\n\r\n");
    const auto& none = noneState.bodyPlan;
    RUVIA_CHECK(none.withoutBody() != nullptr);
    RUVIA_CHECK(none.knownLength() == nullptr);
    RUVIA_CHECK(none.chunked() == nullptr);
    RUVIA_CHECK(!none.requiresConsumption());

    const auto emptyLengthState = parser.parseMessage(
        "POST / HTTP/1.1\r\nHost: x\r\n"
        "Expect: 100-continue\r\nContent-Length: 0\r\n\r\n");
    const auto& emptyLength = emptyLengthState.bodyPlan;
    const auto* knownLength = emptyLength.knownLength();
    RUVIA_CHECK(knownLength != nullptr);
    RUVIA_CHECK(emptyLength.withoutBody() == nullptr);
    RUVIA_CHECK(emptyLength.chunked() == nullptr);
    if (knownLength != nullptr) {
        RUVIA_CHECK_EQ(knownLength->contentLength(), std::size_t{0});
    }
    RUVIA_CHECK(!emptyLength.requiresConsumption());
    RUVIA_CHECK(emptyLength.expectations().hasContinue());
    const auto emptyExpectationPlan = emptyLength.expectationPlan(HttpUnsupportedExpectationPolicy::kReject);
    RUVIA_CHECK(emptyExpectationPlan.noAction() != nullptr);

    const auto compressedChunkedState = parser.parseMessage(
        "POST / HTTP/1.1\r\nHost: x\r\n"
        "Expect: 100-continue\r\n"
        "Transfer-Encoding: gzip, chunked\r\n\r\n0\r\n\r\n");
    const auto& compressedChunked = compressedChunkedState.bodyPlan;
    const auto* chunkedBody = compressedChunked.chunked();
    RUVIA_CHECK(chunkedBody != nullptr);
    RUVIA_CHECK(compressedChunked.withoutBody() == nullptr);
    RUVIA_CHECK(compressedChunked.knownLength() == nullptr);
    RUVIA_CHECK(compressedChunked.requiresConsumption());
    const auto compressedExpectationPlan = compressedChunked.expectationPlan(HttpUnsupportedExpectationPolicy::kReject);
    RUVIA_CHECK(compressedExpectationPlan.sendContinue() != nullptr);
    if (chunkedBody != nullptr) {
        RUVIA_CHECK_EQ(chunkedBody->transferCodings().count, std::size_t{1});
    }
}

RUVIA_TEST(request_body_gzip_round_trip) {
    const std::string plain = "The quick brown fox jumps over the lazy dog";
    const std::string gz = gzipCompress(plain);
    RUVIA_CHECK(!gz.empty());
    RUVIA_CHECK_EQ(decoded(HttpContentCoding::kGzip, gz, kDecodedBodyLimit), plain);
}

RUVIA_TEST(request_body_gzip_bomb_rejected) {
    const std::string big(1u << 20, 'a');  // 1 MiB, compresses to a tiny gzip
    const std::string gz = gzipCompress(big);
    // A small cap must stop the expansion, not decode the whole megabyte.
    RUVIA_CHECK(decodeError(HttpContentCoding::kGzip, gz, 1024) == HttpContentDecodeError::kDecodedSizeExceeded);
}

RUVIA_TEST(request_body_gzip_truncated_rejected) {
    const std::string plain(4096, 'q');
    std::string gz = gzipCompress(plain);
    RUVIA_CHECK(gz.size() > 6);
    gz.resize(gz.size() - 6);  // cut into the gzip trailer -> incomplete stream
    RUVIA_CHECK(decodeError(HttpContentCoding::kGzip, gz) == HttpContentDecodeError::kInvalidContent);
}

RUVIA_TEST(request_body_gzip_decodes_every_rfc1952_member) {
    const std::string first = gzipCompress("first-");
    const std::string second = gzipCompress("second");
    RUVIA_CHECK(!first.empty());
    RUVIA_CHECK(!second.empty());
    RUVIA_CHECK_EQ(decoded(HttpContentCoding::kGzip, first + second, kDecodedBodyLimit), std::string("first-second"));
}

RUVIA_TEST(request_body_gzip_rejects_bytes_after_the_last_member) {
    std::string encoded = gzipCompress("complete");
    encoded.append("not-a-gzip-member");
    RUVIA_CHECK(decodeError(HttpContentCoding::kGzip, encoded) == HttpContentDecodeError::kInvalidContent);
}

RUVIA_TEST(request_body_brotli_round_trip) {
    const std::string plain = "permessage brotli body content, repeated repeated repeated";
    const std::string br = brotliCompress(plain);
    RUVIA_CHECK(!br.empty());
    RUVIA_CHECK_EQ(decoded(HttpContentCoding::kBrotli, br, kDecodedBodyLimit), plain);
}

RUVIA_TEST(request_body_brotli_bomb_rejected) {
    const std::string big(1u << 20, 'a');
    const std::string br = brotliCompress(big);
    RUVIA_CHECK(!br.empty());
    RUVIA_CHECK(decodeError(HttpContentCoding::kBrotli, br, 1024) == HttpContentDecodeError::kDecodedSizeExceeded);
}

RUVIA_TEST(request_body_brotli_rejects_trailing_bytes) {
    std::string encoded = brotliCompress("complete");
    encoded.append("trailing");
    RUVIA_CHECK(decodeError(HttpContentCoding::kBrotli, encoded) == HttpContentDecodeError::kInvalidContent);
}

RUVIA_TEST(request_body_zstd_round_trip) {
    const std::string plain = "zstd request body content, repeated repeated repeated repeated";
    const std::string zz = zstdCompress(plain);
    RUVIA_CHECK(!zz.empty());
    RUVIA_CHECK_EQ(decoded(HttpContentCoding::kZstd, zz, kDecodedBodyLimit), plain);
}

RUVIA_TEST(request_body_zstd_bomb_rejected) {
    const std::string big(1u << 20, 'a');  // 1 MiB, compresses to a tiny zstd frame
    const std::string zz = zstdCompress(big);
    RUVIA_CHECK(!zz.empty());
    // A small cap must stop the expansion mid-stream, not decode the whole megabyte.
    RUVIA_CHECK(decodeError(HttpContentCoding::kZstd, zz, 1024) == HttpContentDecodeError::kDecodedSizeExceeded);
}

RUVIA_TEST(request_body_zstd_decodes_every_rfc8878_frame) {
    const std::string first = zstdCompress("first-");
    const std::string second = zstdCompress("second");
    RUVIA_CHECK(!first.empty());
    RUVIA_CHECK(!second.empty());
    RUVIA_CHECK_EQ(decoded(HttpContentCoding::kZstd, first + second, kDecodedBodyLimit), std::string("first-second"));
}

RUVIA_TEST(request_body_zstd_rejects_bytes_after_the_last_frame) {
    std::string encoded = zstdCompress("complete");
    encoded.append("not-a-zstd-frame");
    RUVIA_CHECK(decodeError(HttpContentCoding::kZstd, encoded) == HttpContentDecodeError::kInvalidContent);
}

RUVIA_TEST(http_request_content_decoder_owns_protocol_failure_status) {
    auto* resource = std::pmr::get_default_resource();

    const auto invalid = decodeHttpRequestContent(HttpContentCoding::kGzip, "not-gzip", {.maxDecodedBytes = 1024, .resource = resource});
    RUVIA_CHECK(invalid.protocolFailure() != nullptr);
    RUVIA_CHECK(invalid.decoderFailure() == nullptr);
    RUVIA_CHECK_EQ(invalid.protocolFailure()->protocolError().status(), ruvia::http_status::kBadRequest);

    const auto oversized = decodeHttpRequestContent(HttpContentCoding::kIdentity, "too large", {.maxDecodedBytes = 4, .resource = resource});
    RUVIA_CHECK(oversized.protocolFailure() != nullptr);
    RUVIA_CHECK(oversized.decoderFailure() == nullptr);
    RUVIA_CHECK_EQ(oversized.protocolFailure()->protocolError().status(), ruvia::http_status::kContentTooLarge);

    const auto unsupported = decodeHttpRequestContent(static_cast<HttpContentCoding>(255), {}, {.maxDecodedBytes = 1024, .resource = resource});
    RUVIA_CHECK(unsupported.protocolFailure() != nullptr);
    RUVIA_CHECK(unsupported.decoderFailure() == nullptr);
    RUVIA_CHECK_EQ(unsupported.protocolFailure()->protocolError().status(), ruvia::http_status::kUnsupportedMediaType);
}
