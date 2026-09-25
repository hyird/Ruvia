#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory_resource>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "ruvia/http/Http2Types.h"
#include "ruvia/http/detail/http2/frame/Http2FrameCodec.h"

namespace ruvia::detail {

using ruvia::Http2DataOutputObserver;
using ruvia::Http2OutputBatchResult;
using ruvia::Http2OutputBatchStatus;
using ruvia::Http2OutputConsumeStatus;

// Sole owner of HTTP/2 outbound bytes and their consumed prefix. Connection logic
// selects protocol actions; this component owns contiguous frame serialization and
// buffer lifetime so frame handlers cannot manipulate storage cursors directly.
class Http2OutputBuffer final {
public:
    struct Segment final {
        std::size_t begin;
        std::size_t end;
        std::size_t payloadBegin;
        std::size_t payloadEnd;
        std::uint32_t streamId;
        bool isData;
    };
    explicit Http2OutputBuffer(std::pmr::memory_resource* resource)
        : bytes_(std::string_view{}, resource) {}

    [[nodiscard]] std::string_view pending() const& noexcept {
        return std::string_view(bytes_).substr(consumed_);
    }
    [[nodiscard]] std::string_view pending() const&& = delete;

    [[nodiscard]] bool wantsWrite() const noexcept {
        return consumed_ < bytes_.size();
    }

    // Internal transaction checkpoint. The checkpoint includes already-consumed
    // bytes, so a caller can remove only the frames appended after it without
    // disturbing a transport's pending cursor.
    [[nodiscard]] std::size_t checkpoint() const noexcept {
        return baseOffset_ + bytes_.size();
    }

    void rollbackTo(std::size_t checkpoint) noexcept {
        if (checkpoint < baseOffset_ + consumed_ || checkpoint > baseOffset_ + bytes_.size()) {
            std::terminate();
        }
        const auto physicalCheckpoint = checkpoint - baseOffset_;
        bytes_.resize(physicalCheckpoint);
        while (!segments_.empty() && segments_.back().begin >= physicalCheckpoint) {
            segments_.pop_back();
        }
        if (!segments_.empty() && segments_.back().end > physicalCheckpoint) {
            std::terminate();
        }
    }

    [[nodiscard]] Http2OutputConsumeStatus consume(std::size_t bytes) noexcept {
        const auto remaining = bytes_.size() - consumed_;
        if (bytes > remaining) {
            return Http2OutputConsumeStatus::kOutOfRange;
        }
        if (bytes < remaining) {
            consumed_ += bytes;
            while (segmentOffset_ < segments_.size() &&
                   segments_[segmentOffset_].end <= consumed_) {
                ++segmentOffset_;
            }
            compactConsumedPrefix();
            return Http2OutputConsumeStatus::kPending;
        }
        const auto logicalEnd = baseOffset_ + bytes_.size();
        bytes_.clear();
        segments_.clear();
        segmentOffset_ = 0;
        baseOffset_ = logicalEnd;
        consumed_ = 0;
        return Http2OutputConsumeStatus::kDrained;
    }

    // Moves every pending byte into `into`. With matching allocators and no
    // consumed prefix this swaps storage, retaining the caller's old capacity for
    // future frames; otherwise only the pending suffix is copied.
    void take(std::pmr::string& into);

    void appendBytes(std::string_view bytes) {
        if (!bytes.empty()) {
            reserveSegment();
            const auto begin = bytes_.size();
            bytes_.append(bytes.data(), bytes.size());
            segments_.push_back(Segment{begin, bytes_.size(), begin, bytes_.size(), 0, false});
        }
    }

    // Reserve storage for a whole sequence before its first frame is emitted.
    // Callers that need multi-frame wire atomicity use this once, while
    // appendFrame() applies the same guarantee to an individual frame.
    void reserveSegmentsAdditional(std::size_t additional) {
        if (additional > segments_.max_size() - segments_.size()) {
            throw std::length_error("HTTP/2 output segment count overflow");
        }
        if (segmentOffset_ >= 32 && segmentOffset_ >= segments_.size() - segmentOffset_) {
            const auto remaining = segments_.size() - segmentOffset_;
            std::move(segments_.begin() + static_cast<std::ptrdiff_t>(segmentOffset_),
                segments_.end(), segments_.begin());
            segments_.resize(remaining);
            segmentOffset_ = 0;
        }
        const auto required = segments_.size() + additional;
        if (required > segments_.capacity()) {
            segments_.reserve(required);
        }
    }

    void reserveAdditional(std::size_t additional) {
        if (additional > bytes_.max_size() - bytes_.size()) {
            throw std::length_error("HTTP/2 output buffer size overflow");
        }
        bytes_.reserve(bytes_.size() + additional);
    }

