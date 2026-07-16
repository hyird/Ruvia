#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "ruvia/core/detail/AsioAwait.h"
#include "ruvia/core/detail/WorkerSignal.h"
#include "ruvia/core/Task.h"
#include "ruvia/core/memory/PmrObject.h"
#include "ruvia/core/memory/PmrResource.h"
#include "ruvia/http/ProtocolByteLimit.h"
#include "ruvia/http/detail/HttpRequestBodyFailure.h"
#include "ruvia/http/detail/PmrString.h"
#include "ruvia/http/detail/http2/Http2StreamState.h"
#include "ruvia/web/detail/http2/Http2SansIoTermination.h"
#include "ruvia/web/detail/router/RouteModes.h"
#include "ruvia/web/detail/router/RouteResolution.h"

namespace ruvia::detail {

inline constexpr std::size_t kHttp2WebRetainedBodyChunkCapacity = 16;

// The one asynchronous wake primitive owned by a dispatched Web stream. Body
// readers and send-window-blocked writers share it and always re-check their own
// readiness after wakeup, so cancellation is only a level-change notification.
class Http2SansIoStreamSignal final {
public:
    Http2SansIoStreamSignal(
        const WorkerHandle& worker,
        Http2SansIoTermination& termination)
        : signal_(worker), termination_(termination) {}
    Http2SansIoStreamSignal(WorkerHandle&&) = delete;

    void wake() noexcept {
        signal_.notify();
    }

    [[nodiscard]] bool terminated() const noexcept {
        return termination_.terminated();
    }

    [[nodiscard]] std::error_code terminalError() const noexcept {
        return termination_.error();
    }

    [[nodiscard]] Http2SansIoTermination& termination() noexcept {
        return termination_;
    }

    [[nodiscard]] Task<void> wait() {
        if (terminated()) {
            co_return;
        }
        co_await signal_.wait();
    }

private:
    WorkerSignal signal_;
    Http2SansIoTermination& termination_;
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
        if (queuedChunk_.empty() && !hasOverflowChunk()) {
            queuedChunk_.assign(data.data(), data.size());
            queuedBytes_ += data.size();
            return;
        }
        std::pmr::string chunk(
            data.data(), data.size(), overflowChunks_.get_allocator());
        overflowChunks_.push_back(std::move(chunk));
        queuedBytes_ += data.size();
    }

    [[nodiscard]] bool empty() const noexcept {
        return queuedChunk_.empty() && !hasOverflowChunk();
    }

    [[nodiscard]] std::size_t queuedBytes() const noexcept {
        return queuedBytes_;
    }

