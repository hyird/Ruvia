#pragma once

#include <cstddef>
#include <exception>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "ruvia/core/PmrString.h"
#include "ruvia/core/memory/PmrObject.h"
#include "ruvia/core/memory/PmrResource.h"
#include "ruvia/http/Http2Connection.h"
#include "ruvia/http/HttpRequestBodyFailure.h"
#include "ruvia/http/ProtocolByteLimit.h"
#include "ruvia/web/detail/http2/Http2SansIoStreamSignal.h"
#include "ruvia/web/detail/router/RouteModes.h"

// Where a dispatched HTTP/2 request body lives on the Web side. The protocol
// core emits ordered DATA events without knowing whether the route buffers or
// streams them; everything here applies product limits after route resolution,
// and keeps an HTTP content failure distinguishable from local backlog
// exhaustion because the two need different runtime policy.

namespace ruvia::detail {

// Web-owned storage for body/tunnel bytes that have already crossed the
// Http2Connection event boundary. The HTTP/2 core owns framing and flow-control
// debt; this queue owns only runtime buffering for a suspended route handler.
// Pending DATA is concatenated so each frame is copied once, not into a new
// string per frame. pop() swaps the pending buffer aside so enqueue cannot
// invalidate the borrowed view.
class Http2SansIoBodyQueue final {
public:
    explicit Http2SansIoBodyQueue(std::pmr::memory_resource* resource = nullptr)
        : queuedChunk_(pmrResourceOrDefault(resource)),
          activeChunk_(pmrResourceOrDefault(resource)) {}

    void enqueue(std::string_view data) {
        if (data.empty()) {
            return;
        }
        const auto before = queuedChunk_.size();
        queuedChunk_.append(data.data(), data.size());
        queuedBytes_ += queuedChunk_.size() - before;
    }

    void enqueue(std::string_view data, Http2ReceivedDataCredit&& credit) {
        if (data.empty()) {
            return;
        }
        const auto before = queuedChunk_.size();
        // Commit byte accounting only after append succeeds. The incoming
        // credit is still untouched if allocation throws, so its owner can
        // safely return it during unwinding.
        queuedChunk_.append(data.data(), data.size());
        queuedBytes_ += queuedChunk_.size() - before;
        if (!credit.valid()) {
            return;
        }
        if (!queuedCredit_.has_value()) {
            queuedCredit_.emplace(std::move(credit));
            return;
        }
        if (queuedCredit_->merge(std::move(credit)) !=
            Http2ReceivedDataCreditMergeStatus::kMerged) {
            std::terminate();
        }
    }

    [[nodiscard]] bool enqueueBounded(std::string_view data,
        Http2ReceivedDataCredit&& credit, std::size_t backlogLimit) {
        if (queuedBytes_ > backlogLimit || data.size() > backlogLimit - queuedBytes_) {
            return false;
        }
        enqueue(data, std::move(credit));
        return true;
    }

    // Release credits only after the consumer has copied/finished the active view.
    void releaseActiveCredits() noexcept {
        activeCredit_.reset();
    }

    [[nodiscard]] bool empty() const noexcept {
        return queuedChunk_.empty();
    }

    [[nodiscard]] std::size_t queuedBytes() const noexcept {
        return queuedBytes_;
    }

    // The returned view remains valid until the next pop().
    [[nodiscard]] std::string_view pop() & {
        ::ruvia::clearPmrStringRetainingSmall(activeChunk_);
        releaseActiveCredits();
        if (queuedChunk_.empty()) {
            return {};
        }
        activeChunk_.swap(queuedChunk_);
        if (queuedCredit_.has_value()) {
            activeCredit_.emplace(std::move(*queuedCredit_));
            queuedCredit_.reset();
        }
        queuedBytes_ = 0;
        ::ruvia::clearPmrStringRetainingSmall(queuedChunk_);
        return std::string_view(activeChunk_);
    }
    std::string_view pop() && = delete;

private:
    // The byte storage is destroyed before its credits, so return-window side
    // effects cannot outlive the copied data they account for.
    std::optional<Http2ReceivedDataCredit> queuedCredit_;
    std::optional<Http2ReceivedDataCredit> activeCredit_;
    std::pmr::string queuedChunk_;
    std::pmr::string activeChunk_;
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
    [[nodiscard]] constexpr const Http2RequestBodyStored* stored() const& noexcept {
        return state_ == State::kStored ? &value_.stored : nullptr;
    }
    const Http2RequestBodyStored* stored() const&& = delete;

