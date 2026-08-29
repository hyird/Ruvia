#include "ruvia/http/Http2Connection.h"

#include <array>
#include <exception>
#include <limits>
#include <memory>
#include <stdexcept>
#include <variant>

#include "ruvia/http/detail/client/HttpClientAccess.h"
#include "ruvia/http/detail/http2/Http2Connection.h"
#include "ruvia/http/detail/http2/message/Http2RequestBuilder.h"
#include "ruvia/http/detail/response/HttpResponseBodyAccess.h"
#include "ruvia/http/detail/server/HttpResponseWritePlan.h"
#include "ruvia/http/detail/util/PmrResource.h"

namespace ruvia::detail {

// Events retain this endpoint intrusively, so its address cannot be recycled while
// a stale lease/credit still exists. Detaching the target makes destruction safe
// when an event outlives its connection. The endpoint also retains the connection
// storage after owner destruction because request and body views can still refer to it.
class Http2ConnectionOwnerEndpoint final {
public:
    using AbandonRequest = void (*)(void*, std::uint32_t) noexcept;
    using AbandonCredit = void (*)(void*, std::uint32_t, std::uint32_t) noexcept;
    using DestroyStorage = void (*)(void*) noexcept;

    Http2ConnectionOwnerEndpoint(
        void* target, AbandonRequest abandonRequest, AbandonCredit abandonCredit) noexcept
        : target_(target),
          abandonRequest_(abandonRequest),
          abandonCredit_(abandonCredit) {}

    void retain() noexcept {
        ++references_;
    }
    void retainStorage(void* storage, DestroyStorage destroyStorage) noexcept {
        if (retainedStorage_ != nullptr || storage == nullptr || destroyStorage == nullptr) {
            std::terminate();
        }
        retainedStorage_ = storage;
        destroyStorage_ = destroyStorage;
    }
    void release() noexcept {
        if (references_ == 0) {
            std::terminate();
        }
        if (--references_ == 0) {
            if (retainedStorage_ != nullptr) {
                destroyStorage_(retainedStorage_);
            }
            delete this;
        }
    }
    void detach() noexcept {
        target_ = nullptr;
    }
    void abandonRequest(std::uint32_t streamId) noexcept {
        if (target_ != nullptr) {
            abandonRequest_(target_, streamId);
        }
    }
    void abandonCredit(std::uint32_t streamId, std::uint32_t bytes) noexcept {
        if (target_ != nullptr) {
            abandonCredit_(target_, streamId, bytes);
        }
    }

private:
    std::size_t references_{1};
    void* target_;
    AbandonRequest abandonRequest_;
    AbandonCredit abandonCredit_;
    void* retainedStorage_{nullptr};
    DestroyStorage destroyStorage_{nullptr};
};

}  // namespace ruvia::detail

