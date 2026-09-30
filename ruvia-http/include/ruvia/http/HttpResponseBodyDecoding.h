#pragma once

#include <cstddef>
#include <limits>
#include <memory_resource>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

#include "ruvia/http/HttpTransferCodingDecoder.h"
#include "ruvia/http/ProtocolByteLimit.h"
#include "ruvia/http/detail/http1/Http1ChunkedBodyDecoder.h"
#include "ruvia/http/detail/server/HttpResponseTrailers.h"

namespace ruvia {

// Incremental HTTP/1 chunk framing decoder for response content. The decoder
// validates response trailer semantics and returns the trailer block as a view
// into caller input when the final chunk is reached.
class HttpResponseChunkedBodyDecoder final {
public:
    enum class State : unsigned char { kNeedMore,
        kBody,
        kComplete,
        kInvalid };

    class Result final {
    public:
        [[nodiscard]] State state() const noexcept {
            return state_;
        }
        [[nodiscard]] std::size_t consumedBytes() const noexcept {
            return consumed_;
        }
        [[nodiscard]] std::string_view body() const noexcept {
            return body_;
        }
        [[nodiscard]] std::string_view trailers() const noexcept {
            return trailers_;
        }

    private:
        friend class HttpResponseChunkedBodyDecoder;
        Result(State state, std::size_t consumed, std::string_view body,
            std::string_view trailers = {}) noexcept
            : state_(state),
              consumed_(consumed),
              body_(body),
              trailers_(trailers) {}
        State state_;
        std::size_t consumed_;
        std::string_view body_;
        std::string_view trailers_;
    };

    explicit HttpResponseChunkedBodyDecoder(ProtocolByteLimit bodyLimit) noexcept
        : decoder_(bodyLimit, detail::Http1ChunkTrailerRole::kResponse) {}

    [[nodiscard]] Result decode(std::string_view input) {
        return decode(input, std::numeric_limits<std::size_t>::max());
    }

    [[nodiscard]] Result decode(std::string_view input, std::size_t maxBodyBytes) {
        const auto decoded = decoder_.decode(input, maxBodyBytes);
        if (const auto* body = decoded.bodyChunk()) {
            return {State::kBody, body->consumedBytes(), body->bytes()};
        }
        if (const auto* complete = decoded.complete()) {
            return {State::kComplete, complete->consumedBytes(), {}, complete->trailers()};
        }
        if (decoded.failure()) {
            return {State::kInvalid, decoded.consumedBytes(), {}};
        }
        return {State::kNeedMore, decoded.consumedBytes(), {}};
    }

private:
    detail::Http1ChunkedBodyDecoder decoder_;
};

// Visit normalized response trailer fields from a validated HTTP/1 trailer block.
template <typename Visitor>
[[nodiscard]] inline bool visitHttpResponseTrailers(std::string_view block, Visitor&& visitor) {
    return detail::visitHttpResponseTrailerFields(block, std::forward<Visitor>(visitor));
}

}  // namespace ruvia