    void appendFrame(Http2FrameType type, std::uint8_t flags, std::uint32_t streamId,
        std::string_view first, std::string_view second = {}) {
        if (first.size() > kHttp2MaxFrameSizeLimit ||
            second.size() > kHttp2MaxFrameSizeLimit - first.size()) {
            std::terminate();
        }

        std::array<char, kHttp2FrameHeaderBytes> header;
        http2EncodeFrameHeader(header.data(),
            static_cast<std::uint32_t>(first.size() + second.size()), type, flags, streamId);
        reserveSegment();
        const auto begin = bytes_.size();
        // A frame is the smallest wire-level transaction. Reserve the complete
        // frame before appending any part so a throwing PMR resource cannot leave
        // a header or prefix without its payload in pendingOutput().
        reserveAdditional(kHttp2FrameHeaderBytes + first.size() + second.size());
        appendRaw(std::string_view(header.data(), header.size()));
        appendRaw(first);
        appendRaw(second);
        segments_.push_back(Segment{begin, bytes_.size(), begin + kHttp2FrameHeaderBytes,
            bytes_.size(), streamId, type == Http2FrameType::kData});
    }
    void appendGoawayFrame(
        std::uint32_t lastStreamId, Http2ErrorCode error, std::string_view debug = {});
    void appendRstStream(std::uint32_t streamId, Http2ErrorCode error);

    [[nodiscard]] Http2OutputBatchResult takeBatch(std::size_t maxBytes, std::pmr::string& into,
        Http2DataOutputObserver observer, void* observerContext) {
        if (consumed_ == bytes_.size()) {
            return {Http2OutputBatchStatus::kEmpty, 0};
        }
        if (segmentOffset_ == segments_.size() || segments_[segmentOffset_].begin != consumed_) {
            return {Http2OutputBatchStatus::kUnaligned, 0};
        }
        std::size_t end = consumed_;
        std::size_t segmentCount = 0;
        for (std::size_t index = segmentOffset_; index < segments_.size(); ++index) {
            const auto& segment = segments_[index];
            if (segment.begin != end) {
                return {Http2OutputBatchStatus::kUnaligned, 0};
            }
            if (segmentCount != 0 && segment.end - consumed_ > maxBytes) {
                break;
            }
            end = segment.end;
            ++segmentCount;
            if (end - consumed_ >= maxBytes) {
                break;
            }
        }
        if (segmentCount == 0) {
            return {Http2OutputBatchStatus::kUnaligned, 0};
        }
        const auto batchBytes = end - consumed_;
        into.append(bytes_.data() + consumed_, batchBytes);
        if (observer != nullptr) {
            for (std::size_t i = 0; i < segmentCount; ++i) {
                const auto& segment = segments_[segmentOffset_ + i];
                if (segment.isData) {
                    observer(observerContext, segment.streamId,
                        segment.payloadEnd - segment.payloadBegin);
                }
            }
        }
        consumeBatch(batchBytes);
        return {Http2OutputBatchStatus::kTaken, batchBytes};
    }

    [[nodiscard]] std::size_t pendingDataBytes(std::uint32_t streamId) const noexcept {
        std::size_t total = 0;
        for (std::size_t index = segmentOffset_; index < segments_.size(); ++index) {
            const auto& segment = segments_[index];
            if (!segment.isData || segment.streamId != streamId || consumed_ >= segment.payloadEnd) {
                continue;
            }
            const auto begin = std::max(consumed_, segment.payloadBegin);
            total += segment.payloadEnd - begin;
        }
        return total;
    }

    void consumeBatch(std::size_t bytes) noexcept {
        const auto status = consume(bytes);
        if (status == Http2OutputConsumeStatus::kOutOfRange) {
            std::terminate();
        }
    }

private:
    void appendRaw(std::string_view bytes) {
        if (!bytes.empty()) {
            bytes_.append(bytes.data(), bytes.size());
        }
    }

    void compactConsumedPrefix() noexcept {
        if (consumed_ < 64 * 1024 || consumed_ < bytes_.size() - consumed_ ||
            (segmentOffset_ < segments_.size() && segments_[segmentOffset_].begin != consumed_)) {
            return;
        }
        const auto prefix = consumed_;
        bytes_.erase(0, prefix);
        for (std::size_t index = segmentOffset_; index < segments_.size(); ++index) {
            auto& segment = segments_[index];
            segment.begin = segment.begin > prefix ? segment.begin - prefix : 0;
            segment.end -= prefix;
            segment.payloadBegin =
                segment.payloadBegin > prefix ? segment.payloadBegin - prefix : 0;
            segment.payloadEnd -= prefix;
        }
        segments_.erase(segments_.begin(),
            segments_.begin() + static_cast<std::ptrdiff_t>(segmentOffset_));
        baseOffset_ += prefix;
        segmentOffset_ = 0;
        consumed_ = 0;
    }

    void reserveSegment() {
        reserveSegmentsAdditional(1);
    }

    std::pmr::string bytes_;
    std::pmr::vector<Segment> segments_{bytes_.get_allocator().resource()};
    std::size_t segmentOffset_{0};
    std::size_t consumed_{0};
    std::size_t baseOffset_{0};
};

}  // namespace ruvia::detail
