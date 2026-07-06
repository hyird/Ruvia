#pragma once

#ifdef RUVIA_ENABLE_HTTP_CLIENT

#include <asio/awaitable.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/ssl/context.hpp>
#include <asio/ssl/stream.hpp>
#include <chrono>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <optional>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#include "ruvia/app/Task.h"
#include "ruvia/http/HttpClient.h"
#include "ruvia/memory/PmrObject.h"
#include "HttpClientAccess.h"
#include "HttpClientBackend.h"
#include "../../net/http2/Http2Hpack.h"
#include "../../net/http2/Http2FrameTypes.h"
#include "../../net/http2/Http2PeerSettings.h"
#include "../../net/http2/Http2StreamFlowControl.h"

namespace ruvia::detail {

// A single multiplexed HTTP/2 connection to one origin. A background read loop parses frames
// and drives per-stream state; a background flush loop serializes all outbound writes. Each
// fetch() opens one client (odd) stream and suspends until the stream completes.
class Http2ClientSession final : public HttpClientBackend {
public:
    Http2ClientSession(
        asio::io_context& ioContext,
        HttpClientConfig config,
        std::pmr::memory_resource* resource = nullptr);
    ~Http2ClientSession() override;

    Http2ClientSession(const Http2ClientSession&) = delete;
    Http2ClientSession& operator=(const Http2ClientSession&) = delete;

    Task<void> connect() override;
    void closeNow() noexcept override;
    [[nodiscard]] bool hasAnyTimeout() const noexcept override;
    void scanDeadlines(std::chrono::steady_clock::time_point now) noexcept override;
    [[nodiscard]] Task<FetchResponse> fetch(
        std::string_view path,
        const FetchOptions& options,
        std::pmr::memory_resource* resource) override;
    [[nodiscard]] Task<FetchResponseStream> fetchStream(
        std::string_view path,
        const FetchOptions& options,
        std::pmr::memory_resource* resource) override;

    void destroy() noexcept override { destroyPmrObject(this, resource_); }

    // Called by Http2StreamSource (the streaming pimpl) to pull the body and release the stream.
    [[nodiscard]] Task<std::pmr::string> streamReadChunk(std::uint32_t streamId);
    void streamClose(std::uint32_t streamId) noexcept;

private:
    using TlsStream = asio::ssl::stream<asio::ip::tcp::socket&>;
    using TlsStreamDeleter = PmrObjectDeleter<TlsStream>;

    enum class State : std::uint8_t { kIdle, kConnecting, kReady, kClosed };

    struct Stream final {
        explicit Stream(std::pmr::memory_resource* requestResource)
            : response(FetchResponseAccess::make(requestResource)) {}

        Http2StreamFlowControl flow;
        FetchResponse response;         // status/headers accumulate here; body too (buffered mode)
        std::string_view pendingBody{}; // request body bytes still to send (borrowed)
        std::pmr::memory_resource* requestResource{nullptr};
        std::size_t maxBodyBytes{0};
        std::size_t responseContentLength{0};
        std::size_t responseBodyBytes{0};
        std::int32_t flowDebt{0};       // received bytes awaiting a WINDOW_UPDATE on consume (streaming)
        std::chrono::steady_clock::time_point deadline{};
        std::uint32_t id{0};
        std::size_t informationalResponses{0};
        bool streaming{false};          // deliver DATA incrementally with flow-control backpressure
        bool responseBodyAllowed{true};
        bool responseHasContentLength{false};
        bool hasDeadline{false};
        bool headersComplete{false};
        bool localEndSent{false};       // we sent END_STREAM for the request
        bool remoteEnded{false};        // server sent END_STREAM
        bool failed{false};
        bool completed{false};          // the fetch/fetchStream waiter has been signaled
        bool slotReleased{false};       // stream no longer counts toward MAX_CONCURRENT_STREAMS
        std::coroutine_handle<> waiter{};  // fetch (buffered) or fetchStream (streaming, headers)
        std::coroutine_handle<> reader{};  // streaming readChunk awaiting the next body slice
    };

    // A fetch parked at MAX_CONCURRENT_STREAMS has no Stream yet, so the per-stream deadline
    // scan can't see it. This carries the parked fetch's own deadline so scanDeadlines can time
    // it out -- otherwise a peer that advertises MAX_CONCURRENT_STREAMS=0 (or never frees a
    // slot) would hang the fetch forever. Lives on the beginRequest coroutine frame; a pointer
    // is registered in streamSlotWaiters_ while parked and removed before the frame is resumed.
    struct SlotWaiter final {
        std::coroutine_handle<> handle{};
        std::chrono::steady_clock::time_point deadline{};
        bool hasDeadline{false};
        bool timedOut{false};
    };

