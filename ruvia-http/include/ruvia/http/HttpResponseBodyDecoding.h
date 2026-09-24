#pragma once

#include <cstddef>
#include <memory_resource>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

#include "ruvia/http/HttpProtocolError.h"
#include "ruvia/http/HttpTransferCoding.h"
#include "ruvia/http/ProtocolByteLimit.h"
#include "ruvia/http/detail/coding/HttpTransferCodingDecoder.h"
#include "ruvia/http/detail/http1/Http1ChunkedBodyDecoder.h"
#include "ruvia/http/detail/server/HttpResponseTrailers.h"

namespace ruvia {

// Incremental sans-I/O decoder for a response transfer-coding. Input and output
// remain caller-owned; output bytes are valid only until the next decode call.
class HttpTransferCodingDecoder final {
public:
    enum class State : unsigned char { kNeedInput,
        kOutput,
        kComplete,
        kProtocolError,
        kDecoderError };

    class Result final {
    public:
        [[nodiscard]] State state() const noexcept {
            return state_;
        }
        [[nodiscard]] std::size_t consumedBytes() const noexcept {
            return consumed_;
        }
        [[nodiscard]] std::string_view output() const noexcept {
            return output_;
        }
        [[nodiscard]] const HttpProtocolError* protocolError() const noexcept {
            return error_ ? &*error_ : nullptr;
        }

    private:
        friend class HttpTransferCodingDecoder;
        Result(State state, std::size_t consumed, std::string_view output,
            std::optional<HttpProtocolError> error = std::nullopt) noexcept
            : state_(state),
              consumed_(consumed),
              output_(output),
              error_(std::move(error)) {}
        State state_;
        std::size_t consumed_;
        std::string_view output_;
        std::optional<HttpProtocolError> error_;
    };

    HttpTransferCodingDecoder(HttpTransferCoding coding, std::pmr::memory_resource* resource,
        ProtocolByteLimit decodedLimit)
        : decoder_(coding, resource, decodedLimit) {}

    [[nodiscard]] Result decode(std::string_view input, std::span<char> output) noexcept {
        return adapt(decoder_.decode(input, output));
    }
    [[nodiscard]] Result finishInput() noexcept {
        return adapt(decoder_.finishInput());
    }

private:
    [[nodiscard]] static Result adapt(const detail::TransferCodingDecodeResult& result) noexcept {
        if (const auto* value = result.output()) {
            return {State::kOutput, value->consumedBytes(), value->bytes()};
        }
        if (const auto* value = result.needInput()) {
            return {State::kNeedInput, value->consumedBytes(), {}};
        }
        if (const auto* value = result.complete()) {
            return {State::kComplete, value->consumedBytes(), {}};
        }
        if (const auto* value = result.protocolFailure()) {
            return {State::kProtocolError, value->consumedBytes(), {}, value->protocolError()};
        }
        return {State::kDecoderError, result.consumedBytes(), {}};
    }
    detail::TransferCodingDecoder decoder_;
};

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
        const auto decoded = decoder_.decode(input);
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
