#include "field_parsing_fixture.h"

// Chunked framing at its limits: quoted extensions, the discriminated scan result, and the trailer
// section bounds.

RUVIA_TEST(chunk_extension_quoted_pair_allows_escaped_htab) {
    using ruvia::detail::scanHttpChunkedBody;

    const std::string_view body = "1;note=\"a\\\tb\"\r\nx\r\n0\r\n\r\n";
    const auto result = scanHttpChunkedBody(body);
    RUVIA_CHECK(result.complete() != nullptr);
    RUVIA_CHECK_EQ(result.complete()->consumedBytes(), body.size());
    RUVIA_CHECK(result.needMore() == nullptr);
    RUVIA_CHECK(result.failure() == nullptr);
}

RUVIA_TEST(chunk_scan_result_is_discriminated) {
    const auto needMore = ruvia::detail::scanHttpChunkedBody("1\r\nx");
    RUVIA_CHECK(needMore.needMore() != nullptr);
    RUVIA_CHECK(needMore.complete() == nullptr);
    RUVIA_CHECK(needMore.failure() == nullptr);

    const auto failure = ruvia::detail::scanHttpChunkedBody("xyz\r\n");
    RUVIA_CHECK(failure.failure() != nullptr);
    RUVIA_CHECK(failure.failure()->error() == ruvia::detail::HttpChunkScanError::kInvalidSize);
    RUVIA_CHECK(failure.needMore() == nullptr);
    RUVIA_CHECK(failure.complete() == nullptr);
}

RUVIA_TEST(chunk_trailer_section_enforces_field_and_byte_limits) {
    std::string tooMany = "0\r\n";
    for (std::size_t i = 0; i <= ruvia::kMaxHttpHeaderFields; ++i) {
        tooMany.append("X-Trace: value\r\n");
    }
    tooMany.append("\r\n");
    const auto tooManyResult = ruvia::detail::scanHttpChunkedBody(tooMany);
    RUVIA_CHECK(tooManyResult.failure() != nullptr);
    if (const auto* failure = tooManyResult.failure()) {
        RUVIA_CHECK(failure->error() == ruvia::detail::HttpChunkScanError::kTooLarge);
    }

    std::string oversized = "0\r\nX-Trace: ";
    oversized.append(ruvia::kMaxHttpHeaderBytes, 'x');
    oversized.append("\r\n\r\n");
    const auto oversizedResult = ruvia::detail::scanHttpChunkedBody(oversized);
    RUVIA_CHECK(oversizedResult.failure() != nullptr);
    if (const auto* failure = oversizedResult.failure()) {
        RUVIA_CHECK(failure->error() == ruvia::detail::HttpChunkScanError::kTooLarge);
    }
}

RUVIA_TEST(chunk_scan_rejects_unterminated_framing_at_the_header_limit) {
    std::string oversizedSizeLine(ruvia::kMaxHttpHeaderBytes, '1');
    const auto sizeLineResult = ruvia::detail::scanHttpChunkedBody(oversizedSizeLine);
    RUVIA_CHECK(sizeLineResult.failure() != nullptr);
    if (const auto* failure = sizeLineResult.failure()) {
        RUVIA_CHECK(failure->error() == ruvia::detail::HttpChunkScanError::kTooLarge);
    }

    std::string oversizedTerminatedSizeLine = "1;x=";
    oversizedTerminatedSizeLine.append(ruvia::kMaxHttpHeaderBytes, 'a');
    oversizedTerminatedSizeLine.append("\r\n");
    const auto terminatedSizeLineResult = ruvia::detail::scanHttpChunkedBody(oversizedTerminatedSizeLine);
    RUVIA_CHECK(terminatedSizeLineResult.failure() != nullptr);
    if (const auto* failure = terminatedSizeLineResult.failure()) {
        RUVIA_CHECK(failure->error() == ruvia::detail::HttpChunkScanError::kTooLarge);
    }

    std::string oversizedTrailers = "0\r\nX-Trace: ";
    oversizedTrailers.append(ruvia::kMaxHttpHeaderBytes, 'x');
    const auto trailerResult = ruvia::detail::scanHttpChunkedBody(oversizedTrailers);
    RUVIA_CHECK(trailerResult.failure() != nullptr);
    if (const auto* failure = trailerResult.failure()) {
        RUVIA_CHECK(failure->error() == ruvia::detail::HttpChunkScanError::kTooLarge);
    }

    std::string boundarySizeLine = "1;x=";
    boundarySizeLine.append(ruvia::kMaxHttpHeaderBytes - boundarySizeLine.size() - 2, 'a');
    boundarySizeLine.append("\r\nx\r\n0\r\n\r\n");
    const auto boundaryResult = ruvia::detail::scanHttpChunkedBody(boundarySizeLine);
    RUVIA_CHECK(boundaryResult.complete() != nullptr);
}
