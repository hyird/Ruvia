#pragma once

namespace ruvia::detail {

[[noreturn]] inline void throwRequestBodyTooLarge() {
    throw HttpProtocolError(413, "request body is too large");
}

[[noreturn]] inline void throwIncompleteRequestBody() {
    throw HttpProtocolError(400, "incomplete request body");
}

[[noreturn]] inline void throwTransferCodingDecodeFailure(
    TransferCodingDecodeError error) {
    switch (error) {
        case TransferCodingDecodeError::kInvalidContent:
            throw HttpProtocolError(400, "invalid transfer-coding body");
        case TransferCodingDecodeError::kDecodedSizeExceeded:
            throwRequestBodyTooLarge();
        case TransferCodingDecodeError::kDecoderFailure:
            throw std::runtime_error("transfer-coding decoder failure");
    }
    throw std::runtime_error("transfer-coding decoder failure");
}

inline void requireCompleteTransferCoding(
    TransferCodingDecoder& decoder) {
    if (decoder.finishInput() != TransferCodingFinishStatus::kComplete) {
        throw HttpProtocolError(400, "incomplete transfer-coding body");
    }
}

template <typename Stream>
StreamBodyReader<Stream>::StreamBodyReader(
    Stream& stream,
    std::pmr::polymorphic_allocator<char> allocator,
    std::string_view initialBodyAndPipeline,
    Http1RequestBodyPlan bodyPlan,
    ProtocolByteLimit bodyLimit,
    ConnectionScanner::Entry& scannerEntry)
    : stream_(stream),
      buffer_(allocator),
      transferOutput_(allocator),
      transferDecoderAllocator_(allocator.resource()),
      initialBodyAndPipeline_(initialBodyAndPipeline),
      bodyPlan_(bodyPlan),
      bodyLimit_(bodyLimit),
      chunkDecoder_(bodyLimit),
      scannerEntry_(scannerEntry),
      finished_(!bodyPlan_.requiresConsumption()) {
    const auto* chunked = bodyPlan_.chunked();
    if (chunked != nullptr && !chunked->transferCodings().empty()) {
        transferDecoder_ = transferDecoderAllocator_.allocate(1);
        try {
            std::construct_at(
                transferDecoder_,
                chunked->transferCodings().values[0],
                allocator.resource(),
                bodyLimit);
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
Http1RequestBodyConsumption StreamBodyReader<Stream>::consumption() const noexcept {
    return finished_ && (transferDecoder_ == nullptr || transferFinished_)
        ? Http1RequestBodyConsumption::kComplete
        : Http1RequestBodyConsumption::kIncomplete;
}

template <typename Stream>
void StreamBodyReader<Stream>::restorePipeline(std::pmr::string& readBuffer, std::size_t& usedBytes) {
    compactPending();
    restorePipelineBytes(readBuffer, usedBytes, initialPipelineRemainder(), bufferedPipelineRemainder());
    resetPipelineState();
}

template <typename Stream>
Task<std::optional<std::string_view>> StreamBodyReader<Stream>::read() {
    if (bodyPlan_.chunked() != nullptr) {
        co_await ensureContinue();
        co_return co_await readTransferDecodedChunked();
    }
    const auto* knownLength = bodyPlan_.knownLength();
    if (knownLength == nullptr) {
        co_return std::nullopt;
    }
    if (exceedsLimit(knownLength->contentLength())) {
        throwRequestBodyTooLarge();
    }

    co_await ensureContinue();
    co_return co_await readKnownLength(knownLength->contentLength());
}

template <typename Stream>
Task<std::string_view> StreamBodyReader<Stream>::readAll(std::pmr::string& body) {
    if (const auto* knownLength = bodyPlan_.knownLength()) {
        co_return co_await readKnownLengthAll(
            body,
            knownLength->contentLength());
    }
    if (bodyPlan_.withoutBody() != nullptr) {
        co_return std::string_view(body.data(), body.size());
    }

    co_await ensureContinue();
    while (auto chunk = co_await readChunked()) {
        if (transferDecoder_ != nullptr) {
            decodeTransferAppend(*chunk, body);
        } else {
            if (bodyLimit_.additionExceeds(body.size(), chunk->size())) {
                throwRequestBodyTooLarge();
            }
            body.append(chunk->data(), chunk->size());
        }
    }
    if (transferDecoder_ != nullptr) {
        requireCompleteTransferCoding(*transferDecoder_);
        transferFinished_ = true;
    }
    co_return std::string_view(body.data(), body.size());
}

template <typename Stream>
void StreamBodyReader<Stream>::decodeTransferAppend(
    std::string_view input,
    std::pmr::string& target) {
    for (;;) {
        const auto oldSize = target.size();
        resizePmrStringForOverwrite(
            target, oldSize + kBodyReadChunkBytes);
        const auto result = transferDecoder_->decode(
            input,
            std::span<char>(
                target.data() + oldSize,
                kBodyReadChunkBytes));
        input.remove_prefix(std::min(input.size(), result.consumedBytes()));
        if (const auto* output = result.output()) {
            target.resize(oldSize + output->bytes().size());
            continue;
        }
        target.resize(oldSize);
        if (const auto* failure = result.failure()) {
            throwTransferCodingDecodeFailure(failure->error());
        }
        if (result.complete() != nullptr) {
            return;
        }
        if (result.needInput() != nullptr) {
            return;
        }
        throw std::logic_error("unexpected transfer-coding decode result");
    }
}

template <typename Stream>
Task<void> StreamBodyReader<Stream>::ensureContinue() {
    if (bodyPlan_.expectationAction() ==
            HttpServerExpectationAction::kSend100Continue &&
        !continueSent_) {
        co_await writeHttp1Continue(stream_);
        continueSent_ = true;
    }
}

template <typename Stream>
Task<void> StreamBodyReader<Stream>::readMore() {
    compactPending();
    const auto oldSize = buffer_.size();
    const auto hardLimit = bodyPlan_.chunked() != nullptr
        ? kChunkedEncodedBufferBytes
        : bodyLimit_.readCeiling();
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
        throwIncompleteRequestBody();
    }

    buffer_.resize(oldSize + bytesRead);
    scannerEntry_.touch();
}

template <typename Stream>
bool StreamBodyReader<Stream>::exceedsLimit(std::size_t bytes) const noexcept {
    return bodyLimit_.exceeds(bytes);
}

template <typename Stream>
void StreamBodyReader<Stream>::markFinished() noexcept {
    finished_ = true;
    scannerEntry_.setPhase(ConnectionScanner::Phase::kIdle);
}

}  // namespace ruvia::detail
