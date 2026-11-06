#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "ruvia/http/http2_framing.h"
#include "ruvia/http/http2_types.h"

#include "util/geometric_reserve.h"

namespace ruvia::detail {

// Sole owner of HTTP/2 outbound bytes and their consumed prefix. nghttp2
// serializes frames; the connection appends each frame it emits as one segment
// so batches stay frame-aligned and DATA payload bytes stay attributable.
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

    // Appends one complete frame (or the client connection preface) as a
    // segment; both reservations precede the copy, so a throwing resource
    // leaves the pending output unchanged.
    void append_segment(std::string_view bytes_value, std::uint32_t stream_id, bool is_data) {
        if (bytes_value.empty()) {
            return;
        }
        reserve_segment();
        if (bytes_value.size() > bytes_.max_size() - bytes_.size()) {
            throw std::length_error("HTTP/2 output buffer size overflow");
        }
        reserve_geometric(bytes_, bytes_.size() + bytes_value.size());
        const auto begin = bytes_.size();
        bytes_.append(bytes_value.data(), bytes_value.size());
        segments_.push_back(segment_type{begin, bytes_.size(), stream_id, is_data});
    }

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
        if (transfer_storage) {
            into.swap(bytes_);
        } else {
            into.append(bytes_.data() + consumed_, batch_bytes);
        }
        if (observer != nullptr) {
            ++observer_depth_;
        }
        if (transfer_storage) {
            reset_after_drain();
        } else {
            consume_batch(batch_bytes, segment_count);
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
    void consume_batch(std::size_t bytes_value, std::size_t segment_count) noexcept {
        if (bytes_value == bytes_.size() - consumed_) {
            reset_after_drain();
            return;
        }
        consumed_ += bytes_value;
        segment_offset_ += segment_count;
        compact_consumed_prefix();
    }

    void reset_after_drain() noexcept {
        bytes_.clear();
        if (observer_depth_ == 0) {
            segments_.clear();
            segment_offset_ = 0;
        } else {
            segment_offset_ = segments_.size();
        }
        consumed_ = 0;
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
        segment_offset_ = 0;
        consumed_ = 0;
    }

    void reserve_segment() {
        if (observer_depth_ == 0 && segment_offset_ >= 32 &&
            segment_offset_ >= segments_.size() - segment_offset_) {
            const auto remaining = segments_.size() - segment_offset_;
            std::move(segments_.begin() + static_cast<std::ptrdiff_t>(segment_offset_),
                segments_.end(), segments_.begin());
            segments_.resize(remaining);
            segment_offset_ = 0;
        }
        // A burst of frames shares one geometric growth policy.
        reserve_geometric(segments_, segments_.size() + 1);
    }

    std::pmr::string bytes_;
    std::pmr::vector<segment_type> segments_;
    std::size_t segment_offset_{0};
    std::size_t consumed_{0};
    std::size_t observer_depth_{0};
};

}  // namespace ruvia::detail
