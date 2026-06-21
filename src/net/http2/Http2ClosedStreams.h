#pragma once

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <vector>

#include "Http2Frame.h"
#include "Http2StreamState.h"

namespace ruvia::detail {

class Http2ClosedStreamHistory final {
public:
    explicit Http2ClosedStreamHistory(std::pmr::memory_resource* resource)
        : records_(resource == nullptr ? std::pmr::get_default_resource() : resource) {
        records_.reserve(kRecordLimit);
    }

    void remember(std::uint32_t streamId, Http2StreamCloseSource source) {
        if (streamId == 0 || source == Http2StreamCloseSource::kNone) {
            return;
        }
        for (auto& record : records_) {
            if (record.id == streamId) {
                record.source = source;
                return;
            }
        }
        if (records_.size() < kRecordLimit) {
            records_.push_back(ClosedStreamRecord{streamId, source});
            return;
        }
        records_[replaceIndex_] = ClosedStreamRecord{streamId, source};
        replaceIndex_ = (replaceIndex_ + 1) % kRecordLimit;
    }

    [[nodiscard]] Http2StreamCloseSource source(std::uint32_t streamId) const noexcept {
        for (const auto& record : records_) {
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

    std::pmr::vector<ClosedStreamRecord> records_;
    std::size_t replaceIndex_{0};
};

}  // namespace ruvia::detail
