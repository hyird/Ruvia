#pragma once

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <string_view>
#include <utility>

#include "ruvia/http/HttpKnownMethod.h"
#include "ruvia/http/detail/http2/message/Http2LocalContentState.h"
#include "ruvia/http/detail/http2/message/Http2RemoteContentState.h"
#include "ruvia/http/detail/http2/flow/Http2ReceiveWindowCredit.h"
#include "ruvia/http/detail/http2/stream/Http2StreamFlowControl.h"
#include "ruvia/http/detail/http2/stream/Http2StreamHeaderBlocks.h"
#include "ruvia/http/detail/http2/stream/Http2StreamLifecycle.h"
#include "ruvia/http/detail/http2/stream/Http2StreamRequestData.h"
#include "ruvia/http/detail/http2/stream/Http2StreamRequestState.h"
#include "ruvia/http/detail/http2/stream/Http2TunnelState.h"
#include "ruvia/http/detail/field/HeaderTokenUtils.h"
#include "ruvia/http/detail/field/HttpExpectations.h"

namespace ruvia::detail {

class Http2StreamHeaderDecodeTransaction;

enum class Http2LocalRequestContentGate : std::uint8_t {
    kOpen,
    kAwaitingContinue,
    kCanceled,
};

class Http2StreamState final {
    friend class Http2StreamHeaderDecodeTransaction;

    std::uint32_t id_{0};
    Http2RemoteContentState remoteContent_;
    Http2LocalContentState localContent_;
    Http2StreamLifecycle lifecycle_;
    HttpRequestExpectations expectations_;
    Http2StreamRequestState requestState_;
    Http2TunnelState tunnelState_;
    Http2StreamFlowControl flowControl_;
    std::uint32_t windowDebt_{0};
    Http2ReceiveWindowCredit receiveWindowCredit_;
    Http2StreamHeaderBlocks headerBlocks_;
    Http2StreamRequestData messageData_;
    std::optional<std::size_t> remoteInitialHeaderCount_;
    Http2LocalRequestContentGate localRequestContentGate_{Http2LocalRequestContentGate::kOpen};

public:
    explicit Http2StreamState(std::uint32_t streamId, std::pmr::memory_resource* resource)
        : id_(streamId),
          headerBlocks_(resource),
          messageData_(resource) {}

    [[nodiscard]] std::uint32_t id() const noexcept {
        return id_;
    }

    void setSendWindow(std::int32_t window) noexcept {
        flowControl_.setSendWindow(window);
    }

    [[nodiscard]] bool addSendWindow(std::int64_t delta) noexcept {
        return flowControl_.addSendWindow(delta);
    }

    [[nodiscard]] std::int32_t sendWindow() const noexcept {
        return flowControl_.sendWindow();
    }

    void consumeSendWindow(std::size_t bytes) noexcept {
        flowControl_.consumeSend(bytes);
    }

    [[nodiscard]] bool consumeReceiveWindow(std::int32_t bytes) noexcept {
        return flowControl_.consumeReceive(bytes);
    }

    [[nodiscard]] std::int32_t receiveWindow() const noexcept {
        return flowControl_.receiveWindow();
    }

    void addWindowDebt(std::uint32_t bytes) noexcept {
        windowDebt_ += bytes;
    }

    [[nodiscard]] std::uint32_t windowDebt() const noexcept {
        return windowDebt_;
    }

    [[nodiscard]] std::uint32_t takeWindowDebt() noexcept {
        return std::exchange(windowDebt_, 0);
    }

    [[nodiscard]] bool takeWindowDebt(std::uint32_t bytes) noexcept {
        if (bytes == 0 || bytes > windowDebt_) {
            return false;
        }
        windowDebt_ -= bytes;
        return true;
    }

    [[nodiscard]] Http2ReceiveWindowCredit& receiveWindowCredit() & noexcept {
        return receiveWindowCredit_;
    }
    [[nodiscard]] Http2ReceiveWindowCredit& receiveWindowCredit() && = delete;

