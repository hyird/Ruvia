#pragma once

namespace ruvia::detail {

template <typename Stream>
void StreamBodyReader<Stream>::compactPending() {
    if (pendingCompactUntil_ == 0) {
        return;
    }
    if (!initialBodyAndPipeline_.empty()) {
        readCursor_ = std::min(pendingCompactUntil_, initialBodyAndPipeline_.size());
        pendingCompactUntil_ = 0;
        if (readCursor_ == initialBodyAndPipeline_.size()) {
            initialBodyAndPipeline_ = {};
            readCursor_ = 0;
        }
        return;
    }
    if (pendingCompactUntil_ > buffer_.size()) {
        pendingCompactUntil_ = 0;
        readCursor_ = 0;
        return;
    }

    const auto removed = pendingCompactUntil_;
    const auto remaining = buffer_.size() - pendingCompactUntil_;
    if (remaining > 0) {
        std::memmove(buffer_.data(), buffer_.data() + pendingCompactUntil_, remaining);
    }
    buffer_.resize(buffer_.size() - removed);
    pendingCompactUntil_ = 0;
    readCursor_ = 0;
}

template <typename Stream>
std::string_view StreamBodyReader<Stream>::initialPipelineRemainder() const noexcept {
    if (initialBodyAndPipeline_.empty()) {
        return {};
    }
    if (bodyPlan_.chunked() != nullptr) {
        return initialBodyAndPipeline_.substr(
            std::min(readCursor_, initialBodyAndPipeline_.size()));
    }
    const auto* knownLength = bodyPlan_.knownLength();
    if (knownLength == nullptr) {
        return initialBodyAndPipeline_;
    }
    const auto initialBodyBytes =
        std::min(knownLength->contentLength(), initialBodyAndPipeline_.size());
    return initialBodyAndPipeline_.substr(initialBodyBytes);
}

template <typename Stream>
std::string_view StreamBodyReader<Stream>::bufferedPipelineRemainder() const noexcept {
    if (readCursor_ >= buffer_.size()) {
        return {};
    }
    return std::string_view(buffer_.data() + readCursor_, buffer_.size() - readCursor_);
}

template <typename Stream>
void StreamBodyReader<Stream>::resetPipelineState() noexcept {
    buffer_.clear();
    initialBodyAndPipeline_ = {};
    readCursor_ = 0;
    pendingCompactUntil_ = 0;
}

template <typename Stream>
void StreamBodyReader<Stream>::materializeInitialRemainder() {
    if (initialBodyAndPipeline_.empty()) {
        return;
    }
    buffer_.assign(
        initialBodyAndPipeline_.data() + readCursor_, initialBodyAndPipeline_.size() - readCursor_);
    initialBodyAndPipeline_ = {};
    readCursor_ = 0;
    pendingCompactUntil_ = 0;
}

}  // namespace ruvia::detail
