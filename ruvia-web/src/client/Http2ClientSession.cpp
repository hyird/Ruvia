
#include "Http2ClientSession.h"

#include <asio/as_tuple.hpp>
#include <asio/co_spawn.hpp>
#include <asio/connect.hpp>
#include <asio/detached.hpp>
#include <asio/ip/address.hpp>
#include <asio/post.hpp>
#include <asio/read.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/write.hpp>
#include <openssl/ssl.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstring>
#include <stdexcept>
#include <string>

#include "runtime/AsioAwait.h"
#include "HeaderTokenUtils.h"
#include "parser/HttpRequestTarget.h"
#include "client/HttpClientContentEncoding.h"
#include "client/HttpClientConfigValidation.h"
#include "client/HttpClientDecodingStreamSource.h"
#include "client/HttpClientRedirect.h"
#include "client/HttpClientResponseLimits.h"
#include "HttpClientTlsVerification.h"
#include "ruvia/http/HttpCommon.h"
#include "ruvia/memory/PmrResource.h"

namespace ruvia::detail {
namespace {

constexpr std::size_t kHttp2ReadChunk = 16 * 1024;

// Client role: the session enforces its own response-size policy (maxResponseBodyBytes
// for buffered fetches, flow-control backpressure for streaming), so the core's
// protocol-level body caps are disabled.
[[nodiscard]] Http2CoreConfig http2ClientCoreConfig() noexcept {
    Http2CoreConfig config;
    config.maxStreamBodyBytes = 0;
    config.maxBufferedBodyBytes = 0;
    return config;
}

// Connection-specific headers are forbidden in HTTP/2 (RFC 7540 §8.1.2.2), as are any
// pseudo-header (leading ':') the caller tries to inject and the client-managed framing headers.
[[nodiscard]] bool isForbiddenH2RequestHeader(std::string_view name) noexcept {
    if (!name.empty() && name.front() == ':') {
        return true;
    }
    static constexpr std::string_view kForbidden[] = {
        "connection", "keep-alive", "proxy-connection", "transfer-encoding",
        "upgrade", "host", "content-length", "trailer"};
    for (const auto forbidden : kForbidden) {
        if (asciiEqualsIgnoreCase(name, forbidden)) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool isAllowedH2RequestHeader(
    std::string_view name,
    std::string_view value) noexcept {
    if (isForbiddenH2RequestHeader(name)) {
        return false;
    }
    return !asciiEqualsIgnoreCase(name, "te") || value == "trailers";
}

[[nodiscard]] bool http2ResponseStatusMayHaveBody(std::uint16_t status) noexcept {
    return status >= 200 && status != 204 && status != 205 && status != 304;
}

}  // namespace

Http2ClientSession::Http2ClientSession(
    asio::io_context& ioContext,
    HttpClientConfig config,
    std::pmr::memory_resource* resource)
    : ioContext_(ioContext),
      config_(std::move(config)),
      resource_(pmrResourceOrDefault(resource)),
      socket_(ioContext),
      resolver_(ioContext),
      conn_(resource_, http2ClientCoreConfig(), Http2Role::kClient),
      authority_(makeHttpClientHostHeader(config_, resource_)),
      scheme_(config_.tls ? "https" : "http"),
      readBuffer_(resource_),
      streams_(resource_),
      readyWaiters_(resource_),
      streamSlotWaiters_(resource_),
      sendWindowWaiters_(resource_) {
    if (config_.tls) {
        configureClientTlsContext(sslContext_, config_.tlsOptions);
    }
    tlsStream_ = std::unique_ptr<TlsStream, TlsStreamDeleter>(nullptr, TlsStreamDeleter{resource_});
}

Http2ClientSession::~Http2ClientSession() {
    for (auto& [id, stream] : streams_) {
        destroyPmrObject(stream, resource_);
    }
    streams_.clear();
}

// --- I/O primitives ------------------------------------------------------

Task<std::pair<std::error_code, std::size_t>> Http2ClientSession::readSome(asio::mutable_buffer buffer) {
    co_return co_await asyncResult<std::size_t>([this, buffer](auto handler) mutable {
        if (tlsStream_) {
            tlsStream_->async_read_some(buffer, std::move(handler));
        } else {
            socket_.async_read_some(buffer, std::move(handler));
        }
    });
}

Task<std::error_code> Http2ClientSession::writeBytes(std::string_view bytes) {
    co_return co_await asyncError([this, bytes](auto handler) mutable {
        const auto buffer = asio::buffer(bytes.data(), bytes.size());
        if (tlsStream_) {
            asio::async_write(*tlsStream_, buffer, std::move(handler));
        } else {
            asio::async_write(socket_, buffer, std::move(handler));
        }
    });
}

// --- Connection setup ----------------------------------------------------

Task<void> Http2ClientSession::connect() {
    if (state_ == State::kReady) {
        co_return;
    }
    if (state_ == State::kClosed) {
        throw std::runtime_error("http/2 session is closed");
    }
    if (state_ == State::kConnecting) {
        co_await ReadyAwaiter{this};
        if (state_ != State::kReady) {
            throw std::runtime_error("http/2 connection failed");
        }
        co_return;
    }
    state_ = State::kConnecting;
    co_await doConnect();
}

Task<void> Http2ClientSession::doConnect() {
    if (config_.proxyConnectTimeout.count() > 0) {
        connectDeadline_ = std::chrono::steady_clock::now() + config_.proxyConnectTimeout;
        hasConnectDeadline_ = true;
    }
    try {
        std::array<char, 5> portBuffer;
        const auto [portEnd, portEc] = std::to_chars(
            portBuffer.data(), portBuffer.data() + portBuffer.size(), config_.port);
        if (portEc != std::errc{}) {
            throw std::logic_error("http/2: invalid port");
        }
        const auto port = std::string_view(portBuffer.data(), static_cast<std::size_t>(portEnd - portBuffer.data()));

        auto [resolveEc, endpoints] = co_await asyncResult<asio::ip::tcp::resolver::results_type>(
            [this, port](auto handler) {
                resolver_.async_resolve(std::string_view(config_.host), port, std::move(handler));
            });
        if (resolveEc) {
            throw std::system_error(resolveEc, "http/2: resolve failed");
        }

        auto [connectEc, endpoint] = co_await asyncResult<asio::ip::tcp::endpoint>(
            [this, &endpoints](auto handler) {
                asio::async_connect(socket_, endpoints, std::move(handler));
            });
        (void)endpoint;
        if (connectEc) {
            throw std::system_error(connectEc, "http/2: connect failed");
        }
        {
            std::error_code ignored;
            socket_.set_option(asio::ip::tcp::no_delay(true), ignored);
        }

        if (config_.tls) {
            tlsStream_ = makePmrObject<TlsStream>(resource_, socket_, *sslContext_);
            // RFC 6066 SNI + RFC 6125 host-name verification, shared with the HTTP/1.1
            // pool via one owner; ALPN below is HTTP/2-specific.
            applyClientTlsIdentity(
                *tlsStream_,
                config_.tlsOptions.sniHost.empty() ? config_.host : config_.tlsOptions.sniHost,
                config_.tlsOptions.insecureSkipVerify,
                resource_);
            static constexpr unsigned char kAlpnH2[] = {2, 'h', '2'};
            if (SSL_set_alpn_protos(tlsStream_->native_handle(), kAlpnH2, sizeof(kAlpnH2)) != 0) {
                throw std::runtime_error("http/2: failed to set ALPN");
            }
            const auto handshakeEc = co_await asyncError([this](auto handler) {
                tlsStream_->async_handshake(asio::ssl::stream_base::client, std::move(handler));
            });
            if (handshakeEc) {
                throw std::system_error(handshakeEc, "http/2: TLS handshake failed");
            }
            const unsigned char* selected = nullptr;
            unsigned selectedLen = 0;
            SSL_get0_alpn_selected(tlsStream_->native_handle(), &selected, &selectedLen);
            if (selectedLen != 2 || selected == nullptr || std::memcmp(selected, "h2", 2) != 0) {
                throw std::runtime_error("http/2: server did not negotiate h2 over ALPN");
            }
        }

        // Send the client connection preface, our SETTINGS, and the connection-level
        // WINDOW_UPDATE that opens our receive window (all queued by the core).
        conn_.queueClientPreface();
        const auto preface = conn_.pendingOutput();
        const auto prefaceEc = co_await writeBytes(preface);
        if (prefaceEc) {
            throw std::system_error(prefaceEc, "http/2: failed to send preface");
        }
        conn_.consumeOutput(preface.size());
    } catch (...) {
        state_ = State::kClosed;
        std::pmr::vector<std::coroutine_handle<>> waiters(resource_);
        waiters.swap(readyWaiters_);
        for (auto handle : waiters) {
            resume(handle);
        }
        throw;
    }

    // Two detached loops hold `this`; count them so isQuiescent() knows when both have exited.
    runningLoops_ += 2;
    asyncStartTask(readLoop(), asio::detached);
    asyncStartTask(flushLoop(), asio::detached);

    co_await ReadyAwaiter{this};
    if (state_ != State::kReady) {
        throw std::runtime_error("http/2 connection closed before settings");
    }
}

// --- Background loops ----------------------------------------------------

Task<void> Http2ClientSession::readLoop() {
    struct LoopExit { std::size_t& n; ~LoopExit() { --n; } } loopExit{runningLoops_};
    readBuffer_.resize(kHttp2ReadChunk);
    for (;;) {
        auto [ec, n] = co_await readSome(asio::buffer(readBuffer_.data(), readBuffer_.size()));
        if (ec || n == 0) {
            break;
        }
        const auto result = conn_.feed(std::string_view(readBuffer_.data(), n));
        if (!settingsReceived_ && conn_.receivedPeerSettings()) {
            settingsReceived_ = true;
            if (state_ == State::kConnecting) {
                state_ = State::kReady;
            }
            std::pmr::vector<std::coroutine_handle<>> waiters(resource_);
            waiters.swap(readyWaiters_);
            for (auto handle : waiters) {
                resume(handle);
            }
        }
        drainCoreEvents();
        wakeFlusher();
        if (result.status == Http2FeedStatus::kError || conn_.closing() ||
            state_ == State::kClosed) {
            break;
        }
    }
    // Tear down through closeNow() so the socket is actually closed. On a protocol-error exit
    // state_ is not yet kClosed, and leaving the socket open would hang a peer still reading from
    // us (and a later closeNow() would early-return without closing it).
    closeNow();
    co_return;
}

Task<void> Http2ClientSession::flushLoop() {
    struct LoopExit { std::size_t& n; ~LoopExit() { --n; } } loopExit{runningLoops_};
    std::pmr::string scratch(resource_);
    for (;;) {
        co_await FlushAwaiter{this};
        if (!conn_.wantsWrite()) {
            if (state_ == State::kClosed) {
                break;
            }
            continue;
        }
        // Move out (allocator-matching swap, copy-free): the core buffer can grow
        // (reads, submits) while the write is in flight.
        conn_.takeOutput(scratch);
        const auto ec = co_await writeBytes(scratch);
        if (ec) {
            closeNow();
            break;
        }
        if (state_ == State::kClosed && !conn_.wantsWrite()) {
            break;
        }
    }
    co_return;
}

// --- Core event pump ------------------------------------------------------

void Http2ClientSession::drainCoreEvents() {
    for (;;) {
        const auto event = conn_.nextEvent();
        if (event.kind == Http2Event::Kind::kNone) {
            break;
        }
        switch (event.kind) {
            case Http2Event::Kind::kMessageHead:
                onResponseHead(event.streamId);
                break;
            case Http2Event::Kind::kMessageBodyChunk:
                onResponseChunk(event.streamId, event.bytes);
                break;
            case Http2Event::Kind::kMessageEnd:
                if (Stream* stream = findStream(event.streamId)) {
                    touchStreamDeadline(*stream);
                    markRemoteEnd(*stream);
                }
                break;
            case Http2Event::Kind::kStreamClosed:
                // Peer RST (or an equivalent local abort inside the core): the stream
                // storage stays alive under our pin; fail the fetch/reader.
                if (Stream* stream = findStream(event.streamId)) {
                    failStream(*stream, Http2ErrorCode::kCancel);
                }
                break;
            case Http2Event::Kind::kGoaway:
                handlePeerGoaway(event.streamId);  // streamId carries last-processed id
                break;
            default:
                break;
        }
    }
    // Send-window drain reports: the paced request-body senders can pull more.
    for (const auto streamId : conn_.takeUnblockedStreams()) {
        if (Stream* stream = findStream(streamId)) {
            touchStreamDeadline(*stream);  // upload progressed
        }
    }
    // Defensive sweep: a core stream can be reset on paths that do not emit
    // kStreamClosed (e.g. malformed WINDOW_UPDATE); its fetch must still fail.
    for (auto& [id, stream] : streams_) {
        if (stream->failed || stream->remoteEnded) {
            continue;
        }
        auto* core = conn_.stream(id);
        if (core == nullptr || core->isReset()) {
            failStream(*stream, Http2ErrorCode::kCancel);
        }
    }
    wakeSendWindow();  // waiters re-check readiness (drain, failure, or teardown)
}

void Http2ClientSession::onResponseHead(std::uint32_t streamId) {
    Stream* stream = findStream(streamId);
    auto* core = conn_.stream(streamId);
    if (stream == nullptr || core == nullptr) {
        return;
    }
    stream->headersComplete = true;
    const auto status = core->responseStatus();
    FetchResponseAccess::setStatus(stream->response, status);
    auto& headers = FetchResponseAccess::headers(stream->response);
    const auto count = core->requestHeaderCount();
    if (count != 0 && headers.empty()) {
        headers.reserve(count);
    }
    for (std::size_t i = 0; i < count; ++i) {
        const auto header = core->requestHeaderAt(i);
        headers.emplace_back(
            FetchResponseHeaderAccess::make(header.name, header.value, stream->requestResource));
    }
    stream->responseBodyAllowed =
        stream->responseBodyAllowed && http2ResponseStatusMayHaveBody(status);
    touchStreamDeadline(*stream);
    // For a streaming request, deliver the response headers to fetchStream as soon as
    // they arrive (the body then flows through streamReadChunk).
    if (stream->streaming) {
        signalWaiter(*stream);
    }
}

void Http2ClientSession::onResponseChunk(std::uint32_t streamId, std::string_view data) {
    Stream* stream = findStream(streamId);
    if (stream == nullptr) {
        return;
    }
    if (stream->failed) {
        // A failed streaming stream banks deferred window debt; return the credit so
        // the connection window does not shrink permanently.
        conn_.releaseStreamWindow(streamId);
        return;
    }
    if (!stream->responseBodyAllowed && !data.empty()) {
        resetStream(streamId, Http2ErrorCode::kProtocolError);
        failStream(*stream, Http2ErrorCode::kProtocolError);
        return;
    }
    touchStreamDeadline(*stream);
    if (stream->streaming) {
        // Backpressure: buffer the data; the receive-window credit stays banked in the
        // core until readChunk consumes (releaseStreamWindow), so a slow reader stalls
        // the peer instead of growing memory without bound.
        FetchResponseAccess::body(stream->response).append(data.data(), data.size());
        wakeReader(*stream);
        return;
    }
    auto& body = FetchResponseAccess::body(stream->response);
    if (stream->maxBodyBytes != 0 && body.size() + data.size() > stream->maxBodyBytes) {
        resetStream(streamId, Http2ErrorCode::kCancel);
        failStream(*stream, Http2ErrorCode::kCancel);
        return;
    }
    body.append(data.data(), data.size());
}

void Http2ClientSession::handlePeerGoaway(std::uint32_t lastStreamId) noexcept {
    // Streams above the advertised id were not processed; fail them so their fetches
    // surface the refusal. Streams at or below it keep running to completion; new
    // opens are refused by the core (openLocalStream returns 0 after peerGoaway).
    for (auto& [id, stream] : streams_) {
        if (id > lastStreamId) {
            failStream(*stream, Http2ErrorCode::kRefusedStream);
        }
    }
}

void Http2ClientSession::resetStream(std::uint32_t streamId, Http2ErrorCode error) noexcept {
    conn_.submitReset(streamId, static_cast<std::uint32_t>(error));
    wakeFlusher();
}

// --- Waking / resumption -------------------------------------------------

void Http2ClientSession::resume(std::coroutine_handle<> handle) noexcept {
    if (!handle) {
        return;
    }
    asio::post(ioContext_, [handle]() { handle.resume(); });
}

void Http2ClientSession::wakeFlusher() noexcept {
    if (flushWaiter_) {
        auto handle = flushWaiter_;
        flushWaiter_ = {};
        resume(handle);
    }
}

// --- Stream bookkeeping --------------------------------------------------

Http2ClientSession::Stream* Http2ClientSession::findStream(std::uint32_t id) noexcept {
    const auto it = streams_.find(id);
    return it == streams_.end() ? nullptr : it->second;
}

void Http2ClientSession::destroyStream(std::uint32_t id) noexcept {
    const auto it = streams_.find(id);
    if (it == streams_.end()) {
        return;
    }
    Stream* stream = it->second;
    // Return any deferred receive-window credit before the core stream is freed, then
    // release the pin: the core removes the stream and remembers it as closed (late
    // frames on the id are handled by the core's closed-stream rules).
    conn_.releaseStreamWindow(id);
    conn_.unpinStream(id);
    streams_.erase(it);
    destroyPmrObject(stream, resource_);
}

void Http2ClientSession::touchStreamDeadline(Stream& stream) noexcept {
    if (!stream.hasDeadline) {
        return;
    }
    // Sending the request body -> proxy_send_timeout; reading the response -> proxy_read_timeout.
    // A body fully handed to the core but still window-blocked is still "sending".
    const bool stillSending = !stream.localEndSent || conn_.hasBlockedSend(stream.id);
    const auto timeout = stillSending ? stream.sendTimeout : stream.readTimeout;
    if (timeout.count() > 0) {
        stream.deadline = std::chrono::steady_clock::now() + timeout;
    }
}

void Http2ClientSession::signalWaiter(Stream& stream) noexcept {
    if (stream.completed) {
        return;
    }
    stream.completed = true;
    auto handle = stream.waiter;
    stream.waiter = {};
    resume(handle);
}

void Http2ClientSession::wakeReader(Stream& stream) noexcept {
    if (stream.reader) {
        auto handle = stream.reader;
        stream.reader = {};
        resume(handle);
    }
}

void Http2ClientSession::releaseSlot(Stream& stream) noexcept {
    if (stream.slotReleased) {
        return;
    }
    stream.slotReleased = true;
    wakeStreamSlot();
}

void Http2ClientSession::markRemoteEnd(Stream& stream) noexcept {
    if (stream.remoteEnded) {
        return;
    }
    stream.remoteEnded = true;
    releaseSlot(stream);
    // An in-progress streamed request-body send should abandon once the response is complete.
    wakeSendWindow();
    if (stream.streaming) {
        wakeReader(stream);   // headers were already signaled; the reader observes EOF
    } else {
        signalWaiter(stream);  // buffered: the whole response is now available
    }
}

void Http2ClientSession::failStream(Stream& stream, Http2ErrorCode error) noexcept {
    (void)error;
    if (stream.failed) {
        return;
    }
    stream.failed = true;
    // Return any deferred receive-window credit a streaming stream banked in the core:
    // a failed stream is never read (readChunk) nor app-closed (streamClose), and the
    // withheld credit would otherwise shrink the connection window permanently.
    if (state_ != State::kClosed) {
        conn_.releaseStreamWindow(stream.id);
        wakeFlusher();
    }
    releaseSlot(stream);
    signalWaiter(stream);
    wakeReader(stream);
    wakeSendWindow();  // unblock a streamed request body waiting on this stream's send window
}

void Http2ClientSession::wakeSendWindow() noexcept {
    if (sendWindowWaiters_.empty()) {
        return;
    }
    std::pmr::vector<std::coroutine_handle<>> waiters(resource_);
    waiters.swap(sendWindowWaiters_);
    for (auto handle : waiters) {
        resume(handle);
    }
}

std::size_t Http2ClientSession::openStreamCount() const noexcept {
    std::size_t open = 0;
    for (const auto& [id, stream] : streams_) {
        if (!stream->slotReleased) {
            ++open;
        }
    }
    return open;
}

void Http2ClientSession::wakeStreamSlot() noexcept {
    if (streamSlotWaiters_.empty()) {
        return;
    }
    std::pmr::vector<SlotWaiter*> waiters(resource_);
    waiters.swap(streamSlotWaiters_);
    for (auto* waiter : waiters) {
        resume(waiter->handle);
    }
}

void Http2ClientSession::failAllStreams() noexcept {
    for (auto& [id, stream] : streams_) {
        // A stream whose complete response already arrived (END_STREAM seen) is a
        // SUCCESS; connection teardown right after it -- e.g. the server closing once
        // done -- must not turn it into a failure while its fetch resume is still
        // queued. Buffered fetches were signaled at remote end; a streaming reader
        // observes remoteEnded as EOF.
        if (stream->remoteEnded) {
            continue;
        }
        failStream(*stream, Http2ErrorCode::kInternalError);
    }
}

// --- Request path --------------------------------------------------------

namespace {

// The streaming pull source: a thin handle forwarding to the owning session by stream id, exposed
// as an HttpBodyStream. streamNextChunk holds the pulled slice in currentChunk_ (view valid until
// the next call); streamDestroy closes the stream and frees the handle.
class Http2StreamSource final {
public:
    Http2StreamSource(
        Http2ClientSession* session,
        std::uint32_t streamId,
        std::pmr::memory_resource* resource) noexcept
        : session_(session),
          currentChunk_(resource),
          resource_(resource),
          streamId_(streamId) {}

    ~Http2StreamSource() { close(); }

    static Task<std::string_view> streamNextChunk(void* self) {
        auto* source = static_cast<Http2StreamSource*>(self);
        source->currentChunk_ = co_await source->session_->streamReadChunk(source->streamId_);
        co_return std::string_view(source->currentChunk_.data(), source->currentChunk_.size());
    }
    static void streamDestroy(void* self) noexcept {
        auto* source = static_cast<Http2StreamSource*>(self);
        destroyPmrObject(source, source->resource_);
    }

private:
    void close() noexcept {
        if (!closed_) {
            closed_ = true;
            session_->streamClose(streamId_);
        }
    }

    Http2ClientSession* session_;
    std::pmr::string currentChunk_;  // backing store for the view streamNextChunk hands out
    std::pmr::memory_resource* resource_;
    std::uint32_t streamId_;
    bool closed_{false};
};

}  // namespace

Task<void> Http2ClientSession::streamRequestBody(
    std::uint32_t streamId,
    const RequestBodyStream& bodyStream) {
    // Stop sending the request body. A reset/closed stream is left to fetch() (which observes the
    // failure via the Stream and destroys it); an early full response makes the upload pointless,
    // so cancel it. Never throws; that would leak the Stream that beginRequest already registered.
    // Returns true when the caller should stop.
    auto shouldStop = [&](Stream* stream) -> bool {
        if (stream == nullptr || stream->localEndSent || stream->failed || state_ != State::kReady) {
            return true;
        }
        if (stream->remoteEnded) {
            // The server delivered a complete response before consuming the body: abandon our
            // unfinished send side with RST_STREAM, but keep the (valid) buffered response; do
            // NOT fail the stream, or fetch() would report an error despite a good response.
            resetStream(streamId, Http2ErrorCode::kCancel);
            stream->localEndSent = true;
            return true;
        }
        return false;
    };

    for (;;) {
        if (shouldStop(findStream(streamId))) {
            co_return;
        }

        std::string_view chunk;
        try {
            chunk = co_await bodyStream.nextChunk();
        } catch (...) {
            if (Stream* stream = findStream(streamId); stream != nullptr && !stream->failed) {
                resetStream(streamId, Http2ErrorCode::kInternalError);
                failStream(*stream, Http2ErrorCode::kInternalError);
            }
            co_return;
        }

        // Re-check after the producer suspension: the read loop may have reset/ended the stream.
        Stream* stream = findStream(streamId);
        if (shouldStop(stream)) {
            co_return;
        }
        if (chunk.empty()) {
            (void)conn_.submitData(streamId, {}, /*endStream=*/true);
            stream->localEndSent = true;
            wakeFlusher();
            co_return;
        }

        const auto result = conn_.submitData(streamId, chunk, /*endStream=*/false);
        wakeFlusher();
        touchStreamDeadline(*stream);
        if (result == Http2SubmitResult::kClosed) {
            co_return;  // reset while sending; fetch observes the failure
        }
        // The core queued any window-blocked remainder in order; wait for it to drain
        // before pulling the next chunk so the upload is paced by the peer's windows.
        while (conn_.hasBlockedSend(streamId)) {
            co_await SendWindowAwaiter{this, streamId};
            if (shouldStop(findStream(streamId))) {
                co_return;
            }
        }
    }
}

Task<Http2ClientSession::Stream*> Http2ClientSession::beginRequest(
    std::string_view path,
    const FetchOptions& options,
    std::pmr::memory_resource* requestResource,
    bool streaming) {
    co_await connect();
    if (options.timeout.count() < 0) {
        throw std::invalid_argument("http/2 request timeout must not be negative");
    }
    // nginx-style inactivity timeouts; FetchOptions::timeout overrides both for this request.
    const auto readTimeout = options.timeout.count() > 0 ? options.timeout : config_.proxyReadTimeout;
    const auto sendTimeout = options.timeout.count() > 0 ? options.timeout : config_.proxySendTimeout;
    // Honor the peer's SETTINGS_MAX_CONCURRENT_STREAMS: park until an open slot frees rather
    // than letting the server RST_STREAM the excess. Bound the wait by the read timeout so a
    // peer that advertises MAX_CONCURRENT_STREAMS=0 (or never frees a slot) can't hang the fetch
    // forever -- scanDeadlines resumes a past-deadline SlotWaiter with timedOut set.
    SlotWaiter slotWaiter;
    if (readTimeout.count() > 0) {
        slotWaiter.deadline = std::chrono::steady_clock::now() + readTimeout;
        slotWaiter.hasDeadline = true;
    }
    while (state_ == State::kReady &&
           openStreamCount() >= conn_.peerMaxConcurrentStreams()) {
        co_await StreamSlotAwaiter{this, &slotWaiter};
        if (slotWaiter.timedOut) {
            throw std::runtime_error("http/2: timed out waiting for a concurrency slot");
        }
    }
    if (state_ != State::kReady) {
        throw std::runtime_error("http/2 session is not ready");
    }
    if (conn_.peerGoaway()) {
        throw std::runtime_error("http/2 server is going away");
    }

    const auto method = options.method.empty() ? std::string_view("GET") : options.method;
    const auto target = path.empty() ? std::string_view("/") : path;
    if (!isValidHttpHeaderName(method)) {
        throw std::invalid_argument("http/2: invalid request method");
    }
    if (!isValidOriginFormTarget(target)) {
        throw std::invalid_argument("http/2: invalid request target");
    }
    if (options.bodyStream && !options.body.empty()) {
        throw std::invalid_argument("http/2: set either body or bodyStream, not both");
    }

    // Validate + lowercase the user headers (HTTP/2 field names are lowercase on the
    // wire); the core encodes the block.
    std::pmr::vector<std::pmr::string> loweredNames(requestResource);
    std::pmr::vector<HttpHeaderView> headerViews(requestResource);
    loweredNames.reserve(options.headers.size());
    headerViews.reserve(options.headers.size());
    for (const auto& userHeader : options.headers) {
        const auto name = userHeader.name();
        const auto value = userHeader.value();
        if (!isValidHttpHeaderName(name) || !isValidHttpHeaderValue(value)) {
            throw std::invalid_argument("http/2: invalid request header");
        }
        if (!isAllowedH2RequestHeader(name, value)) {
            throw std::invalid_argument("http/2: request header is not allowed over HTTP/2");
        }
        auto& lowerName = loweredNames.emplace_back();  // vector propagates its pmr allocator
        lowerName.reserve(name.size());
        for (const auto ch : name) {
            lowerName.push_back(static_cast<char>(asciiToLower(static_cast<unsigned char>(ch))));
        }
        headerViews.push_back(HttpHeaderView{
            std::string_view(lowerName.data(), lowerName.size()), value});
    }

    const bool streamedBody = static_cast<bool>(options.bodyStream);
    const bool hasBody = !options.body.empty() || streamedBody;
    const auto id = conn_.openLocalStream();
    if (id == 0) {
        throw std::runtime_error("http/2 stream ids exhausted");
    }
    // Pin: the core keeps the stream (and its decoded response storage) alive until
    // destroyStream unpins it, even across a peer RST.
    conn_.pinStream(id);
    if (streaming) {
        conn_.deferStreamWindowRelease(id);  // consume-paced receive window (backpressure)
    }

    Stream* stream = constructPmrObject<Stream>(resource_, requestResource);
    stream->id = id;
    stream->streaming = streaming;
    stream->responseBodyAllowed = !asciiEqualsIgnoreCase(method, "HEAD");
    stream->requestResource = requestResource;
    stream->maxBodyBytes = streaming ? 0 : config_.maxResponseBodyBytes;
    // Every stream (buffered AND streaming) gets an inactivity deadline refreshed on each frame
    // (touchStreamDeadline): proxy_send_timeout while the body is still going out, then
    // proxy_read_timeout for the response. Matches h1 and nginx; bounds stalls in either direction.
    stream->readTimeout = readTimeout;
    stream->sendTimeout = sendTimeout;
    // The initial phase is "sending" when there is a body to send, else "reading".
    const auto initialTimeout = hasBody ? sendTimeout : readTimeout;
    if (readTimeout.count() > 0 || sendTimeout.count() > 0) {
        const auto armWith = initialTimeout.count() > 0 ? initialTimeout
                                                        : std::max(readTimeout, sendTimeout);
        stream->deadline = std::chrono::steady_clock::now() + armWith;
        stream->hasDeadline = true;
    }
    try {
        streams_.emplace(id, stream);
    } catch (...) {
        conn_.unpinStream(id);
        destroyPmrObject(stream, resource_);
        throw;
    }

    conn_.submitRequestHead(
        id, method, scheme_, std::string_view(authority_), target,
        std::span<const HttpHeaderView>(headerViews.data(), headerViews.size()),
        /*endStream=*/!hasBody);
    if (!hasBody) {
        stream->localEndSent = true;
        wakeFlusher();
    } else if (streamedBody) {
        wakeFlusher();  // flush the HEADERS, then pull + send the body concurrently with the read loop
        co_await streamRequestBody(id, options.bodyStream);
    } else {
        // Buffered body: one submit; the core sends up to the send window and drains
        // the remainder in order as WINDOW_UPDATEs arrive (the read loop feeds them).
        (void)conn_.submitData(id, options.body, /*endStream=*/true);
        stream->localEndSent = true;
        wakeFlusher();
    }
    co_return stream;
}

Task<FetchResponse> Http2ClientSession::fetch(
    std::string_view path,
    const FetchOptions& options,
    std::pmr::memory_resource* resource) {
    auto* const requestResource = resource == nullptr ? resource_ : resource;
    std::string_view currentPath = path;
    FetchOptions currentOptions = options;
    std::pmr::string redirectTarget(requestResource);
    std::uint32_t hopsRemaining = options.maxRedirects;

    for (;;) {
        Stream* stream = co_await beginRequest(
            currentPath, currentOptions, requestResource, /*streaming=*/false);
        const auto id = stream->id;

        co_await StreamAwaiter{stream};
        if (stream->remoteEnded && !stream->failed) {
            // Keep the stream visible for one I/O turn after END_STREAM so same-burst late
            // frames can still fail the fetch instead of racing with destroyStream().
            // This covers both header-only responses and trailer-terminated responses.
            co_await IoTurnAwaiter{this};
        }

        const bool failed = stream->failed;
        FetchResponse response = std::move(stream->response);
        destroyStream(id);
        if (failed) {
            throw std::runtime_error("http/2 request failed");
        }
        decodeHttpClientResponseContentEncoding(
            response,
            config_.maxResponseBodyBytes != 0
                ? config_.maxResponseBodyBytes
                : kMaxDecodedRequestBodyBytes);

        if (hopsRemaining == 0 || !isHttpClientRedirectStatus(response.status())) {
            co_return response;
        }
        if (!canReplayHttpClientRedirectRequest(currentOptions, response.status())) {
            co_return response;
        }
        const auto location = findUniqueHttpClientResponseHeader(response, "location");
        if (location.empty() ||
            !resolveHttpClientSameOriginRedirect(config_, location, redirectTarget)) {
            co_return response;
        }
        currentPath = redirectTarget;
        applyHttpClientRedirectMethod(currentOptions, response.status());
        --hopsRemaining;
    }
}

Task<FetchResponseStream> Http2ClientSession::fetchStream(
    std::string_view path,
    const FetchOptions& options,
    std::pmr::memory_resource* resource) {
    auto* const requestResource = resource == nullptr ? resource_ : resource;
    Stream* stream = co_await beginRequest(path, options, requestResource, /*streaming=*/true);
    const auto id = stream->id;

    // Resumed once the response headers are decoded (or the stream fails).
    co_await StreamAwaiter{stream};
    if (stream->failed) {
        destroyStream(id);
        throw std::runtime_error("http/2 request failed");
    }

    // The source pulls DATA by stream id; status + headers go to the FetchResponseStream directly.
    auto* source = constructPmrObject<Http2StreamSource>(requestResource, this, id, requestResource);
    HttpBodyStream body(source, &Http2StreamSource::streamNextChunk, &Http2StreamSource::streamDestroy);
    auto& responseHeaders = FetchResponseAccess::headers(stream->response);
    body = maybeWrapDecodingStreamSource(
        std::move(body), responseHeaders, options.decodeStream, requestResource);
    co_return FetchResponseStreamAccess::make(
        stream->response.status(), std::move(responseHeaders), std::move(body));
}

Task<std::pmr::string> Http2ClientSession::streamReadChunk(std::uint32_t streamId) {
    for (;;) {
        Stream* stream = findStream(streamId);
        if (stream == nullptr) {
            co_return std::pmr::string(resource_);  // stream already released: end of stream
        }
        if (stream->failed) {
            destroyStream(streamId);
            throw std::runtime_error("http/2 stream failed");
        }
        auto& responseBody = FetchResponseAccess::body(stream->response);
        if (!responseBody.empty()) {
            std::pmr::string chunk(stream->requestResource);
            chunk.swap(responseBody);
            // The consumer drained the buffered bytes: re-advertise the banked
            // receive-window credit so the peer can send the next slices.
            conn_.releaseStreamWindow(streamId);
            wakeFlusher();
            co_return chunk;
        }
        if (stream->remoteEnded) {
            auto* const chunkResource = stream->requestResource;
            destroyStream(streamId);
            co_return std::pmr::string(chunkResource);  // end of stream
        }
        co_await StreamReaderAwaiter{stream};
    }
}

void Http2ClientSession::streamClose(std::uint32_t streamId) noexcept {
    Stream* stream = findStream(streamId);
    if (stream == nullptr) {
        return;
    }
    if (!stream->failed && state_ != State::kClosed) {
        // Tell a still-open peer to stop; a fully-received (remoteEnded) stream is
        // already closed and must not be RST.
        if (!stream->remoteEnded) {
            resetStream(streamId, Http2ErrorCode::kCancel);
        }
        // Return any banked receive-window credit for buffered-but-undrained DATA
        // (destroyStream also releases; doing it here keeps the wire prompt).
        conn_.releaseStreamWindow(streamId);
        wakeFlusher();
    }
    // Resume any reader parked on this stream (defensive against close() racing a suspended
    // readChunk on another coroutine) before the Stream is freed; the woken reader re-finds a
    // now-absent stream and returns end-of-stream.
    wakeReader(*stream);
    destroyStream(streamId);
}

// --- Lifecycle -----------------------------------------------------------

void Http2ClientSession::closeNow() noexcept {
    if (state_ == State::kClosed) {
        return;
    }
    state_ = State::kClosed;
    std::error_code ignored;
    resolver_.cancel();
    if (tlsStream_) {
        tlsStream_->lowest_layer().cancel(ignored);
        tlsStream_->lowest_layer().close(ignored);
    } else {
        socket_.cancel(ignored);
        socket_.close(ignored);
    }
    failAllStreams();
    wakeFlusher();
    wakeStreamSlot();
    wakeSendWindow();
    std::pmr::vector<std::coroutine_handle<>> waiters(resource_);
    waiters.swap(readyWaiters_);
    for (auto handle : waiters) {
        resume(handle);
    }
}

bool Http2ClientSession::isQuiescent() const noexcept {
    // Closed and both detached loops have exited (they hold `this`), so the session -- and the
    // stream coroutines it drives -- are done and it is safe to destroy.
    return state_ == State::kClosed && runningLoops_ == 0;
}

bool Http2ClientSession::hasAnyTimeout() const noexcept {
    return config_.proxyConnectTimeout.count() > 0 ||
           config_.proxyReadTimeout.count() > 0 ||
           config_.proxySendTimeout.count() > 0;
}

void Http2ClientSession::scanDeadlines(std::chrono::steady_clock::time_point now) noexcept {
    // A stalled handshake (no server SETTINGS) is bounded by the connect timeout.
    if (state_ == State::kConnecting && hasConnectDeadline_ && now > connectDeadline_) {
        closeNow();
        return;
    }
    if (state_ != State::kReady) {
        return;
    }
    // Time out any fetch parked waiting for a concurrency slot (peer at/over
    // MAX_CONCURRENT_STREAMS, e.g. a pathological advertise of 0). It has no Stream yet, so the
    // per-stream scan below can't see it; resume it with timedOut set so beginRequest throws.
    if (!streamSlotWaiters_.empty()) {
        std::pmr::vector<SlotWaiter*> stillWaiting(resource_);
        std::pmr::vector<SlotWaiter*> expired(resource_);
        for (auto* waiter : streamSlotWaiters_) {
            if (waiter->hasDeadline && now > waiter->deadline) {
                waiter->timedOut = true;
                expired.push_back(waiter);
            } else {
                stillWaiting.push_back(waiter);
            }
        }
        streamSlotWaiters_.swap(stillWaiting);
        for (auto* waiter : expired) {
            resume(waiter->handle);
        }
    }
    // Reset any stream idle past its inactivity deadline; failStream wakes its fetch/reader and
    // any parked request-body send. Guard on !remoteEnded && !failed rather than !completed: for a
    // STREAMING stream `completed` only means the headers were delivered (the body is still
    // arriving), so it must remain eligible; for a buffered stream completed <=> remoteEnded||failed.
    for (auto& [id, stream] : streams_) {
        if (stream->hasDeadline && !stream->remoteEnded && !stream->failed && now > stream->deadline) {
            resetStream(stream->id, Http2ErrorCode::kCancel);
            failStream(*stream, Http2ErrorCode::kCancel);
        }
    }
}

}  // namespace ruvia::detail