namespace ruvia {
namespace {

[[nodiscard]] detail::Http2RequestContent toInternal(Http2RequestContent content) noexcept {
    if (content.withoutContent() != nullptr) {
        return detail::Http2RequestContent::none();
    }
    if (const auto* known = content.knownLengthContent()) {
        return detail::Http2RequestContent::knownLength(known->length());
    }
    return detail::Http2RequestContent::streaming();
}

[[nodiscard]] detail::Http2Role toInternal(Http2Role role) {
    switch (role) {
        case Http2Role::kServer:
            return detail::Http2Role::kServer;
        case Http2Role::kClient:
            return detail::Http2Role::kClient;
    }
    throw std::invalid_argument("invalid HTTP/2 role");
}

[[nodiscard]] detail::Http2EndStream toInternal(Http2EndStream endStream) {
    switch (endStream) {
        case Http2EndStream::kKeepOpen:
            return detail::Http2EndStream::kKeepOpen;
        case Http2EndStream::kEndStream:
            return detail::Http2EndStream::kEndStream;
    }
    throw std::invalid_argument("invalid HTTP/2 end-stream value");
}

[[nodiscard]] detail::Http2ErrorCode toInternal(Http2ErrorCode error) {
    switch (error) {
        case Http2ErrorCode::kNoError:
            return detail::Http2ErrorCode::kNoError;
        case Http2ErrorCode::kProtocolError:
            return detail::Http2ErrorCode::kProtocolError;
        case Http2ErrorCode::kInternalError:
            return detail::Http2ErrorCode::kInternalError;
        case Http2ErrorCode::kFlowControlError:
            return detail::Http2ErrorCode::kFlowControlError;
        case Http2ErrorCode::kSettingsTimeout:
            return detail::Http2ErrorCode::kSettingsTimeout;
        case Http2ErrorCode::kStreamClosed:
            return detail::Http2ErrorCode::kStreamClosed;
        case Http2ErrorCode::kFrameSizeError:
            return detail::Http2ErrorCode::kFrameSizeError;
        case Http2ErrorCode::kRefusedStream:
            return detail::Http2ErrorCode::kRefusedStream;
        case Http2ErrorCode::kCancel:
            return detail::Http2ErrorCode::kCancel;
        case Http2ErrorCode::kCompressionError:
            return detail::Http2ErrorCode::kCompressionError;
        case Http2ErrorCode::kConnectError:
            return detail::Http2ErrorCode::kConnectError;
        case Http2ErrorCode::kEnhanceYourCalm:
            return detail::Http2ErrorCode::kEnhanceYourCalm;
        case Http2ErrorCode::kInadequateSecurity:
            return detail::Http2ErrorCode::kInadequateSecurity;
        case Http2ErrorCode::kHttp11Required:
            return detail::Http2ErrorCode::kHttp11Required;
    }
    throw std::invalid_argument("invalid HTTP/2 error code");
}

[[nodiscard]] Http2ErrorCode toPublic(detail::Http2ErrorCode error) noexcept {
    switch (error) {
        case detail::Http2ErrorCode::kNoError:
            return Http2ErrorCode::kNoError;
        case detail::Http2ErrorCode::kProtocolError:
            return Http2ErrorCode::kProtocolError;
        case detail::Http2ErrorCode::kInternalError:
            return Http2ErrorCode::kInternalError;
        case detail::Http2ErrorCode::kFlowControlError:
            return Http2ErrorCode::kFlowControlError;
        case detail::Http2ErrorCode::kSettingsTimeout:
            return Http2ErrorCode::kSettingsTimeout;
        case detail::Http2ErrorCode::kStreamClosed:
            return Http2ErrorCode::kStreamClosed;
        case detail::Http2ErrorCode::kFrameSizeError:
            return Http2ErrorCode::kFrameSizeError;
        case detail::Http2ErrorCode::kRefusedStream:
            return Http2ErrorCode::kRefusedStream;
        case detail::Http2ErrorCode::kCancel:
            return Http2ErrorCode::kCancel;
        case detail::Http2ErrorCode::kCompressionError:
            return Http2ErrorCode::kCompressionError;
        case detail::Http2ErrorCode::kConnectError:
            return Http2ErrorCode::kConnectError;
        case detail::Http2ErrorCode::kEnhanceYourCalm:
            return Http2ErrorCode::kEnhanceYourCalm;
        case detail::Http2ErrorCode::kInadequateSecurity:
            return Http2ErrorCode::kInadequateSecurity;
        case detail::Http2ErrorCode::kHttp11Required:
            return Http2ErrorCode::kHttp11Required;
    }
    std::terminate();
}

[[nodiscard]] Http2StreamCloseSource toPublic(detail::Http2StreamCloseSource source) noexcept {
    switch (source) {
        case detail::Http2StreamCloseSource::kLocal:
            return Http2StreamCloseSource::kLocal;
        case detail::Http2StreamCloseSource::kPeer:
            return Http2StreamCloseSource::kPeer;
        case detail::Http2StreamCloseSource::kPeerGoaway:
            return Http2StreamCloseSource::kPeerGoaway;
    }
    std::terminate();
}

[[nodiscard]] Http2OutputConsumeStatus toPublic(detail::Http2OutputConsumeStatus status) noexcept {
    switch (status) {
        case detail::Http2OutputConsumeStatus::kPending:
            return Http2OutputConsumeStatus::kPending;
        case detail::Http2OutputConsumeStatus::kDrained:
            return Http2OutputConsumeStatus::kDrained;
        case detail::Http2OutputConsumeStatus::kOutOfRange:
            return Http2OutputConsumeStatus::kOutOfRange;
    }
    std::terminate();
}

[[nodiscard]] Http2SubmitStatus toPublic(detail::Http2SubmitStatus status) noexcept {
    switch (status) {
        case detail::Http2SubmitStatus::kAccepted:
            return Http2SubmitStatus::kAccepted;
        case detail::Http2SubmitStatus::kClosed:
            return Http2SubmitStatus::kClosed;
        case detail::Http2SubmitStatus::kInvalidState:
            return Http2SubmitStatus::kInvalidState;
        case detail::Http2SubmitStatus::kInvalidMessage:
            return Http2SubmitStatus::kInvalidMessage;
        case detail::Http2SubmitStatus::kPeerCapabilityUnavailable:
            return Http2SubmitStatus::kPeerCapabilityUnavailable;
    }
    std::terminate();
}

[[nodiscard]] Http2DataSubmitStatus toPublic(detail::Http2DataSubmitStatus status) noexcept {
    switch (status) {
        case detail::Http2DataSubmitStatus::kAccepted:
            return Http2DataSubmitStatus::kAccepted;
        case detail::Http2DataSubmitStatus::kQueued:
            return Http2DataSubmitStatus::kQueued;
        case detail::Http2DataSubmitStatus::kBackpressured:
            return Http2DataSubmitStatus::kBackpressured;
        case detail::Http2DataSubmitStatus::kExpectationPending:
            return Http2DataSubmitStatus::kExpectationPending;
        case detail::Http2DataSubmitStatus::kClosed:
            return Http2DataSubmitStatus::kClosed;
        case detail::Http2DataSubmitStatus::kInvalidState:
            return Http2DataSubmitStatus::kInvalidState;
        case detail::Http2DataSubmitStatus::kContentLengthExceeded:
            return Http2DataSubmitStatus::kContentLengthExceeded;
        case detail::Http2DataSubmitStatus::kContentLengthIncomplete:
            return Http2DataSubmitStatus::kContentLengthIncomplete;
    }
    std::terminate();
}

[[nodiscard]] Http2RequestContentReleaseStatus toPublic(
    detail::Http2RequestContentReleaseStatus status) noexcept {
    switch (status) {
        case detail::Http2RequestContentReleaseStatus::kReleased:
            return Http2RequestContentReleaseStatus::kReleased;
        case detail::Http2RequestContentReleaseStatus::kNotPending:
            return Http2RequestContentReleaseStatus::kNotPending;
        case detail::Http2RequestContentReleaseStatus::kClosed:
            return Http2RequestContentReleaseStatus::kClosed;
    }
    std::terminate();
}

[[nodiscard]] Http2RequestHeadSubmitError toPublic(
    detail::Http2RequestHeadSubmitError error) noexcept {
    switch (error) {
        case detail::Http2RequestHeadSubmitError::kInvalidState:
            return Http2RequestHeadSubmitError::kInvalidState;
        case detail::Http2RequestHeadSubmitError::kConnectionNotStarted:
            std::terminate();
        case detail::Http2RequestHeadSubmitError::kConnectionUnavailable:
            return Http2RequestHeadSubmitError::kConnectionUnavailable;
        case detail::Http2RequestHeadSubmitError::kPeerStreamLimitReached:
            return Http2RequestHeadSubmitError::kPeerStreamLimitReached;
        case detail::Http2RequestHeadSubmitError::kLocalStreamCapacityReached:
            return Http2RequestHeadSubmitError::kLocalStreamCapacityReached;
        case detail::Http2RequestHeadSubmitError::kPeerCapabilityUnavailable:
            return Http2RequestHeadSubmitError::kPeerCapabilityUnavailable;
        case detail::Http2RequestHeadSubmitError::kInvalidMessage:
            return Http2RequestHeadSubmitError::kInvalidMessage;
    }
    std::terminate();
}

using RequestHeadSubmitOutcome = std::variant<std::uint32_t, Http2RequestHeadSubmitError>;

[[nodiscard]] RequestHeadSubmitOutcome pinSubmittedRequest(
    detail::Http2Connection& connection, const detail::Http2RequestHeadSubmitResult& result) {
    if (const auto* submitted = result.submitted()) {
        try {
            connection.pinStream(submitted->streamId());
        } catch (...) {
            const auto original = std::current_exception();
            try {
                (void)connection.submitReset(
                    submitted->streamId(), detail::Http2ErrorCode::kCancel);
                // Preserve the original pinning failure; this reset is rollback only.
                // NOLINTNEXTLINE(bugprone-empty-catch)
            } catch (...) {
            }
            std::rethrow_exception(original);
        }
        return submitted->streamId();
    }
    return toPublic(result.failure()->error());
}

[[nodiscard]] HttpClientResponseHead responseHeadFromStream(
    const detail::Http2StreamState& stream, std::pmr::memory_resource* resource) {
    const auto* status = stream.responseStatus();
    if (status == nullptr) {
        throw std::logic_error("HTTP/2 final response event has no status");
    }
    auto result =
        detail::HttpClientResponseHeadAccess::make(*status, HttpProtocolVersion::kHttp2, resource);
    auto& headers = detail::HttpClientResponseHeadAccess::headers(result);
    const auto count = stream.remoteInitialHeaderCount().value_or(stream.remoteHeaderCount());
    headers.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        const auto field = stream.remoteHeaderAt(index);
        headers.push_back(
            detail::HttpClientResponseHeaderAccess::make(field.name, field.value, resource));
    }
    return result;
}

}  // namespace

