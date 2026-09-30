#include "ruvia/http/Http1ChunkedBodyDecoder.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

#include "ruvia/http/HttpLimits.h"
#include "ruvia/http/detail/parser/HttpChunkParser.h"
#include "ruvia/http/detail/server/HttpResponseTrailers.h"

namespace ruvia {

Http1ChunkDecodeResult Http1ChunkDecodeResult::makeNeedMore(std::size_t consumedBytes) noexcept {
    return Http1ChunkDecodeResult(Http1ChunkDecodeNeedMore(consumedBytes));
}

Http1ChunkDecodeResult Http1ChunkDecodeResult::makeBodyChunk(
    std::size_t consumedBytes, std::string_view bytes) noexcept {
    return Http1ChunkDecodeResult(Http1ChunkDecodeBodyChunkView(consumedBytes, bytes));
}

Http1ChunkDecodeResult Http1ChunkDecodeResult::makeComplete(
    std::size_t consumedBytes, std::string_view trailers) noexcept {
    return Http1ChunkDecodeResult(Http1ChunkDecodeCompleteView(consumedBytes, trailers));
}

Http1ChunkDecodeResult Http1ChunkDecodeResult::makeFailure(
    std::size_t consumedBytes, Http1ChunkDecodeError error) noexcept {
    return Http1ChunkDecodeResult(Http1ChunkDecodeFailure(consumedBytes, error));
}

Http1ChunkedBodyDecoder::Http1ChunkedBodyDecoder(Http1ChunkedBodyDecoderConfig config)
    : bodyLimit_(config.bodyLimit),
      trailerRole_(config.trailerRole) {
    if (trailerRole_ != Http1ChunkTrailerRole::kRequest &&
        trailerRole_ != Http1ChunkTrailerRole::kResponse) {
        throw std::invalid_argument("invalid HTTP/1 chunk trailer role");
    }
}

Http1ChunkedBodyDecoder::~Http1ChunkedBodyDecoder() = default;
Http1ChunkedBodyDecoder::Http1ChunkedBodyDecoder(Http1ChunkedBodyDecoder&&) noexcept = default;
Http1ChunkedBodyDecoder& Http1ChunkedBodyDecoder::operator=(Http1ChunkedBodyDecoder&&) noexcept = default;

Http1ChunkDecodeResult Http1ChunkedBodyDecoder::decode(std::string_view available) {
    return decode(available, std::numeric_limits<std::size_t>::max());
}

Http1ChunkDecodeResult Http1ChunkedBodyDecoder::decode(
    std::string_view available, std::size_t maxBodyBytes) {
    if (maxBodyBytes == 0) {
        throw std::invalid_argument("HTTP/1 chunk decode body quota must be greater than zero");
    }
    if (!state_) {
        return Http1ChunkDecodeResult::makeFailure(0, state_.error());
    }

    std::size_t cursor = 0;
    for (;;) {
        switch (*state_) {
            case ProgressState::kSizeLine: {
                const auto lineEnd = available.find("\r\n", cursor);
                if (lineEnd == std::string_view::npos) {
                    if (available.size() - cursor >= kMaxHttpHeaderBytes) {
                        return fail(cursor, Http1ChunkDecodeError::kFramingLimitExceeded);
                    }
                    return Http1ChunkDecodeResult::makeNeedMore(cursor);
                }
                if (lineEnd - cursor + 2 > kMaxHttpHeaderBytes) {
                    return fail(cursor, Http1ChunkDecodeError::kFramingLimitExceeded);
                }
                std::size_t chunkSize = 0;
                if (!detail::parseHttpChunkSize(available.substr(cursor, lineEnd - cursor), chunkSize)) {
                    return fail(cursor, Http1ChunkDecodeError::kInvalidFraming);
                }
                if (const auto error = accountFraming(lineEnd - cursor + 2)) {
                    return fail(cursor, *error);
                }
                cursor = lineEnd + 2;
                if (chunkSize == 0) {
                    state_ = ProgressState::kTrailers;
                    trailerSearchOffset_ = 0;
                } else {
                    if (bodyLimit_.additionExceeds(decodedBytes_, chunkSize)) {
                        return fail(cursor, Http1ChunkDecodeError::kBodyLimitExceeded);
                    }
                    decodedBytes_ += chunkSize;
                    remaining_ = chunkSize;
                    state_ = ProgressState::kBody;
                }
                break;
            }
            case ProgressState::kBody: {
                if (cursor == available.size()) {
                    return Http1ChunkDecodeResult::makeNeedMore(cursor);
                }
                const auto bytes = std::min({remaining_, available.size() - cursor, maxBodyBytes});
                if (bytes == 0) {
                    return Http1ChunkDecodeResult::makeNeedMore(cursor);
                }
                const auto body = available.substr(cursor, bytes);
                remaining_ -= bytes;
                cursor += bytes;
                if (remaining_ == 0) {
                    if (available.size() - cursor >= 2) {
                        if (const auto error = consumeDelimiter(available.substr(cursor))) {
                            return fail(cursor, *error);
                        }
                        cursor += 2;
                        state_ = ProgressState::kSizeLine;
                    } else {
                        state_ = ProgressState::kDelimiter;
                    }
                }
                return Http1ChunkDecodeResult::makeBodyChunk(cursor, body);
            }
            case ProgressState::kDelimiter:
                if (available.size() - cursor < 2) {
                    return Http1ChunkDecodeResult::makeNeedMore(cursor);
                }
                if (const auto error = consumeDelimiter(available.substr(cursor))) {
                    return fail(cursor, *error);
                }
                cursor += 2;
                state_ = ProgressState::kSizeLine;
                break;
            case ProgressState::kTrailers: {
                const auto trailers = available.substr(cursor);
                if (trailers.starts_with("\r\n")) {
                    if (const auto error = accountFraming(2)) {
                        return fail(cursor, *error);
                    }
                    state_ = ProgressState::kComplete;
                    return Http1ChunkDecodeResult::makeComplete(cursor + 2);
                }
                const auto trailerEnd = trailers.find("\r\n\r\n", trailerSearchOffset_);
                if (trailerEnd == std::string_view::npos) {
                    if (trailers.size() >= kMaxHttpHeaderBytes) {
                        return fail(cursor, Http1ChunkDecodeError::kFramingLimitExceeded);
                    }
                    trailerSearchOffset_ = trailers.size() > 3 ? trailers.size() - 3 : 0;
                    return Http1ChunkDecodeResult::makeNeedMore(cursor);
                }
                if (!trailersValid(trailers.substr(0, trailerEnd))) {
                    return fail(cursor, Http1ChunkDecodeError::kInvalidFraming);
                }
                const auto trailerBytes = trailerEnd + 4;
                if (const auto error = accountFraming(trailerBytes)) {
                    return fail(cursor, *error);
                }
                state_ = ProgressState::kComplete;
                return Http1ChunkDecodeResult::makeComplete(
                    cursor + trailerBytes, trailers.substr(0, trailerEnd));
            }
            case ProgressState::kComplete:
                return Http1ChunkDecodeResult::makeComplete(cursor);
        }
    }
}

std::optional<Http1ChunkDecodeError> Http1ChunkedBodyDecoder::accountFraming(
    std::size_t bytes) noexcept {
    if (encodedOverheadBytes_ > kMaxHttpHeaderBytes ||
        bytes > kMaxHttpHeaderBytes - encodedOverheadBytes_) {
        return Http1ChunkDecodeError::kFramingLimitExceeded;
    }
    encodedOverheadBytes_ += bytes;
    return std::nullopt;
}

std::optional<Http1ChunkDecodeError> Http1ChunkedBodyDecoder::consumeDelimiter(
    std::string_view available) noexcept {
    if (!available.starts_with("\r\n")) {
        return Http1ChunkDecodeError::kInvalidFraming;
    }
    return accountFraming(2);
}

bool Http1ChunkedBodyDecoder::trailersValid(std::string_view trailers) const {
    switch (trailerRole_) {
        case Http1ChunkTrailerRole::kRequest:
            return !detail::validateHttpChunkTrailers(trailers).has_value();
        case Http1ChunkTrailerRole::kResponse:
            return detail::httpResponseTrailerBlockValid(trailers);
    }
    return false;
}

Http1ChunkDecodeResult Http1ChunkedBodyDecoder::fail(
    std::size_t consumedBytes, Http1ChunkDecodeError error) noexcept {
    state_ = std::unexpected(error);
    return Http1ChunkDecodeResult::makeFailure(consumedBytes, error);
}

}  // namespace ruvia
