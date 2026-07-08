#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <ios>
#include <limits>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <asio.hpp>
#include "net/http2/Http2Frame.h"
#include "net/http2/Http2BodyQueue.h"
#include "net/http2/Http2BodyState.h"
#include "net/http2/Http2ClosedStreams.h"
#include "net/http2/Http2ContinuationQueue.h"
#include "net/http2/Http2FlowControl.h"
#include "net/http2/Http2HeaderBlock.h"
#include "net/http2/Http2FramePhase.h"
#include "net/http2/Http2HeaderContinuation.h"
#include "net/http2/Http2HeaderDecode.h"
#include "net/http2/Http2Hpack.h"
#include "net/http2/Http2InputBuffer.h"
#include "net/http2/Http2LocalSettings.h"
#include "net/http2/Http2OffsetVector.h"
#include "net/http2/Http2PayloadSlice.h"
#include "net/http2/Http2PeerSettings.h"
#include "net/http2/Http2ReadyQueue.h"
#include "net/http2/Http2RequestBuilder.h"
#include "net/http2/Http2RequestBodyReader.h"
#include "net/http2/Http2RequestHeaders.h"
#include "net/http2/Http2ResponseHeaders.h"
#include "net/http2/Http2ResponseStreamSink.h"
#include "net/http2/Http2SessionAwaiters.h"
#include "net/http2/Http2SessionResults.h"
#include "net/http2/Http2StreamTable.h"
#include "net/http2/Http2WebSocket.h"
#include "net/http2/Http2WebSocketHandshake.h"
#include "net/http2/Http2WindowUpdate.h"
#include "net/ws/HttpWebSocketSession.h"
#include "net/HttpFileOpen.h"
#include "net/RequestMemoryArena.h"
#include "net/RequestBodyLimit.h"
#include "net/server/RateLimitDecision.h"
#include "HttpResponseBodyAccess.h"
#include "HttpResponseFileAccess.h"
#include "HttpResponseFileBody.h"
#include "HttpRequestInternal.h"
#include "HttpParserInternal.h"
#include "net/server/ConnectionScanner.h"
#include "net/server/HttpBufferedResponse.h"
#include "net/server/HttpFileFallback.h"
#include "net/server/HttpServerAccessLog.h"
#include "net/server/HttpResponseStreamDispatch.h"
#include "runtime/AsioAwait.h"
#include "router/RequestDispatcher.h"
#include "ruvia/http/Error.h"
#include "ruvia/http/detail/PmrString.h"
#include "ruvia/memory/MemoryPool.h"

namespace ruvia::detail {

template <typename Stream>
class Http2ServerSession final {
    template <typename Session>
    friend class Http2WebSocketTransport;
    template <typename Session>
    friend class Http2RequestBodyReader;
    template <typename Session>
    friend class Http2ResponseStreamSink;
    template <typename Session>
    friend class Http2DispatchGuard;
    template <typename Session>
    friend class Http2WriteTurnAwaiter;
    template <typename Session>
    friend class Http2BodyChunkAwaiter;
    template <typename Session>
    friend class Http2SendWindowAwaiter;
    template <typename Session>
    friend class Http2DispatchDrainAwaiter;

public:
    Http2ServerSession(
        Stream& stream,
        asio::ip::tcp::socket& socket,
        WorkerMemory& memory,
        const RequestDispatcher& routes,
        DbRegistry* databases,
        RedisRegistry* redis,
        HttpClientRegistry* httpClients,
        const HttpServerOptions& options,
        ConnectionScanner::Entry& scannerEntry,
        std::string_view remoteAddress,
        RateLimiter* rateLimiter = nullptr,
        std::string_view clientCertificate = {},
        const std::atomic_bool* serverStarted = nullptr);

    Task<void> run(std::string_view initialBytes = {});

    Task<void> runUpgraded(
        const HttpServerParseResult& parsed,
        std::string_view settingsPayload,
        std::string_view body,
        std::string_view initialBytes = {});

private:
    static constexpr std::size_t kReadChunkBytes = 16 * 1024;

    Task<void> runFrameLoop();

    [[nodiscard]] std::size_t availableInput() const noexcept;

    [[nodiscard]] std::string_view inputView(std::size_t size) const noexcept;