class Http2Connection::Impl final {
public:
    struct Storage final {
        Storage(std::pmr::memory_resource* requested, Http2Role publicRole)
            : resource(detail::httpPmrResourceOrDefault(requested)),
              role(publicRole),
              connection(resource, toInternal(publicRole)) {}

        std::pmr::memory_resource* resource;
        Http2Role role;
        detail::Http2Connection connection;
    };

    Impl(std::pmr::memory_resource* requested, Http2Role publicRole)
        : storage(std::make_unique<Storage>(requested, publicRole)),
          resource(storage->resource),
          role(publicRole),
          connection(storage->connection),
          endpoint(new detail::Http2ConnectionOwnerEndpoint(
              this, &Impl::abandonRequestThunk, &Impl::abandonCreditThunk)) {}

    ~Impl() {
        endpoint->detach();
        endpoint->retainStorage(storage.release(), &Impl::destroyStorage);
        endpoint->release();
    }

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    static void destroyStorage(void* raw) noexcept {
        delete static_cast<Storage*>(raw);
    }

    enum class DeferredReleaseKind : std::uint8_t { kAfterCredits, kAbandon };
    struct DeferredRelease final {
        std::uint32_t streamId{0};
        DeferredReleaseKind kind{DeferredReleaseKind::kAfterCredits};
    };
    struct DeferredCredit final {
        std::uint32_t streamId{0};
        std::uint32_t bytes{0};
    };

