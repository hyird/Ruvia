#pragma once

#include "body/HttpStreamBodyReaderErrors.h"

namespace ruvia::detail {

template <typename Stream>
Task<std::string_view> StreamBodyReader<Stream>::readKnownLengthAll(
    std::pmr::string& body, std::size_t contentLength) {
    compactPending();
    if (finished_) {
        co_return std::string_view(body);
    }
    if (exceedsLimit(contentLength)) {
        throwRequestBodyTooLarge();
    }

    const auto initialBodyBytes = std::min(contentLength, initialBodyAndPipeline_.size());
    if (initialBodyBytes == contentLength) {
        markFinished();
        co_return initialBodyAndPipeline_.substr(0, contentLength);
    }

    co_await ensureContinue();

    // Only bytes actually received grow the owning buffer. In particular, a
    // header-only peer cannot allocate its declared Content-Length up front.
    while (auto chunk = co_await readKnownLength(contentLength)) {
        body.append(::ruvia::asChars(*chunk));
    }

    co_return std::string_view(body);
}

template <typename Stream>
Task<std::optional<std::span<const std::byte>>> StreamBodyReader<Stream>::readKnownLength(
    std::size_t contentLength) {
    compactPending();
    if (finished_) {
        co_return std::nullopt;
    }
    if (exceedsLimit(contentLength)) {
        throwRequestBodyTooLarge();
    }
    if (contentLength == 0 || deliveredBytes_ == contentLength) {
        markFinished();
        co_return std::nullopt;
    }

    const auto initialBodyBytes = std::min(contentLength, initialBodyAndPipeline_.size());
    if (deliveredBytes_ < initialBodyBytes) {
        const auto remainingBody = contentLength - deliveredBytes_;
        const auto available = initialBodyBytes - deliveredBytes_;
        const auto chunkBytes = std::min(available, remainingBody);
        auto chunk = initialBodyAndPipeline_.substr(deliveredBytes_, chunkBytes);
        deliveredBytes_ += chunkBytes;
        if (deliveredBytes_ == contentLength) {
            markFinished();
        }
        co_return ::ruvia::asBytes(chunk);
    }

    // The initial segment is now fully consumed as body: a partial-body prefix
    // cannot be followed by pipelined bytes, so the whole borrowed view was
    // body. Drop it before recording any buffer_-relative pendingCompactUntil_,
    // so compactPending() compacts buffer_ instead of misreading that offset as
    // an initial-view offset (mirrors the chunked path's
    // materializeInitialRemainder()).
    if (initialBodyBytes == initialBodyAndPipeline_.size()) {
        initialBodyAndPipeline_ = {};
    }

    while (buffer_.size() <= readCursor_) {
        co_await readMore();
    }

    const auto remainingBody = contentLength - deliveredBytes_;
    const auto available = buffer_.size() - readCursor_;
    const auto chunkBytes = std::min(available, remainingBody);
    auto chunk = std::string_view(buffer_.data() + readCursor_, chunkBytes);
    pendingCompactUntil_ = readCursor_ + chunkBytes;
    deliveredBytes_ += chunkBytes;
    if (deliveredBytes_ == contentLength) {
        markFinished();
    }

    co_return ::ruvia::asBytes(chunk);
}

}  // namespace ruvia::detail
