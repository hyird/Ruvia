#pragma once

namespace ruvia::detail {

[[noreturn]] inline void throwRequestBodyTooLarge() {
    throw HttpRequestBodyFailure::tooLarge().protocolError();
}

[[noreturn]] inline void throwIncompleteRequestBody() {
    throw HttpRequestBodyFailure::incomplete().protocolError();
}

[[noreturn]] inline void throwTransferCodingProtocolFailure(
    const TransferCodingDecodeProtocolFailure& failure) {
    throw failure.protocolError();
}

[[noreturn]] inline void throwTransferCodingDecoderFailure() {
    throw std::runtime_error("transfer-coding decoder failure");
}

inline void requireCompleteTransferCoding(
    TransferCodingDecoder& decoder) {
    const auto finishResult = decoder.finishInput();
    if (finishResult.complete() != nullptr) {
        return;
    }
    if (const auto* failure = finishResult.protocolFailure()) {
        throwTransferCodingProtocolFailure(*failure);
    }
    if (finishResult.decoderFailure() != nullptr) {
        throwTransferCodingDecoderFailure();
    }
    throw std::logic_error("unexpected transfer-coding finish result");
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
      transferDecoder_(
          nullptr,
          PmrObjectDeleter<TransferCodingDecoder>{allocator.resource()}),
      initialBodyAndPipeline_(initialBodyAndPipeline),
      bodyPlan_(bodyPlan),
      bodyLimit_(bodyLimit),
      chunkDecoder_(bodyLimit),
      scannerEntry_(scannerEntry),
      finished_(!bodyPlan_.requiresConsumption()) {
    const auto* chunked = bodyPlan_.chunked();
    if (chunked != nullptr && !chunked->transferCodings().empty()) {
        transferDecoder_ = makePmrObject<TransferCodingDecoder>(
            allocator.resource(),
            chunked->transferCodings().values[0],
            allocator.resource(),
            bodyLimit);
    }
}

template <typename Stream>
Http1RequestBodyConsumption StreamBodyReader<Stream>::consumption() const noexcept {
    return finished_
        ? Http1RequestBodyConsumption::kComplete
        : Http1RequestBodyConsumption::kIncomplete;
}

template <typename Stream>
void StreamBodyReader<Stream>::takePipeline(std::pmr::string& stash) {
    compactPending();
    // The remainder is split across at most two places: the bytes still sitting
    // in the connection read buffer behind the body, and the bytes this reader
    // over-read from the socket into its own buffer. Neither aliases `stash`.
    const auto initialPipeline = initialPipelineRemainder();
    const auto bufferedPipeline = bufferedPipelineRemainder();
    stash.clear();
    stash.reserve(initialPipeline.size() + bufferedPipeline.size());
    stash.append(initialPipeline);
    stash.append(bufferedPipeline);
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
        co_return std::string_view(body);
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
    }
    markFinished();
    co_return std::string_view(body);
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
        if (const auto* failure = result.protocolFailure()) {
            throwTransferCodingProtocolFailure(*failure);
        }
        if (result.decoderFailure() != nullptr) {
            throwTransferCodingDecoderFailure();
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
    const auto expectationPlan = bodyPlan_.expectationPlan(
        HttpUnsupportedExpectationPolicy::kReject);
    if (expectationPlan.sendContinue() != nullptr && !continueSent_) {
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
    auto readCompletion = co_await asyncAsio<std::size_t>(
        [this, oldSize, writable](auto handler) mutable {
            stream_.async_read_some(
                asio::buffer(buffer_.data() + oldSize, writable),
                std::move(handler));
        });
    const auto ec = readCompletion.errorCode();
    const auto bytesRead = readCompletion.result();
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