    // The returned view remains valid until the next pop().
    [[nodiscard]] std::string_view pop() {
        clearPmrStringRetainingSmall(activeChunk_);
        if (!queuedChunk_.empty()) {
            activeChunk_.swap(queuedChunk_);
            queuedBytes_ -= activeChunk_.size();
            clearPmrStringRetainingSmall(queuedChunk_);
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
};

class Http2BufferedRequestBody;
class Http2StreamingRequestBody;

class Http2RequestBodyStored final {
private:
    friend class Http2RequestBodyStoreResult;
    constexpr Http2RequestBodyStored() noexcept = default;
};

class Http2RequestBodyBacklogOverflow final {
private:
    friend class Http2RequestBodyStoreResult;
    constexpr Http2RequestBodyBacklogOverflow() noexcept = default;
};

// A product-owned body store distinguishes an HTTP content failure from local
// streaming backlog exhaustion. They require different runtime policy and must
// never collapse into a generic non-accepted enum value.
class Http2RequestBodyStoreResult final {
public:
    [[nodiscard]] constexpr const Http2RequestBodyStored*
    stored() const & noexcept {
        return std::get_if<Http2RequestBodyStored>(&value_);
    }
    const Http2RequestBodyStored* stored() const && = delete;

    [[nodiscard]] constexpr const HttpRequestBodyFailure*
    protocolFailure() const & noexcept {
        return std::get_if<HttpRequestBodyFailure>(&value_);
    }
    const HttpRequestBodyFailure* protocolFailure() const && = delete;

    [[nodiscard]] constexpr const Http2RequestBodyBacklogOverflow*
    backlogOverflow() const & noexcept {
        return std::get_if<Http2RequestBodyBacklogOverflow>(&value_);
    }
    const Http2RequestBodyBacklogOverflow* backlogOverflow() const && = delete;

private:
    friend class Http2BufferedRequestBody;
    friend class Http2StreamingRequestBody;

    using Value = std::variant<
        Http2RequestBodyStored,
        HttpRequestBodyFailure,
        Http2RequestBodyBacklogOverflow>;

    template <typename Alternative>
    explicit constexpr Http2RequestBodyStoreResult(
        Alternative alternative) noexcept
        : value_(alternative) {}

    [[nodiscard]] static constexpr Http2RequestBodyStoreResult makeStored()
        noexcept {
        return Http2RequestBodyStoreResult(Http2RequestBodyStored());
    }

    [[nodiscard]] static constexpr Http2RequestBodyStoreResult
    makeProtocolFailure(HttpRequestBodyFailure failure) noexcept {
        return Http2RequestBodyStoreResult(failure);
    }

    [[nodiscard]] static constexpr Http2RequestBodyStoreResult
    makeBacklogOverflow() noexcept {
        return Http2RequestBodyStoreResult(
            Http2RequestBodyBacklogOverflow());
    }

    Value value_;
};

static_assert(std::is_trivially_copyable_v<Http2RequestBodyStoreResult>);
static_assert(sizeof(Http2RequestBodyStoreResult) <= 2);

class Http2BufferedRequestBody final {
public:
    explicit Http2BufferedRequestBody(
        std::pmr::memory_resource* resource) noexcept
        : bytes_(pmrResourceOrDefault(resource)) {}

    [[nodiscard]] Http2RequestBodyStoreResult store(
        std::string_view data,
        ProtocolByteLimit totalLimit) {
        if (const auto failure = httpRequestBodyAdditionFailure(
                receivedBytes_, data.size(), totalLimit)) {
            return Http2RequestBodyStoreResult::makeProtocolFailure(*failure);
        }
        receivedBytes_ += data.size();
        if (!data.empty()) {
            bytes_.append(data.data(), data.size());
        }
        return Http2RequestBodyStoreResult::makeStored();
    }

    [[nodiscard]] std::size_t receivedBytes() const noexcept {
        return receivedBytes_;
    }

    [[nodiscard]] std::string_view bytes() const & noexcept {
        return bytes_;
    }
    std::string_view bytes() const && = delete;

private:
    std::size_t receivedBytes_{0};
    std::pmr::string bytes_;
};

class Http2StreamingRequestBody final {
public:
    explicit Http2StreamingRequestBody(
        std::pmr::memory_resource* resource) noexcept
        : queue_(pmrResourceOrDefault(resource)) {}

    [[nodiscard]] Http2RequestBodyStoreResult store(
        std::string_view data,
        ProtocolByteLimit totalLimit,
        std::size_t backlogLimit) {
        if (const auto failure = httpRequestBodyAdditionFailure(
                receivedBytes_, data.size(), totalLimit)) {
            return Http2RequestBodyStoreResult::makeProtocolFailure(*failure);
        }
        if (queue_.queuedBytes() > backlogLimit ||
            data.size() > backlogLimit - queue_.queuedBytes()) {
            return Http2RequestBodyStoreResult::makeBacklogOverflow();
        }
        receivedBytes_ += data.size();
        queue_.enqueue(data);
        return Http2RequestBodyStoreResult::makeStored();
    }

    [[nodiscard]] std::size_t receivedBytes() const noexcept {
        return receivedBytes_;
    }

    [[nodiscard]] Http2SansIoBodyQueue& queue() & noexcept {
        return queue_;
    }
    Http2SansIoBodyQueue& queue() && = delete;

    [[nodiscard]] const Http2SansIoBodyQueue& queue() const & noexcept {
        return queue_;
    }
    const Http2SansIoBodyQueue& queue() const && = delete;

private:
    std::size_t receivedBytes_{0};
    Http2SansIoBodyQueue queue_;
};

// Route-selected request-body storage belongs to ruvia-web. The protocol core
// emits ordered DATA events without knowing whether an application buffers or
// streams them; this runtime applies product limits only after route resolution.
class Http2RequestBodyRuntime final {
public:
    [[nodiscard]] RequestBodyMode mode() const noexcept {
        return std::holds_alternative<Http2BufferedRequestBody>(storage_)
            ? RequestBodyMode::kBuffered
            : RequestBodyMode::kStream;
    }

    [[nodiscard]] Http2BufferedRequestBody* buffered() & noexcept {
        return std::get_if<Http2BufferedRequestBody>(&storage_);
    }
    Http2BufferedRequestBody* buffered() && = delete;

    [[nodiscard]] const Http2BufferedRequestBody* buffered() const & noexcept {
        return std::get_if<Http2BufferedRequestBody>(&storage_);
    }
    const Http2BufferedRequestBody* buffered() const && = delete;

    [[nodiscard]] Http2StreamingRequestBody* streaming() & noexcept {
        return std::get_if<Http2StreamingRequestBody>(&storage_);
    }
    Http2StreamingRequestBody* streaming() && = delete;

    [[nodiscard]] const Http2StreamingRequestBody* streaming() const & noexcept {
        return std::get_if<Http2StreamingRequestBody>(&storage_);
    }
    const Http2StreamingRequestBody* streaming() const && = delete;

    [[nodiscard]] Http2RequestBodyStoreResult store(
        std::string_view data,
        ProtocolByteLimit totalLimit,
        std::size_t streamingBacklogLimit) {
        if (auto* value = buffered()) {
            return value->store(data, totalLimit);
        }
        return std::get<Http2StreamingRequestBody>(storage_).store(
            data, totalLimit, streamingBacklogLimit);
    }

    [[nodiscard]] std::size_t receivedBytes() const noexcept {
        if (const auto* value = buffered()) {
            return value->receivedBytes();
        }
        return std::get<Http2StreamingRequestBody>(storage_).receivedBytes();
    }

private:
    friend class Http2SansIoSelectedRoute;

    using Storage = std::variant<
        Http2BufferedRequestBody,
        Http2StreamingRequestBody>;

    Http2RequestBodyRuntime(
        RequestBodyMode mode,
        std::pmr::memory_resource* resource) noexcept
        : storage_(makeStorage(mode, resource)) {}

    [[nodiscard]] static Storage makeStorage(
        RequestBodyMode mode,
        std::pmr::memory_resource* resource) noexcept {
        if (mode == RequestBodyMode::kBuffered) {
            return Storage(
                std::in_place_type<Http2BufferedRequestBody>, resource);
        }
        if (mode == RequestBodyMode::kStream) {
            return Storage(
                std::in_place_type<Http2StreamingRequestBody>, resource);
        }
        std::terminate();
    }

    Storage storage_;
};

class Http2SansIoSelectedRoute final {
public:
    [[nodiscard]] const RouteResolution& resolution() const & noexcept {
        return resolution_;
    }
    const RouteResolution& resolution() const && = delete;

    [[nodiscard]] Http2RequestBodyRuntime& body() & noexcept {
        return body_;
    }
    Http2RequestBodyRuntime& body() && = delete;

    [[nodiscard]] const Http2RequestBodyRuntime& body() const & noexcept {
        return body_;
    }
    const Http2RequestBodyRuntime& body() const && = delete;

    [[nodiscard]] bool dispatched() const noexcept {
        return signal() != nullptr;
    }

    [[nodiscard]] Http2SansIoStreamSignal* signal() & noexcept {
        return std::get_if<Http2SansIoStreamSignal>(&dispatch_);
    }
    [[nodiscard]] Http2SansIoStreamSignal* signal() && = delete;

    [[nodiscard]] const Http2SansIoStreamSignal* signal() const & noexcept {
        return std::get_if<Http2SansIoStreamSignal>(&dispatch_);
    }
    [[nodiscard]] const Http2SansIoStreamSignal* signal() const && = delete;

private:
    friend class Http2SansIoStreamRuntime;

    // The private token lets the owning runtime construct this non-movable
    // address-stable state directly inside its optional storage.
    struct Token final {};
    struct AwaitingDispatch final {};

    using DispatchState = std::variant<
        AwaitingDispatch,
        Http2SansIoStreamSignal>;

public:
    Http2SansIoSelectedRoute(
        Token,
        RouteResolution resolution,
        RequestBodyMode bodyMode,
        std::pmr::memory_resource* resource) noexcept
        : resolution_(std::move(resolution)),
          body_(bodyMode, resource) {}

private:
    [[nodiscard]] Http2SansIoStreamSignal* beginDispatch(
        const WorkerHandle& worker,
        Http2SansIoTermination& termination) {
        if (dispatched()) {
            return nullptr;
        }
        dispatch_.emplace<Http2SansIoStreamSignal>(worker, termination);
        return signal();
    }

    RouteResolution resolution_;
    Http2RequestBodyRuntime body_;
    DispatchState dispatch_;
};

class Http2SansIoStreamRuntime final {
public:
    Http2SansIoStreamRuntime(
        std::uint32_t streamId,
        std::pmr::memory_resource* resource)
        : streamId_(streamId), resource_(pmrResourceOrDefault(resource)) {}

    [[nodiscard]] std::uint32_t streamId() const noexcept {
        return streamId_;
    }

    [[nodiscard]] bool selectRoute(
        RouteResolution resolution,
        RequestBodyMode bodyMode) noexcept {
        if (selectedRoute_) {
            return false;
        }
        selectedRoute_.emplace(
            Http2SansIoSelectedRoute::Token{},
            std::move(resolution),
            bodyMode,
            resource_);
        return true;
    }

    [[nodiscard]] Http2SansIoSelectedRoute* selectedRoute() & noexcept {
        return selectedRoute_ ? &*selectedRoute_ : nullptr;
    }
    Http2SansIoSelectedRoute* selectedRoute() && = delete;

    [[nodiscard]] const Http2SansIoSelectedRoute*
    selectedRoute() const & noexcept {
        return selectedRoute_ ? &*selectedRoute_ : nullptr;
    }
    const Http2SansIoSelectedRoute* selectedRoute() const && = delete;

    [[nodiscard]] bool dispatched() const noexcept {
        const auto* selected = selectedRoute();
        return selected != nullptr && selected->dispatched();
    }

    [[nodiscard]] Http2SansIoStreamSignal* signal() noexcept {
        auto* selected = selectedRoute();
        return selected != nullptr ? selected->signal() : nullptr;
    }

    [[nodiscard]] const Http2SansIoStreamSignal* signal() const noexcept {
        const auto* selected = selectedRoute();
        return selected != nullptr ? selected->signal() : nullptr;
    }

private:
    friend class Http2SansIoStreamRuntimeTable;

    [[nodiscard]] Http2SansIoStreamSignal* beginDispatch(
        const WorkerHandle& worker,
        Http2SansIoTermination& termination) {
        auto* selected = selectedRoute();
        return selected != nullptr
            ? selected->beginDispatch(worker, termination)
            : nullptr;
    }

    std::uint32_t streamId_;
    std::pmr::memory_resource* resource_;
    std::optional<Http2SansIoSelectedRoute> selectedRoute_;
};

// Stable per-stream Web runtime storage. The common multiplexing case uses inline
// slots; overflow objects are PMR-owned and remain stable when the pointer vector
// compacts, so request body views and dispatch signal references cannot be invalidated
// by another stream being admitted or erased. dispatchedCount_ is acquired before a
// handler is scheduled and released only when that same runtime is removed.
class Http2SansIoStreamRuntimeTable final {
public:
    explicit Http2SansIoStreamRuntimeTable(
        std::pmr::memory_resource* resource,
        Http2SansIoTermination& termination)
        : resource_(pmrResourceOrDefault(resource)),
          termination_(termination),
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

    // Protocol admission is already committed before Web receives the live
    // Http2StreamState. This table only attaches application runtime state; it
    // must not reapply the protocol's concurrent-stream limit or manufacture a
    // nullable second admission result.
    [[nodiscard]] Http2SansIoStreamRuntime& ensureAccepted(
        const Http2StreamState& acceptedStream) {
        const auto streamId = acceptedStream.id();
        if (auto* existing = find(streamId)) {
            return *existing;
        }
        for (auto& slot : inline_) {
            if (!slot) {
                slot.emplace(streamId, resource_);
                ++size_;
                return *slot;
            }
        }
        auto runtime = makePmrObject<Http2SansIoStreamRuntime>(
            resource_, streamId, resource_);
        auto* result = runtime.get();
        overflow_.push_back(std::move(runtime));
        ++size_;
        return *result;
    }

    [[nodiscard]] Http2SansIoStreamSignal* beginDispatch(
        std::uint32_t streamId,
        const WorkerHandle& worker) {
        auto* runtime = find(streamId);
        if (runtime == nullptr) {
            return nullptr;
        }
        auto* signal = runtime->beginDispatch(worker, termination_);
        if (signal != nullptr) {
            ++dispatchedCount_;
        }
        return signal;
    }
    Http2SansIoStreamSignal* beginDispatch(
        std::uint32_t,
        WorkerHandle&&) = delete;

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
    // Mirrors Http2StreamTable::kInlineCapacity: two inline slots for the
    // typical cadence, pmr overflow for deeper multiplexing, so the table's
    // resident footprint stays small in every connection.
    static constexpr std::size_t kInlineCapacity = 2;
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
    Http2SansIoTermination& termination_;
    std::array<std::optional<Http2SansIoStreamRuntime>,
               kInlineCapacity> inline_{};
    std::pmr::vector<OverflowRuntime> overflow_;
    std::size_t size_{0};
    std::size_t dispatchedCount_{0};
};

}  // namespace ruvia::detail
