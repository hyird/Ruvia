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

        if (readingTrailers_) {
            if (source.size() >= readCursor_ + 2 && source.substr(readCursor_, 2) == "\r\n") {
                pendingCompactUntil_ = readCursor_ + 2;
                chunkDecoder_.consumeTrailers(2);
                readingTrailers_ = false;
                trailerSearchOffset_ = 0;
                markFinished();
                compactPending();
                co_return std::nullopt;
            }

            const auto searchOffset = std::max(readCursor_, trailerSearchOffset_);
            const auto trailerEnd = source.find("\r\n\r\n", searchOffset);
            if (trailerEnd == std::string_view::npos) {
                trailerSearchOffset_ = source.size() > 3
                    ? std::max(readCursor_, source.size() - 3)
                    : readCursor_;
                materializeInitialRemainder();
                co_await readMore();
                continue;
            }

            if (validateHttpChunkTrailers(source.substr(readCursor_, trailerEnd - readCursor_)) !=
                HttpChunkScanStatus::kComplete) {
                throw std::invalid_argument("invalid chunked request body");
            }

            pendingCompactUntil_ = trailerEnd + 4;
            chunkDecoder_.consumeTrailers(trailerEnd - readCursor_ + 4);
            readingTrailers_ = false;
            trailerSearchOffset_ = 0;
            markFinished();
            compactPending();
            co_return std::nullopt;
        }

        if (chunkDecoder_.awaitingDelimiter()) {
            const auto available = source.substr(readCursor_);
            switch (chunkDecoder_.checkDelimiter(available)) {
                case ChunkDelimiterStatus::kNeedMore:
                    materializeInitialRemainder();
                    co_await readMore();
                    continue;
                case ChunkDelimiterStatus::kInvalid:
                    throw std::invalid_argument("invalid chunked request body");
                case ChunkDelimiterStatus::kOk:
                    break;
            }

            pendingCompactUntil_ = readCursor_ + 2;
            chunkDecoder_.consumeDelimiter();
            compactPending();
            continue;
        }

        if (chunkDecoder_.remaining() > 0) {
            if (readCursor_ >= source.size()) {
                materializeInitialRemainder();
                co_await readMore();
                continue;
            }

            const auto availableBodyBytes = source.size() - readCursor_;
            const auto remaining = chunkDecoder_.remaining();
            if (remaining <= availableBodyBytes) {
                if (availableBodyBytes - remaining < 2) {
                    auto chunk = std::string_view(source.data() + readCursor_, remaining);
                    pendingCompactUntil_ = readCursor_ + remaining;
                    chunkDecoder_.consumeBodyBytes(remaining);
                    co_return chunk;
                }
                if (source.substr(readCursor_ + remaining, 2) != "\r\n") {
                    throw std::invalid_argument("invalid chunked request body");
                }

                auto chunk = std::string_view(source.data() + readCursor_, remaining);
                pendingCompactUntil_ = readCursor_ + remaining + 2;
                chunkDecoder_.consumeBodyBytes(remaining);
                chunkDecoder_.consumeDelimiter();
                co_return chunk;
            }

            auto chunk = std::string_view(source.data() + readCursor_, availableBodyBytes);
            chunkDecoder_.consumeBodyBytes(availableBodyBytes);
            pendingCompactUntil_ = source.size();
            co_return chunk;
        }

        const auto available = source.substr(readCursor_);
        const auto lineEnd = available.find("\r\n");
        if (lineEnd == std::string_view::npos) {
            materializeInitialRemainder();
            co_await readMore();
            continue;
        }

        std::size_t chunkSize = 0;
        const auto hasChunkBody = chunkDecoder_.parseSizeLine(available.substr(0, lineEnd), chunkSize);

        const auto chunkDataStart = readCursor_ + lineEnd + 2;
        if (!hasChunkBody) {
            readCursor_ = chunkDataStart;
            readingTrailers_ = true;
            trailerSearchOffset_ = readCursor_;
            continue;
        }

        if (chunkDataStart > source.size()) {
            throwRequestBodyTooLarge();
        }
        readCursor_ = chunkDataStart;
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
