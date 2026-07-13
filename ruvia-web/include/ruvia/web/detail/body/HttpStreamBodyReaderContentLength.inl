#pragma once

namespace ruvia::detail {

template <typename Stream>
Task<std::string_view> StreamBodyReader<Stream>::readKnownLengthAll(
    std::pmr::string& body,
    std::size_t contentLength) {
    compactPending();
    if (finished_) {
        co_return std::string_view(body.data(), body.size());
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

    resizePmrStringForOverwrite(body, contentLength);
    if (initialBodyBytes > 0) {
        std::memcpy(body.data(), initialBodyAndPipeline_.data(), initialBodyBytes);
    }

    std::size_t offset = initialBodyBytes;
    while (offset < contentLength) {
        scannerEntry_.setPhase(ConnectionScanner::Phase::kReadingPayload);
        const auto [ec, bytesRead] = co_await asyncResult<std::size_t>(
            [this, &body, offset](auto handler) mutable {
                stream_.async_read_some(
                    asio::buffer(body.data() + offset, body.size() - offset),
                    std::move(handler));
            });
        if (ec || bytesRead == 0) {
            throwIncompleteRequestBody();
        }
        offset += bytesRead;
        scannerEntry_.touch();
    }

    deliveredBytes_ = contentLength;
    markFinished();
    co_return std::string_view(body.data(), body.size());
}

template <typename Stream>
Task<std::optional<std::string_view>> StreamBodyReader<Stream>::readKnownLength(
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
        co_return chunk;
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

    co_return chunk;
}

}  // namespace ruvia::detail