    static void abandonRequestThunk(void* target, std::uint32_t streamId) noexcept {
        static_cast<Impl*>(target)->abandonRequest(streamId);
    }
    static void abandonCreditThunk(
        void* target, std::uint32_t streamId, std::uint32_t bytes) noexcept {
        static_cast<Impl*>(target)->abandonCredit(streamId, bytes);
    }

    [[nodiscard]] DeferredRelease* deferred(std::uint32_t streamId) noexcept {
        for (std::size_t i = 0; i < deferredReleaseCount; ++i) {
            if (deferredReleases[i].streamId == streamId) {
                return &deferredReleases[i];
            }
        }
        return nullptr;
    }

    void defer(std::uint32_t streamId, DeferredReleaseKind kind) noexcept {
        if (auto* existing = deferred(streamId)) {
            if (kind == DeferredReleaseKind::kAbandon) {
                existing->kind = kind;
            }
            return;
        }
        if (deferredReleaseCount == deferredReleases.size()) {
            std::terminate();
        }
        deferredReleases[deferredReleaseCount++] = DeferredRelease{streamId, kind};
    }

    void eraseDeferred(std::uint32_t streamId) noexcept {
        for (std::size_t i = 0; i < deferredReleaseCount; ++i) {
            if (deferredReleases[i].streamId != streamId) {
                continue;
            }
            deferredReleases[i] = deferredReleases[--deferredReleaseCount];
            return;
        }
    }

    [[nodiscard]] DeferredCredit* deferredCredit(std::uint32_t streamId) noexcept {
        for (std::size_t i = 0; i < deferredCreditCount; ++i) {
            if (deferredCredits[i].streamId == streamId) {
                return &deferredCredits[i];
            }
        }
        return nullptr;
    }

    void deferCredit(std::uint32_t streamId, std::uint32_t bytes) noexcept {
        if (auto* existing = deferredCredit(streamId)) {
            if (bytes > (std::numeric_limits<std::uint32_t>::max)() - existing->bytes) {
                std::terminate();
            }
            existing->bytes += bytes;
            return;
        }
        if (deferredCreditCount == deferredCredits.size()) {
            std::terminate();
        }
        deferredCredits[deferredCreditCount++] = DeferredCredit{streamId, bytes};
    }

    void eraseDeferredCredit(std::uint32_t streamId) noexcept {
        for (std::size_t i = 0; i < deferredCreditCount; ++i) {
            if (deferredCredits[i].streamId != streamId) {
                continue;
            }
            deferredCredits[i] = deferredCredits[--deferredCreditCount];
            return;
        }
    }

    void releaseOwnerAfterCredits(std::uint32_t streamId) {
        auto* stream = connection.stream(streamId);
        if (stream != nullptr && stream->windowDebt() != 0) {
            defer(streamId, DeferredReleaseKind::kAfterCredits);
            return;
        }
        connection.unpinStream(streamId);
        eraseDeferred(streamId);
    }

    void abandonRequestChecked(std::uint32_t streamId) {
        auto* stream = connection.stream(streamId);
        if (stream == nullptr) {
            eraseDeferred(streamId);
            return;
        }
        if (!stream->isAborted()) {
            (void)connection.submitReset(streamId, detail::Http2ErrorCode::kCancel);
        }
        releaseOwnerAfterCredits(streamId);
    }