    void consumeInput(std::size_t size);

    Task<Http2InputReadResult> ensureInput(std::size_t size);

    Task<Http2InputReadResult> readPreface();

    Task<Http2InputReadResult> readFrame(Http2FrameHeader& header, std::string_view& payload);

    template <typename WriteOperation>
    Task<void> writeSerialized(WriteOperation operation, bool finalWrite = false);

    Task<void> writeRaw(std::string_view bytes, bool finalWrite = false);

    Task<void> sendLocalSettings();

    Task<void> sendSettingsAck();

    Task<void> sendGoaway(std::uint32_t lastStreamId, Http2ErrorCode error, std::string_view debug = {});

    Task<void> sendRstStream(std::uint32_t streamId, Http2ErrorCode error);

    Task<void> sendDataWindowUpdates(std::uint32_t streamId, std::uint32_t increment);

    Task<void> sendConnectionWindowUpdate(std::uint32_t increment);

    // Drop an incoming DATA frame while keeping the connection alive, returning its
    // connection-level flow-control credit to the peer (RFC 9113 6.9.1). `flowBytes`
    // is the frame payload length; `windowConsumed` says whether the connection
    // receive window was already debited for it (so it must be restored too).
    Task<Http2SessionFlow> dropDataFrameKeepConnection(std::size_t flowBytes, bool windowConsumed);

    Task<Http2SessionFlow> processFrame(const Http2FrameHeader& header, std::string_view payload);

    Task<Http2SessionFlow> processSettings(const Http2FrameHeader& header, std::string_view payload);

    Task<Http2SessionFlow> applySettingsPayload(std::string_view payload);

    Task<Http2SessionFlow> processPing(const Http2FrameHeader& header, std::string_view payload);

    Task<Http2SessionFlow> processPriority(const Http2FrameHeader& header, std::string_view payload);

    Task<Http2SessionFlow> processRstStream(const Http2FrameHeader& header, std::string_view payload);

    Task<Http2SessionFlow> processWindowUpdate(const Http2FrameHeader& header, std::string_view payload);

    Task<Http2SessionFlow> processHeaders(const Http2FrameHeader& header, std::string_view payload);

    Task<Http2SessionFlow> processTrailerHeaders(
        Http2StreamState& stream,
        const Http2FrameHeader& header,
        std::string_view payload);

    Task<Http2SessionFlow> processContinuation(const Http2FrameHeader& header, std::string_view payload);

    Task<Http2SessionFlow> processData(const Http2FrameHeader& header, std::string_view payload);

    Task<Http2SessionFlow> handleHeaderDecodeFailure(Http2StreamState& stream, HeaderDecodeStatus status);

    [[nodiscard]] HeaderDecodeStatus decodeHeaderBlock(Http2StreamState& stream);

    [[nodiscard]] HeaderDecodeStatus finishTrailerBlock(Http2StreamState& stream);

    [[nodiscard]] HeaderDecodeStatus decodeRefusedHeaderBlock(Http2StreamState& stream);

    void queueInitialStreamIfReady(Http2StreamState& stream);

    Task<void> admitDecodedInitialStream(Http2StreamState& stream);

    void resolveStreamRoute(Http2StreamState& stream) noexcept;

    [[nodiscard]] bool seedUpgradedStream(const HttpServerParseResult& parsed, std::string_view body);

    [[nodiscard]] Http2StreamState* findStream(std::uint32_t streamId) noexcept;

    [[nodiscard]] bool isIdleStream(std::uint32_t streamId) const noexcept;

    [[nodiscard]] Http2StreamState* createStream(std::uint32_t streamId);

    void removeStream(std::uint32_t streamId) noexcept;

    void closeStream(std::uint32_t streamId, Http2StreamCloseSource source = Http2StreamCloseSource::kLocal);

    void removeReadyStream(std::uint32_t streamId) noexcept;

    void cleanupClosedStreams() noexcept;

    void queueReady(std::uint32_t streamId);

    [[nodiscard]] bool hasReadyStream() const noexcept;

    [[nodiscard]] std::uint32_t popReadyStream() noexcept;

    void launchStreamDispatch(Http2StreamState& stream);

