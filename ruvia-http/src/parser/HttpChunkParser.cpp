#include "ruvia/http/detail/parser/HttpChunkParser.h"

#include "ruvia/http/detail/field/HeaderTokenUtils.h"
#include "ruvia/http/detail/field/HttpTrailerFields.h"
#include "ruvia/http/detail/parser/HttpParserSyntax.h"
#include "ruvia/http/HttpLimits.h"

namespace ruvia::detail {

HttpChunkTrailerParseResult HttpChunkTrailerParser::fail(HttpChunkScanError error) noexcept {
    failure_ = error;
    return HttpChunkTrailerParseResult(HttpChunkTrailerFailure(error));
}

HttpChunkTrailerParseResult HttpChunkTrailerParser::next() noexcept {
    if (failure_) return HttpChunkTrailerParseResult(HttpChunkTrailerFailure(*failure_));
    if (cursor_ == 0 && trailers_.size() > kMaxHttpHeaderBytes) {
        return fail(HttpChunkScanError::kTooLarge);
    }
    if (cursor_ == trailers_.size()) {
        return HttpChunkTrailerParseResult(HttpChunkTrailerEnd());
    }
    if (fieldCount_ == kMaxHttpHeaderFields) return fail(HttpChunkScanError::kTooLarge);
    ++fieldCount_;

    const auto lineEnd = trailers_.find("\r\n", cursor_);
    const auto line = lineEnd == std::string_view::npos
                          ? trailers_.substr(cursor_)
                          : trailers_.substr(cursor_, lineEnd - cursor_);
    if (line.empty() || line.front() == ' ' || line.front() == '\t') {
        return fail(HttpChunkScanError::kInvalidTrailer);
    }
    const auto colon = line.find(':');
    if (colon == std::string_view::npos || colon == 0) {
        return fail(HttpChunkScanError::kInvalidTrailer);
    }
    const auto name = line.substr(0, colon);
    const auto value = httpTrimOws(line.substr(colon + 1));
    if (!isValidHttpHeaderName(name) || !isValidHttpHeaderValue(value) ||
        isForbiddenHttpRequestTrailerName(name)) {
        return fail(HttpChunkScanError::kInvalidTrailer);
    }
    cursor_ = lineEnd == std::string_view::npos ? trailers_.size() : lineEnd + 2;
    return HttpChunkTrailerParseResult(HttpChunkTrailerField(name, value));
}

std::optional<HttpChunkScanError> validateHttpChunkTrailers(std::string_view trailers) noexcept {
    if (trailers.size() > kMaxHttpHeaderBytes) {
        return HttpChunkScanError::kTooLarge;
    }
    HttpChunkTrailerParser parser(trailers);
    for (;;) {
        const auto result = parser.next();
        if (const auto* failure = result.failure()) return failure->error();
        if (result.end()) return std::nullopt;
    }
}

bool parseHttpChunkSize(std::string_view value, std::size_t& size) noexcept {
    return parseHttpChunkSizeLine(value, size) == ChunkSizeLineStatus::kOk;
}

HttpChunkScanResult scanHttpChunkedBody(std::string_view body) noexcept {
    std::size_t cursor = 0;
    std::size_t decoded = 0;
    std::size_t encodedOverhead = 0;
    const auto addOverhead = [&encodedOverhead](std::size_t bytes) noexcept {
        if (bytes > kDefaultMaxBufferedBodyBytes ||
            encodedOverhead > kDefaultMaxBufferedBodyBytes - bytes) {
            return false;
        }
        encodedOverhead += bytes;
        return true;
    };
    for (;;) {
        const auto lineEnd = body.find("\r\n", cursor);
        if (lineEnd == std::string_view::npos) {
            // A future CRLF can begin at most one byte before the current end.
            // Once the unterminated line itself reaches the framing-line byte
            // limit, no completion can make this a bounded chunk-size line.
            if (body.size() - cursor >= kMaxHttpHeaderBytes) {
                return HttpChunkScanResult::makeFailure(HttpChunkScanError::kTooLarge);
            }
            return HttpChunkScanResult::makeNeedMore();
        }
        if (lineEnd - cursor + 2 > kMaxHttpHeaderBytes) {
            return HttpChunkScanResult::makeFailure(HttpChunkScanError::kTooLarge);
        }

        std::size_t chunkSize = 0;
        switch (parseHttpChunkSizeLine(body.substr(cursor, lineEnd - cursor), chunkSize)) {
            case ChunkSizeLineStatus::kOk:
                break;
            case ChunkSizeLineStatus::kInvalidSize:
                return HttpChunkScanResult::makeFailure(HttpChunkScanError::kInvalidSize);
            case ChunkSizeLineStatus::kOverflow:
                return HttpChunkScanResult::makeFailure(HttpChunkScanError::kSizeOverflow);
            case ChunkSizeLineStatus::kInvalidExtension:
                return HttpChunkScanResult::makeFailure(HttpChunkScanError::kInvalidExtension);
        }
        if (!addOverhead(lineEnd - cursor + 2)) {
            return HttpChunkScanResult::makeFailure(HttpChunkScanError::kTooLarge);
        }
        cursor = lineEnd + 2;

        if (chunkSize == 0) {
            if (body.substr(cursor, 2) == "\r\n") {
                if (!addOverhead(2)) {
                    return HttpChunkScanResult::makeFailure(HttpChunkScanError::kTooLarge);
                }
                return HttpChunkScanResult::makeComplete(cursor + 2);
            }
            const auto trailerEnd = body.find("\r\n\r\n", cursor);
            if (trailerEnd != std::string_view::npos) {
                if (trailerEnd - cursor > kMaxHttpHeaderBytes - 4) {
                    return HttpChunkScanResult::makeFailure(HttpChunkScanError::kTooLarge);
                }
                if (const auto trailerError =
                        validateHttpChunkTrailers(body.substr(cursor, trailerEnd - cursor));
                    trailerError.has_value()) {
                    return HttpChunkScanResult::makeFailure(*trailerError);
                }
                if (!addOverhead(trailerEnd - cursor + 4)) {
                    return HttpChunkScanResult::makeFailure(HttpChunkScanError::kTooLarge);
                }
                return HttpChunkScanResult::makeComplete(trailerEnd + 4);
            }
            // Preserve the last three bytes as a possible delimiter prefix.
            // At kMaxHttpHeaderBytes available bytes, even the earliest future
            // CRLFCRLF would exceed the complete trailer-section limit.
            if (body.size() - cursor >= kMaxHttpHeaderBytes) {
                return HttpChunkScanResult::makeFailure(HttpChunkScanError::kTooLarge);
            }
            return HttpChunkScanResult::makeNeedMore();
        }

        if (chunkSize > kDefaultMaxBufferedBodyBytes ||
            decoded > kDefaultMaxBufferedBodyBytes - chunkSize) {
            return HttpChunkScanResult::makeFailure(HttpChunkScanError::kTooLarge);
        }
        if (body.size() < cursor + chunkSize + 2) {
            return HttpChunkScanResult::makeNeedMore();
        }
        if (body.substr(cursor + chunkSize, 2) != "\r\n") {
            return HttpChunkScanResult::makeFailure(HttpChunkScanError::kInvalidCrlf);
        }
        if (!addOverhead(2)) {
            return HttpChunkScanResult::makeFailure(HttpChunkScanError::kTooLarge);
        }

        decoded += chunkSize;
        cursor += chunkSize + 2;
    }
}

}  // namespace ruvia::detail
