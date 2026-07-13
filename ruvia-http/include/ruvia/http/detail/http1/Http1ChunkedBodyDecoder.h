#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>
#include <variant>

#include "ruvia/http/HttpLimits.h"
#include "ruvia/http/ProtocolByteLimit.h"
#include "ruvia/http/detail/parser/HttpChunkParser.h"

namespace ruvia::detail {

class Http1ChunkDecodeNeedMore final {
public:
    [[nodiscard]] constexpr std::size_t consumedBytes() const noexcept {
        return consumedBytes_;
    }

private:
    friend class Http1ChunkDecodeResult;

    explicit constexpr Http1ChunkDecodeNeedMore(
        std::size_t consumedBytes) noexcept
        : consumedBytes_(consumedBytes) {}

    std::size_t consumedBytes_;
};

class Http1ChunkDecodeBodyChunk final {
public:
    [[nodiscard]] constexpr std::size_t consumedBytes() const noexcept {
        return consumedBytes_;
    }

    [[nodiscard]] constexpr std::string_view bytes() const noexcept {
        return bytes_;
    }

private:
    friend class Http1ChunkDecodeResult;

    constexpr Http1ChunkDecodeBodyChunk(
        std::size_t consumedBytes,
        std::string_view bytes) noexcept
        : consumedBytes_(consumedBytes), bytes_(bytes) {}

    std::size_t consumedBytes_;
    std::string_view bytes_;
};

class Http1ChunkDecodeComplete final {
public:
    [[nodiscard]] constexpr std::size_t consumedBytes() const noexcept {
        return consumedBytes_;
    }

private:
    friend class Http1ChunkDecodeResult;

    explicit constexpr Http1ChunkDecodeComplete(
        std::size_t consumedBytes) noexcept
        : consumedBytes_(consumedBytes) {}

    std::size_t consumedBytes_;
};

enum class Http1ChunkDecodeError : std::uint8_t {
    kInvalidFraming,
    kBodyTooLarge,
    kFramingTooLarge,
};

class Http1ChunkDecodeFailure final {
public:
    [[nodiscard]] constexpr std::size_t consumedBytes() const noexcept {
        return consumedBytes_;
    }

    [[nodiscard]] constexpr Http1ChunkDecodeError error() const noexcept {
        return error_;
    }

private:
    friend class Http1ChunkDecodeResult;

    constexpr Http1ChunkDecodeFailure(
        std::size_t consumedBytes,
        Http1ChunkDecodeError error) noexcept
        : consumedBytes_(consumedBytes), error_(error) {}

    std::size_t consumedBytes_;
    Http1ChunkDecodeError error_;
};

// Incremental chunk decoding has four mutually exclusive outcomes. The
// consumed prefix is meaningful for all of them, while only a body result can
// expose bytes. The borrowed body view remains valid until the caller compacts
// or otherwise mutates its input buffer.
class Http1ChunkDecodeResult final {
public:
    [[nodiscard]] std::size_t consumedBytes() const noexcept {
        return std::visit(
            [](const auto& result) { return result.consumedBytes(); },
            value_);
    }

    [[nodiscard]] const Http1ChunkDecodeNeedMore* needMore() const noexcept {
        return std::get_if<Http1ChunkDecodeNeedMore>(&value_);
    }

    [[nodiscard]] const Http1ChunkDecodeBodyChunk* bodyChunk() const noexcept {
        return std::get_if<Http1ChunkDecodeBodyChunk>(&value_);
    }

    [[nodiscard]] const Http1ChunkDecodeComplete* complete() const noexcept {
        return std::get_if<Http1ChunkDecodeComplete>(&value_);
    }

    [[nodiscard]] const Http1ChunkDecodeFailure* failure() const noexcept {
        return std::get_if<Http1ChunkDecodeFailure>(&value_);
    }

private:
    friend class Http1ChunkedBodyDecoder;