    // fetch()/fetchStream() suspends here until the read loop signals the stream (buffered: whole
    // response ready; streaming: response headers ready) or fails it.
    struct StreamAwaiter final {
        Stream* stream;
        [[nodiscard]] bool await_ready() const noexcept { return stream->completed; }
        void await_suspend(std::coroutine_handle<> handle) noexcept { stream->waiter = handle; }
        void await_resume() const noexcept {}
    };

    // A streaming readChunk() suspends here until the next body slice arrives, the stream ends,
    // or it fails.
    struct StreamReaderAwaiter final {
        Stream* stream;
        [[nodiscard]] bool await_ready() const noexcept {
            return !FetchResponseAccess::body(stream->response).empty() || stream->remoteEnded || stream->failed;
        }
        void await_suspend(std::coroutine_handle<> handle) noexcept { stream->reader = handle; }
        void await_resume() const noexcept {}
    };

    // connect() suspends here until the peer's first SETTINGS arrives (or the connection fails).
    struct ReadyAwaiter final {
        Http2ClientSession* session;
        [[nodiscard]] bool await_ready() const noexcept {
            return session->settingsReceived_ || session->state_ == State::kClosed;
        }
        void await_suspend(std::coroutine_handle<> handle) noexcept {
            session->readyWaiters_.push_back(handle);
        }
        void await_resume() const noexcept {}
    };

    // The flush loop parks here until bytes are queued or the session closes.
    struct FlushAwaiter final {
        Http2ClientSession* session;
        [[nodiscard]] bool await_ready() const noexcept {
            return !session->outBuffer_.empty() || session->state_ == State::kClosed;
        }
        void await_suspend(std::coroutine_handle<> handle) noexcept { session->flushWaiter_ = handle; }
        void await_resume() const noexcept {}
    };

    // A streamed request-body send parks here until the connection + stream send windows both
    // reopen (WINDOW_UPDATE), the stream fails, or the session closes.
    struct SendWindowAwaiter final {
        Http2ClientSession* session;
        std::uint32_t streamId;
        [[nodiscard]] bool await_ready() const noexcept {
            if (session->state_ != State::kReady) {
                return true;
            }
            const auto it = session->streams_.find(streamId);
            if (it == session->streams_.end() || it->second->failed || it->second->remoteEnded) {
                return true;  // gone/reset, or the server already sent a full response
            }
            return session->connectionSendWindow_ > 0 && it->second->flow.sendWindow() > 0;
        }
        void await_suspend(std::coroutine_handle<> handle) noexcept {
            session->sendWindowWaiters_.push_back(handle);
        }
        void await_resume() const noexcept {}
    };

    Task<void> doConnect();
    Task<void> readLoop();
    Task<void> flushLoop();
    Task<std::pair<std::error_code, std::size_t>> readSome(asio::mutable_buffer buffer);
    Task<std::error_code> writeBytes(std::string_view bytes);
    Task<bool> ensureInput(std::size_t needed);

    void queueBytes(std::string_view bytes);
    void wakeFlusher() noexcept;
    void resume(std::coroutine_handle<> handle) noexcept;

    [[nodiscard]] Stream* findStream(std::uint32_t id) noexcept;
    void destroyStream(std::uint32_t id) noexcept;
    void signalWaiter(Stream& stream) noexcept;   // resume the fetch/fetchStream waiter (once)
    void wakeReader(Stream& stream) noexcept;      // resume a streaming readChunk awaiter
    void releaseSlot(Stream& stream) noexcept;     // free the concurrency slot (once)
    void markRemoteEnd(Stream& stream) noexcept;   // server END_STREAM: release slot + signal reader/waiter
    void failStream(Stream& stream, Http2ErrorCode error) noexcept;
    void failAllStreams() noexcept;

    // Frame handlers (all synchronous — they mutate state and queue bytes, never await).
    // Return false on a fatal protocol error that must tear down the connection.
    [[nodiscard]] bool onFrame(const Http2FrameHeader& header, std::string_view payload);
    [[nodiscard]] bool onSettings(const Http2FrameHeader& header, std::string_view payload);
    [[nodiscard]] bool onHeaders(const Http2FrameHeader& header, std::string_view payload);
    [[nodiscard]] bool onContinuation(const Http2FrameHeader& header, std::string_view payload);
    [[nodiscard]] bool onData(const Http2FrameHeader& header, std::string_view payload);
    [[nodiscard]] bool onWindowUpdate(const Http2FrameHeader& header, std::string_view payload);
    [[nodiscard]] bool onPing(const Http2FrameHeader& header, std::string_view payload);
    [[nodiscard]] bool onRstStream(const Http2FrameHeader& header, std::string_view payload);
    void onGoaway(std::string_view payload) noexcept;
    // Decode the fully-assembled header block. HPACK is stateful and connection-global, so this
    // ALWAYS decodes (keeping the dynamic table synced) even when the target stream is missing
    // (closed) or the block is trailers to be discarded — `applyStream` non-null means apply the
    // decoded response headers, null means decode-and-discard.
    [[nodiscard]] bool finalizeHeaderBlock(Stream* stream, bool apply, bool endStream);
    [[nodiscard]] bool beginHeaderBlock(
        std::uint32_t streamId, Stream* stream, bool discard, bool endStream, bool endHeaders,
        std::string_view fragment);