    void restoreReceiveWindow(std::int32_t bytes) noexcept {
        flowControl_.restoreReceive(bytes);
    }

    [[nodiscard]] std::pmr::string& remoteHeaderBlock() & noexcept {
        return headerBlocks_.remote();
    }
    [[nodiscard]] std::pmr::string& remoteHeaderBlock() && = delete;

    [[nodiscard]] const std::pmr::string& remoteHeaderBlock() const& noexcept {
        return headerBlocks_.remote();
    }
    [[nodiscard]] const std::pmr::string& remoteHeaderBlock() const&& = delete;

    [[nodiscard]] std::pmr::string& localHeaderBlock() & noexcept {
        return headerBlocks_.local();
    }
    [[nodiscard]] std::pmr::string& localHeaderBlock() && = delete;

    [[nodiscard]] const std::pmr::string& localHeaderBlock() const& noexcept {
        return headerBlocks_.local();
    }
    [[nodiscard]] const std::pmr::string& localHeaderBlock() const&& = delete;

    [[nodiscard]] bool declareRemoteContentLength(std::size_t value) noexcept {
        return remoteContent_.declareKnownLength(value);
    }

    [[nodiscard]] bool selectRemoteContentMetadataOnly() noexcept {
        return remoteContent_.selectMetadataOnly();
    }

    [[nodiscard]] Http2RemoteContentAccountingResult accountRemoteContent(std::size_t bytes) noexcept {
        return remoteContent_.account(bytes);
    }

    [[nodiscard]] const Http2RemoteContentState& remoteContent() const& noexcept {
        return remoteContent_;
    }
    [[nodiscard]] const Http2RemoteContentState& remoteContent() const&& = delete;

    void beginLocalContentForbidden() noexcept {
        localContent_.beginForbidden();
    }

    void beginLocalContentUnbounded() noexcept {
        localContent_.beginUnbounded();
    }

    void beginLocalContentKnownLength(std::uint64_t length) noexcept {
        localContent_.beginKnownLength(length);
    }

    [[nodiscard]] Http2LocalContentCheck checkLocalContentAccept(std::size_t bytes, bool terminal) const noexcept {
        return localContent_.checkAccept(bytes, terminal);
    }

    void acceptLocalContent(std::size_t bytes) noexcept {
        localContent_.accept(bytes);
    }

    void commitLocalContent(std::size_t bytes) noexcept {
        localContent_.commit(bytes);
    }

    [[nodiscard]] const Http2LocalContentState& localContent() const& noexcept {
        return localContent_;
    }
    [[nodiscard]] const Http2LocalContentState& localContent() const&& = delete;

    [[nodiscard]] bool isAborted() const noexcept {
        return lifecycle_.aborted();
    }

    [[nodiscard]] const Http2LocalSendState& localSend() const& noexcept {
        return lifecycle_.localSend();
    }
    [[nodiscard]] const Http2LocalSendState& localSend() const&& = delete;

    [[nodiscard]] const Http2RemoteReceiveState& remoteReceive() const& noexcept {
        return lifecycle_.remoteReceive();
    }
    [[nodiscard]] const Http2RemoteReceiveState& remoteReceive() const&& = delete;

    [[nodiscard]] bool queued() const noexcept {
        return lifecycle_.queued();
    }

    [[nodiscard]] bool dispatchStarted() const noexcept {
        return lifecycle_.dispatchStarted();
    }

    [[nodiscard]] bool holdPeerConcurrencySlot() noexcept {
        return lifecycle_.holdPeerConcurrencySlot();
    }

    [[nodiscard]] bool releasePeerConcurrencySlot() noexcept {
        return lifecycle_.releasePeerConcurrencySlot();
    }

    [[nodiscard]] bool abort(Http2StreamCloseSource source) noexcept {
        return lifecycle_.abort(source);
    }

