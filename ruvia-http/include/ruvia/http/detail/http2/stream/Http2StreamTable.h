#pragma once

#include <cstddef>
#include <cstdint>
#include <array>
#include <memory>
#include <memory_resource>
#include <limits>
#include <optional>
#include <vector>

#include "ruvia/http/detail/http2/settings/Http2LocalSettings.h"
#include "ruvia/http/detail/http2/stream/Http2StreamState.h"
#include "ruvia/http/detail/util/HttpPmrObject.h"

namespace ruvia::detail {

class Http2StreamTable final {
public:
    explicit Http2StreamTable(std::pmr::memory_resource* resource)
        : resource_(httpPmrResourceOrDefault(resource)),
          overflow_(resource_) {}

    [[nodiscard]] std::size_t size() const noexcept {
        return size_;
    }

    [[nodiscard]] Http2StreamState* find(std::uint32_t streamId) & noexcept {
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
    [[nodiscard]] Http2StreamState* find(std::uint32_t) && = delete;

    [[nodiscard]] const Http2StreamState* find(std::uint32_t streamId) const& noexcept {
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
    [[nodiscard]] const Http2StreamState* find(std::uint32_t) const&& = delete;

    // NOTE on slot reuse: a closed stream's Http2StreamState is destroyed here (inline
    // slot .reset() / overflow erase), freeing its per-stream pmr strings. We do NOT
    // pool the storage and reset-in-place to retain capacity, deliberately. A correct
    // resetForReuse would have to reinitialise ~85 fields across 8 sub-objects, and a
    // single missed field would leak one request's decoded headers / routing / body
    // into the next reused slot -- a cross-request data-disclosure risk not worth taking
    // for a micro-optimisation off the measured hot path.
    [[nodiscard]] Http2StreamState* create(std::uint32_t streamId, std::int32_t peerInitialWindowSize) & {
        if (auto* existing = find(streamId); existing != nullptr) {
            return existing;
        }
        if (size_ >= Http2LocalSettings::kMaxConcurrentStreams) {
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
        auto stream = makeHttpPmrObject<Http2StreamState>(resource_, streamId, resource_);
        stream->setSendWindow(peerInitialWindowSize);
        auto* result = stream.get();
        overflow_.push_back(std::move(stream));
        ++size_;
        return result;
    }
    [[nodiscard]] Http2StreamState* create(std::uint32_t, std::int32_t) && = delete;

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
        SnapshotIterationGuard guard(*this);
        for (auto& slot : inline_) {
            if (slot) {
                callback(*slot);
            }
        }
        const auto overflowEnd = overflow_.size();
        for (std::size_t i = 0; i < overflowEnd; ++i) {
            auto& stream = overflow_[i];
            if (stream != nullptr) {
                callback(*stream);
            }
        }
    }

    template <typename Callback>
    void removeAborted(Callback&& callback) {
        for (auto& slot : inline_) {
            if (!slot || !slot->isAborted()) {
                continue;
            }
            callback(*slot);
            slot.reset();
            --size_;
        }
        for (std::size_t i = 0; i < overflow_.size();) {
            auto& stream = overflow_[i];
            if (stream == nullptr || !stream->isAborted()) {
                ++i;
                continue;
            }
            callback(*stream);
            eraseOverflowAt(i);
            --size_;
        }
    }

    [[nodiscard]] bool applySendWindowDelta(std::int64_t delta) noexcept {
        // SETTINGS_INITIAL_WINDOW_SIZE applies to every active stream as one
        // protocol transaction. Preflight the complete table first; applying
        // the delta while discovering a later overflow would leave earlier
        // streams with a different window even though the SETTINGS is rejected.
        bool fits = true;
        forEach([&fits, delta](Http2StreamState& stream) noexcept {
            if (!fits) {
                return;
            }
            const auto current = static_cast<std::int64_t>(stream.sendWindow());
            if (delta > static_cast<std::int64_t>((std::numeric_limits<std::int32_t>::max)()) - current || delta < static_cast<std::int64_t>((std::numeric_limits<std::int32_t>::min)()) - current) {
                fits = false;
            }
        });
        if (!fits) {
            return false;
        }
        forEach([delta](Http2StreamState& stream) noexcept {
            (void)stream.addSendWindow(delta);
        });
        return true;
    }

private:
    // Two inline slots cover the typical request/response cadence without
    // paying the full concurrent-stream budget up front: each slot embeds a
    // ~1.3 KB Http2StreamState, and the table lives in the connection object
    // for the connection's whole lifetime, so the two slots put ~2.6 KB into
    // every connection. Deeper multiplexing spills to the pmr overflow path.
    static constexpr std::size_t kInlineCapacity = 2;
    using OverflowStream = std::unique_ptr<Http2StreamState, HttpPmrObjectDeleter<Http2StreamState>>;

    class SnapshotIterationGuard final {
    public:
        explicit SnapshotIterationGuard(Http2StreamTable& table) noexcept
            : table_(table) {
            ++table_.snapshotIterationDepth_;
        }

        SnapshotIterationGuard(const SnapshotIterationGuard&) = delete;
        SnapshotIterationGuard& operator=(const SnapshotIterationGuard&) = delete;

        ~SnapshotIterationGuard() {
            --table_.snapshotIterationDepth_;
            table_.compactOverflowIfIdle();
        }

    private:
        Http2StreamTable& table_;
    };

    void eraseOverflowAt(std::size_t index) noexcept {
        if (snapshotIterationDepth_ != 0) {
            overflow_[index].reset();
            overflowNeedsCompact_ = true;
            return;
        }
        if (index + 1 != overflow_.size()) {
            overflow_[index] = std::move(overflow_.back());
        }
        overflow_.pop_back();
    }

    void compactOverflowIfIdle() noexcept {
        if (snapshotIterationDepth_ != 0 || !overflowNeedsCompact_) {
            return;
        }
        for (std::size_t i = 0; i < overflow_.size();) {
            if (overflow_[i] == nullptr) {
                if (i + 1 != overflow_.size()) {
                    overflow_[i] = std::move(overflow_.back());
                }
                overflow_.pop_back();
                continue;
            }
            ++i;
        }
        overflowNeedsCompact_ = false;
    }

    std::pmr::memory_resource* resource_;
    std::array<std::optional<Http2StreamState>, kInlineCapacity> inline_{};
    std::pmr::vector<OverflowStream> overflow_;
    std::size_t size_{0};
    std::size_t snapshotIterationDepth_{0};
    bool overflowNeedsCompact_{false};
};

[[nodiscard]] inline bool http2IsIdleStream(std::uint32_t streamId, std::uint32_t lastStreamId) noexcept {
    return streamId > lastStreamId || (streamId & 1U) == 0;
}

inline bool http2ApplyStreamSendWindowDelta(Http2StreamTable& streams, std::int64_t delta) noexcept {
    return streams.applySendWindowDelta(delta);
}

}  // namespace ruvia::detail
