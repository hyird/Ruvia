#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <variant>

#include "ruvia/http/HttpProtocolError.h"
#include "ruvia/http/ProtocolByteLimit.h"
#include "ruvia/http/detail/parser/HttpChunkParser.h"

namespace ruvia::detail {

enum class Http1ChunkDelimiterStatus : std::uint8_t {
    kOk,
    kNeedMore,
    kInvalid
};

class Http1ChunkDecoder final {
public:
    explicit Http1ChunkDecoder(ProtocolByteLimit bodyLimit) noexcept
        : bodyLimit_(bodyLimit) {}

    [[nodiscard]] std::size_t remaining() const noexcept {
        return remaining_;
    }

    [[nodiscard]] bool awaitingDelimiter() const noexcept {
        return awaitingDelimiter_;
    }

    [[nodiscard]] Http1ChunkDelimiterStatus checkDelimiter(
        std::string_view available) const noexcept {
        if (available.size() < 2) {
            return Http1ChunkDelimiterStatus::kNeedMore;
        }
        return available.substr(0, 2) == "\r\n"
            ? Http1ChunkDelimiterStatus::kOk
            : Http1ChunkDelimiterStatus::kInvalid;
    }

    [[nodiscard]] bool parseSizeLine(
        std::string_view line,
        std::size_t& chunkSize) {
        if (!parseHttpChunkSize(line, chunkSize)) {
            throw std::invalid_argument("invalid chunked request body");
        }
        consumeFramingBytes(line.size() + 2);
        if (chunkSize == 0) {
            return false;
        }
        if (bodyLimit_.additionExceeds(decodedBytes_, chunkSize)) {
            throw HttpProtocolError(413, "request body is too large");
        }
        decodedBytes_ += chunkSize;
        remaining_ = chunkSize;
        return true;
    }

    void consumeBodyBytes(std::size_t bytes) noexcept {
        remaining_ -= bytes;
        awaitingDelimiter_ = remaining_ == 0;
    }

    void consumeDelimiter() {
        consumeFramingBytes(2);
        awaitingDelimiter_ = false;
    }

    void consumeTrailers(std::size_t bytes) {
        consumeFramingBytes(bytes);
    }

private:
    void consumeFramingBytes(std::size_t bytes) {
        if (!bodyLimit_.isLimited() || bytes == 0) {
            return;
        }
        if (bodyLimit_.additionExceeds(encodedOverheadBytes_, bytes)) {
            throw HttpProtocolError(413, "request body framing is too large");
        }
        encodedOverheadBytes_ += bytes;
    }

    ProtocolByteLimit bodyLimit_;
    std::size_t remaining_{0};
    std::size_t decodedBytes_{0};
    std::size_t encodedOverheadBytes_{0};
    bool awaitingDelimiter_{false};
};

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

// Incremental chunk decoding has three mutually exclusive outcomes. The
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

private:
    friend class Http1ChunkedBodyDecoder;

    using Value = std::variant<
        Http1ChunkDecodeNeedMore,
        Http1ChunkDecodeBodyChunk,
        Http1ChunkDecodeComplete>;

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

    Value value_;
};

// Incremental sans-I/O decoder for HTTP/1 chunked content. The caller owns the
// input buffer and removes result.consumedBytes() only after a returned body
// view is no longer needed. Framing, trailer validation, and size accounting
// remain protocol-owned; a runtime only refills input on a need-more result.
class Http1ChunkedBodyDecoder final {
public:
    explicit Http1ChunkedBodyDecoder(ProtocolByteLimit bodyLimit) noexcept
        : chunks_(bodyLimit) {}

    [[nodiscard]] Http1ChunkDecodeResult decode(
        std::string_view available) {
        std::size_t cursor = 0;
        for (;;) {
            switch (state_) {
                case State::kSizeLine: {
                    const auto lineEnd = available.find("\r\n", cursor);
                    if (lineEnd == std::string_view::npos) {
                        return Http1ChunkDecodeResult::makeNeedMore(cursor);
                    }
                    std::size_t chunkSize = 0;
                    const auto hasBody = chunks_.parseSizeLine(
                        available.substr(cursor, lineEnd - cursor), chunkSize);
                    cursor = lineEnd + 2;
                    if (!hasBody) {
                        state_ = State::kTrailers;
                        trailerSearchOffset_ = 0;
                    } else {
                        state_ = State::kBody;
                    }
                    break;
                }
                case State::kBody: {
                    if (cursor == available.size()) {
                        return Http1ChunkDecodeResult::makeNeedMore(cursor);
                    }
                    const auto bytes = std::min(
                        chunks_.remaining(), available.size() - cursor);
                    const auto body = available.substr(cursor, bytes);
                    chunks_.consumeBodyBytes(bytes);
                    cursor += bytes;
                    if (chunks_.awaitingDelimiter()) {
                        if (available.size() - cursor >= 2) {
                            consumeDelimiter(available.substr(cursor));
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
                    consumeDelimiter(available.substr(cursor));
                    cursor += 2;
                    state_ = State::kSizeLine;
                    break;
                case State::kTrailers: {
                    const auto trailers = available.substr(cursor);
                    if (trailers.starts_with("\r\n")) {
                        chunks_.consumeTrailers(2);
                        state_ = State::kComplete;
                        return Http1ChunkDecodeResult::makeComplete(cursor + 2);
                    }
                    const auto trailerEnd = trailers.find(
                        "\r\n\r\n", trailerSearchOffset_);
                    if (trailerEnd == std::string_view::npos) {
                        trailerSearchOffset_ = trailers.size() > 3
                            ? trailers.size() - 3
                            : 0;
                        return Http1ChunkDecodeResult::makeNeedMore(cursor);
                    }
                    if (validateHttpChunkTrailers(
                            trailers.substr(0, trailerEnd)).has_value()) {
                        throw std::invalid_argument(
                            "invalid chunked request body");
                    }
                    const auto trailerBytes = trailerEnd + 4;
                    chunks_.consumeTrailers(trailerBytes);
                    state_ = State::kComplete;
                    return Http1ChunkDecodeResult::makeComplete(
                        cursor + trailerBytes);
                }
                case State::kComplete:
                    return Http1ChunkDecodeResult::makeComplete(cursor);
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
    };

    void consumeDelimiter(std::string_view available) {
        if (chunks_.checkDelimiter(available) !=
            Http1ChunkDelimiterStatus::kOk) {
            throw std::invalid_argument("invalid chunked request body");
        }
        chunks_.consumeDelimiter();
    }

    Http1ChunkDecoder chunks_;
    State state_{State::kSizeLine};
    std::size_t trailerSearchOffset_{0};
};

}  // namespace ruvia::detail
