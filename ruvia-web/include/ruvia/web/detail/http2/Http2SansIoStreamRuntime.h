#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <optional>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "ruvia/core/Task.h"
#include "ruvia/core/memory/PmrObject.h"
#include "ruvia/core/memory/PmrResource.h"
#include "ruvia/http/detail/http2/stream/Http2StreamState.h"
#include "ruvia/web/detail/http2/Http2SansIoRequestBody.h"
#include "ruvia/web/detail/http2/Http2SansIoStreamSignal.h"
#include "ruvia/web/detail/http2/Http2SansIoTermination.h"
#include "ruvia/web/detail/router/RouteResolution.h"

namespace ruvia::detail {



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

    // The dispatch signal of a stream that has one. A stream with no runtime,
    // or one admitted but not yet dispatched, has nothing to wake.
    [[nodiscard]] Http2SansIoStreamSignal* signalFor(
        std::uint32_t streamId) noexcept {
        auto* runtime = find(streamId);
        return runtime != nullptr ? runtime->signal() : nullptr;
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
