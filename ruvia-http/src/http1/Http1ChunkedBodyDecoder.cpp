#include "ruvia/http/Http1ChunkedBodyDecoder.h"

#include <limits>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include "ruvia/http/HttpLimits.h"

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
    : framing_({
          .body_limit = config.bodyLimit,
          .framing_limit = kMaxHttpHeaderBytes,
          .trailer_section_limit = ProtocolByteLimit::unlimited(),
          .trailer_role = config.trailerRole == Http1ChunkTrailerRole::kResponse
                              ? detail::chunk_trailer_role::response
                              : detail::chunk_trailer_role::request,
      }) {
    if (config.trailerRole != Http1ChunkTrailerRole::kRequest &&
        config.trailerRole != Http1ChunkTrailerRole::kResponse) {
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
    return std::visit([](const auto& result) {
        using result_type = std::remove_cvref_t<decltype(result)>;
        if constexpr (std::is_same_v<result_type, detail::chunk_framing_need_more>) {
            return Http1ChunkDecodeResult::makeNeedMore(result.consumed_bytes);
        } else if constexpr (std::is_same_v<result_type, detail::chunk_framing_body>) {
            return Http1ChunkDecodeResult::makeBodyChunk(result.consumed_bytes, result.bytes);
        } else if constexpr (std::is_same_v<result_type, detail::chunk_framing_complete>) {
            return Http1ChunkDecodeResult::makeComplete(result.consumed_bytes, result.trailers);
        } else {
            auto error = Http1ChunkDecodeError::kInvalidFraming;
            if (result.error == detail::chunk_framing_error::body_limit_exceeded) {
                error = Http1ChunkDecodeError::kBodyLimitExceeded;
            } else if (result.error == detail::chunk_framing_error::framing_limit_exceeded) {
                error = Http1ChunkDecodeError::kFramingLimitExceeded;
            }
            return Http1ChunkDecodeResult::makeFailure(result.consumed_bytes, error);
        }
    },
        framing_.decode(available, maxBodyBytes));
}

}  // namespace ruvia