    void retryDeferredRelease(std::uint32_t streamId) noexcept {
        const auto* pending = deferred(streamId);
        if (pending == nullptr) {
            return;
        }
        if (pending->kind == DeferredReleaseKind::kAbandon) {
            try {
                abandonRequestChecked(streamId);
                // The deferred entry remains queued for the next noexcept retry.
                // NOLINTNEXTLINE(bugprone-empty-catch)
            } catch (...) {
            }
            return;
        }
        if (pending->kind == DeferredReleaseKind::kAfterCredits) {
            const auto* stream = connection.stream(streamId);
            if (stream != nullptr && stream->windowDebt() != 0) {
                return;
            }
        }
        try {
            connection.unpinStream(streamId);
            eraseDeferred(streamId);
            // The deferred entry remains queued for the next noexcept retry.
            // NOLINTNEXTLINE(bugprone-empty-catch)
        } catch (...) {
        }
    }

    void retryDeferredCredits() noexcept {
        std::size_t index = 0;
        while (index < deferredCreditCount) {
            const auto pending = deferredCredits[index];
            try {
                (void)connection.releaseReceivedData(pending.streamId, pending.bytes);
                eraseDeferredCredit(pending.streamId);
                retryDeferredRelease(pending.streamId);
            } catch (...) {
                ++index;
            }
        }
    }

    void retryDeferred() noexcept {
        retryDeferredCredits();
        std::size_t index = 0;
        while (index < deferredReleaseCount) {
            const auto streamId = deferredReleases[index].streamId;
            retryDeferredRelease(streamId);
            if (index < deferredReleaseCount && deferredReleases[index].streamId == streamId) {
                ++index;
            }
        }
    }

    void abandonRequest(std::uint32_t streamId) noexcept {
        try {
            abandonRequestChecked(streamId);
        } catch (...) {
            defer(streamId, DeferredReleaseKind::kAbandon);
        }
    }

    void abandonCredit(std::uint32_t streamId, std::uint32_t bytes) noexcept {
        try {
            (void)connection.releaseReceivedData(streamId, bytes);
            retryDeferredRelease(streamId);
        } catch (...) {
            deferCredit(streamId, bytes);
        }
    }

    std::unique_ptr<Storage> storage;
    std::pmr::memory_resource* resource;
    Http2Role role;
    detail::Http2Connection& connection;
    detail::Http2ConnectionOwnerEndpoint* endpoint;
    std::array<DeferredRelease, detail::Http2LocalSettings::kMaxConcurrentStreams>
        deferredReleases{};
    std::size_t deferredReleaseCount{0};
    std::array<DeferredCredit, detail::Http2LocalSettings::kMaxConcurrentStreams> deferredCredits{};
    std::size_t deferredCreditCount{0};
};

Http2ReceivedDataCredit::Http2ReceivedDataCredit(detail::Http2ConnectionOwnerEndpoint* endpoint,
    std::uint32_t streamId, std::uint32_t bytes) noexcept
    : endpoint_(endpoint),
      streamId_(streamId),
      bytes_(bytes) {
    endpoint_->retain();
}

Http2ReceivedDataCredit::~Http2ReceivedDataCredit() {
    if (endpoint_ == nullptr) {
        return;
    }
    endpoint_->abandonCredit(streamId_, bytes_);
    endpoint_->release();
}

Http2ReceivedDataCredit::Http2ReceivedDataCredit(Http2ReceivedDataCredit&& other) noexcept
    : endpoint_(std::exchange(other.endpoint_, nullptr)),
      streamId_(std::exchange(other.streamId_, 0)),
      bytes_(std::exchange(other.bytes_, 0)) {}

Http2RequestHeadEvent::Http2RequestHeadEvent(detail::Http2ConnectionOwnerEndpoint* endpoint,
    std::uint32_t streamId, HttpRequest request, HttpRequestExpectations expectations,
    HttpRequestContentIndication content) noexcept
    : streamId_(streamId),
      request_(request),
      expectations_(expectations),
      content_(content),
      endpoint_(endpoint) {
    endpoint_->retain();
}

Http2RequestHeadEvent::~Http2RequestHeadEvent() {
    if (endpoint_ == nullptr) {
        return;
    }
    endpoint_->abandonRequest(streamId_);
    endpoint_->release();
}

Http2RequestHeadEvent::Http2RequestHeadEvent(Http2RequestHeadEvent&& other) noexcept
    : streamId_(std::exchange(other.streamId_, 0)),
      request_(other.request_),
      expectations_(other.expectations_),
      content_(other.content_),
      endpoint_(std::exchange(other.endpoint_, nullptr)) {}

