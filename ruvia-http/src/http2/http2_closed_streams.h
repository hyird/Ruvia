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
        for (std::size_t i = 0; i < size_; ++i) {
            auto& record = records_[i];
            if (record.id_ == stream_id) {
                record.source_ = source_value;
                record.reset_sent_ = record.reset_sent_ || reset_sent;
                return;
            }
        }
        if (size_ < record_limit) {
            records_[size_++] = closed_stream_record_type{stream_id, source_value, reset_sent};
            return;
        }
        const auto& evicted = records_[replace_index_];
        if (evicted.source_ == http2_stream_close_source::local && evicted.reset_sent_ &&
            (evicted.id_ & 1U) != 0) {
            forgotten_local_request_reset_through_ =
                std::max(forgotten_local_request_reset_through_, evicted.id_);
        }
        records_[replace_index_] = closed_stream_record_type{stream_id, source_value, reset_sent};
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
        for (std::size_t i = 0; i < size_; ++i) {
            const auto& record = records_[i];
            if (record.id_ == stream_id) {
                return record.source_;
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] bool local_reset_sent(std::uint32_t stream_id) const noexcept {
        for (std::size_t i = 0; i < size_; ++i) {
            const auto& record = records_[i];
            if (record.id_ == stream_id) {
                return record.source_ == http2_stream_close_source::local && record.reset_sent_;
            }
        }
        return false;
    }

    [[nodiscard]] bool claim_late_local_reset(std::uint32_t stream_id) noexcept {
        for (std::size_t i = 0; i < size_; ++i) {
            auto& record = records_[i];
            if (record.id_ == stream_id) {
                if (record.source_ != http2_stream_close_source::local || record.reset_sent_) {
                    return false;
                }
                record.reset_sent_ = true;
                return true;
            }
        }
        return false;
    }

private:
    static constexpr std::size_t record_limit =
        static_cast<std::size_t>(http2_local_settings::max_concurrent_streams) * 16;

    struct closed_stream_record_type final {
        std::uint32_t id_{0};
        http2_stream_close_source source_{http2_stream_close_source::local};
        bool reset_sent_{false};
    };

    std::array<closed_stream_record_type, record_limit> records_{};
    std::size_t size_{0};
    std::size_t replace_index_{0};
    std::uint32_t forgotten_local_request_reset_through_{0};
};

}  // namespace ruvia::detail
