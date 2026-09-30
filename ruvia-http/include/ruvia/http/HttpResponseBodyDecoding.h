#pragma once

#include <cstddef>
#include <string_view>
#include <utility>

#include "ruvia/http/Http1ChunkedBodyDecoder.h"
#include "ruvia/http/HttpTransferCodingDecoder.h"
#include "ruvia/http/ProtocolByteLimit.h"
#include "ruvia/http/detail/server/HttpResponseTrailers.h"
#include "ruvia/http/detail/util/BorrowedView.h"

namespace ruvia {

// HTTP/1 chunk framing with response trailer semantics fixed at construction.
// Results use the same typed contract as the role-neutral framing core. Consume
// borrowed body/trailer views before mutating or discarding their input prefix.
class HttpResponseChunkedBodyDecoder final {
public:
    explicit HttpResponseChunkedBodyDecoder(ProtocolByteLimit bodyLimit)
        : decoder_({.bodyLimit = bodyLimit, .trailerRole = Http1ChunkTrailerRole::kResponse}) {}

    [[nodiscard]] Http1ChunkDecodeResult decode(std::string_view input) {
        return decoder_.decode(input);
    }

    [[nodiscard]] Http1ChunkDecodeResult decode(std::string_view input, std::size_t maxBodyBytes) {
        return decoder_.decode(input, maxBodyBytes);
    }

    template <detail::HttpTemporaryOwningCharString Input>
    Http1ChunkDecodeResult decode(Input&&) = delete;
    template <detail::HttpTemporaryOwningCharString Input>
    Http1ChunkDecodeResult decode(Input&&, std::size_t) = delete;

private:
    Http1ChunkedBodyDecoder decoder_;
};

// Visit normalized response trailer fields from a validated HTTP/1 trailer block.
template <typename Visitor>
[[nodiscard]] inline bool visitHttpResponseTrailers(std::string_view block, Visitor&& visitor) {
    return detail::visitHttpResponseTrailerFields(block, std::forward<Visitor>(visitor));
}

}  // namespace ruvia