Http2Connection::Http2Connection(std::pmr::memory_resource* resource, Http2Role role)
    : impl_(std::make_unique<Impl>(resource, role)) {
    impl_->connection.beginConnection();
}
Http2Connection Http2Connection::server(Http2ConnectionOptions options) {
    return Http2Connection(options.resource, Http2Role::kServer);
}
Http2Connection Http2Connection::client(Http2ConnectionOptions options) {
    return Http2Connection(options.resource, Http2Role::kClient);
}
Http2Connection::~Http2Connection() = default;
Http2Connection::Http2Connection(Http2Connection&&) noexcept = default;
Http2Connection& Http2Connection::operator=(Http2Connection&&) noexcept = default;

Http2Role Http2Connection::role() const noexcept {
    return impl_->role;
}
Http2FeedResult Http2Connection::feed(std::string_view input) {
    impl_->retryDeferred();
    switch (impl_->connection.feed(input)) {
        case detail::Http2FeedResult::kConnectionNotStarted:
            std::terminate();
        case detail::Http2FeedResult::kEventsPending:
            return Http2FeedResult::kEventsPending;
        case detail::Http2FeedResult::kAccepted:
            return Http2FeedResult::kAccepted;
        case detail::Http2FeedResult::kNeedInput:
            return Http2FeedResult::kNeedInput;
        case detail::Http2FeedResult::kProtocolFailure:
            return Http2FeedResult::kProtocolFailure;
    }
    std::terminate();
}

std::optional<Http2Event> Http2Connection::nextEvent() {
    impl_->retryDeferred();
    auto* event = impl_->connection.peekEvent();
    if (event == nullptr) {
        return std::nullopt;
    }
    if (auto* value = event->informationalHead()) {
        const auto streamId = value->streamId();
        const auto signal = value->requestContentSignal();
        auto result = Http2Event::informationalHead(streamId, std::move(*value).takeHead(), signal);
        impl_->connection.consumeEvent();
        return std::optional<Http2Event>(std::move(result));
    }
    if (const auto* value = event->messageHead()) {
        if (impl_->role == Http2Role::kServer) {
            auto* stream = impl_->connection.stream(value->streamId());
            if (stream == nullptr) {
                throw std::logic_error("HTTP/2 request event has no stream");
            }
            auto request = detail::HttpRequestAccess::make();
            const auto build =
                detail::Http2RequestBuilder::build(*stream, request, impl_->resource, {});
            if (build.built() == nullptr) {
                (void)impl_->connection.submitReset(
                    value->streamId(), detail::Http2ErrorCode::kCancel);
                impl_->connection.consumeEvent();
                throw std::logic_error("validated HTTP/2 request cannot be materialized");
            }
            impl_->connection.pinStream(value->streamId());
            auto result = Http2Event::requestHead(impl_->endpoint, value->streamId(), request,
                stream->requestExpectations(), stream->requestContentIndication());
            impl_->connection.consumeEvent();
            return std::optional<Http2Event>(std::move(result));
        }
        auto* stream = impl_->connection.stream(value->streamId());
        if (stream == nullptr) {
            throw std::logic_error("HTTP/2 response event has no stream");
        }
        auto head = responseHeadFromStream(*stream, impl_->resource);
        auto result = Http2Event::responseHead(
            value->streamId(), std::move(head), value->requestContentSignal());
        impl_->connection.consumeEvent();
        return std::optional<Http2Event>(std::move(result));
    }
    if (const auto* value = event->messageBodyChunk()) {
        auto result = Http2Event::messageBodyChunk(
            impl_->endpoint, value->streamId(), value->bytes(), value->flowControlBytes());
        impl_->connection.consumeEvent();
        return std::optional<Http2Event>(std::move(result));
    }
    if (const auto* value = event->messageEnd()) {
        auto result = Http2Event::messageEnd(value->streamId());
        if (impl_->role == Http2Role::kClient) {
            impl_->releaseOwnerAfterCredits(value->streamId());
        }
        impl_->connection.consumeEvent();
        return std::optional<Http2Event>(std::move(result));
    }
    if (const auto* value = event->tunnelData()) {
        auto result = Http2Event::tunnelData(
            impl_->endpoint, value->streamId(), value->bytes(), value->flowControlBytes());
        impl_->connection.consumeEvent();
        return std::optional<Http2Event>(std::move(result));
    }
    if (const auto* value = event->tunnelEnd()) {
        auto result = Http2Event::tunnelEnd(value->streamId());
        if (impl_->role == Http2Role::kClient) {
            impl_->releaseOwnerAfterCredits(value->streamId());
        }
        impl_->connection.consumeEvent();
        return std::optional<Http2Event>(std::move(result));
    }
    if (const auto* value = event->streamClosed()) {
        auto result = Http2Event::streamClosed(
            value->streamId(), toPublic(value->source()), toPublic(value->error()));
        if (impl_->role == Http2Role::kClient) {
            impl_->releaseOwnerAfterCredits(value->streamId());
        }
        impl_->connection.consumeEvent();
        return std::optional<Http2Event>(std::move(result));
    }
    if (const auto* value = event->requestUnprocessed()) {
        auto result = Http2Event::requestUnprocessed(value->streamId());
        if (impl_->role == Http2Role::kClient) {
            impl_->releaseOwnerAfterCredits(value->streamId());
        }
        impl_->connection.consumeEvent();
        return std::optional<Http2Event>(std::move(result));
    }
    const auto* value = event->goaway();
    auto result = Http2Event::goaway(value->lastStreamId(), toPublic(value->error()));
    impl_->connection.consumeEvent();
    return std::optional<Http2Event>(std::move(result));
}

