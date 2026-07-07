#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "Http2FrameTypes.h"
#include "Http2StreamLifecycle.h"

namespace ruvia::detail {

class Http2ClosedStreamHistory final {
public:
    void remember(std::uint32_t streamId, Http2StreamCloseSource source) {
        if (streamId == 0 || source == Http2StreamCloseSource::kNone) {
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

    [[nodiscard]] Http2StreamCloseSource source(std::uint32_t streamId) const noexcept {
        for (std::size_t i = 0; i < size_; ++i) {
            const auto& record = records_[i];
            if (record.id == streamId) {
                return record.source;
            }
        }
        return Http2StreamCloseSource::kNone;
    }

private:
    static constexpr std::size_t kRecordLimit = kHttp2LocalMaxConcurrentStreams * 4;

    struct ClosedStreamRecord final {
        std::uint32_t id{0};
        Http2StreamCloseSource source{Http2StreamCloseSource::kNone};
    };

    std::array<ClosedStreamRecord, kRecordLimit> records_{};
    std::size_t size_{0};
    std::size_t replaceIndex_{0};
};

}  // namespace ruvia::detail
