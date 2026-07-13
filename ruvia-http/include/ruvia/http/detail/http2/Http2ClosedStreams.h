#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "ruvia/http/detail/http2/Http2LocalSettings.h"
#include "ruvia/http/detail/http2/Http2StreamCloseSource.h"

namespace ruvia::detail {

class Http2ClosedStreamHistory final {
public:
    void remember(std::uint32_t streamId, Http2StreamCloseSource source) {
        if (streamId == 0 || !http2IsValidStreamCloseSource(source)) {
            return;
        }
        for (std::size_t i = 0; i < size_; ++i) {
            auto& record = records_[i];
            if (record.id == streamId) {
                record.source = source;
                return;
            }
        }
        if (size_ < kRecordLimit) {
            records_[size_++] = ClosedStreamRecord{streamId, source};
            return;
        }
        records_[replaceIndex_] = ClosedStreamRecord{streamId, source};
        replaceIndex_ = (replaceIndex_ + 1) % kRecordLimit;
    }

    [[nodiscard]] std::optional<Http2StreamCloseSource>
    source(std::uint32_t streamId) const noexcept {
        for (std::size_t i = 0; i < size_; ++i) {
            const auto& record = records_[i];
            if (record.id == streamId) {
                return record.source;
            }
        }
        return std::nullopt;
    }

private:
    static constexpr std::size_t kRecordLimit =
        Http2LocalSettings::kMaxConcurrentStreams * 4;

    struct ClosedStreamRecord final {
        std::uint32_t id{0};
        Http2StreamCloseSource source{Http2StreamCloseSource::kLocal};
    };

    std::array<ClosedStreamRecord, kRecordLimit> records_{};
    std::size_t size_{0};
    std::size_t replaceIndex_{0};
};

}  // namespace ruvia::detail
