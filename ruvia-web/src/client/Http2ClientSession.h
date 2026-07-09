#pragma once

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
#include "detail/HttpPmrObject.h"
#include "client/HttpClientAccess.h"
#include "HttpClientBackend.h"
#include "net/http2/Http2Connection.h"

namespace ruvia::detail {

// A single multiplexed HTTP/2 connection to one origin, driven over the sans-I/O
// Http2Connection core in client role (the SAME state machine the server runs). A
// background read loop feeds raw bytes to the core and reacts to its events; a
// background flush loop drains the core's outbound buffer. Each fetch() opens one
// client (odd) stream and suspends until the stream completes. Everything protocol
// (framing, HPACK, flow control, response validation, GOAWAY) lives in the core;
// this class holds only client policy: sockets/TLS, deadlines, concurrency slots,
// response accumulation, redirects, and the streaming source.
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
    [[nodiscard]] bool isQuiescent() const noexcept override;
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

    void destroy() noexcept override { destroyHttpPmrObject(this, resource_); }

    // Called by Http2StreamSource (the streaming HttpBodyStream source) to pull the body and
    // release the stream.
    [[nodiscard]] Task<std::pmr::string> streamReadChunk(std::uint32_t streamId);
    void streamClose(std::uint32_t streamId) noexcept;

private:
    using TlsStream = asio::ssl::stream<asio::ip::tcp::socket&>;
    using TlsStreamDeleter = HttpPmrObjectDeleter<TlsStream>;

    enum class State : std::uint8_t { kIdle, kConnecting, kReady, kClosed };

    // Client policy for one in-flight request; all protocol state (windows, decode,
    // content-length accounting) lives in the core's Http2StreamState, kept alive by
    // pinStream until destroyStream unpins it.
    struct Stream final {
        explicit Stream(std::pmr::memory_resource* requestResource)
            : response(FetchResponseAccess::make(requestResource)) {}

        FetchResponse response;         // status/headers accumulate here; body too (buffered mode)
        std::pmr::memory_resource* requestResource{nullptr};
        std::size_t maxBodyBytes{0};
        // nginx-style inactivity deadline, refreshed on every frame for this stream (scanDeadlines
        // RSTs it, waking both the reader and any parked request-body send). While the request body
        // is still going out the gap is bounded by sendTimeout (proxy_send_timeout); once fully
        // sent, by readTimeout (proxy_read_timeout). Applies to buffered + streaming.
        std::chrono::milliseconds readTimeout{0};
        std::chrono::milliseconds sendTimeout{0};
        std::chrono::steady_clock::time_point deadline{};
        std::uint32_t id{0};
        bool streaming{false};          // deliver DATA incrementally with flow-control backpressure
        bool responseBodyAllowed{true};
        bool hasDeadline{false};
        bool headersComplete{false};
        bool localEndSent{false};       // we sent (or queued) END_STREAM for the request
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

    struct IoTurnAwaiter final {
        Http2ClientSession* session;
        [[nodiscard]] bool await_ready() const noexcept { return false; }
        void await_suspend(std::coroutine_handle<> handle) noexcept {
            // Counted like resume(): the resumed fetch coroutine touches the session
            // (destroyStream etc.), so the session must not be reaped while this is
            // in flight -- isQuiescent() stays false until the post runs.
            auto* owner = session;
            ++owner->pendingResumes_;
            auto executor = owner->ioContext_.get_executor();
            asio::post(executor, [owner, executor, handle]() mutable {
                asio::post(executor, [owner, handle]() {
                    --owner->pendingResumes_;
                    handle.resume();
                });
            });
        }
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

    // The flush loop parks here until the core has bytes to write or the session closes.
    struct FlushAwaiter final {
        Http2ClientSession* session;
        [[nodiscard]] bool await_ready() const noexcept {
            return session->conn_->wantsWrite() || session->state_ == State::kClosed;
        }
        void await_suspend(std::coroutine_handle<> handle) noexcept { session->flushWaiter_ = handle; }
        void await_resume() const noexcept {}
    };

    // A streamed request-body send parks here until the core drained this stream's
    // window-blocked remainder (WINDOW_UPDATE/SETTINGS), the stream fails/ends, or the
    // session closes.
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
            return !session->conn_->hasBlockedSend(streamId);
        }
        void await_suspend(std::coroutine_handle<> handle) noexcept {
            session->sendWindowWaiters_.push_back(handle);
        }
        void await_resume() const noexcept {}
    };

