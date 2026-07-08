#pragma once

#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <string>
#include <string_view>

#include "Http2Frame.h"
#include "Http2StreamBodyAccounting.h"
#include "Http2StreamBodyQueue.h"
#include "Http2StreamFlowControl.h"
#include "Http2StreamHeaderBlocks.h"
#include "Http2StreamLifecycle.h"
#include "Http2StreamRequestData.h"
#include "Http2StreamRequestState.h"
#include "Http2StreamRouting.h"
#include "ruvia/http/HttpCommon.h"

namespace ruvia::detail {

class Http2StreamState final {
    std::uint32_t id_{0};
    Http2StreamBodyAccounting bodyAccounting_;
    Http2StreamLifecycle lifecycle_;
    Http2StreamBodyQueue bodyQueue_;
    Http2StreamRouting routing_;
    Http2StreamRequestState requestState_;
    Http2StreamFlowControl flowControl_;
    Http2StreamHeaderBlocks headerBlocks_;
    Http2StreamRequestData requestData_;

public:
    explicit Http2StreamState(std::uint32_t streamId, std::pmr::memory_resource* resource)
        : id_(streamId),
          bodyQueue_(resource),
          headerBlocks_(resource),
          requestData_(resource) {}

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

    void restoreReceiveWindow(std::int32_t bytes) noexcept {
        flowControl_.restoreReceive(bytes);
    }

    [[nodiscard]] std::pmr::string& requestHeaderBlock() noexcept {
        return headerBlocks_.request();
    }

    [[nodiscard]] const std::pmr::string& requestHeaderBlock() const noexcept {
        return headerBlocks_.request();
    }

    [[nodiscard]] std::pmr::string& responseHeaderBlock() noexcept {
        return headerBlocks_.response();
    }

    [[nodiscard]] const std::pmr::string& responseHeaderBlock() const noexcept {
        return headerBlocks_.response();
    }

    [[nodiscard]] bool setContentLength(std::size_t value) noexcept {
        return bodyAccounting_.setContentLength(value);
    }

    [[nodiscard]] bool hasContentLength() const noexcept {
        return bodyAccounting_.hasContentLength();
    }

    [[nodiscard]] std::size_t contentLength() const noexcept {
        return bodyAccounting_.contentLength();
    }

    void setReceivedBodyBytes(std::size_t value) noexcept {
        bodyAccounting_.setReceivedBytes(value);
    }

    [[nodiscard]] bool addReceivedBodyBytes(std::size_t value) noexcept {
        return bodyAccounting_.addReceivedBytes(value);
    }

    [[nodiscard]] std::size_t receivedBodyBytes() const noexcept {
        return bodyAccounting_.receivedBytes();
    }

    [[nodiscard]] bool receivedBodyExceedsContentLength() const noexcept {
        return bodyAccounting_.exceedsContentLength();
    }

    [[nodiscard]] bool bufferedBodyExceedsContentLength() const noexcept {
        return hasContentLength() && requestBodySize() > contentLength();
    }

    [[nodiscard]] bool bodyLengthComplete() const noexcept {
        return bodyAccounting_.lengthComplete();
    }

    [[nodiscard]] bool isReset() const noexcept {
        return lifecycle_.reset();
    }

    [[nodiscard]] bool bodyEnded() const noexcept {
        return lifecycle_.bodyEnded();
    }

    [[nodiscard]] bool peerEndStream() const noexcept {
        return lifecycle_.peerEndStream();
    }

    [[nodiscard]] bool queued() const noexcept {
        return lifecycle_.queued();
    }

    [[nodiscard]] bool dispatchStarted() const noexcept {
        return lifecycle_.dispatchStarted();
    }

    [[nodiscard]] Http2StreamCloseSource closeSource() const noexcept {
        return lifecycle_.closeSource();
    }

    void markReset(Http2StreamCloseSource source = Http2StreamCloseSource::kLocal) noexcept {
        lifecycle_.markReset(source);
    }

    void markClosed(Http2StreamCloseSource source) noexcept {
        lifecycle_.markClosed(source);
    }

    void markPeerEndStream() noexcept {
        lifecycle_.markPeerEndStream();
    }

