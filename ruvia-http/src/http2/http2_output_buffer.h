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

#include "ruvia/http/http2_types.h"

#include "http2/http2_frame_codec.h"

namespace ruvia::detail {

// Sole owner of HTTP/2 outbound bytes and their consumed prefix. Connection logic
// selects protocol actions; this component owns contiguous frame serialization and
// buffer lifetime so frame handlers cannot manipulate storage cursors directly.
class http2_output_buffer final {
public:
    struct segment_type final {
        // DATA payload spans [begin + http2_frame_header_bytes, end). Raw byte
        // segments have no DATA metadata.
        std::size_t begin_;
        std::size_t end_;
        std::uint32_t stream_id_;
        bool is_data_;
    };
    explicit http2_output_buffer(std::pmr::memory_resource* resource)
        : bytes_(std::string_view{}, resource),
          segments_(std::size_t{0}, resource) {}

    [[nodiscard]] bool wants_write() const noexcept {
        return consumed_ < bytes_.size();
    }

    // Internal transaction checkpoint. The checkpoint includes already-consumed
    // bytes, so a caller can remove only the frames appended after it without
    // disturbing a transport's pending cursor.
    [[nodiscard]] std::size_t checkpoint() const noexcept {
        return base_offset_ + bytes_.size();
    }

    void rollback_to(std::size_t checkpoint) noexcept {
        if (checkpoint < base_offset_ + consumed_ || checkpoint > base_offset_ + bytes_.size()) {
            std::terminate();
        }
        const auto physical_checkpoint = checkpoint - base_offset_;
        bytes_.resize(physical_checkpoint);
        while (segments_.size() > segment_offset_ && segments_.back().begin_ >= physical_checkpoint) {
            segments_.pop_back();
        }
        if (segments_.size() > segment_offset_ && segments_.back().end_ > physical_checkpoint) {
            std::terminate();
        }
    }

    // Moves every pending byte into `into`. With matching allocators and no
    // consumed prefix this swaps storage, retaining the caller's old capacity for
    // future frames; otherwise only the pending suffix is copied.
    void take(std::pmr::string& into);

    void append_bytes(std::string_view bytes_value) {
        if (!bytes_value.empty()) {
            reserve_segment();
            const auto begin = bytes_.size();
            bytes_.append(bytes_value.data(), bytes_value.size());
            segments_.push_back(segment_type{begin, bytes_.size(), 0, false});
        }
    }

    // Reserve storage for a whole sequence before its first frame is emitted.
    // Callers that need multi-frame wire atomicity use this once, while
    // append_frame() applies the same guarantee to an individual frame.
    void reserve_segments_additional(std::size_t additional) {
        if (additional > segments_.max_size() - segments_.size()) {
            throw std::length_error("HTTP/2 output segment count overflow");
        }
        if (observer_depth_ == 0 && segment_offset_ >= 32 &&
            segment_offset_ >= segments_.size() - segment_offset_) {
            const auto remaining = segments_.size() - segment_offset_;
            std::move(segments_.begin() + static_cast<std::ptrdiff_t>(segment_offset_),
                segments_.end(), segments_.begin());
            segments_.resize(remaining);
            segment_offset_ = 0;
        }
        const auto required = segments_.size() + additional;
        if (required > segments_.capacity()) {
            // Individual frame submissions share the same growth policy as a
            // preflighted batch, so a burst does not reallocate for every frame.
            const auto capacity = segments_.capacity();
            const auto increment = capacity / 2;
            const auto grown = capacity > segments_.max_size() - increment
                                   ? segments_.max_size()
                                   : capacity + increment;
            segments_.reserve(std::max(required, grown));
        }
    }

    void reserve_additional(std::size_t additional) {
        if (additional > bytes_.max_size() - bytes_.size()) {
            throw std::length_error("HTTP/2 output buffer size overflow");
        }
        bytes_.reserve(bytes_.size() + additional);
    }

    void append_frame(http2_frame_type type, std::uint8_t flags, std::uint32_t stream_id,
        std::string_view first, std::string_view second = {}) {
        if (first.size() > http2_max_frame_size ||
            second.size() > http2_max_frame_size - first.size()) {
            std::terminate();
        }

        std::array<char, http2_frame_header_bytes> header;
        http2_encode_frame_header(header.data(),
            static_cast<std::uint32_t>(first.size() + second.size()), type, flags, stream_id);
        reserve_segment();
        const auto begin = bytes_.size();
        // A frame is the smallest wire-level transaction. Reserve the complete
        // frame before appending any part so a throwing PMR resource cannot leave
        // a header or prefix without its payload in the pending output.
        reserve_additional(http2_frame_header_bytes + first.size() + second.size());
        append_raw(std::string_view(header.data(), header.size()));
        append_raw(first);
        append_raw(second);
        segments_.push_back(segment_type{begin, bytes_.size(), stream_id, type == http2_frame_type::data});
    }
    void append_goaway_frame(
        std::uint32_t last_stream_id, http2_error_code error, std::string_view debug = {});
    void append_rst_stream(std::uint32_t stream_id, http2_error_code error);