    [[nodiscard]] bool recordRemoteHeadEndStream() noexcept {
        return lifecycle_.recordRemoteHeadEndStream();
    }

    [[nodiscard]] bool rollbackRemoteHeadEndStreamForRetry() noexcept {
        return lifecycle_.rollbackRemoteHeadEndStream();
    }

    [[nodiscard]] bool finalizeRemoteContentHead() noexcept {
        if (tunnelState_.notConnect() == nullptr) {
            return false;
        }
        return lifecycle_.finalizeRemoteContentHead();
    }

    [[nodiscard]] bool finalizeRemoteConnectHead() noexcept {
        if (tunnelState_.pending() == nullptr) {
            return false;
        }
        return lifecycle_.finalizeRemoteConnectHead();
    }

    [[nodiscard]] bool finishRemoteContent() noexcept {
        return lifecycle_.finishRemoteContent();
    }

    [[nodiscard]] bool finishRemotePendingConnect() noexcept {
        if (tunnelState_.pending() == nullptr) {
            return false;
        }
        return lifecycle_.finishRemotePendingConnect();
    }

    [[nodiscard]] bool finishRemoteTunnel() noexcept {
        if (tunnelState_.open() == nullptr) {
            return false;
        }
        return lifecycle_.finishRemoteTunnel();
    }

    [[nodiscard]] bool finishRemoteRejectedConnect() noexcept {
        if (tunnelState_.rejected() == nullptr) {
            return false;
        }
        return lifecycle_.finishRemoteRejectedConnect();
    }

    [[nodiscard]] bool beginLocalRequestContent() noexcept {
        return lifecycle_.beginLocalRequestContent();
    }

    void awaitRequestContinue() noexcept {
        localRequestContentGate_ = Http2LocalRequestContentGate::kAwaitingContinue;
    }

    [[nodiscard]] bool requestContinuePending() const noexcept {
        return localRequestContentGate_ == Http2LocalRequestContentGate::kAwaitingContinue;
    }

    [[nodiscard]] bool releaseRequestContinue() noexcept {
        if (!requestContinuePending()) {
            return false;
        }
        localRequestContentGate_ = Http2LocalRequestContentGate::kOpen;
        return true;
    }

    [[nodiscard]] bool cancelPendingRequestContinue() noexcept {
        if (!requestContinuePending()) {
            return false;
        }
        localRequestContentGate_ = Http2LocalRequestContentGate::kCanceled;
        return true;
    }

    [[nodiscard]] bool requestContentCanceled() const noexcept {
        return localRequestContentGate_ == Http2LocalRequestContentGate::kCanceled;
    }

    [[nodiscard]] bool beginLocalResponseContent() noexcept {
        return lifecycle_.beginLocalResponseContent();
    }

    [[nodiscard]] bool beginLocalResponseTrailersOnly() noexcept {
        return lifecycle_.beginLocalResponseTrailersOnly();
    }

    [[nodiscard]] bool commitLocalHeadEndStream() noexcept {
        return lifecycle_.commitLocalHeadEndStream();
    }

    [[nodiscard]] bool beginLocalConnectRequest() noexcept {
        if (tunnelState_.pending() == nullptr) {
            return false;
        }
        return lifecycle_.beginLocalConnectRequest();
    }

    [[nodiscard]] bool openLocalConnectTunnel() noexcept {
        if (tunnelState_.open() == nullptr) {
            return false;
        }
        return lifecycle_.openLocalConnectTunnel();
    }

    [[nodiscard]] bool rejectLocalConnect() noexcept {
        if (tunnelState_.rejected() == nullptr) {
            return false;
        }
        return lifecycle_.rejectLocalConnect();
    }

    [[nodiscard]] bool queueLocalEndStream() noexcept {
        return lifecycle_.queueLocalEndStream();
    }

    [[nodiscard]] bool commitLocalEndStream() noexcept {
        return lifecycle_.commitLocalEndStream();
    }

