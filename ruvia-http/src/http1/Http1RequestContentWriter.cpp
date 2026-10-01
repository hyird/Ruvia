#include "ruvia/http/Http1RequestContentWriter.h"

#include <algorithm>
#include <cstring>
#include <limits>

#include "ruvia/http/Http1ChunkedFraming.h"
#include "ruvia/http/HttpLimits.h"
#include "ruvia/http/detail/field/HttpTrailerFields.h"

namespace ruvia {
using Error = Http1RequestContentWriteError;
Http1RequestContentWriter::Http1RequestContentWriter(const Http1ClientStreamingRequestContent& plan) noexcept
    : length_(plan.contentLength()),
      gated_(plan.continueGated()) {}
void Http1RequestContentWriter::releaseContent() noexcept {
    gated_ = false;
}
void Http1RequestContentWriter::abort() noexcept {
    stopped_ = true;
    writing_ = false;
    finishing_ = false;
}
std::expected<Http1RequestContentWriter::Chunk, Error> Http1RequestContentWriter::planChunk(std::span<const char> payload) noexcept {
    if (stopped_ || finished_) {
        return std::unexpected(Error::kStopped);
    }
    if (gated_) {
        return std::unexpected(Error::kAwaitingContinue);
    }
    if (writing_ || finishing_) {
        return std::unexpected(Error::kWritePending);
    }
    if (payload.size() > std::numeric_limits<std::uint64_t>::max() - committed_) {
        return std::unexpected(Error::kLengthOverflow);
    }
    if (length_ && payload.size() > *length_ - committed_) {
        return std::unexpected(Error::kLengthMismatch);
    }
    Chunk chunk{.payload = payload};
    if (!length_ && !payload.empty()) {
        Http1ChunkHeader header(payload.size());
        auto prefix = header.view();
        std::copy(prefix.begin(), prefix.end(), chunk.prefix.begin());
        chunk.prefixSize = prefix.size();
        chunk.suffix = kHttp1ChunkDataTerminator;
    }
    writing_ = true;
    pending_ = payload.size();
    return chunk;
}
std::expected<void, Error> Http1RequestContentWriter::commitChunk(std::size_t payloadBytes) noexcept {
    if (stopped_ || finished_) {
        return std::unexpected(Error::kStopped);
    }
    if (!writing_) {
        return std::unexpected(Error::kNoWritePending);
    }
    if (payloadBytes != pending_) {
        return std::unexpected(Error::kCommitMismatch);
    }
    committed_ += pending_;
    pending_ = 0;
    writing_ = false;
    return {};
}
std::expected<std::string_view, Error> Http1RequestContentWriter::planFinish(std::span<char> buffer, std::span<const HttpHeaderView> trailers) noexcept {
    if (stopped_ || finished_) {
        return std::unexpected(Error::kStopped);
    }
    if (gated_) {
        return std::unexpected(Error::kAwaitingContinue);
    }
    if (writing_ || finishing_) {
        return std::unexpected(Error::kWritePending);
    }
    if (length_ && committed_ != *length_) {
        return std::unexpected(Error::kLengthMismatch);
    }
    if (length_ && !trailers.empty()) {
        return std::unexpected(Error::kTrailersRequireChunked);
    }
    if (trailers.size() > kMaxHttpHeaderFields) {
        return std::unexpected(Error::kTrailerLimit);
    }
    std::size_t required = length_ ? 0 : 5;
    for (const auto& field : trailers) {
        if (!isValidHttpHeaderName(field.name()) || !isValidHttpHeaderValue(field.value()) || detail::isForbiddenHttpRequestTrailerName(field.name())) {
            return std::unexpected(Error::kInvalidTrailer);
        }
        if (required > kMaxHttpHeaderBytes || field.name().size() > kMaxHttpHeaderBytes - required ||
            field.value().size() > kMaxHttpHeaderBytes - required - field.name().size() ||
            kMaxHttpHeaderBytes - required - field.name().size() - field.value().size() < 4) {
            return std::unexpected(Error::kTrailerLimit);
        }
        required += field.name().size() + field.value().size() + 4;
    }
    if (buffer.size() < required) {
        return std::unexpected(Error::kOutputTooSmall);
    }
    char* cursor = buffer.data();
    auto append = [&](std::string_view value) {if (!value.empty()){std::memcpy(cursor,value.data(),value.size());cursor+=value.size();} };
    if (!length_) {
        append(kHttp1LastChunkPrefix);
        for (const auto& field : trailers) {
            append(field.name());
            append(": ");
            append(field.value());
            append("\r\n");
        }
        append(kHttp1TrailerSectionTerminator);
    }
    finishing_ = true;
    return required ? std::string_view(buffer.data(), required) : std::string_view{};
}
std::expected<void, Error> Http1RequestContentWriter::commitFinish() noexcept {
    if (stopped_ || finished_) {
        return std::unexpected(Error::kStopped);
    }
    if (!finishing_) {
        return std::unexpected(Error::kNoWritePending);
    }
    finishing_ = false;
    finished_ = true;
    return {};
}
}  // namespace ruvia