std::string_view Http2Connection::pendingOutput() const& noexcept {
    impl_->retryDeferred();
    return impl_->connection.pendingOutput();
}
Http2OutputConsumeStatus Http2Connection::consumeOutput(std::size_t bytes) noexcept {
    impl_->retryDeferred();
    return toPublic(impl_->connection.consumeOutput(bytes));
}
void Http2Connection::takeOutput(std::pmr::string& output) {
    impl_->retryDeferred();
    impl_->connection.takeOutput(output);
}
bool Http2Connection::wantsWrite() const noexcept {
    impl_->retryDeferred();
    return impl_->connection.wantsWrite();
}

Http2RequestHeadSubmitResult Http2Connection::submitRequestHead(
    const Http2RegularRequestHeadView& request) {
    std::optional<std::string_view> authority;
    if (request.authority) {
        authority = request.authority->view();
    }
    const auto result = impl_->connection.submitRegularRequestHead(request.method.view(),
        request.scheme.view(), authority, request.target.view(),
        static_cast<std::span<const HttpHeaderView>>(request.headers), toInternal(request.content),
        request.expectation);
    const auto outcome = pinSubmittedRequest(impl_->connection, result);
    if (const auto* streamId = std::get_if<std::uint32_t>(&outcome)) {
        return Http2RequestHeadSubmitResult::makeSubmitted(*streamId);
    }
    return Http2RequestHeadSubmitResult::makeFailure(
        std::get<Http2RequestHeadSubmitError>(outcome));
}

Http2RequestHeadSubmitResult Http2Connection::submitRequestHead(
    const Http2ConnectRequestHeadView& request) {
    const auto result = impl_->connection.submitConnectRequestHead(
        request.authority.view(), static_cast<std::span<const HttpHeaderView>>(request.headers));
    const auto outcome = pinSubmittedRequest(impl_->connection, result);
    if (const auto* streamId = std::get_if<std::uint32_t>(&outcome)) {
        return Http2RequestHeadSubmitResult::makeSubmitted(*streamId);
    }
    return Http2RequestHeadSubmitResult::makeFailure(
        std::get<Http2RequestHeadSubmitError>(outcome));
}

Http2RequestHeadSubmitResult Http2Connection::submitRequestHead(
    const Http2ExtendedConnectRequestHeadView& request) {
    const auto result = impl_->connection.submitExtendedConnectRequestHead(request.protocol.view(),
        request.scheme.view(), request.authority.view(), request.target.view(),
        static_cast<std::span<const HttpHeaderView>>(request.headers));
    const auto outcome = pinSubmittedRequest(impl_->connection, result);
    if (const auto* streamId = std::get_if<std::uint32_t>(&outcome)) {
        return Http2RequestHeadSubmitResult::makeSubmitted(*streamId);
    }
    return Http2RequestHeadSubmitResult::makeFailure(
        std::get<Http2RequestHeadSubmitError>(outcome));
}

Http2DataSubmitStatus Http2Connection::submitData(
    std::uint32_t streamId, std::string_view bytes, Http2EndStream endStream) {
    return toPublic(impl_->connection.submitData(streamId, bytes, toInternal(endStream)));
}
Http2RequestContentReleaseStatus Http2Connection::releaseRequestContent(
    std::uint32_t streamId) noexcept {
    return toPublic(impl_->connection.releaseRequestContent(streamId));
}
Http2SubmitStatus Http2Connection::submitInterimResponseHead(
    std::uint32_t streamId, const HttpInterimResponseHead& response) {
    return toPublic(impl_->connection.submitInterimResponseHead(streamId, response));
}

