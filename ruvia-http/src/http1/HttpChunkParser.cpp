#include "ruvia/http/detail/parser/HttpChunkParser.h"

#include <limits>
#include <utility>

#include "ruvia/http/HttpLimits.h"
#include "ruvia/http/detail/field/HeaderTokenUtils.h"
#include "ruvia/http/detail/field/HttpTrailerFields.h"
#include "ruvia/http/detail/parser/HttpParserSyntax.h"
#include "ruvia/http/detail/parser/http_chunk_framing.h"

namespace ruvia::detail {

HttpChunkTrailerParseResult HttpChunkTrailerParser::fail(HttpChunkScanError error) noexcept {
    failure_ = error;
    return HttpChunkTrailerParseResult(HttpChunkTrailerFailure(error));
}

HttpChunkTrailerParseResult HttpChunkTrailerParser::next() noexcept {
    if (failure_) {
        return HttpChunkTrailerParseResult(HttpChunkTrailerFailure(*failure_));
    }
    if (cursor_ == 0 && trailers_.size() > kMaxHttpHeaderBytes) {
        return fail(HttpChunkScanError::kTooLarge);
    }
    if (cursor_ == trailers_.size()) {
        return HttpChunkTrailerParseResult(HttpChunkTrailerEnd());
    }
    if (fieldCount_ == kMaxHttpHeaderFields) {
        return fail(HttpChunkScanError::kTooLarge);
    }
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
        if (const auto* failure = result.failure()) {
            return failure->error();
        }
        if (result.end()) {
            return std::nullopt;
        }
    }
}

HttpChunkScanResult scanHttpChunkedBody(std::string_view body) noexcept {
    http_chunk_framing framing({
        .body_limit = ProtocolByteLimit::limited(kDefaultMaxBufferedBodyBytes),
        .framing_limit = kDefaultMaxBufferedBodyBytes,
        .trailer_section_limit = ProtocolByteLimit::limited(kMaxHttpHeaderBytes),
        .trailer_role = chunk_trailer_role::request,
    });
    std::size_t consumed = 0;
    for (;;) {
        const auto result = framing.decode(body.substr(consumed), std::numeric_limits<std::size_t>::max());
        if (const auto* complete = std::get_if<chunk_framing_complete>(&result)) {
            return HttpChunkScanResult::makeComplete(consumed + complete->consumed_bytes);
        }
        if (const auto* failure = std::get_if<chunk_framing_failure>(&result)) {
            switch (failure->error) {
                case chunk_framing_error::invalid_size:
                    return HttpChunkScanResult::makeFailure(HttpChunkScanError::kInvalidSize);
                case chunk_framing_error::size_overflow:
                    return HttpChunkScanResult::makeFailure(HttpChunkScanError::kSizeOverflow);
                case chunk_framing_error::invalid_extension:
                    return HttpChunkScanResult::makeFailure(HttpChunkScanError::kInvalidExtension);
                case chunk_framing_error::invalid_crlf:
                    return HttpChunkScanResult::makeFailure(HttpChunkScanError::kInvalidCrlf);
                case chunk_framing_error::invalid_trailer:
                    return HttpChunkScanResult::makeFailure(HttpChunkScanError::kInvalidTrailer);
                case chunk_framing_error::trailer_limit_exceeded:
                case chunk_framing_error::body_limit_exceeded:
                case chunk_framing_error::framing_limit_exceeded:
                    return HttpChunkScanResult::makeFailure(HttpChunkScanError::kTooLarge);
            }
            std::unreachable();
        }
        if (std::holds_alternative<chunk_framing_need_more>(result)) {
            return HttpChunkScanResult::makeNeedMore();
        }
        consumed += std::get<chunk_framing_body>(result).consumed_bytes;
    }
}

}  // namespace ruvia::detail