    [[nodiscard]] http2_output_batch_result take_batch(std::size_t max_bytes, std::pmr::string& into,
        http2_data_output_observer_type observer, void* observer_context) {
        if (consumed_ == bytes_.size()) {
            return {http2_output_batch_status::empty, 0};
        }
        // Every append records a segment, and the cursor only advances by whole
        // segments, so the first pending segment always begins at consumed_.
        std::size_t end = consumed_;
        std::size_t segment_count = 0;
        for (std::size_t index = segment_offset_; index < segments_.size(); ++index) {
            const auto& segment = segments_[index];
            if (segment_count != 0 && segment.end_ - consumed_ > max_bytes) {
                break;
            }
            end = segment.end_;
            ++segment_count;
            if (end - consumed_ >= max_bytes) {
                break;
            }
        }
        const auto first_segment = segment_offset_;
        const auto batch_bytes = end - consumed_;
        const bool transfer_storage = consumed_ == 0 && end == bytes_.size() &&
                                      into.empty() && into.get_allocator() == bytes_.get_allocator();
        std::size_t logical_end{};
        if (transfer_storage) {
            logical_end = base_offset_ + end;
            into.swap(bytes_);
        } else {
            into.append(bytes_.data() + consumed_, batch_bytes);
        }
        if (observer != nullptr) {
            ++observer_depth_;
        }
        if (transfer_storage) {
            reset_after_drain(logical_end);
        } else {
            consume_batch(batch_bytes);
        }
        if (observer != nullptr) {
            // A callback may append or drain more frames. Their vector may
            // reallocate, so retain indices and snapshot each entry by value.
            for (std::size_t i = 0; i < segment_count; ++i) {
                const auto segment = segments_[first_segment + i];
                if (segment.is_data_) {
                    observer(observer_context, segment.stream_id_,
                        segment.end_ - segment.begin_ - http2_frame_header_bytes);
                }
            }
            --observer_depth_;
            if (observer_depth_ == 0) {
                if (segment_offset_ == segments_.size()) {
                    segments_.clear();
                    segment_offset_ = 0;
                } else {
                    compact_consumed_prefix();
                }
            }
        }
        return {http2_output_batch_status::taken, batch_bytes};
    }

    [[nodiscard]] std::size_t pending_data_bytes(std::uint32_t stream_id) const noexcept {
        std::size_t total = 0;
        for (std::size_t index = segment_offset_; index < segments_.size(); ++index) {
            const auto& segment = segments_[index];
            if (segment.is_data_ && segment.stream_id_ == stream_id) {
                total += segment.end_ - segment.begin_ - http2_frame_header_bytes;
            }
        }
        return total;
    }

private:
    // Advances the cursor past whole segments; the cursor always stays on a
    // segment boundary, so no partial-segment bookkeeping exists.
    void consume_batch(std::size_t bytes_value) noexcept {
        if (bytes_value == bytes_.size() - consumed_) {
            reset_after_drain(base_offset_ + bytes_.size());
            return;
        }
        consumed_ += bytes_value;
        while (segment_offset_ < segments_.size() && segments_[segment_offset_].end_ <= consumed_) {
            ++segment_offset_;
        }
        compact_consumed_prefix();
    }

    void reset_after_drain(std::size_t logical_end) noexcept {
        bytes_.clear();
        if (observer_depth_ == 0) {
            segments_.clear();
            segment_offset_ = 0;
        } else {
            segment_offset_ = segments_.size();
        }
        consumed_ = 0;
        base_offset_ = logical_end;
    }

    void append_raw(std::string_view bytes_value) {
        if (!bytes_value.empty()) {
            bytes_.append(bytes_value.data(), bytes_value.size());
        }
    }

    void compact_consumed_prefix() noexcept {
        if (observer_depth_ != 0 || consumed_ < 64 * 1024 || consumed_ < bytes_.size() - consumed_) {
            return;
        }
        const auto prefix = consumed_;
        bytes_.erase(0, prefix);
        for (std::size_t index = segment_offset_; index < segments_.size(); ++index) {
            auto& segment = segments_[index];
            segment.begin_ = segment.begin_ > prefix ? segment.begin_ - prefix : 0;
            segment.end_ -= prefix;
        }
        segments_.erase(segments_.begin(),
            segments_.begin() + static_cast<std::ptrdiff_t>(segment_offset_));
        base_offset_ += prefix;
        segment_offset_ = 0;
        consumed_ = 0;
    }

    void reserve_segment() {
        reserve_segments_additional(1);
    }

    std::pmr::string bytes_;
    std::pmr::vector<segment_type> segments_;
    std::size_t segment_offset_{0};
    std::size_t consumed_{0};
    std::size_t base_offset_{0};
    std::size_t observer_depth_{0};
};

}  // namespace ruvia::detail