    [[nodiscard]] bool tryMarkQueued() noexcept {
        return lifecycle_.tryMarkQueued();
    }

    void clearQueued() noexcept {
        lifecycle_.clearQueued();
    }

    [[nodiscard]] bool tryStartDispatch() noexcept {
        return lifecycle_.tryStartDispatch();
    }

    void parseRequestExpectationField(std::string_view value) noexcept {
        expectations_.parseField(value);
    }

    [[nodiscard]] HttpRequestExpectations requestExpectations() const noexcept {
        return expectations_;
    }

    [[nodiscard]] HttpRequestContentIndication requestContentIndication() const noexcept {
        const auto* knownLength = remoteContent_.allowedKnownLength();
        // An open receive half can still be metadata-only, known-empty, or
        // awaiting only an empty END_STREAM frame.
        const bool contentCanFollow = lifecycle_.remoteReceive().contentOpen() != nullptr && (remoteContent_.allowedWithoutLength() != nullptr || (knownLength != nullptr && knownLength->receivedBytes() < knownLength->declaredLength()));
        return contentCanFollow ? HttpRequestContentIndication::kWillFollow : HttpRequestContentIndication::kNoContent;
    }

    [[nodiscard]] HttpServerExpectationPlan expectationPlan(HttpUnsupportedExpectationPolicy unsupportedPolicy) const noexcept {
        return expectations_.serverPlan(requestContentIndication(), unsupportedPolicy);
    }

    [[nodiscard]] std::string_view requestMethod() const& noexcept {
        return messageData_.method();
    }
    [[nodiscard]] std::string_view requestMethod() const&& = delete;

    [[nodiscard]] HttpKnownMethod requestKnownMethod() const noexcept {
        return messageData_.knownMethod();
    }

    void assignRequestMethod(std::string_view method) {
        messageData_.assignMethod(method);
    }

    [[nodiscard]] std::string_view requestAuthority() const& noexcept {
        return messageData_.authority();
    }
    [[nodiscard]] std::string_view requestAuthority() const&& = delete;

    void assignRequestAuthority(std::string_view value) {
        messageData_.assignAuthority(value);
    }

    [[nodiscard]] std::string_view requestPath() const& noexcept {
        return messageData_.path();
    }
    [[nodiscard]] std::string_view requestPath() const&& = delete;

    void assignRequestPath(std::string_view value) {
        messageData_.assignPath(value);
    }

    [[nodiscard]] std::string_view requestProtocol() const& noexcept {
        return messageData_.protocol();
    }
    [[nodiscard]] std::string_view requestProtocol() const&& = delete;

    [[nodiscard]] std::string_view requestCookie() const& noexcept {
        return messageData_.cookie();
    }
    [[nodiscard]] std::string_view requestCookie() const&& = delete;

    [[nodiscard]] bool appendRequestCookieHeaderValue(std::string_view value, bool hasExistingCookie) {
        return messageData_.appendCookieHeaderValue(value, hasExistingCookie);
    }

    [[nodiscard]] bool remoteHeadersFull() const noexcept {
        return messageData_.headersFull();
    }

    [[nodiscard]] std::size_t remoteHeaderCount() const noexcept {
        return messageData_.headerCount();
    }

    [[nodiscard]] Http2StoredHeaderView remoteHeaderAt(std::size_t index) const& noexcept {
        return messageData_.headerAt(index);
    }
    [[nodiscard]] Http2StoredHeaderView remoteHeaderAt(std::size_t) const&& = delete;

    [[nodiscard]] bool appendRemoteHeader(std::string_view name, std::string_view value, RequestHeaderKind kind) {
        return messageData_.appendHeader(name, value, kind);
    }

    [[nodiscard]] std::optional<std::size_t> remoteInitialHeaderCount() const noexcept {
        return remoteInitialHeaderCount_;
    }

