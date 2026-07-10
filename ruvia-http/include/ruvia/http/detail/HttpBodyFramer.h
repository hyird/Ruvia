#pragma once

#include <algorithm>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <string_view>

#include "ruvia/http/detail/parser/HttpChunkParser.h"
#include "ruvia/http/HttpProtocolError.h"

namespace ruvia::detail {

enum class ChunkDelimiterStatus {
    kOk,
    kNeedMore,
    kInvalid
};

class HttpChunkDecoder final {
public:
    explicit HttpChunkDecoder(std::size_t maxBodyBytes) noexcept
        : maxBodyBytes_(maxBodyBytes) {}

    [[nodiscard]] std::size_t remaining() const noexcept {
        return remaining_;
    }

    [[nodiscard]] bool awaitingDelimiter() const noexcept {
        return awaitingDelimiter_;
    }

    [[nodiscard]] ChunkDelimiterStatus checkDelimiter(std::string_view available) const noexcept {
        if (available.size() < 2) {
            return ChunkDelimiterStatus::kNeedMore;
        }
        return available.substr(0, 2) == "\r\n"
            ? ChunkDelimiterStatus::kOk
            : ChunkDelimiterStatus::kInvalid;
    }

    [[nodiscard]] bool parseSizeLine(std::string_view line, std::size_t& chunkSize) {
        if (!parseHttpChunkSize(line, chunkSize)) {
            throw std::invalid_argument("invalid chunked request body");
        }
        consumeFramingBytes(line.size() + 2);
        if (chunkSize == 0) {
            return false;
        }
        if (exceedsLimit(chunkSize) ||
            (maxBodyBytes_ != 0 && decodedBytes_ > maxBodyBytes_ - chunkSize) ||
            chunkSize > (std::numeric_limits<std::size_t>::max)() - decodedBytes_) {
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
    [[nodiscard]] bool exceedsLimit(std::size_t bytes) const noexcept {
        return maxBodyBytes_ != 0 && bytes > maxBodyBytes_;
    }

    void consumeFramingBytes(std::size_t bytes) {
        if (maxBodyBytes_ == 0 || bytes == 0) {
            return;
        }
        if (bytes > maxBodyBytes_ || encodedOverheadBytes_ > maxBodyBytes_ - bytes) {
            throw HttpProtocolError(413, "request body framing is too large");
        }
        encodedOverheadBytes_ += bytes;
    }

    std::size_t maxBodyBytes_{0};
    std::size_t remaining_{0};
    std::size_t decodedBytes_{0};
    std::size_t encodedOverheadBytes_{0};
    bool awaitingDelimiter_{false};
};

enum class HttpChunkDecodeEventKind : std::uint8_t {
    kNeedMore,
    kBody,
    kComplete,
};

struct HttpChunkDecodeEvent final {
    HttpChunkDecodeEventKind kind{HttpChunkDecodeEventKind::kNeedMore};
    std::size_t consumedBytes{0};
    std::string_view body;
};

// Incremental sans-I/O decoder for HTTP/1 chunked content. The caller owns the
// input buffer and removes `consumedBytes` only after any returned body view is
// no longer needed. Protocol framing, trailer validation and size accounting
// stay here; a runtime driver only refills its buffer on kNeedMore.
class HttpChunkedBodyDecoder final {
public:
    explicit HttpChunkedBodyDecoder(std::size_t maxBodyBytes) noexcept
        : chunks_(maxBodyBytes) {}

    [[nodiscard]] HttpChunkDecodeEvent decode(std::string_view available) {
        std::size_t cursor = 0;
        for (;;) {
            switch (state_) {
                case State::kSizeLine: {
                    const auto lineEnd = available.find("\r\n", cursor);
                    if (lineEnd == std::string_view::npos) {
                        return needMore(cursor);
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
                        return needMore(cursor);
                    }
                    const auto bytes = std::min(chunks_.remaining(), available.size() - cursor);
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
                    return HttpChunkDecodeEvent{
                        .kind = HttpChunkDecodeEventKind::kBody,
                        .consumedBytes = cursor,
                        .body = body};
                }
                case State::kDelimiter:
                    if (available.size() - cursor < 2) {
                        return needMore(cursor);
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
                        return complete(cursor + 2);
                    }
                    const auto trailerEnd = trailers.find("\r\n\r\n", trailerSearchOffset_);
                    if (trailerEnd == std::string_view::npos) {
                        trailerSearchOffset_ = trailers.size() > 3 ? trailers.size() - 3 : 0;
                        return needMore(cursor);
                    }
                    if (validateHttpChunkTrailers(trailers.substr(0, trailerEnd)) !=
                        HttpChunkScanStatus::kComplete) {
                        throw std::invalid_argument("invalid chunked request body");
                    }
                    const auto trailerBytes = trailerEnd + 4;
                    chunks_.consumeTrailers(trailerBytes);
                    state_ = State::kComplete;
                    return complete(cursor + trailerBytes);
                }
                case State::kComplete:
                    return complete(cursor);
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

    [[nodiscard]] static HttpChunkDecodeEvent needMore(std::size_t consumed) noexcept {
        return HttpChunkDecodeEvent{
            .kind = HttpChunkDecodeEventKind::kNeedMore,
            .consumedBytes = consumed};
    }

    [[nodiscard]] static HttpChunkDecodeEvent complete(std::size_t consumed) noexcept {
        return HttpChunkDecodeEvent{
            .kind = HttpChunkDecodeEventKind::kComplete,
            .consumedBytes = consumed};
    }

    void consumeDelimiter(std::string_view available) {
        if (chunks_.checkDelimiter(available) != ChunkDelimiterStatus::kOk) {
            throw std::invalid_argument("invalid chunked request body");
        }
        chunks_.consumeDelimiter();
    }

    HttpChunkDecoder chunks_;
    State state_{State::kSizeLine};
    std::size_t trailerSearchOffset_{0};
};

}  // namespace ruvia::detail