    // Connect (if needed), respect MAX_CONCURRENT_STREAMS, encode + send the request headers/body,
    // and register the new stream. Shared by fetch() (buffered) and fetchStream() (streaming).
    [[nodiscard]] Task<Stream*> beginRequest(
        std::string_view path,
        const FetchOptions& options,
        std::pmr::memory_resource* requestResource,
        bool streaming);

    void sendPendingData(Stream& stream);
    // Pull a streamed request body and send it as flow-controlled DATA frames ending in
    // END_STREAM. Runs inline in beginRequest (the read loop concurrently supplies WINDOW_UPDATE),
    // so the borrowed body stream outlives the send.
    Task<void> streamRequestBody(std::uint32_t streamId, const RequestBodyStream& bodyStream);
    void wakeSendWindow() noexcept;
    void wakeStreamSlot() noexcept;
    [[nodiscard]] std::size_t openStreamCount() const noexcept;
    void appendFrame(Http2FrameType type, std::uint8_t flags, std::uint32_t streamId, std::string_view payload);
    void queueHeaders(std::uint32_t streamId, std::string_view block, bool endStream);
    void queueSettingsAck();
    void queueWindowUpdate(std::uint32_t streamId, std::uint32_t increment, bool includeStream);
    void sendRstStream(std::uint32_t streamId, Http2ErrorCode error);

    asio::io_context& ioContext_;
    HttpClientConfig config_;
    std::pmr::memory_resource* resource_;
    asio::ip::tcp::socket socket_;
    asio::ip::tcp::resolver resolver_;
    std::optional<asio::ssl::context> sslContext_;
    std::unique_ptr<TlsStream, TlsStreamDeleter> tlsStream_;

    HpackDecoder decoder_;
    Http2PeerSettings peerSettings_;
    std::pmr::string authority_;   // host[:port] for the :authority pseudo-header
    std::string_view scheme_;      // "https" or "http"

    std::pmr::string input_;
    std::size_t inputOffset_{0};
    std::pmr::string outBuffer_;
    std::pmr::string encodeScratch_;   // reused HPACK request-header block
    std::pmr::string headerAssembly_;  // HEADERS + CONTINUATION reassembly (connection-global)

    std::pmr::unordered_map<std::uint32_t, Stream*> streams_;
    std::pmr::vector<std::coroutine_handle<>> readyWaiters_;
    std::pmr::vector<SlotWaiter*> streamSlotWaiters_;  // fetches parked at MAX_CONCURRENT_STREAMS
    std::pmr::vector<std::coroutine_handle<>> sendWindowWaiters_;  // request-body sends parked on flow control
    std::coroutine_handle<> flushWaiter_{};

    std::int32_t connectionSendWindow_{kHttp2DefaultInitialWindowSize};
    std::uint32_t nextStreamId_{1};
    std::uint32_t continuationStream_{0};
    bool continuationEndStream_{false};
    bool continuationDiscard_{false};

    std::chrono::steady_clock::time_point connectDeadline_{};
    bool hasConnectDeadline_{false};

    State state_{State::kIdle};
    bool settingsReceived_{false};
    bool goawayReceived_{false};

    // fetch() parks here (returns not-ready) until an open-stream slot frees below the peer's
    // SETTINGS_MAX_CONCURRENT_STREAMS limit.
    struct StreamSlotAwaiter final {
        Http2ClientSession* session;
        SlotWaiter* waiter;
        [[nodiscard]] bool await_ready() const noexcept {
            return session->state_ != State::kReady ||
                   session->openStreamCount() < session->peerSettings_.maxConcurrentStreams();
        }
        void await_suspend(std::coroutine_handle<> handle) noexcept {
            waiter->handle = handle;
            session->streamSlotWaiters_.push_back(waiter);
        }
        void await_resume() const noexcept {}
    };
};

}  // namespace ruvia::detail

#endif  // RUVIA_ENABLE_HTTP_CLIENT