    [[nodiscard]] bool setRemoteInitialHeaderCount(std::size_t count) noexcept {
        if (remoteInitialHeaderCount_) return false;
        remoteInitialHeaderCount_ = count;
        return true;
    }

    [[nodiscard]] bool hasMethod() const noexcept {
        return !requestMethod().empty();
    }

    [[nodiscard]] bool hasProtocol() const noexcept {
        return requestState_.hasProtocol();
    }

    [[nodiscard]] bool protocolIsWebSocket() const noexcept {
        return httpAsciiEqualsIgnoreCase(requestProtocol(), "websocket");
    }

    void setProtocol(std::string_view value) {
        messageData_.assignProtocol(value);
        requestState_.markProtocol();
    }

    [[nodiscard]] bool hasScheme() const noexcept {
        return requestState_.hasScheme();
    }

    [[nodiscard]] std::string_view requestScheme() const& noexcept {
        return messageData_.scheme();
    }
    [[nodiscard]] std::string_view requestScheme() const&& = delete;

    void assignRequestScheme(std::string_view value) {
        messageData_.assignScheme(value);
    }

    void markScheme(std::uint16_t defaultPort) noexcept {
        requestState_.markScheme(defaultPort);
    }

    [[nodiscard]] std::uint16_t schemeDefaultPort() const noexcept {
        return requestState_.schemeDefaultPort();
    }

    [[nodiscard]] bool hasAuthority() const noexcept {
        return requestState_.hasAuthority();
    }

    void markAuthority() noexcept {
        requestState_.markAuthority();
    }

    [[nodiscard]] bool hasPath() const noexcept {
        return requestState_.hasPath();
    }

    void markPath() noexcept {
        requestState_.markPath();
    }

    [[nodiscard]] bool hasHost() const noexcept {
        return requestState_.hasHost();
    }

    void markHost() noexcept {
        requestState_.markHost();
    }

    [[nodiscard]] bool hasCookie() const noexcept {
        return requestState_.hasCookie();
    }

    void markCookie() noexcept {
        requestState_.markCookie();
    }

    [[nodiscard]] bool regularHeaderSeen() const noexcept {
        return requestState_.regularHeaderSeen();
    }

    void markRegularHeaderSeen() noexcept {
        requestState_.markRegularHeaderSeen();
    }

    [[nodiscard]] bool markSingletonRequestHeader(std::uint32_t bit) noexcept {
        return requestState_.markSingletonHeader(bit);
    }

    [[nodiscard]] bool hasSingletonRequestHeader(std::uint32_t bit) const noexcept {
        return requestState_.hasSingletonHeader(bit);
    }

    [[nodiscard]] bool markSingletonResponseHeader(std::uint32_t bit) noexcept {
        return requestState_.markSingletonHeader(bit);
    }

    [[nodiscard]] bool beginStandardConnect() noexcept {
        return tunnelState_.begin(Http2ConnectForm::kStandard);
    }

    [[nodiscard]] bool beginExtendedConnect() noexcept {
        return tunnelState_.begin(Http2ConnectForm::kExtended);
    }

    [[nodiscard]] bool acceptConnect() noexcept {
        if (tunnelState_.pending() == nullptr || !lifecycle_.canAcceptRemoteConnect()) {
            return false;
        }
        if (!tunnelState_.accept()) {
            return false;
        }
        return lifecycle_.acceptRemoteConnect();
    }

    [[nodiscard]] bool rejectConnect() noexcept {
        if (tunnelState_.pending() == nullptr || !lifecycle_.canRejectRemoteConnect()) {
            return false;
        }
        if (!tunnelState_.reject()) {
            return false;
        }
        return lifecycle_.rejectRemoteConnect();
    }

    [[nodiscard]] const Http2TunnelState& tunnel() const& noexcept {
        return tunnelState_;
    }
    [[nodiscard]] const Http2TunnelState& tunnel() const&& = delete;

