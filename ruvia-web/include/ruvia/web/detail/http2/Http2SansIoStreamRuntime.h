#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <asio/steady_timer.hpp>

#include "ruvia/core/detail/AsioAwait.h"
#include "ruvia/core/Task.h"
#include "ruvia/core/memory/PmrObject.h"
#include "ruvia/core/memory/PmrResource.h"
#include "ruvia/http/detail/PmrString.h"
#include "ruvia/http/detail/http2/Http2LocalSettings.h"
#include "ruvia/web/RouteModes.h"
#include "ruvia/web/detail/router/RouteResolution.h"

namespace ruvia::detail {

inline constexpr std::size_t kHttp2WebRetainedBodyChunkCapacity = 16;

// The one asynchronous wake primitive owned by a dispatched Web stream. Body
// readers and send-window-blocked writers share it and always re-check their own
// readiness after wakeup, so cancellation is only a level-change notification.
class Http2SansIoStreamSignal final {
public:
    template <typename Executor>
    explicit Http2SansIoStreamSignal(const Executor& executor)
        : timer_(executor) {
        timer_.expires_at((asio::steady_timer::time_point::max)());
    }

    void wake() noexcept {
        asio::error_code ignored;
        timer_.cancel(ignored);
    }

    void end() noexcept {
        ended_ = true;
        wake();
    }

    [[nodiscard]] bool ended() const noexcept {
        return ended_;
    }

    [[nodiscard]] Task<void> wait() {
        if (ended_) {
            co_return;
        }
        (void)co_await asyncError([this](auto handler) mutable {
            timer_.async_wait(std::move(handler));
        });
    }

private:
    asio::steady_timer timer_;
    bool ended_{false};
};

// Web-owned storage for body/tunnel bytes that have already crossed the
// Http2Connection event boundary. The HTTP/2 core owns framing and flow-control
// debt; this queue owns only runtime buffering for a suspended route handler.
class Http2SansIoBodyQueue final {
public:
    explicit Http2SansIoBodyQueue(
        std::pmr::memory_resource* resource = nullptr)
        : queuedChunk_(pmrResourceOrDefault(resource)),
          activeChunk_(pmrResourceOrDefault(resource)),
          overflowChunks_(pmrResourceOrDefault(resource)) {}

    void enqueue(std::string_view data) {
        if (data.empty()) {
            return;
        }
        queuedBytes_ += data.size();
        if (!hasQueuedChunk_ && !hasOverflowChunk()) {
            queuedChunk_.assign(data.data(), data.size());
            hasQueuedChunk_ = true;
            return;
        }
        auto& chunk = overflowChunks_.emplace_back();
        chunk.assign(data.data(), data.size());
    }

    [[nodiscard]] bool empty() const noexcept {
        return !hasQueuedChunk_ && !hasOverflowChunk();
    }

    [[nodiscard]] std::size_t queuedBytes() const noexcept {
        return queuedBytes_;
    }

    // The returned view remains valid until the next pop().
    [[nodiscard]] std::string_view pop() {
        clearPmrStringRetainingSmall(activeChunk_);
        if (hasQueuedChunk_) {
            activeChunk_.swap(queuedChunk_);
            queuedBytes_ -= activeChunk_.size();
            clearPmrStringRetainingSmall(queuedChunk_);
            hasQueuedChunk_ = false;
            return std::string_view(activeChunk_);
        }
        if (!hasOverflowChunk()) {
            return {};
        }
        activeChunk_ = std::move(overflowChunks_[overflowOffset_++]);
        queuedBytes_ -= activeChunk_.size();
        compactOverflow();
        return std::string_view(activeChunk_);
    }

private:
    [[nodiscard]] bool hasOverflowChunk() const noexcept {
        return overflowOffset_ < overflowChunks_.size();
    }

    void compactOverflow() {
        if (overflowOffset_ == overflowChunks_.size()) {
            overflowChunks_.clear();
            overflowOffset_ = 0;
        } else if (overflowOffset_ >= kHttp2WebRetainedBodyChunkCapacity &&
                   overflowOffset_ * 2 >= overflowChunks_.size()) {
            const auto remaining = overflowChunks_.size() - overflowOffset_;
            for (std::size_t i = 0; i < remaining; ++i) {
                overflowChunks_[i] =
                    std::move(overflowChunks_[overflowOffset_ + i]);
            }
            overflowChunks_.resize(remaining);
            overflowOffset_ = 0;
        }
        if (!overflowChunks_.empty() ||
            overflowChunks_.capacity() <=
                kHttp2WebRetainedBodyChunkCapacity) {
            return;
        }
        std::pmr::vector<std::pmr::string> empty(
            overflowChunks_.get_allocator());
        overflowChunks_.swap(empty);
    }

