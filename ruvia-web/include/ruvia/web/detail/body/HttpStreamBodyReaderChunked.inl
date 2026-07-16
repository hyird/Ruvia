#pragma once

namespace ruvia::detail {

template <typename Stream>
Task<std::optional<std::string_view>> StreamBodyReader<Stream>::readChunked() {
    compactPending();
    if (finished_) {
        co_return std::nullopt;
    }

    for (;;) {
        const auto source = !initialBodyAndPipeline_.empty()
            ? std::string_view(initialBodyAndPipeline_)
            : std::string_view(buffer_);
        const auto result = chunkDecoder_.decode(source.substr(readCursor_));
        if (result.consumedBytes() != 0) {
            pendingCompactUntil_ = readCursor_ + result.consumedBytes();
        }
        if (const auto* bodyChunk = result.bodyChunk()) {
            co_return bodyChunk->bytes();
        }
        if (result.complete() != nullptr) {
            compactPending();
            co_return std::nullopt;
        }
        if (const auto* failure = result.failure()) {
            throw failure->protocolError();
        }
        if (result.needMore() == nullptr) {
            throw std::logic_error("unexpected HTTP/1 chunk decode result");
        }
        compactPending();
        materializeInitialRemainder();
        co_await readMore();
    }
}

template <typename Stream>
Task<std::optional<std::string_view>> StreamBodyReader<Stream>::readTransferDecodedChunked() {
    if (transferDecoder_ == nullptr) {
        auto chunk = co_await readChunked();
        if (!chunk) {
            markFinished();
        }
        co_return chunk;
    }

    if (transferOutput_.empty()) {
        resizePmrStringForOverwrite(
            transferOutput_, kBodyReadChunkBytes);
    }

    // Keep the borrowed encoded chunk until the decoder reports its consumed
    // prefix. readChunked() is called only after that view is empty, so its
    // compaction cannot invalidate decoder input across application reads.
    for (;;) {
        const auto result = transferDecoder_->decode(
            transferInput_,
            std::span<char>(transferOutput_));
        transferInput_.remove_prefix(
            std::min(transferInput_.size(), result.consumedBytes()));
        if (const auto* output = result.output()) {
            co_return output->bytes();
        }
        if (const auto* failure = result.protocolFailure()) {
            throwTransferCodingProtocolFailure(*failure);
        }
        if (result.decoderFailure() != nullptr) {
            throwTransferCodingDecoderFailure();
        }
        if (result.complete() == nullptr &&
            result.needInput() == nullptr) {
            throw std::logic_error(
                "unexpected transfer-coding decode result");
        }

        auto chunk = co_await readChunked();
        if (!chunk) {
            requireCompleteTransferCoding(*transferDecoder_);
            markFinished();
            co_return std::nullopt;
        }
        transferInput_ = *chunk;
    }
}

}  // namespace ruvia::detail