    [[nodiscard]] const HttpStatusCode* responseStatus() const& noexcept {
        return requestState_.responseStatus();
    }
    [[nodiscard]] const HttpStatusCode* responseStatus() const&& = delete;

    [[nodiscard]] bool setResponseStatus(HttpStatusCode status) noexcept {
        return requestState_.setResponseStatus(status);
    }

    [[nodiscard]] std::uint8_t interimResponseCount() const noexcept {
        return requestState_.interimResponseCount();
    }

    void countInterimResponse() noexcept {
        requestState_.countInterimResponse();
    }
};

// Header callbacks write into stream-owned PMR storage and typed protocol state. A
// callback is allowed to throw (most commonly from a request-header allocation), so
// the complete field block needs the same strong exception guarantee as HPACK's
// connection-global dynamic table. The transaction swaps out the prior request data
// without allocating, snapshots the scalar protocol state, and restores both on
// destruction unless the decoder explicitly commits the block.
class Http2StreamHeaderDecodeTransaction final {
public:
    explicit Http2StreamHeaderDecodeTransaction(Http2StreamState& stream, bool isolateRequestData = true) noexcept
        : stream_(&stream),
          messageData_(stream.messageData_.resource()),
          remoteContent_(stream.remoteContent_),
          localContent_(stream.localContent_),
          lifecycle_(stream.lifecycle_),
          expectations_(stream.expectations_),
          requestState_(stream.requestState_),
          tunnelState_(stream.tunnelState_),
          remoteInitialHeaderCount_(stream.remoteInitialHeaderCount_),
          localRequestContentGate_(stream.localRequestContentGate_),
          isolateRequestData_(isolateRequestData),
          remoteHeadersCheckpoint_(stream.messageData_.headerCheckpoint()) {
        if (isolateRequestData_) {
            messageData_.swap(stream.messageData_);
        }
    }

    Http2StreamHeaderDecodeTransaction(const Http2StreamHeaderDecodeTransaction&) = delete;
    Http2StreamHeaderDecodeTransaction& operator=(const Http2StreamHeaderDecodeTransaction&) = delete;
    Http2StreamHeaderDecodeTransaction(Http2StreamHeaderDecodeTransaction&&) = delete;
    Http2StreamHeaderDecodeTransaction& operator=(Http2StreamHeaderDecodeTransaction&&) = delete;

    ~Http2StreamHeaderDecodeTransaction() {
        rollback();
    }

    void commit() noexcept {
        active_ = false;
    }

    void rollback() noexcept {
        if (!active_) {
            return;
        }
        if (isolateRequestData_) {
            stream_->messageData_.swap(messageData_);
        } else {
            stream_->messageData_.rollbackHeaders(remoteHeadersCheckpoint_);
        }
        stream_->remoteContent_ = remoteContent_;
        stream_->localContent_ = localContent_;
        stream_->lifecycle_ = lifecycle_;
        stream_->expectations_ = expectations_;
        stream_->requestState_ = requestState_;
        stream_->tunnelState_ = tunnelState_;
        stream_->remoteInitialHeaderCount_ = remoteInitialHeaderCount_;
        stream_->localRequestContentGate_ = localRequestContentGate_;
        active_ = false;
    }

private:
    Http2StreamState* stream_;
    Http2StreamRequestData messageData_;
    Http2RemoteContentState remoteContent_;
    Http2LocalContentState localContent_;
    Http2StreamLifecycle lifecycle_;
    HttpRequestExpectations expectations_;
    Http2StreamRequestState requestState_;
    Http2TunnelState tunnelState_;
    std::optional<std::size_t> remoteInitialHeaderCount_;
    Http2LocalRequestContentGate localRequestContentGate_;
    bool isolateRequestData_;
    Http2StreamRequestData::HeaderCheckpoint remoteHeadersCheckpoint_;
    bool active_{true};
};

}  // namespace ruvia::detail