    Task<void> dispatchStreamTask(std::uint32_t streamId);

    void finishStreamDispatch(std::uint32_t streamId) noexcept;

    void resumeNextWriteWaiter();

    void resumeAllWriteWaiters();

    void resumeBodyWaiter(Http2StreamState& stream) noexcept;

    void resumeAllBodyWaiters() noexcept;

    void resumeSendWindowWaiters();

    Task<std::optional<std::string_view>> readBodyChunk(std::uint32_t streamId);

    Task<void> writeHttp2WebSocketHandshake(Http2StreamState& stream, std::string_view subprotocol);

    [[nodiscard]] ContextServices routeServices() const noexcept;

    Task<Http2RouteDispatchResult> dispatchHttp2WebSocketRoute(
        Http2StreamState& stream,
        const HttpRequest& request,
        const RouteResolution& resolution,
        RequestMemory& requestMemory,
        ContextServices services);

    Task<Http2RouteDispatchResult> dispatchHttp2ResponseStreamRoute(
        Http2StreamState& stream,
        const HttpRequest& request,
        const RouteResolution& resolution,
        RequestMemory& requestMemory,
        ContextServices services);

    Task<void> dispatchStream(Http2StreamState& stream);

    Task<void> writeFramePayload(
        Http2FrameType type,
        std::uint8_t flags,
        std::uint32_t streamId,
        std::string_view firstPayload,
        std::string_view secondPayload = {},
        bool finalWrite = false);

    Task<void> writeHeaders(Http2StreamState& stream, std::string_view headerBlock, bool endStream);

    Task<Http2DataWindowResult> waitForDataWindow(Http2StreamState& stream);

    Task<void> writeData(
        Http2StreamState& stream,
        std::string_view first,
        std::string_view second,
        bool endStream);

    Task<void> writeFileBody(Http2StreamState& stream, ResponseFileBody fileBody, bool endStream);

    Task<void> writeResponse(
        Http2StreamState& stream,
        const HttpResponse& response,
        bool skipBody = false);

    Stream& stream_;
    asio::ip::tcp::socket& socket_;
    WorkerMemory& memory_;
    const RequestDispatcher& routes_;
    DbRegistry* databases_;
    RedisRegistry* redis_;
    HttpClientRegistry* httpClients_;
    const HttpServerOptions& options_;
    ConnectionScanner::Entry& scannerEntry_;
    std::string_view remoteAddress_;
    RateLimiter* rateLimiter_{nullptr};
    std::string_view clientCertificate_;
    const std::atomic_bool* serverStarted_{nullptr};
    std::pmr::string input_;
    Http2StreamTable streams_;
    Http2ClosedStreamHistory closedStreams_;
    Http2ReadyQueue readyQueue_;
    Http2ContinuationQueue writeWaiters_;
    Http2ContinuationQueue sendWindowWaiters_;
    HpackDecoder decoder_;
    std::optional<Http2StreamState> refusedHeaderStream_;
    Http2HeaderContinuation headerContinuation_;
    Http2PeerSettings peerSettings_;
    std::size_t inputOffset_{0};
    std::size_t activeDispatches_{0};
    std::uint32_t localMaxFrameSize_{kHttp2DefaultMaxFrameSize};
    std::uint32_t lastStreamId_{0};
    std::uint32_t dispatchDepth_{0};
    std::int32_t connectionSendWindow_{kHttp2DefaultInitialWindowSize};
    std::int32_t connectionReceiveWindow_{static_cast<std::int32_t>(kHttp2LocalInitialWindowSize)};
    bool receivedFirstSettings_{false};
    bool closing_{false};
    bool draining_{false};
    std::uint32_t goawayLastStreamId_{0};
    bool writeInProgress_{false};
    bool readerRunning_{false};
    std::coroutine_handle<> dispatchDrainWaiter_{};
};

#include "Http2ServerSessionControlFrames.inl"
#include "Http2ServerSessionLifecycle.inl"
#include "Http2ServerSessionInput.inl"
#include "Http2ServerSessionStreamFrames.inl"
#include "Http2ServerSessionStreams.inl"
#include "Http2ServerSessionDispatch.inl"
#include "Http2ServerSessionWrite.inl"

}  // namespace ruvia::detail
