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
            ? std::string_view(initialBodyAndPipeline_.data(), initialBodyAndPipeline_.size())
            : std::string_view(buffer_.data(), buffer_.size());
        const auto result = chunkDecoder_.decode(source.substr(readCursor_));
        if (result.consumedBytes() != 0) {
            pendingCompactUntil_ = readCursor_ + result.consumedBytes();
        }
        if (const auto* bodyChunk = result.bodyChunk()) {
            co_return bodyChunk->bytes();
        }
        if (result.complete() != nullptr) {
            markFinished();
            compactPending();
            co_return std::nullopt;
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
        co_return co_await readChunked();
    }

    // Drain pending decoder output before pulling more encoded bytes:
    // readChunked() compacts the buffer the decoder input view points at,
    // and a single encoded chunk may inflate into many output windows.
    for (;;) {
        const auto decoded = transferDecoder_->produce();
        if (!decoded.empty()) {
            co_return decoded;
        }
        auto chunk = co_await readChunked();
        if (!chunk) {
            transferDecoder_->finish();
            co_return std::nullopt;
        }
        transferDecoder_->setInput(*chunk);
    }
}

}  // namespace ruvia::detail
