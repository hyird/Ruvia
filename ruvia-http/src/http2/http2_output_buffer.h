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

using ruvia::http2_data_output_observer_type;
using ruvia::http2_output_batch_result;
using ruvia::http2_output_batch_status;
using ruvia::http2_output_consume_status;

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

    [[nodiscard]] std::string_view pending() const& noexcept {
        return std::string_view(bytes_).substr(consumed_);
    }
    [[nodiscard]] std::string_view pending() const&& = delete;

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
        while (!segments_.empty() && segments_.back().begin_ >= physical_checkpoint) {
            segments_.pop_back();
        }
        if (!segments_.empty() && segments_.back().end_ > physical_checkpoint) {
            std::terminate();
        }
    }

    [[nodiscard]] http2_output_consume_status consume(std::size_t bytes_value) noexcept {
        const auto remaining = bytes_.size() - consumed_;
        if (bytes_value > remaining) {
            return http2_output_consume_status::out_of_range;
        }
        if (bytes_value < remaining) {
            consumed_ += bytes_value;
            while (segment_offset_ < segments_.size() &&
                   segments_[segment_offset_].end_ <= consumed_) {
                ++segment_offset_;
            }
            compact_consumed_prefix();
            return http2_output_consume_status::pending;
        }
        const auto logical_end = base_offset_ + bytes_.size();
        bytes_.clear();
        segments_.clear();
        segment_offset_ = 0;
        base_offset_ = logical_end;
        consumed_ = 0;
        return http2_output_consume_status::drained;
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
        if (segment_offset_ >= 32 && segment_offset_ >= segments_.size() - segment_offset_) {
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
        if (first.size() > http2_max_frame_size_limit ||
            second.size() > http2_max_frame_size_limit - first.size()) {
            std::terminate();
        }

        std::array<char, http2_frame_header_bytes> header;
        http2_encode_frame_header(header.data(),
            static_cast<std::uint32_t>(first.size() + second.size()), type, flags, stream_id);
        reserve_segment();
        const auto begin = bytes_.size();
        // A frame is the smallest wire-level transaction. Reserve the complete
        // frame before appending any part so a throwing PMR resource cannot leave
        // a header or prefix without its payload in pending_output().
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
        if (segment_offset_ == segments_.size() || segments_[segment_offset_].begin_ != consumed_) {
            return {http2_output_batch_status::unaligned, 0};
        }
        std::size_t end = consumed_;
        std::size_t segment_count = 0;
        for (std::size_t index = segment_offset_; index < segments_.size(); ++index) {
            const auto& segment = segments_[index];
            if (segment.begin_ != end) {
                return {http2_output_batch_status::unaligned, 0};
            }
            if (segment_count != 0 && segment.end_ - consumed_ > max_bytes) {
                break;
            }
            end = segment.end_;
            ++segment_count;
            if (end - consumed_ >= max_bytes) {
                break;
            }
        }
        if (segment_count == 0) {
            return {http2_output_batch_status::unaligned, 0};
        }
        const auto batch_bytes = end - consumed_;
        into.append(bytes_.data() + consumed_, batch_bytes);
        if (observer != nullptr) {
            for (std::size_t i = 0; i < segment_count; ++i) {
                const auto& segment = segments_[segment_offset_ + i];
                if (segment.is_data_) {
                    observer(observer_context, segment.stream_id_,
                        segment.end_ - segment.begin_ - http2_frame_header_bytes);
                }
            }
        }
        consume_batch(batch_bytes);
        return {http2_output_batch_status::taken, batch_bytes};
    }

    [[nodiscard]] std::size_t pending_data_bytes(std::uint32_t stream_id) const noexcept {
        std::size_t total = 0;
        for (std::size_t index = segment_offset_; index < segments_.size(); ++index) {
            const auto& segment = segments_[index];
            if (!segment.is_data_ || segment.stream_id_ != stream_id || consumed_ >= segment.end_) {
                continue;
            }
            const auto begin = std::max(consumed_, segment.begin_ + http2_frame_header_bytes);
            total += segment.end_ - begin;
        }
        return total;
    }

    void consume_batch(std::size_t bytes_value) noexcept {
        const auto status = consume(bytes_value);
        if (status == http2_output_consume_status::out_of_range) {
            std::terminate();
        }
    }

private:
    void append_raw(std::string_view bytes_value) {
        if (!bytes_value.empty()) {
            bytes_.append(bytes_value.data(), bytes_value.size());
        }
    }

    void compact_consumed_prefix() noexcept {
        if (consumed_ < 64 * 1024 || consumed_ < bytes_.size() - consumed_ ||
            (segment_offset_ < segments_.size() && segments_[segment_offset_].begin_ != consumed_)) {
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
};

}  // namespace ruvia::detail
