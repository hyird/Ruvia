#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "http2/http2_local_settings.h"
#include "http2/http2_stream_close_source.h"

namespace ruvia::detail {

class http2_closed_stream_history final {
public:
    void remember(std::uint32_t stream_id, http2_stream_close_source source_value) {
        if (stream_id == 0 || !http2_is_valid_stream_close_source(source_value)) {
            return;
        }
        for (std::size_t i = 0; i < size_; ++i) {
            auto& record = records_[i];
            if (record.id_ == stream_id) {
                record.source_ = source_value;
                return;
            }
        }
        if (size_ < record_limit) {
            records_[size_++] = closed_stream_record_type{stream_id, source_value};
            return;
        }
        records_[replace_index_] = closed_stream_record_type{stream_id, source_value};
        replace_index_ = (replace_index_ + 1) % record_limit;
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

private:
    static constexpr std::size_t record_limit =
        static_cast<std::size_t>(http2_local_settings::max_concurrent_streams) * 4;

    struct closed_stream_record_type final {
        std::uint32_t id_{0};
        http2_stream_close_source source_{http2_stream_close_source::local};
    };

    std::array<closed_stream_record_type, record_limit> records_{};
    std::size_t size_{0};
    std::size_t replace_index_{0};
};

}  // namespace ruvia::detail