    using Value = std::variant<
        Http1ChunkDecodeNeedMore,
        Http1ChunkDecodeBodyChunk,
        Http1ChunkDecodeComplete,
        Http1ChunkDecodeFailure>;

    template <typename Result>
    explicit Http1ChunkDecodeResult(Result result) noexcept
        : value_(std::move(result)) {}

    [[nodiscard]] static Http1ChunkDecodeResult makeNeedMore(
        std::size_t consumedBytes) noexcept {
        return Http1ChunkDecodeResult(
            Http1ChunkDecodeNeedMore(consumedBytes));
    }

    [[nodiscard]] static Http1ChunkDecodeResult makeBodyChunk(
        std::size_t consumedBytes,
        std::string_view bytes) noexcept {
        return Http1ChunkDecodeResult(
            Http1ChunkDecodeBodyChunk(consumedBytes, bytes));
    }

    [[nodiscard]] static Http1ChunkDecodeResult makeComplete(
        std::size_t consumedBytes) noexcept {
        return Http1ChunkDecodeResult(
            Http1ChunkDecodeComplete(consumedBytes));
    }

    [[nodiscard]] static Http1ChunkDecodeResult makeFailure(
        std::size_t consumedBytes,
        Http1ChunkDecodeError error) noexcept {
        return Http1ChunkDecodeResult(
            Http1ChunkDecodeFailure(consumedBytes, error));
    }

    Value value_;
};

// Incremental sans-I/O decoder for HTTP/1 chunked content. The caller owns the
// input buffer and removes result.consumedBytes() only after a returned body
// view is no longer needed. Framing, trailer validation, and size accounting
// remain protocol-owned; a runtime only refills input on a need-more result.
class Http1ChunkedBodyDecoder final {
public:
    explicit Http1ChunkedBodyDecoder(ProtocolByteLimit bodyLimit) noexcept
        : bodyLimit_(bodyLimit) {}