    [[nodiscard]] constexpr const HttpRequestBodyFailure* protocolFailure() const& noexcept {
        return state_ == State::kProtocolFailure ? &value_.protocolFailure : nullptr;
    }
    const HttpRequestBodyFailure* protocolFailure() const&& = delete;

    [[nodiscard]] constexpr const Http2RequestBodyBacklogOverflow* backlogOverflow()
        const& noexcept {
        return state_ == State::kBacklogOverflow ? &value_.backlogOverflow : nullptr;
    }
    const Http2RequestBodyBacklogOverflow* backlogOverflow() const&& = delete;

private:
    friend class Http2BufferedRequestBody;
    friend class Http2StreamingRequestBody;

    enum class State : std::uint8_t { kStored,
        kProtocolFailure,
        kBacklogOverflow };

    union Value {
        constexpr explicit Value(Http2RequestBodyStored value) noexcept
            : stored(value) {}
        constexpr explicit Value(HttpRequestBodyFailure value) noexcept
            : protocolFailure(value) {}
        constexpr explicit Value(Http2RequestBodyBacklogOverflow value) noexcept
            : backlogOverflow(value) {}

        Http2RequestBodyStored stored;
        HttpRequestBodyFailure protocolFailure;
        Http2RequestBodyBacklogOverflow backlogOverflow;
    };

    explicit constexpr Http2RequestBodyStoreResult(Http2RequestBodyStored value) noexcept
        : value_(value),
          state_(State::kStored) {}
    explicit constexpr Http2RequestBodyStoreResult(HttpRequestBodyFailure value) noexcept
        : value_(value),
          state_(State::kProtocolFailure) {}
    explicit constexpr Http2RequestBodyStoreResult(Http2RequestBodyBacklogOverflow value) noexcept
        : value_(value),
          state_(State::kBacklogOverflow) {}

    [[nodiscard]] static constexpr Http2RequestBodyStoreResult makeStored() noexcept {
        return Http2RequestBodyStoreResult(Http2RequestBodyStored());
    }

    [[nodiscard]] static constexpr Http2RequestBodyStoreResult makeProtocolFailure(
        HttpRequestBodyFailure failure) noexcept {
        return Http2RequestBodyStoreResult(failure);
    }

    [[nodiscard]] static constexpr Http2RequestBodyStoreResult makeBacklogOverflow() noexcept {
        return Http2RequestBodyStoreResult(Http2RequestBodyBacklogOverflow());
    }

    Value value_;
    State state_;
};

static_assert(std::is_trivially_copyable_v<Http2RequestBodyStoreResult>);
static_assert(sizeof(Http2RequestBodyStoreResult) <= 2);

class Http2BufferedRequestBody final {
public:
    explicit Http2BufferedRequestBody(std::pmr::memory_resource* resource) noexcept
        : bytes_(pmrResourceOrDefault(resource)) {}

