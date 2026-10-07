#include <algorithm>
#include <array>
#include <limits>

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
    const auto terminatedSizeLineResult =
        ruvia::detail::scanHttpChunkedBody(oversizedTerminatedSizeLine);
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

RUVIA_TEST(chunk_scan_preserves_incomplete_prefixes_and_pipeline_boundary) {
    constexpr std::string_view message = "3;ext=yes\r\nabc\r\n2\r\nde\r\n0\r\nX-Trace: ok\r\n\r\n";
    for (std::size_t length = 0; length < message.size(); ++length) {
        const auto result = ruvia::detail::scanHttpChunkedBody(message.substr(0, length));
        RUVIA_CHECK(result.needMore() != nullptr);
    }
    const std::string wire = std::string(message) + "NEXT";
    const auto complete = ruvia::detail::scanHttpChunkedBody(wire);
    RUVIA_CHECK(complete.complete() != nullptr);
    if (const auto* boundary = complete.complete()) {
        RUVIA_CHECK_EQ(boundary->consumedBytes(), message.size());
        RUVIA_CHECK_EQ(std::string_view(wire).substr(boundary->consumedBytes()), "NEXT");
    }
}

RUVIA_TEST(chunk_scan_reports_detailed_framing_failures) {
    using ruvia::detail::HttpChunkScanError;
    const std::array cases{
        std::pair{std::string_view("xyz\r\n"), HttpChunkScanError::kInvalidSize},
        std::pair{std::string_view("1;=x\r\n"), HttpChunkScanError::kInvalidExtension},
        std::pair{std::string_view("1\r\nxXY"), HttpChunkScanError::kInvalidCrlf},
        std::pair{std::string_view("0\r\nContent-Length: 1\r\n\r\n"), HttpChunkScanError::kInvalidTrailer},
    };
    for (const auto& [wire, expected] : cases) {
        const auto result = ruvia::detail::scanHttpChunkedBody(wire);
        RUVIA_CHECK(result.failure() != nullptr);
        if (const auto* failure = result.failure()) {
            RUVIA_CHECK(failure->error() == expected);
        }
    }
    const std::string overflow = std::string(std::numeric_limits<std::size_t>::digits / 4 + 1, 'f') + "\r\n";
    const auto result = ruvia::detail::scanHttpChunkedBody(overflow);
    RUVIA_CHECK(result.failure() != nullptr);
    if (const auto* failure = result.failure()) {
        RUVIA_CHECK(failure->error() == HttpChunkScanError::kSizeOverflow);
    }
}

RUVIA_TEST(chunk_scan_enforces_its_cumulative_framing_budget_at_exact_boundary) {
    for (const std::size_t excess : {std::size_t{0}, std::size_t{1}}) {
        // Large valid extensions exhaust framing without exhausting payload.
        std::string wire;
        std::size_t remaining = ruvia::kDefaultMaxBufferedBodyBytes - 5 + excess;
        while (remaining != 0) {
            const auto overhead = std::min(remaining, ruvia::kMaxHttpHeaderBytes + 2);
            wire.append("1;x=");
            wire.append(overhead - 8, 'a');
            wire.append("\r\nx\r\n");
            remaining -= overhead;
        }
        wire.append("0\r\n\r\nNEXT");
        const auto result = ruvia::detail::scanHttpChunkedBody(wire);
        if (excess == 0) {
            RUVIA_CHECK(result.complete() != nullptr);
            if (const auto* boundary = result.complete()) {
                RUVIA_CHECK_EQ(std::string_view(wire).substr(boundary->consumedBytes()), "NEXT");
            }
        } else {
            RUVIA_CHECK(result.failure() != nullptr);
            if (const auto* failure = result.failure()) {
                RUVIA_CHECK(failure->error() == ruvia::detail::HttpChunkScanError::kTooLarge);
            }
        }
    }
}