    [[nodiscard]] Http1ChunkDecodeResult decode(
        std::string_view available) {
        std::size_t cursor = 0;
        for (;;) {
            switch (state_) {
                case State::kSizeLine: {
                    const auto lineEnd = available.find("\r\n", cursor);
                    if (lineEnd == std::string_view::npos) {
                        if (available.size() - cursor >= kMaxHttpHeaderBytes) {
                            return fail(
                                cursor,
                                Http1ChunkDecodeError::kFramingTooLarge);
                        }
                        return Http1ChunkDecodeResult::makeNeedMore(cursor);
                    }
                    std::size_t chunkSize = 0;
                    if (!parseHttpChunkSize(
                            available.substr(cursor, lineEnd - cursor),
                            chunkSize)) {
                        return fail(
                            cursor,
                            Http1ChunkDecodeError::kInvalidFraming);
                    }
                    if (const auto error = accountFraming(
                            lineEnd - cursor + 2)) {
                        return fail(cursor, *error);
                    }
                    cursor = lineEnd + 2;
                    if (chunkSize == 0) {
                        state_ = State::kTrailers;
                        trailerSearchOffset_ = 0;
                    } else {
                        if (bodyLimit_.additionExceeds(
                                decodedBytes_, chunkSize)) {
                            return fail(
                                cursor,
                                Http1ChunkDecodeError::kBodyTooLarge);
                        }
                        decodedBytes_ += chunkSize;
                        remaining_ = chunkSize;
                        state_ = State::kBody;
                    }
                    break;
                }
                case State::kBody: {
                    if (cursor == available.size()) {
                        return Http1ChunkDecodeResult::makeNeedMore(cursor);
                    }
                    const auto bytes = std::min(
                        remaining_, available.size() - cursor);
                    const auto body = available.substr(cursor, bytes);
                    remaining_ -= bytes;
                    cursor += bytes;
                    if (remaining_ == 0) {
                        if (available.size() - cursor >= 2) {
                            if (const auto error = consumeDelimiter(
                                    available.substr(cursor))) {
                                return fail(cursor, *error);
                            }
                            cursor += 2;
                            state_ = State::kSizeLine;
                        } else {
                            state_ = State::kDelimiter;
                        }
                    }
                    return Http1ChunkDecodeResult::makeBodyChunk(cursor, body);
                }
                case State::kDelimiter:
                    if (available.size() - cursor < 2) {
                        return Http1ChunkDecodeResult::makeNeedMore(cursor);
                    }
                    if (const auto error = consumeDelimiter(
                            available.substr(cursor))) {
                        return fail(cursor, *error);
                    }
                    cursor += 2;
                    state_ = State::kSizeLine;
                    break;
                case State::kTrailers: {
                    const auto trailers = available.substr(cursor);
                    if (trailers.starts_with("\r\n")) {
                        if (const auto error = accountFraming(2)) {
                            return fail(cursor, *error);
                        }
                        state_ = State::kComplete;
                        return Http1ChunkDecodeResult::makeComplete(cursor + 2);
                    }
                    const auto trailerEnd = trailers.find(
                        "\r\n\r\n", trailerSearchOffset_);
                    if (trailerEnd == std::string_view::npos) {
                        if (trailers.size() >= kMaxHttpHeaderBytes) {
                            return fail(
                                cursor,
                                Http1ChunkDecodeError::kFramingTooLarge);
                        }
                        trailerSearchOffset_ = trailers.size() > 3
                            ? trailers.size() - 3
                            : 0;
                        return Http1ChunkDecodeResult::makeNeedMore(cursor);
                    }
                    if (validateHttpChunkTrailers(
                            trailers.substr(0, trailerEnd)).has_value()) {
                        return fail(
                            cursor,
                            Http1ChunkDecodeError::kInvalidFraming);
                    }
                    const auto trailerBytes = trailerEnd + 4;
                    if (const auto error = accountFraming(trailerBytes)) {
                        return fail(cursor, *error);
                    }
                    state_ = State::kComplete;
                    return Http1ChunkDecodeResult::makeComplete(
                        cursor + trailerBytes);
                }
                case State::kComplete:
                    return Http1ChunkDecodeResult::makeComplete(cursor);
                case State::kFailed:
                    return Http1ChunkDecodeResult::makeFailure(
                        cursor, failure_);
            }
        }
    }

private:
    enum class State : std::uint8_t {
        kSizeLine,
        kBody,
        kDelimiter,
        kTrailers,
        kComplete,
        kFailed,
    };

    [[nodiscard]] std::optional<Http1ChunkDecodeError> accountFraming(
        std::size_t bytes) noexcept {
        if (bodyLimit_.additionExceeds(encodedOverheadBytes_, bytes)) {
            return Http1ChunkDecodeError::kFramingTooLarge;
        }
        encodedOverheadBytes_ += bytes;
        return std::nullopt;
    }

    [[nodiscard]] std::optional<Http1ChunkDecodeError> consumeDelimiter(
        std::string_view available) noexcept {
        if (!available.starts_with("\r\n")) {
            return Http1ChunkDecodeError::kInvalidFraming;
        }
        return accountFraming(2);
    }

    [[nodiscard]] Http1ChunkDecodeResult fail(
        std::size_t consumedBytes,
        Http1ChunkDecodeError error) noexcept {
        state_ = State::kFailed;
        failure_ = error;
        return Http1ChunkDecodeResult::makeFailure(consumedBytes, error);
    }

    ProtocolByteLimit bodyLimit_;
    State state_{State::kSizeLine};
    std::size_t trailerSearchOffset_{0};
    std::size_t remaining_{0};
    std::size_t decodedBytes_{0};
    std::size_t encodedOverheadBytes_{0};
    Http1ChunkDecodeError failure_{Http1ChunkDecodeError::kInvalidFraming};
};

}  // namespace ruvia::detail
