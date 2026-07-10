#pragma once

namespace ruvia::detail {

template <typename Stream>
StreamBodyReader<Stream>::StreamBodyReader(
    Stream& stream,
    std::pmr::polymorphic_allocator<char> allocator,
    std::string_view initialBodyAndPipeline,
    std::size_t contentLength,
    bool chunked,
    HttpTransferCodings transferCodings,
    std::size_t maxBodyBytes,
    ConnectionScanner::Entry& scannerEntry,
    bool sendContinue)
    : stream_(stream),
      buffer_(allocator),
      transferDecoderAllocator_(allocator.resource()),
      initialBodyAndPipeline_(initialBodyAndPipeline),
      contentLength_(contentLength),
      chunked_(chunked),
      maxBodyBytes_(maxBodyBytes),
      chunkDecoder_(maxBodyBytes),
      scannerEntry_(scannerEntry),
      sendContinue_(sendContinue) {
    if (!transferCodings.empty()) {
        transferDecoder_ = transferDecoderAllocator_.allocate(1);
        try {
            std::construct_at(transferDecoder_, transferCodings, allocator, maxBodyBytes);
        } catch (...) {
            transferDecoderAllocator_.deallocate(transferDecoder_, 1);
            transferDecoder_ = nullptr;
            throw;
        }
    }
}

template <typename Stream>
StreamBodyReader<Stream>::~StreamBodyReader() {
    if (transferDecoder_ != nullptr) {
        std::destroy_at(transferDecoder_);
        transferDecoderAllocator_.deallocate(transferDecoder_, 1);
    }
}

template <typename Stream>
bool StreamBodyReader<Stream>::finished() const noexcept {
    return finished_ && (transferDecoder_ == nullptr || transferDecoder_->finished());
}

template <typename Stream>
void StreamBodyReader<Stream>::restorePipeline(std::pmr::string& readBuffer, std::size_t& usedBytes) {
    compactPending();
    restorePipelineBytes(readBuffer, usedBytes, initialPipelineRemainder(), bufferedPipelineRemainder());
    resetPipelineState();
    if (chunked_) {
        chunkDecoder_.resetDelimiter();
    }
}

template <typename Stream>
Task<std::optional<std::string_view>> StreamBodyReader<Stream>::read() {
    if (chunked_) {
        co_await ensureContinue();
        co_return co_await readTransferDecodedChunked();
    }
    if (exceedsLimit(contentLength_)) {
        throwRequestBodyTooLarge();
    }

    co_await ensureContinue();
    co_return co_await readContentLength();
}

template <typename Stream>
Task<std::string_view> StreamBodyReader<Stream>::readAll(std::pmr::string& body) {
    if (!chunked_) {
        co_return co_await readContentLengthAll(body);
    }

    co_await ensureContinue();
    while (auto chunk = co_await readChunked()) {
        if (transferDecoder_ != nullptr) {
            transferDecoder_->decodeAppend(*chunk, body);
        } else {
            if (maxBodyBytes_ != 0 && (chunk->size() > maxBodyBytes_ || body.size() > maxBodyBytes_ - chunk->size())) {
                throwRequestBodyTooLarge();
            }
            body.append(chunk->data(), chunk->size());
        }
    }
    if (transferDecoder_ != nullptr) {
        transferDecoder_->finish();
    }
    co_return std::string_view(body.data(), body.size());
}

template <typename Stream>
Task<void> StreamBodyReader<Stream>::ensureContinue() {
    if (sendContinue_ && !continueSent_) {
        if (!(co_await writeContinue(stream_))) {
            throw std::invalid_argument("failed to write 100 Continue");
        }
        continueSent_ = true;
    }
}

template <typename Stream>
Task<void> StreamBodyReader<Stream>::readMore() {
    compactPending();
    const auto oldSize = buffer_.size();
    const auto hardLimit = chunked_
        ? kChunkedEncodedBufferBytes
        : (maxBodyBytes_ == 0 ? (std::numeric_limits<std::size_t>::max)() : maxBodyBytes_);
    if (oldSize >= hardLimit) {
        throwRequestBodyTooLarge();
    }
    if (oldSize == buffer_.capacity()) {
        const auto nextCapacity = std::min<std::size_t>(
            std::max<std::size_t>(buffer_.capacity() * 2, oldSize + kBodyReadChunkBytes),
            hardLimit);
        buffer_.reserve(nextCapacity);
    }
    const auto writable = std::min<std::size_t>(
        kBodyReadChunkBytes,
        hardLimit - oldSize);
    resizePmrStringForOverwrite(buffer_, oldSize + writable);

    scannerEntry_.setPhase(ConnectionScanner::Phase::kReadingPayload);
    const auto [ec, bytesRead] = co_await asyncResult<std::size_t>(
        [this, oldSize, writable](auto handler) mutable {
            stream_.async_read_some(
                asio::buffer(buffer_.data() + oldSize, writable),
                std::move(handler));
        });
    if (ec || bytesRead == 0) {
        throw std::invalid_argument("incomplete request body");
    }

    buffer_.resize(oldSize + bytesRead);
    scannerEntry_.touch();
}

template <typename Stream>
bool StreamBodyReader<Stream>::exceedsLimit(std::size_t bytes) const noexcept {
    return maxBodyBytes_ != 0 && bytes > maxBodyBytes_;
}

template <typename Stream>
void StreamBodyReader<Stream>::markFinished() noexcept {
    finished_ = true;
    scannerEntry_.setPhase(ConnectionScanner::Phase::kIdle);
}

}  // namespace ruvia::detail