    std::pmr::string queuedChunk_;
    std::pmr::string activeChunk_;
    std::pmr::vector<std::pmr::string> overflowChunks_;
    std::size_t overflowOffset_{0};
    std::size_t queuedBytes_{0};
    bool hasQueuedChunk_{false};
};

enum class Http2RequestBodyStoreResult : std::uint8_t {
    kAccepted,
    kModeNotSelected,
    kTotalLimitExceeded,
    kBacklogLimitExceeded
};

// Route-selected request-body storage belongs to ruvia-web. The protocol core
// emits ordered DATA events without knowing whether an application buffers or
// streams them; this runtime applies product limits only after route resolution.
class Http2RequestBodyRuntime final {
public:
    explicit Http2RequestBodyRuntime(
        std::pmr::memory_resource* resource = nullptr)
        : buffered_(pmrResourceOrDefault(resource)),
          queue_(pmrResourceOrDefault(resource)) {}

    [[nodiscard]] RequestBodyMode mode() const noexcept {
        return mode_;
    }

    [[nodiscard]] bool modeSelected() const noexcept {
        return modeSelected_;
    }

    [[nodiscard]] bool streaming() const noexcept {
        return mode_ == RequestBodyMode::kStream;
    }

    // Route selection is a one-time pre-DATA decision. Re-selecting the current
    // mode is harmless, but switching storage would change the body owner and is
    // rejected even before the first byte arrives.
    [[nodiscard]] bool selectMode(RequestBodyMode mode) noexcept {
        if (modeSelected_) {
            return mode == mode_;
        }
        mode_ = mode;
        modeSelected_ = true;
        return true;
    }

    [[nodiscard]] Http2RequestBodyStoreResult store(
        std::string_view data,
        std::size_t totalLimit,
        std::size_t streamingBacklogLimit) {
        if (!modeSelected_) {
            return Http2RequestBodyStoreResult::kModeNotSelected;
        }
        if (data.size() >
            (std::numeric_limits<std::size_t>::max)() - receivedBytes_) {
            return Http2RequestBodyStoreResult::kTotalLimitExceeded;
        }
        if (totalLimit != 0 &&
            (receivedBytes_ > totalLimit ||
             data.size() > totalLimit - receivedBytes_)) {
            return Http2RequestBodyStoreResult::kTotalLimitExceeded;
        }
        if (streaming() && streamingBacklogLimit != 0 &&
            (queue_.queuedBytes() > streamingBacklogLimit ||
             data.size() >
                 streamingBacklogLimit - queue_.queuedBytes())) {
            return Http2RequestBodyStoreResult::kBacklogLimitExceeded;
        }
        receivedBytes_ += data.size();
        if (streaming()) {
            queue_.enqueue(data);
        } else if (!data.empty()) {
            buffered_.append(data.data(), data.size());
        }
        return Http2RequestBodyStoreResult::kAccepted;
    }

    [[nodiscard]] std::size_t receivedBytes() const noexcept {
        return receivedBytes_;
    }

    [[nodiscard]] std::string_view buffered() const noexcept {
        return buffered_;
    }

    [[nodiscard]] Http2SansIoBodyQueue& queue() noexcept {
        return queue_;
    }

    [[nodiscard]] const Http2SansIoBodyQueue& queue() const noexcept {
        return queue_;
    }

private:
    RequestBodyMode mode_{RequestBodyMode::kBuffered};
    bool modeSelected_{false};
    std::size_t receivedBytes_{0};
    std::pmr::string buffered_;
    Http2SansIoBodyQueue queue_;
};

class Http2SansIoStreamRuntime final {
public:
    Http2SansIoStreamRuntime(
        std::uint32_t streamId,
        std::pmr::memory_resource* resource)
        : streamId_(streamId), body_(resource) {}

    [[nodiscard]] std::uint32_t streamId() const noexcept {
        return streamId_;
    }

    [[nodiscard]] RouteMatch& routeScratch() noexcept {
        return routeScratch_;
    }

    [[nodiscard]] RouteResolution& routeResolution() noexcept {
        return routeResolution_;
    }

    [[nodiscard]] const RouteResolution& routeResolution() const noexcept {
        return routeResolution_;
    }

    [[nodiscard]] Http2RequestBodyRuntime& body() noexcept {
        return body_;
    }

    [[nodiscard]] const Http2RequestBodyRuntime& body() const noexcept {
        return body_;
    }

    [[nodiscard]] bool dispatched() const noexcept {
        return dispatchSignal_.has_value();
    }

    [[nodiscard]] Http2SansIoStreamSignal* signal() noexcept {
        return dispatchSignal_ ? &*dispatchSignal_ : nullptr;
    }

    [[nodiscard]] const Http2SansIoStreamSignal* signal() const noexcept {
        return dispatchSignal_ ? &*dispatchSignal_ : nullptr;
    }

private:
    friend class Http2SansIoStreamRuntimeTable;

