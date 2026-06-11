#pragma once

#include <cstddef>
#include <limits>
#include <string_view>

#include "ruvia/http/Error.h"
#include "ruvia/http/HttpParser.h"

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

    void resetDelimiter() noexcept {
        awaitingDelimiter_ = false;
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
            throw HttpError(413, "payload_too_large", "request body is too large");
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
            throw HttpError(413, "payload_too_large", "request body framing is too large");
        }
        encodedOverheadBytes_ += bytes;
    }

    std::size_t maxBodyBytes_{0};
    std::size_t remaining_{0};
    std::size_t decodedBytes_{0};
    std::size_t encodedOverheadBytes_{0};
    bool awaitingDelimiter_{false};
};

}  // namespace ruvia::detail
