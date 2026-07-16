#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>
#include <variant>

#include "ruvia/http/HttpLimits.h"
#include "ruvia/http/HttpProtocolError.h"
#include "ruvia/http/detail/BorrowedView.h"
#include "ruvia/http/detail/HttpRequestBodyFailure.h"
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

    [[nodiscard]] HttpProtocolError protocolError() const noexcept {
        switch (error_) {
            case Http1ChunkDecodeError::kInvalidFraming:
                return HttpProtocolError(http_status::kBadRequest, "invalid chunked request body");
            case Http1ChunkDecodeError::kBodyTooLarge:
                return HttpRequestBodyFailure::tooLarge().protocolError();
            case Http1ChunkDecodeError::kFramingTooLarge:
                return HttpProtocolError(http_status::kContentTooLarge, "request body framing is too large");
        }
        return HttpProtocolError(http_status::kBadRequest, "invalid chunked request body");
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

    [[nodiscard]] const Http1ChunkDecodeNeedMore* needMore() const & noexcept {
        return std::get_if<Http1ChunkDecodeNeedMore>(&value_);
    }
    const Http1ChunkDecodeNeedMore* needMore() const && = delete;

    [[nodiscard]] const Http1ChunkDecodeBodyChunk* bodyChunk() const & noexcept {
        return std::get_if<Http1ChunkDecodeBodyChunk>(&value_);
    }
    const Http1ChunkDecodeBodyChunk* bodyChunk() const && = delete;

    [[nodiscard]] const Http1ChunkDecodeComplete* complete() const & noexcept {
        return std::get_if<Http1ChunkDecodeComplete>(&value_);
    }
    const Http1ChunkDecodeComplete* complete() const && = delete;

    [[nodiscard]] const Http1ChunkDecodeFailure* failure() const & noexcept {
        return std::get_if<Http1ChunkDecodeFailure>(&value_);
    }
    const Http1ChunkDecodeFailure* failure() const && = delete;

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
// Representation bytes use the configured body limit, while chunk-size lines,
// delimiters, and trailers share an independent fixed framing budget.
class Http1ChunkedBodyDecoder final {
public:
    explicit Http1ChunkedBodyDecoder(ProtocolByteLimit bodyLimit) noexcept
        : bodyLimit_(bodyLimit) {}

    [[nodiscard]] Http1ChunkDecodeResult decode(
        std::string_view available) {
        if (const auto* failure =
                std::get_if<Http1ChunkDecodeError>(&state_)) {
            return Http1ChunkDecodeResult::makeFailure(0, *failure);
        }
        std::size_t cursor = 0;
        for (;;) {
            switch (std::get<ProgressState>(state_)) {
                case ProgressState::kSizeLine: {
                    const auto lineEnd = available.find("\r\n", cursor);
                    if (lineEnd == std::string_view::npos) {
                        if (available.size() - cursor >= kMaxHttpHeaderBytes) {
                            return fail(
                                cursor,
                                Http1ChunkDecodeError::kFramingTooLarge);
                        }
                        return Http1ChunkDecodeResult::makeNeedMore(cursor);
                    }
                    if (lineEnd - cursor + 2 > kMaxHttpHeaderBytes) {
                        return fail(
                            cursor,
                            Http1ChunkDecodeError::kFramingTooLarge);
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
                        state_ = ProgressState::kTrailers;
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
                        state_ = ProgressState::kBody;
                    }
                    break;
                }
                case ProgressState::kBody: {
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
                    if (const auto error = consumeDelimiter(
                            available.substr(cursor))) {
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
                    state_ = ProgressState::kComplete;
                    return Http1ChunkDecodeResult::makeComplete(
                        cursor + trailerBytes);
                }
                case ProgressState::kComplete:
                    return Http1ChunkDecodeResult::makeComplete(cursor);
            }
        }
    }

    template <HttpTemporaryOwningCharString Input>
    Http1ChunkDecodeResult decode(Input&&) = delete;

private:
    enum class ProgressState : std::uint8_t {
        kSizeLine,
        kBody,
        kDelimiter,
        kTrailers,
        kComplete,
    };

    // Progress and terminal failure are mutually exclusive. Keeping the exact
    // failure in the state value makes repeated decode() calls stable without
    // a kFailed marker plus a separately combinable error side channel.
    using State = std::variant<ProgressState, Http1ChunkDecodeError>;

    [[nodiscard]] std::optional<Http1ChunkDecodeError> accountFraming(
        std::size_t bytes) noexcept {
        if (encodedOverheadBytes_ > kMaxHttpHeaderBytes ||
            bytes > kMaxHttpHeaderBytes - encodedOverheadBytes_) {
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
        state_ = error;
        return Http1ChunkDecodeResult::makeFailure(consumedBytes, error);
    }

    ProtocolByteLimit bodyLimit_;
    State state_{ProgressState::kSizeLine};
    std::size_t trailerSearchOffset_{0};
    std::size_t remaining_{0};
    std::size_t decodedBytes_{0};
    std::size_t encodedOverheadBytes_{0};
};

}  // namespace ruvia::detail