    [[nodiscard]] Http2RequestBodyStoreResult store(
        std::string_view data, ProtocolByteLimit totalLimit) {
        if (const auto failure =
                httpRequestBodyAdditionFailure(receivedBytes_, data.size(), totalLimit)) {
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

    [[nodiscard]] std::string_view bytes() const& noexcept {
        return bytes_;
    }
    std::string_view bytes() const&& = delete;

private:
    std::size_t receivedBytes_{0};
    std::pmr::string bytes_;
};

class Http2StreamingRequestBody final {
public:
    explicit Http2StreamingRequestBody(std::pmr::memory_resource* resource) noexcept
        : queue_(pmrResourceOrDefault(resource)) {}

    [[nodiscard]] Http2RequestBodyStoreResult store(
        std::string_view data, ProtocolByteLimit totalLimit, std::size_t backlogLimit) {
        return storeImpl(data, totalLimit, backlogLimit, nullptr);
    }

    [[nodiscard]] Http2RequestBodyStoreResult store(std::string_view data,
        ProtocolByteLimit totalLimit, std::size_t backlogLimit,
        Http2ReceivedDataCredit&& credit) {
        return storeImpl(data, totalLimit, backlogLimit, &credit);
    }

    [[nodiscard]] std::size_t receivedBytes() const noexcept {
        return receivedBytes_;
    }

    [[nodiscard]] Http2SansIoBodyQueue& queue() & noexcept {
        return queue_;
    }
    Http2SansIoBodyQueue& queue() && = delete;

    [[nodiscard]] const Http2SansIoBodyQueue& queue() const& noexcept {
        return queue_;
    }
    const Http2SansIoBodyQueue& queue() const&& = delete;

private:
    [[nodiscard]] Http2RequestBodyStoreResult storeImpl(std::string_view data,
        ProtocolByteLimit totalLimit, std::size_t backlogLimit,
        Http2ReceivedDataCredit* credit) {
        if (const auto failure =
                httpRequestBodyAdditionFailure(receivedBytes_, data.size(), totalLimit)) {
            return Http2RequestBodyStoreResult::makeProtocolFailure(*failure);
        }
        if (queue_.queuedBytes() > backlogLimit ||
            data.size() > backlogLimit - queue_.queuedBytes()) {
            return Http2RequestBodyStoreResult::makeBacklogOverflow();
        }
        if (credit != nullptr) {
            queue_.enqueue(data, std::move(*credit));
        } else {
            queue_.enqueue(data);
        }
        receivedBytes_ += data.size();
        return Http2RequestBodyStoreResult::makeStored();
    }

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

    [[nodiscard]] const Http2BufferedRequestBody* buffered() const& noexcept {
        return std::get_if<Http2BufferedRequestBody>(&storage_);
    }
    const Http2BufferedRequestBody* buffered() const&& = delete;

    [[nodiscard]] Http2StreamingRequestBody* streaming() & noexcept {
        return std::get_if<Http2StreamingRequestBody>(&storage_);
    }
    Http2StreamingRequestBody* streaming() && = delete;

    [[nodiscard]] const Http2StreamingRequestBody* streaming() const& noexcept {
        return std::get_if<Http2StreamingRequestBody>(&storage_);
    }
    const Http2StreamingRequestBody* streaming() const&& = delete;

    [[nodiscard]] Http2RequestBodyStoreResult store(
        std::string_view data, ProtocolByteLimit totalLimit, std::size_t streamingBacklogLimit) {
        if (auto* value = buffered()) {
            return value->store(data, totalLimit);
        }
        return std::get<Http2StreamingRequestBody>(storage_).store(
            data, totalLimit, streamingBacklogLimit);
    }

    [[nodiscard]] Http2RequestBodyStoreResult store(std::string_view data,
        ProtocolByteLimit totalLimit, std::size_t streamingBacklogLimit,
        Http2ReceivedDataCredit&& credit) {
        if (auto* value = buffered()) {
            return value->store(data, totalLimit);
        }
        return std::get<Http2StreamingRequestBody>(storage_).store(
            data, totalLimit, streamingBacklogLimit, std::move(credit));
    }

    [[nodiscard]] std::size_t receivedBytes() const noexcept {
        if (const auto* value = buffered()) {
            return value->receivedBytes();
        }
        return std::get<Http2StreamingRequestBody>(storage_).receivedBytes();
    }

private:
    friend class Http2SansIoSelectedRoute;

    using Storage = std::variant<Http2BufferedRequestBody, Http2StreamingRequestBody>;

    Http2RequestBodyRuntime(RequestBodyMode mode, std::pmr::memory_resource* resource) noexcept
        : storage_(makeStorage(mode, resource)) {}

    [[nodiscard]] static Storage makeStorage(
        RequestBodyMode mode, std::pmr::memory_resource* resource) noexcept {
        if (mode == RequestBodyMode::kBuffered) {
            return Storage(std::in_place_type<Http2BufferedRequestBody>, resource);
        }
        if (mode == RequestBodyMode::kStream) {
            return Storage(std::in_place_type<Http2StreamingRequestBody>, resource);
        }
        std::terminate();
    }

    Storage storage_;
};

}  // namespace ruvia::detail