Http2SubmitStatus Http2Connection::submitBufferedResponse(
    std::uint32_t streamId, const HttpResponse& response) {
    auto* stream = impl_->connection.stream(streamId);
    if (stream == nullptr) {
        return Http2SubmitStatus::kClosed;
    }
    const auto& body = detail::responseBody(response);
    if (body.file().has_value()) {
        return Http2SubmitStatus::kInvalidMessage;
    }
    const auto plan = detail::httpBufferedResponseWritePlan(stream->requestKnownMethod(), response);
    const auto result = impl_->connection.submitResponseHead(streamId, response, plan);
    if (const auto* failure = result.failure()) {
        switch (failure->error()) {
            case detail::Http2ResponseHeadSubmitError::kClosed:
                return Http2SubmitStatus::kClosed;
            case detail::Http2ResponseHeadSubmitError::kInvalidState:
            case detail::Http2ResponseHeadSubmitError::kResponsePlanMismatch:
                return Http2SubmitStatus::kInvalidState;
            case detail::Http2ResponseHeadSubmitError::kInvalidMessage:
                return Http2SubmitStatus::kInvalidMessage;
        }
    }
    if (plan.sendBody()) {
        const auto data = impl_->connection.submitData(
            streamId, body.bytes(), detail::Http2EndStream::kEndStream);
        if (data != detail::Http2DataSubmitStatus::kAccepted &&
            data != detail::Http2DataSubmitStatus::kQueued) {
            return Http2SubmitStatus::kInvalidState;
        }
    }
    return Http2SubmitStatus::kAccepted;
}

Http2SubmitStatus Http2Connection::submitReset(std::uint32_t streamId, Http2ErrorCode error) {
    const auto status = impl_->connection.submitReset(streamId, toInternal(error));
    if (status == detail::Http2SubmitStatus::kAccepted && impl_->role == Http2Role::kClient) {
        // Owner-originated resets deliberately produce no terminal event. The
        // public client facade must therefore release the pin established by
        // submitRequestHead() here rather than waiting for nextEvent().
        impl_->releaseOwnerAfterCredits(streamId);
    }
    return toPublic(status);
}
Http2ReceivedDataAcknowledgeStatus Http2Connection::acknowledge(Http2ReceivedDataCredit&& credit) {
    if (!credit.valid() || credit.endpoint_ != impl_->endpoint) {
        return Http2ReceivedDataAcknowledgeStatus::kInvalidCredit;
    }
    const auto streamId = credit.streamId_;
    const auto bytes = credit.bytes_;
    const bool acknowledged = impl_->connection.releaseReceivedData(streamId, bytes);
    auto* endpoint = std::exchange(credit.endpoint_, nullptr);
    credit.streamId_ = 0;
    credit.bytes_ = 0;
    endpoint->release();
    if (acknowledged) {
        impl_->retryDeferredRelease(streamId);
    }
    return acknowledged ? Http2ReceivedDataAcknowledgeStatus::kAcknowledged
                        : Http2ReceivedDataAcknowledgeStatus::kClosed;
}
bool Http2Connection::hasQueuedData(std::uint32_t streamId) const noexcept {
    return impl_->connection.hasQueuedData(streamId);
}
std::span<const std::uint32_t> Http2Connection::takeDrainedDataStreams() & noexcept {
    return impl_->connection.takeDrainedDataStreams();
}
Http2ServerRequestReleaseStatus Http2Connection::release(Http2RequestHeadEvent&& request) {
    if (request.endpoint_ == nullptr || request.streamId_ == 0 ||
        request.endpoint_ != impl_->endpoint) {
        return Http2ServerRequestReleaseStatus::kInvalidLease;
    }
    if (impl_->connection.stream(request.streamId_) == nullptr) {
        auto* endpoint = std::exchange(request.endpoint_, nullptr);
        request.streamId_ = 0;
        detail::HttpRequestAccess::reset(request.request_);
        endpoint->release();
        return Http2ServerRequestReleaseStatus::kClosed;
    }
    impl_->releaseOwnerAfterCredits(request.streamId_);
    auto* endpoint = std::exchange(request.endpoint_, nullptr);
    request.streamId_ = 0;
    detail::HttpRequestAccess::reset(request.request_);
    endpoint->release();
    return Http2ServerRequestReleaseStatus::kReleased;
}
void Http2Connection::beginDrain() {
    impl_->connection.beginDrain();
}
bool Http2Connection::draining() const noexcept {
    return impl_->connection.draining();
}
std::optional<Http2ErrorCode> Http2Connection::connectionError() const noexcept {
    const auto error = impl_->connection.connectionError();
    return error ? std::optional<Http2ErrorCode>(toPublic(*error)) : std::nullopt;
}

}  // namespace ruvia