    Task<void> doConnect();
    // Reconnect support: reset a fully-wound-down closed session back to kIdle so the
    // next fetch reopens the origin (self-healing, like the h1 pool).
    [[nodiscard]] bool isReconnectable() const noexcept;
    void resetForReconnect();
    Task<void> readLoop();
    Task<void> flushLoop();
    Task<std::pair<std::error_code, std::size_t>> readSome(asio::mutable_buffer buffer);
    Task<std::error_code> writeBytes(std::string_view bytes);

    void wakeFlusher() noexcept;
    void resume(std::coroutine_handle<> handle) noexcept;

    [[nodiscard]] Stream* findStream(std::uint32_t id) noexcept;
    void destroyStream(std::uint32_t id) noexcept;
    void touchStreamDeadline(Stream& stream) noexcept;  // push the inactivity deadline forward
    void signalWaiter(Stream& stream) noexcept;   // resume the fetch/fetchStream waiter (once)
    void wakeReader(Stream& stream) noexcept;      // resume a streaming readChunk awaiter
    void releaseSlot(Stream& stream) noexcept;     // free the concurrency slot (once)
    void markRemoteEnd(Stream& stream) noexcept;   // server END_STREAM: release slot + signal reader/waiter
    void failStream(Stream& stream, Http2ErrorCode error) noexcept;
    void failAllStreams() noexcept;

    // Core event pump: called after every feed; reacts to response heads/chunks/ends,
    // stream resets, GOAWAY, and send-window drain reports.
    void drainCoreEvents();
    void onResponseHead(std::uint32_t streamId);
    void onResponseChunk(std::uint32_t streamId, std::string_view data);
    void handlePeerGoaway(std::uint32_t lastStreamId) noexcept;
    void resetStream(std::uint32_t streamId, Http2ErrorCode error) noexcept;

    // Connect (if needed), respect MAX_CONCURRENT_STREAMS, encode + submit the request
    // headers/body through the core, and register the new stream. Shared by fetch()
    // (buffered) and fetchStream() (streaming).
    [[nodiscard]] Task<Stream*> beginRequest(
        std::string_view path,
        const FetchOptions& options,
        std::pmr::memory_resource* requestResource,
        bool streaming);

    // Pull a streamed request body and submit it as flow-controlled DATA ending in
    // END_STREAM; waits for the core to drain a window-blocked remainder before pulling
    // the next chunk (the read loop concurrently supplies WINDOW_UPDATE).
    Task<void> streamRequestBody(std::uint32_t streamId, const RequestBodyStream& bodyStream);
    void wakeSendWindow() noexcept;
    void wakeStreamSlot() noexcept;
    [[nodiscard]] std::size_t openStreamCount() const noexcept;

    asio::io_context& ioContext_;
    HttpClientConfig config_;
    std::pmr::memory_resource* resource_;
    asio::ip::tcp::socket socket_;
    asio::ip::tcp::resolver resolver_;
    std::optional<asio::ssl::context> sslContext_;
    std::unique_ptr<TlsStream, TlsStreamDeleter> tlsStream_;

    std::optional<Http2Connection> conn_;  // the sans-I/O h2 core, client role (optional for reconnect reset)
    std::pmr::string authority_;   // host[:port] for the :authority pseudo-header
    std::string_view scheme_;      // "https" or "http"
    std::pmr::string readBuffer_;  // scratch for socket reads fed into the core

    std::pmr::unordered_map<std::uint32_t, Stream*> streams_;
    std::pmr::vector<std::coroutine_handle<>> readyWaiters_;
    std::pmr::vector<SlotWaiter*> streamSlotWaiters_;  // fetches parked at MAX_CONCURRENT_STREAMS
    std::pmr::vector<std::coroutine_handle<>> sendWindowWaiters_;  // request-body sends parked on flow control
    std::coroutine_handle<> flushWaiter_{};

    std::chrono::steady_clock::time_point connectDeadline_{};
    bool hasConnectDeadline_{false};

    State state_{State::kIdle};
    std::size_t runningLoops_{0};    // detached read/flush loops still holding `this`
    std::size_t pendingResumes_{0};  // posted coroutine resumes not yet run (all touch `this`)
    bool settingsReceived_{false};

    // fetch() parks here (returns not-ready) until an open-stream slot frees below the peer's
    // SETTINGS_MAX_CONCURRENT_STREAMS limit.
    struct StreamSlotAwaiter final {
        Http2ClientSession* session;
        SlotWaiter* waiter;
        [[nodiscard]] bool await_ready() const noexcept {
            return session->state_ != State::kReady ||
                   session->openStreamCount() < session->conn_->peerMaxConcurrentStreams();
        }
        void await_suspend(std::coroutine_handle<> handle) noexcept {
            waiter->handle = handle;
            session->streamSlotWaiters_.push_back(waiter);
        }
        void await_resume() const noexcept {}
    };
};

}  // namespace ruvia::detail