    void markBodyEnded() noexcept {
        lifecycle_.markBodyEnded();
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

    void markDispatchStarted() noexcept {
        lifecycle_.markDispatchStarted();
    }

    [[nodiscard]] bool hasOverflowQueuedBodyChunk() const noexcept {
        return bodyQueue_.hasOverflowQueuedChunk();
    }

    void enqueueBodyChunk(std::string_view data) {
        bodyQueue_.enqueue(data);
    }

    void enqueueOwnedBodyChunk(std::pmr::string& body) {
        bodyQueue_.enqueueOwned(body);
    }

    void enqueueBufferedRequestBodyChunk() {
        requestData_.moveBodyToQueue(bodyQueue_);
    }

    [[nodiscard]] bool hasQueuedBodyChunk() const noexcept {
        return bodyQueue_.hasQueuedChunk();
    }

    [[nodiscard]] std::size_t queuedBodyBytes() const noexcept {
        return bodyQueue_.queuedBytes();
    }

    void compactBodyChunks() {
        bodyQueue_.compact();
    }

    [[nodiscard]] std::string_view popBodyChunk() {
        return bodyQueue_.pop();
    }

    void setBodyWaiter(std::coroutine_handle<> continuation) noexcept {
        bodyQueue_.setWaiter(continuation);
    }

    [[nodiscard]] std::coroutine_handle<> takeBodyWaiter() noexcept {
        return bodyQueue_.takeWaiter();
    }

    [[nodiscard]] RouteMatch& routeMatch() noexcept {
        return routing_.match();
    }

    [[nodiscard]] const RouteResolution& routeResolution() const noexcept {
        return routing_.resolution();
    }

    [[nodiscard]] RequestBodyMode bodyMode() const noexcept {
        return routing_.bodyMode();
    }

    [[nodiscard]] bool usesStreamRequestBody() const noexcept {
        return routing_.usesStreamRequestBody();
    }

    void resetRoutingToBuffered() noexcept {
        routing_.resetToBuffered();
    }

    void setRouteResolution(RouteResolution resolution) noexcept {
        routing_.setResolution(resolution);
    }

    void setBodyMode(RequestBodyMode bodyMode) noexcept {
        routing_.setBodyMode(bodyMode);
    }

    [[nodiscard]] HttpMethod requestMethod() const noexcept {
        return requestData_.method();
    }

    void setRequestMethod(HttpMethod method) noexcept {
        requestData_.setMethod(method);
    }

    [[nodiscard]] std::string_view requestAuthority() const noexcept {
        return requestData_.authority();
    }

    void assignRequestAuthority(std::string_view value) {
        requestData_.assignAuthority(value);
    }

    [[nodiscard]] std::string_view requestPath() const noexcept {
        return requestData_.path();
    }

    void assignRequestPath(std::string_view value) {
        requestData_.assignPath(value);
    }

    [[nodiscard]] std::string_view requestCookie() const noexcept {
        return requestData_.cookie();
    }

    [[nodiscard]] bool appendRequestCookieHeaderValue(
        std::string_view value,
        bool hasExistingCookie) {
        return requestData_.appendCookieHeaderValue(value, hasExistingCookie);
    }

    [[nodiscard]] std::size_t requestBodySize() const noexcept {
        return requestData_.bodySize();
    }

    [[nodiscard]] bool requestBodyEmpty() const noexcept {
        return requestData_.bodyEmpty();
    }

    [[nodiscard]] std::string_view requestBodyView() const noexcept {
        return requestData_.bodyView();
    }

    void appendRequestBody(std::string_view value) {
        requestData_.appendBody(value);
    }

    void assignRequestBody(std::string_view value) {
        requestData_.assignBody(value);
    }

    void clearRequestBody() noexcept {
        requestData_.clearBody();
    }

    [[nodiscard]] std::pmr::string& responseCompressionScratch() noexcept {
        return requestData_.responseCompressionScratch();
    }

    [[nodiscard]] bool requestHeadersFull() const noexcept {
        return requestData_.headersFull();
    }

    [[nodiscard]] std::size_t requestHeaderCount() const noexcept {
        return requestData_.headerCount();
    }

    [[nodiscard]] Http2StoredHeaderView requestHeaderAt(std::size_t index) const noexcept {
        return requestData_.headerAt(index);
    }

    [[nodiscard]] bool appendRequestHeader(
        std::string_view name,
        std::string_view value,
        RequestHeaderKind kind) {
        return requestData_.appendHeader(name, value, kind);
    }

    [[nodiscard]] bool hasMethod() const noexcept {
        return requestState_.hasMethod();
    }

    void markMethod() noexcept {
        requestState_.markMethod();
    }

    [[nodiscard]] bool hasProtocol() const noexcept {
        return requestState_.hasProtocol();
    }

    [[nodiscard]] bool protocolIsWebSocket() const noexcept {
        return requestState_.protocolIsWebSocket();
    }

    void setProtocol(bool isWebSocket) noexcept {
        requestState_.setProtocol(isWebSocket);
    }

    [[nodiscard]] bool hasScheme() const noexcept {
        return requestState_.hasScheme();
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

    [[nodiscard]] bool headersDecoded() const noexcept {
        return requestState_.headersDecoded();
    }

    void markHeadersDecoded() noexcept {
        requestState_.markHeadersDecoded();
    }

    [[nodiscard]] bool standardConnect() const noexcept {
        return requestState_.standardConnect();
    }

    void markStandardConnect() noexcept {
        requestState_.markStandardConnect();
    }

    [[nodiscard]] bool extendedConnectWebSocket() const noexcept {
        return requestState_.extendedConnectWebSocket();
    }

    void markExtendedConnectWebSocket() noexcept {
        requestState_.markExtendedConnectWebSocket();
    }

    [[nodiscard]] bool webSocketTunnel() const noexcept {
        return requestState_.webSocketTunnel();
    }

    void markWebSocketTunnel() noexcept {
        requestState_.markWebSocketTunnel();
    }

    [[nodiscard]] std::uint16_t responseStatus() const noexcept {
        return requestState_.responseStatus();
    }

    void setResponseStatus(std::uint16_t status) noexcept {
        requestState_.setResponseStatus(status);
    }

    [[nodiscard]] std::uint8_t interimResponseCount() const noexcept {
        return requestState_.interimResponseCount();
    }

    void countInterimResponse() noexcept {
        requestState_.countInterimResponse();
    }

};

}  // namespace ruvia::detail
