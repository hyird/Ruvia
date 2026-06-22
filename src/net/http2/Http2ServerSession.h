#pragma once

#include <algorithm>
#include <array>
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
#include <asio/bind_allocator.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/recycling_allocator.hpp>

#include "Http2Frame.h"
#include "Http2BodyQueue.h"
#include "Http2BodyState.h"
#include "Http2ClosedStreams.h"
#include "Http2ContinuationQueue.h"
#include "Http2FlowControl.h"
#include "Http2HeaderBlock.h"
#include "Http2HeaderContinuation.h"
#include "Http2HeaderDecode.h"
#include "Http2Hpack.h"
#include "Http2InputBuffer.h"
#include "Http2LocalSettings.h"
#include "Http2OffsetVector.h"
#include "Http2PayloadSlice.h"
#include "Http2PeerSettings.h"
#include "Http2ReadyQueue.h"
#include "Http2RequestBuilder.h"
#include "Http2RequestBodyReader.h"
#include "Http2RequestHeaders.h"
#include "Http2ResponseHeaders.h"
#include "Http2ResponseStreamSink.h"
#include "Http2SessionAwaiters.h"
#include "Http2StreamTable.h"
#include "Http2WebSocket.h"
#include "Http2WebSocketHandshake.h"
#include "Http2WindowUpdate.h"
#include "../ws/HttpWebSocketSession.h"
#include "../HttpFileOpen.h"
#include "../RequestMemoryArena.h"
#include "../../http/HttpResponseBodyAccess.h"
#include "../../http/HttpResponseFileAccess.h"
#include "../../http/HttpResponseFileBody.h"
#include "../../http/HttpRequestInternal.h"
#include "../../http/HttpParserInternal.h"
#include "../server/ConnectionScanner.h"
#include "../server/HttpBufferedResponse.h"
#include "../server/HttpFileChunkBuffer.h"
#include "../../runtime/AsioAwait.h"
#include "../../router/RouteTable.h"
#include "ruvia/http/Context.h"
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
        const RouteTable& routes,
        DbRegistry* databases,
        RedisRegistry* redis,
        HttpClientRegistry* httpClients,
        const HttpServerOptions& options,
        ConnectionScanner::Entry& scannerEntry,
        std::string_view remoteAddress);

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

    Task<bool> ensureInput(std::size_t size);

    Task<bool> readPreface();

    Task<bool> readFrame(Http2FrameHeader& header, std::string_view& payload);

    template <typename WriteOperation>
    Task<void> writeSerialized(WriteOperation operation, bool finalWrite = false);

    Task<void> writeRaw(std::string_view bytes, bool finalWrite = false);

    Task<void> sendLocalSettings();

    Task<void> sendSettingsAck();

    Task<void> sendGoaway(std::uint32_t lastStreamId, Http2ErrorCode error, std::string_view debug = {});

    Task<void> sendRstStream(std::uint32_t streamId, Http2ErrorCode error);

    Task<void> sendDataWindowUpdates(std::uint32_t streamId, std::uint32_t increment);

    Task<bool> processFrame(const Http2FrameHeader& header, std::string_view payload);

    Task<bool> processSettings(const Http2FrameHeader& header, std::string_view payload);

    Task<bool> applySettingsPayload(std::string_view payload);

    Task<bool> processPing(const Http2FrameHeader& header, std::string_view payload);

    Task<bool> processPriority(const Http2FrameHeader& header, std::string_view payload);

    Task<bool> processRstStream(const Http2FrameHeader& header, std::string_view payload);

    Task<bool> processWindowUpdate(const Http2FrameHeader& header, std::string_view payload);

    Task<bool> processHeaders(const Http2FrameHeader& header, std::string_view payload);

    Task<bool> processTrailerHeaders(
        Http2StreamState& stream,
        const Http2FrameHeader& header,
        std::string_view payload);

    Task<bool> processContinuation(const Http2FrameHeader& header, std::string_view payload);

    Task<bool> processData(const Http2FrameHeader& header, std::string_view payload);

    Task<bool> handleHeaderDecodeFailure(Http2StreamState& stream, HeaderDecodeStatus status);

    [[nodiscard]] HeaderDecodeStatus decodeHeaderBlock(Http2StreamState& stream);

    [[nodiscard]] HeaderDecodeStatus finishTrailerBlock(Http2StreamState& stream);

    [[nodiscard]] HeaderDecodeStatus decodeRefusedHeaderBlock(Http2StreamState& stream);

    void queueInitialStreamIfReady(Http2StreamState& stream);

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

    [[nodiscard]] RouteServices routeServices(BodyReader* bodyReader = nullptr) const noexcept;

    Task<bool> dispatchHttp2WebSocketRoute(
        Http2StreamState& stream,
        const HttpRequest& request,
        const RouteResolution& resolution,
        RequestMemory& requestMemory,
        HttpResponse& response);

    Task<bool> dispatchHttp2ResponseStreamRoute(
        Http2StreamState& stream,
        const HttpRequest& request,
        const RouteResolution& resolution,
        RequestMemory& requestMemory,
        BodyReader* bodyReader,
        HttpResponse& response);

    Task<HttpResponse> dispatchHttp2BufferedRoute(
        Http2StreamState& stream,
        const HttpRequest& request,
        const RouteResolution& resolution,
        RequestMemory& requestMemory,
        BodyReader* bodyReader);

    Task<void> dispatchStream(Http2StreamState& stream);

    Task<void> writeFramePayload(
        Http2FrameType type,
        std::uint8_t flags,
        std::uint32_t streamId,
        std::string_view firstPayload,
        std::string_view secondPayload = {},
        bool finalWrite = false);

    Task<void> writeHeaders(Http2StreamState& stream, std::string_view headerBlock, bool endStream);

    Task<bool> waitForDataWindow(Http2StreamState& stream);

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
    const RouteTable& routes_;
    DbRegistry* databases_;
    RedisRegistry* redis_;
    HttpClientRegistry* httpClients_;
    const HttpServerOptions& options_;
    ConnectionScanner::Entry& scannerEntry_;
    std::string_view remoteAddress_;
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
