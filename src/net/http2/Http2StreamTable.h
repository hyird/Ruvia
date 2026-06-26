#pragma once

#include <cstddef>
#include <cstdint>
#include <array>
#include <memory>
#include <memory_resource>
#include <optional>
#include <vector>

#include "Http2Frame.h"
#include "Http2StreamState.h"
#include "ruvia/memory/PmrObject.h"
#include "ruvia/memory/PmrResource.h"

namespace ruvia::detail {

class Http2StreamTable final {
public:
    explicit Http2StreamTable(std::pmr::memory_resource* resource)
        : resource_(pmrResourceOrDefault(resource)),
          overflow_(resource_) {}

    [[nodiscard]] std::size_t size() const noexcept {
        return size_;
    }

    [[nodiscard]] Http2StreamState* find(std::uint32_t streamId) noexcept {
        for (auto& slot : inline_) {
            if (slot && slot->id() == streamId) {
                return &*slot;
            }
        }
        for (auto& stream : overflow_) {
            if (stream != nullptr && stream->id() == streamId) {
                return stream.get();
            }
        }
        return nullptr;
    }

    [[nodiscard]] const Http2StreamState* find(std::uint32_t streamId) const noexcept {
        for (const auto& slot : inline_) {
            if (slot && slot->id() == streamId) {
                return &*slot;
            }
        }
        for (const auto& stream : overflow_) {
            if (stream != nullptr && stream->id() == streamId) {
                return stream.get();
            }
        }
        return nullptr;
    }

    [[nodiscard]] Http2StreamState* create(std::uint32_t streamId, std::int32_t peerInitialWindowSize) {
        if (auto* existing = find(streamId); existing != nullptr) {
            return existing;
        }
        if (size_ >= kHttp2LocalMaxConcurrentStreams) {
            return nullptr;
        }
        for (auto& slot : inline_) {
            if (!slot) {
                slot.emplace(streamId, resource_);
                slot->setSendWindow(peerInitialWindowSize);
                ++size_;
                return &*slot;
            }
        }
        auto stream = makePmrObject<Http2StreamState>(resource_, streamId, resource_);
        stream->setSendWindow(peerInitialWindowSize);
        auto* result = stream.get();
        overflow_.push_back(std::move(stream));
        ++size_;
        return result;
    }

    bool remove(std::uint32_t streamId) noexcept {
        for (auto& slot : inline_) {
            if (slot && slot->id() == streamId) {
                slot.reset();
                --size_;
                return true;
            }
        }
        for (std::size_t i = 0; i < overflow_.size(); ++i) {
            if (overflow_[i] != nullptr && overflow_[i]->id() == streamId) {
                eraseOverflowAt(i);
                --size_;
                return true;
            }
        }
        return false;
    }

    template <typename Callback>
    void forEach(Callback&& callback) {
        for (auto& slot : inline_) {
            if (slot) {
                callback(*slot);
            }
        }
        for (auto& stream : overflow_) {
            if (stream != nullptr) {
                callback(*stream);
            }
        }
    }

    template <typename Callback>
    void removeReset(Callback&& callback) {
        for (auto& slot : inline_) {
            if (!slot || !slot->isReset()) {
                continue;
            }
            callback(*slot);
            slot.reset();
            --size_;
        }
        for (std::size_t i = 0; i < overflow_.size();) {
            auto& stream = overflow_[i];
            if (stream == nullptr || !stream->isReset()) {
                ++i;
                continue;
            }
            callback(*stream);
            eraseOverflowAt(i);
            --size_;
        }
    }

    [[nodiscard]] bool applySendWindowDelta(std::int64_t delta) noexcept {
        bool ok = true;
        forEach([&ok, delta](Http2StreamState& stream) noexcept {
            if (!ok) {
                return;
            }
            if (!stream.addSendWindow(delta)) {
                ok = false;
                return;
            }
        });
        return ok;
    }

private:
    static constexpr std::size_t kInlineCapacity = 16;
    using OverflowStream = std::unique_ptr<Http2StreamState, PmrObjectDeleter<Http2StreamState>>;

    void eraseOverflowAt(std::size_t index) noexcept {
        if (index + 1 != overflow_.size()) {
            overflow_[index] = std::move(overflow_.back());
        }
        overflow_.pop_back();
    }

    std::pmr::memory_resource* resource_;
    std::array<std::optional<Http2StreamState>, kInlineCapacity> inline_{};
    std::pmr::vector<OverflowStream> overflow_;
    std::size_t size_{0};
};

[[nodiscard]] inline bool http2IsIdleStream(std::uint32_t streamId, std::uint32_t lastStreamId) noexcept {
    return streamId > lastStreamId || (streamId & 1U) == 0;
}

inline bool http2ApplyStreamSendWindowDelta(Http2StreamTable& streams, std::int64_t delta) noexcept {
    return streams.applySendWindowDelta(delta);
}

}  // namespace ruvia::detail
