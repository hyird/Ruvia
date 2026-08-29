#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory_resource>
#include <stdexcept>
#include <string>
#include <string_view>

#include "ruvia/http/detail/http2/frame/Http2FrameCodec.h"

namespace ruvia::detail {

// Output acknowledgement is transactional. A transport can distinguish a valid
// partial write from a complete drain, while an impossible over-consumption leaves
// both the pending view and its cursor unchanged.
enum class Http2OutputConsumeStatus : std::uint8_t { kPending,
    kDrained,
    kOutOfRange };

// Sole owner of HTTP/2 outbound bytes and their consumed prefix. Connection logic
// selects protocol actions; this component owns contiguous frame serialization and
// buffer lifetime so frame handlers cannot manipulate storage cursors directly.
class Http2OutputBuffer final {
public:
    explicit Http2OutputBuffer(std::pmr::memory_resource* resource)
        : bytes_(resource) {}

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
        return bytes_.size();
    }

    void rollbackTo(std::size_t checkpoint) noexcept {
        if (checkpoint < consumed_ || checkpoint > bytes_.size()) {
            std::terminate();
        }
        bytes_.resize(checkpoint);
    }

    [[nodiscard]] Http2OutputConsumeStatus consume(std::size_t bytes) noexcept {
        const auto remaining = bytes_.size() - consumed_;
        if (bytes > remaining) {
            return Http2OutputConsumeStatus::kOutOfRange;
        }
        if (bytes < remaining) {
            consumed_ += bytes;
            return Http2OutputConsumeStatus::kPending;
        }
        bytes_.clear();
        consumed_ = 0;
        return Http2OutputConsumeStatus::kDrained;
    }

    // Moves every pending byte into `into`. With matching allocators and no
    // consumed prefix this swaps storage, retaining the caller's old capacity for
    // future frames; otherwise only the pending suffix is copied.
    void take(std::pmr::string& into);

    void appendBytes(std::string_view bytes) {
        if (!bytes.empty()) {
            bytes_.append(bytes.data(), bytes.size());
        }
    }

    // Reserve storage for a whole sequence before its first frame is emitted.
    // Callers that need multi-frame wire atomicity use this once, while
    // appendFrame() applies the same guarantee to an individual frame.
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
        // A frame is the smallest wire-level transaction. Reserve the complete
        // frame before appending any part so a throwing PMR resource cannot leave
        // a header or prefix without its payload in pendingOutput().
        reserveAdditional(kHttp2FrameHeaderBytes + first.size() + second.size());
        appendBytes(std::string_view(header.data(), header.size()));
        appendBytes(first);
        appendBytes(second);
    }
    void appendGoawayFrame(
        std::uint32_t lastStreamId, Http2ErrorCode error, std::string_view debug = {});
    void appendRstStream(std::uint32_t streamId, Http2ErrorCode error);

private:
    std::pmr::string bytes_;
    std::size_t consumed_{0};
};

}  // namespace ruvia::detail