    template <typename Executor>
    [[nodiscard]] Http2SansIoStreamSignal* beginDispatch(
        const Executor& executor) {
        if (dispatchSignal_ || !body_.modeSelected()) {
            return nullptr;
        }
        dispatchSignal_.emplace(executor);
        return &*dispatchSignal_;
    }

    std::uint32_t streamId_;
    RouteMatch routeScratch_;
    RouteResolution routeResolution_;
    Http2RequestBodyRuntime body_;
    std::optional<Http2SansIoStreamSignal> dispatchSignal_;
};

// Stable per-stream Web runtime storage. The common multiplexing case uses inline
// slots; overflow objects are PMR-owned and remain stable when the pointer vector
// compacts, so request body views and dispatch signal references cannot be invalidated
// by another stream being admitted or erased. dispatchedCount_ is acquired before a
// handler is scheduled and released only when that same runtime is removed.
class Http2SansIoStreamRuntimeTable final {
public:
    explicit Http2SansIoStreamRuntimeTable(
        std::pmr::memory_resource* resource)
        : resource_(pmrResourceOrDefault(resource)),
          overflow_(resource_) {}

    [[nodiscard]] Http2SansIoStreamRuntime* find(
        std::uint32_t streamId) noexcept {
        for (auto& slot : inline_) {
            if (slot && slot->streamId() == streamId) {
                return &*slot;
            }
        }
        for (auto& runtime : overflow_) {
            if (runtime != nullptr && runtime->streamId() == streamId) {
                return runtime.get();
            }
        }
        return nullptr;
    }

    [[nodiscard]] const Http2SansIoStreamRuntime* find(
        std::uint32_t streamId) const noexcept {
        for (const auto& slot : inline_) {
            if (slot && slot->streamId() == streamId) {
                return &*slot;
            }
        }
        for (const auto& runtime : overflow_) {
            if (runtime != nullptr && runtime->streamId() == streamId) {
                return runtime.get();
            }
        }
        return nullptr;
    }

    [[nodiscard]] Http2SansIoStreamRuntime* ensure(
        std::uint32_t streamId) {
        if (auto* existing = find(streamId)) {
            return existing;
        }
        if (size_ >= Http2LocalSettings::kMaxConcurrentStreams) {
            return nullptr;
        }
        for (auto& slot : inline_) {
            if (!slot) {
                slot.emplace(streamId, resource_);
                ++size_;
                return &*slot;
            }
        }
        auto runtime = makePmrObject<Http2SansIoStreamRuntime>(
            resource_, streamId, resource_);
        auto* result = runtime.get();
        overflow_.push_back(std::move(runtime));
        ++size_;
        return result;
    }

    template <typename Executor>
    [[nodiscard]] Http2SansIoStreamSignal* beginDispatch(
        std::uint32_t streamId,
        const Executor& executor) {
        auto* runtime = find(streamId);
        if (runtime == nullptr) {
            return nullptr;
        }
        auto* signal = runtime->beginDispatch(executor);
        if (signal != nullptr) {
            ++dispatchedCount_;
        }
        return signal;
    }

    [[nodiscard]] bool remove(std::uint32_t streamId) noexcept {
        for (auto& slot : inline_) {
            if (slot && slot->streamId() == streamId) {
                accountRemoval(*slot);
                slot.reset();
                --size_;
                return true;
            }
        }
        for (std::size_t i = 0; i < overflow_.size(); ++i) {
            if (overflow_[i] == nullptr ||
                overflow_[i]->streamId() != streamId) {
                continue;
            }
            accountRemoval(*overflow_[i]);
            if (i + 1 != overflow_.size()) {
                overflow_[i] = std::move(overflow_.back());
            }
            overflow_.pop_back();
            --size_;
            return true;
        }
        return false;
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return size_;
    }

    [[nodiscard]] std::size_t dispatchedCount() const noexcept {
        return dispatchedCount_;
    }

    template <typename Callback>
    void forEach(Callback&& callback) {
        for (auto& slot : inline_) {
            if (slot) {
                callback(*slot);
            }
        }
        for (auto& runtime : overflow_) {
            if (runtime != nullptr) {
                callback(*runtime);
            }
        }
    }

private:
    static constexpr std::size_t kInlineCapacity = 16;
    using OverflowRuntime = std::unique_ptr<
        Http2SansIoStreamRuntime,
        PmrObjectDeleter<Http2SansIoStreamRuntime>>;

    void accountRemoval(
        const Http2SansIoStreamRuntime& runtime) noexcept {
        if (runtime.dispatched()) {
            --dispatchedCount_;
        }
    }

    std::pmr::memory_resource* resource_;
    std::array<std::optional<Http2SansIoStreamRuntime>,
               kInlineCapacity> inline_{};
    std::pmr::vector<OverflowRuntime> overflow_;
    std::size_t size_{0};
    std::size_t dispatchedCount_{0};
};

}  // namespace ruvia::detail
