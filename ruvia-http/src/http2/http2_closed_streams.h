#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "ruvia/http/http2_types.h"

#include "http2/http2_local_settings.h"

namespace ruvia::detail {

class http2_closed_stream_history final {
public:
    void remember(
        std::uint32_t stream_id, http2_stream_close_source source_value, bool reset_sent = false) {
        if (stream_id == 0 || !http2_is_valid_stream_close_source(source_value)) {
            return;
        }
        for (std::size_t bucket = hash_bucket(stream_id);; bucket = (bucket + 1) & bucket_mask) {
            const auto slot = buckets_[bucket];
            if (slot == 0) {
                break;
            }
            auto& record = records_[slot - 1];
            if (record.id_ == stream_id) {
                record.source_ = source_value;
                record.reset_sent_ = record.reset_sent_ || reset_sent;
                return;
            }
        }
        if (size_ < record_limit) {
            const auto record_index = size_++;
            records_[record_index] = closed_stream_record_type{stream_id, source_value, reset_sent};
            insert_bucket(stream_id, static_cast<std::uint16_t>(record_index + 1));
            return;
        }
        const auto& evicted = records_[replace_index_];
        if (evicted.source_ == http2_stream_close_source::local && evicted.reset_sent_ &&
            (evicted.id_ & 1U) != 0) {
            forgotten_local_request_reset_through_ =
                std::max(forgotten_local_request_reset_through_, evicted.id_);
        }
        erase_bucket(evicted.id_, static_cast<std::uint16_t>(replace_index_ + 1));
        records_[replace_index_] = closed_stream_record_type{stream_id, source_value, reset_sent};
        insert_bucket(stream_id, static_cast<std::uint16_t>(replace_index_ + 1));
        replace_index_ = (replace_index_ + 1) % record_limit;
    }

    // Client-originated stream IDs are consecutive odd values. Once exact close
    // metadata is evicted, any older odd stream can therefore have been reset
    // locally. RFC 9113 section 5.1 permits ignoring frames that might have been
    // sent before the peer observed that reset. This preserves active sibling
    // streams without retaining per-stream state for the connection's whole life.
    [[nodiscard]] bool may_have_forgotten_local_request_reset(
        std::uint32_t stream_id) const noexcept {
        return (stream_id & 1U) != 0 && stream_id <= forgotten_local_request_reset_through_;
    }

    [[nodiscard]] std::optional<http2_stream_close_source> source(
        std::uint32_t stream_id) const noexcept {
        const auto* const record = find_record(stream_id);
        return record != nullptr ? std::optional<http2_stream_close_source>(record->source_)
                                 : std::nullopt;
    }

    [[nodiscard]] bool local_reset_sent(std::uint32_t stream_id) const noexcept {
        const auto* const record = find_record(stream_id);
        return record != nullptr && record->source_ == http2_stream_close_source::local &&
               record->reset_sent_;
    }

private:
    static constexpr std::size_t record_limit =
        static_cast<std::size_t>(http2_local_settings::max_concurrent_streams) * 16;
    static constexpr std::size_t bucket_count = record_limit * 2;
    static constexpr std::size_t bucket_mask = bucket_count - 1;
    static_assert((bucket_count & bucket_mask) == 0);

    struct closed_stream_record_type final {
        std::uint32_t id_;
        http2_stream_close_source source_;
        bool reset_sent_;
    };

    std::array<closed_stream_record_type, record_limit> records_;
    std::array<std::uint16_t, bucket_count> buckets_{};
    std::size_t size_{0};
    std::size_t replace_index_{0};
    std::uint32_t forgotten_local_request_reset_through_{0};
    [[nodiscard]] static constexpr std::size_t hash_bucket(std::uint32_t stream_id) noexcept {
        return static_cast<std::size_t>((stream_id * 0x9e3779b9U) >> 20) & bucket_mask;
    }

    [[nodiscard]] const closed_stream_record_type* find_record(
        std::uint32_t stream_id) const noexcept {
        if (stream_id == 0 || size_ == 0) {
            return nullptr;
        }
        for (std::size_t bucket = hash_bucket(stream_id);; bucket = (bucket + 1) & bucket_mask) {
            const auto slot = buckets_[bucket];
            if (slot == 0) {
                return nullptr;
            }
            const auto& record = records_[slot - 1];
            if (record.id_ == stream_id) {
                return &record;
            }
        }
    }

    void insert_bucket(std::uint32_t stream_id, std::uint16_t one_based_index) noexcept {
        for (std::size_t bucket = hash_bucket(stream_id);; bucket = (bucket + 1) & bucket_mask) {
            if (buckets_[bucket] == 0) {
                buckets_[bucket] = one_based_index;
                return;
            }
        }
    }

    void erase_bucket(std::uint32_t stream_id, std::uint16_t one_based_index) noexcept {
        std::size_t hole = hash_bucket(stream_id);
        while (buckets_[hole] != one_based_index) {
            if (buckets_[hole] == 0) {
                return;
            }
            hole = (hole + 1) & bucket_mask;
        }
        for (std::size_t cursor_value = (hole + 1) & bucket_mask;;
            cursor_value = (cursor_value + 1) & bucket_mask) {
            const auto slot = buckets_[cursor_value];
            if (slot == 0) {
                buckets_[hole] = 0;
                return;
            }
            const auto ideal = hash_bucket(records_[slot - 1].id_);
            if (((cursor_value - ideal) & bucket_mask) >= ((cursor_value - hole) & bucket_mask)) {
                buckets_[hole] = slot;
                hole = cursor_value;
            }
        }
    }
};

}  // namespace ruvia::detail
