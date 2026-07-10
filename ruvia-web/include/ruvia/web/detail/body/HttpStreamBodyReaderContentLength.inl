#pragma once

namespace ruvia::detail {

template <typename Stream>
Task<std::string_view> StreamBodyReader<Stream>::readContentLengthAll(std::pmr::string& body) {
    if (chunked_) {
        throw std::logic_error("chunked request body cannot use Content-Length reader");
    }
    if (transferDecoder_ != nullptr) {
        throw std::logic_error("transfer-coded request body cannot use Content-Length reader");
    }

    compactPending();
    if (finished_) {
        co_return std::string_view(body.data(), body.size());
    }
    if (exceedsLimit(contentLength_)) {
        throwRequestBodyTooLarge();
    }

    const auto initialBodyBytes = std::min(contentLength_, initialBodyAndPipeline_.size());
    if (initialBodyBytes == contentLength_) {
        markFinished();
        co_return initialBodyAndPipeline_.substr(0, contentLength_);
    }

    co_await ensureContinue();

    resizePmrStringForOverwrite(body, contentLength_);
    if (initialBodyBytes > 0) {
        std::memcpy(body.data(), initialBodyAndPipeline_.data(), initialBodyBytes);
    }

    std::size_t offset = initialBodyBytes;
    while (offset < contentLength_) {
        scannerEntry_.setPhase(ConnectionScanner::Phase::kReadingPayload);
        const auto [ec, bytesRead] = co_await asyncResult<std::size_t>(
            [this, &body, offset](auto handler) mutable {
                stream_.async_read_some(
                    asio::buffer(body.data() + offset, body.size() - offset),
                    std::move(handler));
            });
        if (ec || bytesRead == 0) {
            throw std::invalid_argument("incomplete request body");
        }
        offset += bytesRead;
        scannerEntry_.touch();
    }

    deliveredBytes_ = contentLength_;
    markFinished();
    co_return std::string_view(body.data(), body.size());
}

template <typename Stream>
Task<std::optional<std::string_view>> StreamBodyReader<Stream>::readContentLength() {
    compactPending();
    if (finished_) {
        co_return std::nullopt;
    }
    if (exceedsLimit(contentLength_)) {
        throwRequestBodyTooLarge();
    }
    if (contentLength_ == 0 || deliveredBytes_ == contentLength_) {
        markFinished();
        co_return std::nullopt;
    }

    const auto initialBodyBytes = std::min(contentLength_, initialBodyAndPipeline_.size());
    if (deliveredBytes_ < initialBodyBytes) {
        const auto remainingBody = contentLength_ - deliveredBytes_;
        const auto available = initialBodyBytes - deliveredBytes_;
        const auto chunkBytes = std::min(available, remainingBody);
        auto chunk = initialBodyAndPipeline_.substr(deliveredBytes_, chunkBytes);
        deliveredBytes_ += chunkBytes;
        if (deliveredBytes_ == contentLength_) {
            markFinished();
        }
        co_return chunk;
    }

    while (buffer_.size() <= readCursor_) {
        co_await readMore();
    }

    const auto remainingBody = contentLength_ - deliveredBytes_;
    const auto available = buffer_.size() - readCursor_;
    const auto chunkBytes = std::min(available, remainingBody);
    auto chunk = std::string_view(buffer_.data() + readCursor_, chunkBytes);
    pendingCompactUntil_ = readCursor_ + chunkBytes;
    deliveredBytes_ += chunkBytes;
    if (deliveredBytes_ == contentLength_) {
        markFinished();
    }

    co_return chunk;
}

}  // namespace ruvia::detail
