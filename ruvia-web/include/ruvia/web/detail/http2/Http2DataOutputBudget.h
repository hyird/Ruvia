#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <ranges>

#include "ruvia/core/Task.h"
#include "ruvia/core/WorkerSignal.h"
#include "ruvia/http/Http2Connection.h"
#include "ruvia/web/detail/http2/Http2SansIoStreamSignal.h"

namespace ruvia::detail {

inline constexpr std::size_t kHttp2DataOutputCreditBytes = 16 * 1024;
inline constexpr std::size_t kHttp2DataOutputCreditSlots = 4;

// Four frame-sized DATA reservations bound producer-owned/core-queued DATA at
// 64 KiB per connection. HTTP/2 control frames have independent protocol output
// storage and are deliberately not included in this DATA-only bound.
class Http2DataOutputBudget final {
public:
    explicit Http2DataOutputBudget(const WorkerHandle& worker) : changed_(worker) {}

    Http2DataOutputBudget(const Http2DataOutputBudget&) = delete;
    Http2DataOutputBudget& operator=(const Http2DataOutputBudget&) = delete;

    [[nodiscard]] Task<bool> acquire(std::uint32_t streamId, Http2SansIoStreamSignal& stream) {
        while (!stream.terminated()) {
            const bool alreadyHeld = std::ranges::find(held_, streamId) != held_.end();
            if (!alreadyHeld) {
                for (auto& held : held_) {
                    if (held == 0) {
                        held = streamId;
                        co_return true;
                    }
                }
            }
            // A stream may own only one DATA reservation; wait for the current
            // chunk to drain rather than admitting another and confusing release.
            co_await changed_.wait();
        }
        co_return false;
    }

    void release(std::uint32_t streamId) noexcept {
        for (std::size_t i = 0; i < held_.size(); ++i) {
            if (held_[i] == streamId) {
                cancelled_[i] = true;
                if (submitted_[i] == 0) {
                    clear(i);
                }
                return;
            }
        }
    }

    void noteDataSubmitted(std::uint32_t streamId, std::size_t bytes) noexcept {
        for (std::size_t i = 0; i < held_.size(); ++i) {
            if (held_[i] == streamId) {
                submitted_[i] += bytes;
                return;
            }
        }
    }

    void noteDataOutput(std::uint32_t streamId, std::size_t bytes) noexcept {
        for (std::size_t i = 0; i < held_.size(); ++i) {
            if (held_[i] == streamId) {
                emitted_[i] += bytes;
                batchOutput_[i] += bytes;
                return;
            }
        }
    }

    void reconcile(const ruvia::Http2Connection& connection, bool socketWriteCompleted) noexcept {
        for (std::size_t i = 0; i < held_.size(); ++i) {
            const auto streamId = held_[i];
            if (streamId == 0) {
                continue;
            }
            if (socketWriteCompleted) {
                written_[i] += batchOutput_[i];
                batchOutput_[i] = 0;
            }
            // Stream removal is not evidence that serialized DATA was written.
            // Keep its credit until every submitted payload byte crossed the socket.
            if ((cancelled_[i] && written_[i] >= emitted_[i] &&
                    connection.pendingDataOutputBytes(streamId) == 0) ||
                (!cancelled_[i] && written_[i] >= submitted_[i] &&
                    connection.pendingDataOutputBytes(streamId) == 0 &&
                    connection.dataQueueState(streamId) != ruvia::Http2DataQueueState::kQueued)) {
                clear(i);
            }
        }
    }

    [[nodiscard]] Task<void> waitForChange() {
        co_await changed_.wait();
    }

    void wake() noexcept {
        changed_.notify();
    }

private:
    void clear(std::size_t i) noexcept {
        held_[i] = 0;
        submitted_[i] = 0;
        emitted_[i] = 0;
        written_[i] = 0;
        cancelled_[i] = false;
        batchOutput_[i] = 0;
        changed_.notify();
    }

    WorkerSignal changed_;
    std::array<std::uint32_t, kHttp2DataOutputCreditSlots> held_{};
    std::array<std::size_t, kHttp2DataOutputCreditSlots> submitted_{};
    std::array<std::size_t, kHttp2DataOutputCreditSlots> emitted_{};
    std::array<std::size_t, kHttp2DataOutputCreditSlots> written_{};
    std::array<bool, kHttp2DataOutputCreditSlots> cancelled_{};
    std::array<std::size_t, kHttp2DataOutputCreditSlots> batchOutput_{};
};

}  // namespace ruvia::detail
