#ifdef RUVIA_ENABLE_HTTP_CLIENT

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

#include "../../runtime/AsioAwait.h"
#include "../HeaderTokenUtils.h"
#include "FetchStreamSource.h"
#include "HttpClientContentEncoding.h"
#include "HttpClientConfigValidation.h"
#include "HttpClientRedirect.h"
#include "HttpClientTlsVerification.h"
#include "ruvia/http/HttpCommon.h"
#include "ruvia/memory/PmrResource.h"
#include "../../net/http2/Http2FrameCodec.h"
#include "../../net/http2/Http2FramePayload.h"
#include "../../net/http2/Http2FlowControl.h"
#include "../../net/http2/Http2HeaderRules.h"
#include "../../net/http2/Http2InputBuffer.h"
#include "../../net/http2/Http2LocalSettings.h"
#include "../../net/http2/Http2WindowUpdate.h"

namespace ruvia::detail {
namespace {

constexpr std::size_t kHttp2ReadChunk = 16 * 1024;

[[nodiscard]] bool isValidH2OriginTarget(std::string_view target) noexcept {
    if (target.empty()) {
        return false;
    }
    if (target == "*") {
        return true;
    }
    if (target.front() != '/') {
        return false;
    }
    for (const auto ch : target) {
        const auto byte = static_cast<unsigned char>(ch);
        if (byte <= 0x20 || byte == 0x7F || byte == '#' || byte == '\\') {
            return false;
        }
    }
    return true;
}

// Connection-specific headers are forbidden in HTTP/2 (RFC 7540 §8.1.2.2), as are any
// pseudo-header (leading ':') the caller tries to inject and the client-managed framing headers.
[[nodiscard]] bool isForbiddenH2RequestHeader(std::string_view name) noexcept {
    if (!name.empty() && name.front() == ':') {
        return true;
    }
    static constexpr std::string_view kForbidden[] = {
        "connection", "keep-alive", "proxy-connection", "transfer-encoding",
        "upgrade", "host", "content-length"};
    for (const auto forbidden : kForbidden) {
        if (httpAsciiEqualsIgnoreCase(name, forbidden)) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] char asciiLower(char ch) noexcept {
    return (ch >= 'A' && ch <= 'Z') ? static_cast<char>(ch - 'A' + 'a') : ch;
}

[[nodiscard]] bool http2ResponseStatusMayHaveBody(int status) noexcept {
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
      decoder_(resource_),
      authority_(makeHttpClientHostHeader(config_, resource_)),
      scheme_(config_.tls ? "https" : "http"),
      input_(resource_),
      outBuffer_(resource_),
      encodeScratch_(resource_),
      streams_(resource_),
      readyWaiters_(resource_),
      streamSlotWaiters_(resource_),
      sendWindowWaiters_(resource_) {
    decoder_.setMaxDynamicTableSize(4096);
    if (config_.tls) {
        sslContext_.emplace(asio::ssl::context::tls_client);
        sslContext_->set_default_verify_paths();
        sslContext_->set_verify_mode(asio::ssl::verify_peer);
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

Task<bool> Http2ClientSession::ensureInput(std::size_t needed) {
    while (http2AvailableInput(input_, inputOffset_) < needed) {
        const auto oldSize = input_.size();
        input_.resize(oldSize + kHttp2ReadChunk);
        auto [ec, n] = co_await readSome(asio::buffer(input_.data() + oldSize, kHttp2ReadChunk));
        input_.resize(oldSize + (ec ? 0 : n));
        if (ec || n == 0) {
            co_return false;
        }
    }
    co_return true;
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
    if (config_.connectTimeout.count() > 0) {
        connectDeadline_ = std::chrono::steady_clock::now() + config_.connectTimeout;
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
            const auto host = std::string_view(config_.host);
            std::error_code addressEc;
            asio::ip::make_address(host, addressEc);
            if (addressEc) {  // not an IP literal → set SNI
                if (SSL_set_tlsext_host_name(tlsStream_->native_handle(), config_.host.c_str()) != 1) {
                    throw std::runtime_error("http/2: failed to set TLS SNI host name");
                }
            }
            tlsStream_->set_verify_callback(HttpClientHostNameVerification(host, resource_));
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

        // Send the client connection preface, our SETTINGS, and a connection-level WINDOW_UPDATE
        // that opens our receive window from the default 65535 up to the advertised 1 MiB.
        std::array<char, kHttp2ClientPreface.size() + kHttp2LocalSettingsFrameBytes + kHttp2FrameHeaderBytes + 4> preface;
        char* out = preface.data();
        std::memcpy(out, kHttp2ClientPreface.data(), kHttp2ClientPreface.size());
        out += kHttp2ClientPreface.size();
        out = http2WriteLocalSettingsFrame(out);
        out = http2WriteWindowUpdate(out, 0, kHttp2LocalInitialWindowSize - static_cast<std::uint32_t>(kHttp2DefaultInitialWindowSize));
        const auto prefaceEc = co_await writeBytes(
            std::string_view(preface.data(), static_cast<std::size_t>(out - preface.data())));
        if (prefaceEc) {
            throw std::system_error(prefaceEc, "http/2: failed to send preface");
        }
    } catch (...) {
        state_ = State::kClosed;
        std::pmr::vector<std::coroutine_handle<>> waiters(resource_);
        waiters.swap(readyWaiters_);
        for (auto handle : waiters) {
            resume(handle);
        }
        throw;
    }

    asyncStartTask(readLoop(), asio::detached);
    asyncStartTask(flushLoop(), asio::detached);

    co_await ReadyAwaiter{this};
    if (state_ != State::kReady) {
        throw std::runtime_error("http/2 connection closed before settings");
    }
}

// --- Background loops ----------------------------------------------------

Task<void> Http2ClientSession::readLoop() {
    for (;;) {
        if (!co_await ensureInput(kHttp2FrameHeaderBytes)) {
            break;
        }
        const auto header = http2ParseFrameHeader(
            http2InputView(input_, inputOffset_, kHttp2FrameHeaderBytes));
        if (header.length > kHttp2DefaultMaxFrameSize) {
            break;  // exceeds our advertised MAX_FRAME_SIZE (FRAME_SIZE_ERROR)
        }
        if (!co_await ensureInput(kHttp2FrameHeaderBytes + header.length)) {
            break;
        }
        const auto payload = http2InputView(
            input_, inputOffset_ + kHttp2FrameHeaderBytes, header.length);
        const bool ok = onFrame(header, payload);
        http2ConsumeInput(input_, inputOffset_, kHttp2FrameHeaderBytes + header.length);
        http2ReclaimDrainedInput(input_);
        if (!ok || state_ == State::kClosed) {
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
    for (;;) {
        co_await FlushAwaiter{this};
        if (outBuffer_.empty()) {
            if (state_ == State::kClosed) {
                break;
            }
            continue;
        }
        std::pmr::string scratch(resource_);
        scratch.swap(outBuffer_);
        const auto ec = co_await writeBytes(scratch);
        if (ec) {
            closeNow();
            break;
        }
        if (state_ == State::kClosed && outBuffer_.empty()) {
            break;
        }
    }
    co_return;
}

// --- Outbound framing ----------------------------------------------------

void Http2ClientSession::queueBytes(std::string_view bytes) {
    outBuffer_.append(bytes.data(), bytes.size());
}

void Http2ClientSession::appendFrame(
    Http2FrameType type, std::uint8_t flags, std::uint32_t streamId, std::string_view payload) {
    char header[kHttp2FrameHeaderBytes];
    http2EncodeFrameHeader(header, static_cast<std::uint32_t>(payload.size()), type, flags, streamId);
    outBuffer_.append(header, kHttp2FrameHeaderBytes);
    outBuffer_.append(payload.data(), payload.size());
}

void Http2ClientSession::queueHeaders(std::uint32_t streamId, std::string_view block, bool endStream) {
    const std::size_t maxFrame = std::max<std::size_t>(peerSettings_.maxFrameSize(), 1);
    std::size_t offset = 0;
    bool first = true;
    do {
        const std::size_t chunk = std::min(block.size() - offset, maxFrame);
        const bool last = (offset + chunk == block.size());
        const auto flags = static_cast<std::uint8_t>(
            (last ? kHttp2FlagEndHeaders : 0) | ((endStream && last) ? kHttp2FlagEndStream : 0));
        appendFrame(first ? Http2FrameType::kHeaders : Http2FrameType::kContinuation,
                    flags, streamId, block.substr(offset, chunk));
        offset += chunk;
        first = false;
    } while (offset < block.size());
}

void Http2ClientSession::queueSettingsAck() {
    appendFrame(Http2FrameType::kSettings, kHttp2FlagAck, 0, {});
}

void Http2ClientSession::queueWindowUpdate(
    std::uint32_t streamId, std::uint32_t increment, bool includeStream) {
    std::array<char, kHttp2WindowUpdateFrameBytes * 2> buffer;
    char* out = http2WriteWindowUpdate(buffer.data(), 0, increment);
    if (includeStream && streamId != 0) {
        out = http2WriteWindowUpdate(out, streamId, increment);
    }
    queueBytes(std::string_view(buffer.data(), static_cast<std::size_t>(out - buffer.data())));
}

void Http2ClientSession::sendRstStream(std::uint32_t streamId, Http2ErrorCode error) {
    char payload[4];
    http2Write32(payload, static_cast<std::uint32_t>(error));
    appendFrame(Http2FrameType::kRstStream, 0, streamId, std::string_view(payload, 4));
    wakeFlusher();
}

void Http2ClientSession::sendPendingData(Stream& stream) {
    if (stream.localEndSent) {
        return;
    }
    const std::size_t maxFrame = std::max<std::size_t>(peerSettings_.maxFrameSize(), 1);
    bool wroteAnything = false;
    while (!stream.pendingBody.empty()) {
        const std::int32_t available = std::min(connectionSendWindow_, stream.flow.sendWindow());
        if (available <= 0) {
            break;  // flow-control window closed; resume on WINDOW_UPDATE
        }
        const std::size_t chunk = std::min({stream.pendingBody.size(),
                                            static_cast<std::size_t>(available), maxFrame});
        const bool last = (chunk == stream.pendingBody.size());
        appendFrame(Http2FrameType::kData, static_cast<std::uint8_t>(last ? kHttp2FlagEndStream : 0),
                    stream.id, stream.pendingBody.substr(0, chunk));
        connectionSendWindow_ -= static_cast<std::int32_t>(chunk);
        stream.flow.consumeSend(chunk);
        stream.pendingBody.remove_prefix(chunk);
        wroteAnything = true;
        if (last) {
            stream.localEndSent = true;
        }
    }
    if (wroteAnything) {
        wakeFlusher();
    }
}

Task<void> Http2ClientSession::streamRequestBody(
    std::uint32_t streamId,
    const RequestBodyStream& bodyStream) {
    const std::size_t maxFrame = std::max<std::size_t>(peerSettings_.maxFrameSize(), 1);

    // Stop sending the request body. A reset/closed stream is left to fetch() (which observes the
    // failure via the Stream and destroys it); an early full response makes the upload pointless,
    // so cancel it. Never throws — that would leak the Stream that beginRequest already registered.
    // Returns true when the caller should stop.
    auto shouldStop = [&](Stream* stream) -> bool {
        if (stream == nullptr || stream->localEndSent || stream->failed || state_ != State::kReady) {
            return true;
        }
        if (stream->remoteEnded) {
            // The server delivered a complete response before consuming the body: abandon our
            // unfinished send side with RST_STREAM, but keep the (valid) buffered response — do
            // NOT fail the stream, or fetch() would report an error despite a good response.
            sendRstStream(streamId, Http2ErrorCode::kCancel);
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
                sendRstStream(streamId, Http2ErrorCode::kInternalError);
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
            appendFrame(Http2FrameType::kData, kHttp2FlagEndStream, streamId, {});
            stream->localEndSent = true;
            wakeFlusher();
            co_return;
        }

        std::size_t offset = 0;
        while (offset < chunk.size()) {
            stream = findStream(streamId);
            if (shouldStop(stream)) {
                co_return;
            }
            const std::int32_t available = std::min(connectionSendWindow_, stream->flow.sendWindow());
            if (available <= 0) {
                co_await SendWindowAwaiter{this, streamId};
                continue;
            }
            const std::size_t frameBytes = std::min({
                chunk.size() - offset,
                static_cast<std::size_t>(available),
                maxFrame});
            appendFrame(
                Http2FrameType::kData,
                0,
                streamId,
                chunk.substr(offset, frameBytes));
            connectionSendWindow_ -= static_cast<std::int32_t>(frameBytes);
            stream->flow.consumeSend(frameBytes);
            offset += frameBytes;
            wakeFlusher();
        }
    }
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
    streams_.erase(it);
    destroyPmrObject(stream, resource_);
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
    std::pmr::vector<std::coroutine_handle<>> waiters(resource_);
    waiters.swap(streamSlotWaiters_);
    for (auto handle : waiters) {
        resume(handle);
    }
}

void Http2ClientSession::failAllStreams() noexcept {
    for (auto& [id, stream] : streams_) {
        failStream(*stream, Http2ErrorCode::kInternalError);
    }
}

// --- Frame dispatch ------------------------------------------------------

bool Http2ClientSession::onFrame(const Http2FrameHeader& header, std::string_view payload) {
    // A pending header block forbids any interleaved frame except its CONTINUATION.
    if (continuationStream_ != 0 &&
        static_cast<Http2FrameType>(header.type) != Http2FrameType::kContinuation) {
        return false;
    }
    // The peer's first frame must be a (non-ACK) SETTINGS frame (RFC 7540 §3.5).
    if (!settingsReceived_ &&
        (static_cast<Http2FrameType>(header.type) != Http2FrameType::kSettings ||
         (header.flags & kHttp2FlagAck) != 0)) {
        return false;
    }
    switch (static_cast<Http2FrameType>(header.type)) {
        case Http2FrameType::kSettings:
            return onSettings(header, payload);
        case Http2FrameType::kHeaders:
            return onHeaders(header, payload);
        case Http2FrameType::kContinuation:
            return onContinuation(header, payload);
        case Http2FrameType::kData:
            return onData(header, payload);
        case Http2FrameType::kWindowUpdate:
            return onWindowUpdate(header, payload);
        case Http2FrameType::kPing:
            return onPing(header, payload);
        case Http2FrameType::kRstStream:
            return onRstStream(header, payload);
        case Http2FrameType::kGoaway:
            if (header.streamId != 0 || payload.size() < 8) {
                return false;
            }
            onGoaway(payload);
            return true;
        case Http2FrameType::kPriority:
            if (header.streamId == 0 || payload.size() != 5) {
                return false;
            }
            if (http2Read31(reinterpret_cast<const unsigned char*>(payload.data())) == header.streamId) {
                if (Stream* stream = findStream(header.streamId); stream != nullptr) {
                    sendRstStream(stream->id, Http2ErrorCode::kProtocolError);
                    failStream(*stream, Http2ErrorCode::kProtocolError);
                    return true;
                }
                return false;
            }
            return true;  // accepted and ignored
        case Http2FrameType::kPushPromise:
            return false;  // we advertise ENABLE_PUSH=0; server push is a protocol error
    }
    return true;  // unknown frame types must be ignored
}

bool Http2ClientSession::onSettings(const Http2FrameHeader& header, std::string_view payload) {
    if (header.streamId != 0) {
        return false;
    }
    if ((header.flags & kHttp2FlagAck) != 0) {
        return payload.empty();
    }
    if (!http2SettingsPayloadSizeValid(payload)) {
        return false;
    }
    std::int64_t initialWindowDelta = 0;
    bool windowChanged = false;
    for (std::size_t offset = 0; offset + 6 <= payload.size(); offset += 6) {
        const auto entry = http2ReadSettingEntry(payload, offset);
        const auto result = peerSettings_.apply(entry.id, entry.value);
        if (result.status != Http2PeerSettingsStatus::kOk) {
            return false;
        }
        if (result.initialWindowChanged) {
            initialWindowDelta = result.initialWindowDelta;
            windowChanged = true;
        }
    }
    if (windowChanged) {
        for (auto& [id, stream] : streams_) {
            if (!stream->flow.addSendWindow(initialWindowDelta)) {
                return false;  // stream flow-control window overflow
            }
        }
    }
    queueSettingsAck();
    if (!settingsReceived_) {
        settingsReceived_ = true;
        state_ = State::kReady;
        std::pmr::vector<std::coroutine_handle<>> waiters(resource_);
        waiters.swap(readyWaiters_);
        for (auto handle : waiters) {
            resume(handle);
        }
    }
    if (windowChanged) {
        for (auto& [id, stream] : streams_) {
            sendPendingData(*stream);
        }
        wakeSendWindow();
    }
    wakeFlusher();
    return true;
}

bool Http2ClientSession::onHeaders(const Http2FrameHeader& header, std::string_view payload) {
    if (header.streamId == 0 || (header.streamId & 1U) == 0) {
        return false;  // responses arrive on the client's odd stream ids only
    }
    std::string_view fragment;
    std::uint32_t dependency = 0;
    if (!http2StripPadAndPriority(header, payload, true, fragment, &dependency)) {
        return false;
    }
    if ((header.flags & kHttp2FlagPriority) != 0) {
        if (dependency == header.streamId) {
            return false;
        }
    }
    Stream* stream = findStream(header.streamId);
    bool discard;
    if (stream != nullptr) {
        if (stream->remoteEnded) {
            return false;
        }
        // A second header block on an active stream is trailers: decode (for HPACK state) but
        // do not apply it to the response.
        discard = stream->headersComplete;
    } else if (header.streamId < nextStreamId_) {
        discard = true;  // a stream we already closed: decode to keep HPACK synced, discard output
    } else {
        return false;  // HEADERS on an idle stream we never opened → protocol error
    }
    return beginHeaderBlock(
        header.streamId, stream, discard,
        (header.flags & kHttp2FlagEndStream) != 0,
        (header.flags & kHttp2FlagEndHeaders) != 0, fragment);
}

bool Http2ClientSession::onContinuation(const Http2FrameHeader& header, std::string_view payload) {
    if (continuationStream_ == 0 || header.streamId != continuationStream_) {
        return false;
    }
    if (headerAssembly_.size() + payload.size() > kMaxHttpHeaderBytes) {
        return false;
    }
    headerAssembly_.append(payload.data(), payload.size());
    if ((header.flags & kHttp2FlagEndHeaders) != 0) {
        const auto streamId = continuationStream_;
        const bool discard = continuationDiscard_;
        const bool endStream = continuationEndStream_;
        continuationStream_ = 0;
        Stream* stream = findStream(streamId);
        return finalizeHeaderBlock(stream, !discard && stream != nullptr, endStream);
    }
    return true;
}

bool Http2ClientSession::beginHeaderBlock(
    std::uint32_t streamId, Stream* stream, bool discard, bool endStream, bool endHeaders,
    std::string_view fragment) {
    if (fragment.size() > kMaxHttpHeaderBytes) {
        return false;
    }
    headerAssembly_.assign(fragment.data(), fragment.size());
    if (endHeaders) {
        return finalizeHeaderBlock(stream, !discard && stream != nullptr, endStream);
    }
    continuationStream_ = streamId;
    continuationDiscard_ = discard;
    continuationEndStream_ = endStream;
    return true;
}

namespace {

struct H2DecodeContext final {
    FetchResponse* response{nullptr};
    std::pmr::memory_resource* resource{nullptr};
    std::size_t headerCount{0};
    std::size_t contentLength{0};
    int status{0};
    bool sawStatus{false};
    bool sawRegular{false};
    bool hasContentLength{false};
    bool malformed{false};
};

bool h2OnDecodedHeader(void* target, std::string_view name, std::string_view value) {
    auto* ctx = static_cast<H2DecodeContext*>(target);
    if (!name.empty() && name.front() == ':') {
        if (name != ":status" || ctx->sawStatus || ctx->sawRegular) {
            ctx->malformed = true;
            return false;
        }
        int status = 0;
        const auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), status);
        if (ec != std::errc{} || ptr != value.data() + value.size() || status < 100 || status > 999) {
            ctx->malformed = true;
            return false;
        }
        ctx->status = status;
        ctx->sawStatus = true;
        return true;
    }
    if (!http2IsValidRegularHeader(name, value)) {
        ctx->malformed = true;
        return false;
    }
    if (++ctx->headerCount > kMaxRequestHeaders) {
        ctx->malformed = true;
        return false;
    }
    if (name == "content-length") {
        std::size_t parsed = 0;
        const auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), parsed);
        if (ec != std::errc{} || ptr != value.data() + value.size()) {
            ctx->malformed = true;
            return false;
        }
        if (ctx->hasContentLength && ctx->contentLength != parsed) {
            ctx->malformed = true;
            return false;
        }
        ctx->contentLength = parsed;
        ctx->hasContentLength = true;
    }
    ctx->sawRegular = true;
    if (ctx->status < 200) {
        return true;
    }
    if (ctx->response->headers.empty()) {
        ctx->response->headers.reserve(8);
    }
    ctx->response->headers.emplace_back(name, value, ctx->resource);
    return true;
}

bool h2OnDecodedTrailerHeader(void* target, std::string_view name, std::string_view value) {
    auto* ctx = static_cast<H2DecodeContext*>(target);
    if (!name.empty() && name.front() == ':') {
        ctx->malformed = true;
        return false;
    }
    if (!http2IsValidRegularHeader(name, value)) {
        ctx->malformed = true;
        return false;
    }
    if (++ctx->headerCount > kMaxRequestHeaders) {
        ctx->malformed = true;
        return false;
    }
    return true;
}

}  // namespace

bool Http2ClientSession::finalizeHeaderBlock(Stream* stream, bool apply, bool endStream) {
    bool ok;
    if (apply && stream != nullptr) {
        H2DecodeContext ctx{.response = &stream->response, .resource = stream->requestResource};
        const auto result = decoder_.decode(headerAssembly_, &ctx, &h2OnDecodedHeader);
        ok = result.ok() && !ctx.malformed && ctx.sawStatus;
        if (ok) {
            if (ctx.status < 200) {
                ok = !endStream;
            } else {
                stream->response.statusCode = ctx.status;
                stream->headersComplete = true;
                stream->responseBodyAllowed =
                    stream->responseBodyAllowed &&
                    http2ResponseStatusMayHaveBody(stream->response.statusCode);
                stream->responseHasContentLength = ctx.hasContentLength;
                stream->responseContentLength = ctx.contentLength;
            }
        }
    } else if (stream != nullptr && stream->headersComplete) {
        H2DecodeContext ctx{};
        const auto result = decoder_.decode(headerAssembly_, &ctx, &h2OnDecodedTrailerHeader);
        ok = result.ok() && !ctx.malformed && endStream;
    } else {
        // Decode purely to advance the connection-global HPACK dynamic table; discard output.
        const auto result = decoder_.decode(
            headerAssembly_, nullptr,
            [](void*, std::string_view, std::string_view) { return true; });
        ok = result.ok();
    }
    headerAssembly_.clear();
    if (!ok) {
        return false;
    }
    // For a streaming request, deliver the response headers to fetchStream as soon as they arrive.
    if (apply && stream != nullptr && stream->headersComplete && stream->streaming) {
        signalWaiter(*stream);
    }
    if (stream != nullptr &&
        stream->headersComplete &&
        endStream &&
        stream->responseBodyAllowed &&
        stream->responseHasContentLength &&
        stream->responseBodyBytes != stream->responseContentLength) {
        return false;
    }
    if (stream != nullptr && stream->headersComplete && endStream) {
        markRemoteEnd(*stream);
    }
    return true;
}

bool Http2ClientSession::onData(const Http2FrameHeader& header, std::string_view payload) {
    if (header.streamId == 0) {
        return false;
    }
    std::string_view data;
    if (!http2DecodeDataPayload(header, payload, data)) {
        return false;
    }
    // The full frame payload (including padding) counts against flow control.
    const auto consumed = static_cast<std::int32_t>(header.length);
    Stream* stream = findStream(header.streamId);
    if (stream == nullptr) {
        if (header.streamId >= nextStreamId_) {
            return false;
        }
        // Already-closed stream: still replenish the connection window so the peer isn't stalled.
        queueWindowUpdate(0, static_cast<std::uint32_t>(consumed), false);
        wakeFlusher();
        return true;
    }
    if (stream->failed) {
        // Failed stream: still replenish the connection window so the peer isn't stalled.
        queueWindowUpdate(0, static_cast<std::uint32_t>(consumed), false);
        wakeFlusher();
        return true;
    }
    if (stream->remoteEnded) {
        failStream(*stream, Http2ErrorCode::kProtocolError);
        queueWindowUpdate(0, static_cast<std::uint32_t>(consumed), false);
        wakeFlusher();
        return true;
    }
    if (!stream->headersComplete) {
        sendRstStream(stream->id, Http2ErrorCode::kProtocolError);
        failStream(*stream, Http2ErrorCode::kProtocolError);
        queueWindowUpdate(0, static_cast<std::uint32_t>(consumed), false);
        wakeFlusher();
        return true;
    }
    if (!stream->flow.consumeReceive(consumed)) {
        sendRstStream(stream->id, Http2ErrorCode::kFlowControlError);
        failStream(*stream, Http2ErrorCode::kFlowControlError);
        queueWindowUpdate(0, static_cast<std::uint32_t>(consumed), false);
        wakeFlusher();
        return true;
    }
    if (!stream->responseBodyAllowed && !data.empty()) {
        sendRstStream(stream->id, Http2ErrorCode::kProtocolError);
        failStream(*stream, Http2ErrorCode::kProtocolError);
        queueWindowUpdate(0, static_cast<std::uint32_t>(consumed), false);
        wakeFlusher();
        return true;
    }
    if (stream->responseBodyAllowed && stream->responseHasContentLength) {
        if (data.size() > stream->responseContentLength ||
            stream->responseBodyBytes > stream->responseContentLength - data.size()) {
            sendRstStream(stream->id, Http2ErrorCode::kProtocolError);
            failStream(*stream, Http2ErrorCode::kProtocolError);
            queueWindowUpdate(0, static_cast<std::uint32_t>(consumed), false);
            wakeFlusher();
            return true;
        }
        stream->responseBodyBytes += data.size();
    }
    if (stream->streaming) {
        // Backpressure: buffer the data and defer the WINDOW_UPDATE until readChunk consumes it,
        // so a slow reader stalls the peer instead of growing memory without bound.
        stream->response.body.append(data.data(), data.size());
        stream->flowDebt += consumed;
        wakeReader(*stream);
    } else if (stream->maxBodyBytes != 0 &&
               stream->response.body.size() + data.size() > stream->maxBodyBytes) {
        sendRstStream(stream->id, Http2ErrorCode::kCancel);
        failStream(*stream, Http2ErrorCode::kCancel);
        queueWindowUpdate(0, static_cast<std::uint32_t>(consumed), false);
        wakeFlusher();
        return true;
    } else {
        stream->response.body.append(data.data(), data.size());
        stream->flow.restoreReceive(consumed);
        queueWindowUpdate(header.streamId, static_cast<std::uint32_t>(consumed), true);
        wakeFlusher();
    }
    if ((header.flags & kHttp2FlagEndStream) != 0) {
        if (stream->responseBodyAllowed &&
            stream->responseHasContentLength &&
            stream->responseBodyBytes != stream->responseContentLength) {
            sendRstStream(stream->id, Http2ErrorCode::kProtocolError);
            failStream(*stream, Http2ErrorCode::kProtocolError);
            wakeFlusher();
            return true;
        }
        markRemoteEnd(*stream);
    }
    return true;
}

bool Http2ClientSession::onWindowUpdate(const Http2FrameHeader& header, std::string_view payload) {
    if (payload.size() != 4) {
        return false;
    }
    const auto increment = http2WindowUpdateIncrement(payload);
    if (header.streamId == 0) {
        if (increment == 0) {
            return false;
        }
        if (http2ApplyWindowUpdate(connectionSendWindow_, increment) != Http2WindowUpdateResult::kOk) {
            return false;
        }
        for (auto& [id, stream] : streams_) {
            sendPendingData(*stream);
        }
        wakeSendWindow();  // unblock any streamed request bodies parked on the connection window
        return true;
    }
    Stream* stream = findStream(header.streamId);
    if (stream == nullptr) {
        return header.streamId < nextStreamId_;  // idle → error, closed → ignore
    }
    if (increment == 0) {
        sendRstStream(stream->id, Http2ErrorCode::kProtocolError);
        failStream(*stream, Http2ErrorCode::kProtocolError);
        return true;
    }
    if (!stream->flow.addSendWindow(increment)) {
        sendRstStream(stream->id, Http2ErrorCode::kFlowControlError);
        failStream(*stream, Http2ErrorCode::kFlowControlError);
        return true;
    }
    sendPendingData(*stream);
    wakeSendWindow();
    return true;
}

bool Http2ClientSession::onPing(const Http2FrameHeader& header, std::string_view payload) {
    if (header.streamId != 0 || payload.size() != 8) {
        return false;
    }
    if ((header.flags & kHttp2FlagAck) != 0) {
        return true;  // ACK of a ping we did not send; ignore
    }
    appendFrame(Http2FrameType::kPing, kHttp2FlagAck, 0, payload);
    wakeFlusher();
    return true;
}

bool Http2ClientSession::onRstStream(const Http2FrameHeader& header, std::string_view payload) {
    if (header.streamId == 0 || payload.size() != 4) {
        return false;
    }
    Stream* stream = findStream(header.streamId);
    if (stream == nullptr) {
        return header.streamId < nextStreamId_;
    }
    const auto error = static_cast<Http2ErrorCode>(
        http2Read32(reinterpret_cast<const unsigned char*>(payload.data())));
    failStream(*stream, error);
    return true;
}

void Http2ClientSession::onGoaway(std::string_view payload) noexcept {
    goawayReceived_ = true;
    if (payload.size() < 4) {
        return;
    }
    const auto lastStreamId = http2Read31(reinterpret_cast<const unsigned char*>(payload.data()));
    for (auto& [id, stream] : streams_) {
        if (id > lastStreamId) {
            failStream(*stream, Http2ErrorCode::kRefusedStream);
        }
    }
}

// --- Request path --------------------------------------------------------

namespace {

// The streaming pimpl: a thin handle forwarding to the owning session by stream id.
class Http2StreamSource final : public FetchStreamSource {
public:
    Http2StreamSource(
        Http2ClientSession* session,
        std::uint32_t streamId,
        int status,
        std::pmr::vector<FetchResponseHeader> headers,
        std::pmr::memory_resource* resource) noexcept
        : session_(session),
          headers_(std::move(headers)),
          resource_(resource),
          streamId_(streamId),
          status_(status) {}

    ~Http2StreamSource() override { close(); }

    [[nodiscard]] int statusCode() const noexcept override { return status_; }
    [[nodiscard]] const std::pmr::vector<FetchResponseHeader>& headers() const noexcept override {
        return headers_;
    }
    [[nodiscard]] Task<std::pmr::string> readChunk() override {
        return session_->streamReadChunk(streamId_);
    }
    void close() noexcept override {
        if (!closed_) {
            closed_ = true;
            session_->streamClose(streamId_);
        }
    }
    void destroy() noexcept override { destroyPmrObject(this, resource_); }

private:
    Http2ClientSession* session_;
    std::pmr::vector<FetchResponseHeader> headers_;
    std::pmr::memory_resource* resource_;
    std::uint32_t streamId_;
    int status_;
    bool closed_{false};
};

}  // namespace

Task<Http2ClientSession::Stream*> Http2ClientSession::beginRequest(
    std::string_view path,
    const FetchOptions& options,
    std::pmr::memory_resource* requestResource,
    bool streaming) {
    co_await connect();
    // Honor the peer's SETTINGS_MAX_CONCURRENT_STREAMS: park until an open slot frees rather
    // than letting the server RST_STREAM the excess.
    while (state_ == State::kReady &&
           openStreamCount() >= peerSettings_.maxConcurrentStreams()) {
        co_await StreamSlotAwaiter{this};
    }
    if (state_ != State::kReady) {
        throw std::runtime_error("http/2 session is not ready");
    }
    if (goawayReceived_) {
        throw std::runtime_error("http/2 server is going away");
    }
    if (nextStreamId_ > 0x7fffffffU) {
        throw std::runtime_error("http/2 stream ids exhausted");
    }

    const auto method = options.method.empty() ? std::string_view("GET") : options.method;
    const auto target = path.empty() ? std::string_view("/") : path;
    if (!isValidHttpHeaderName(method)) {
        throw std::invalid_argument("http/2: invalid request method");
    }
    if (!isValidH2OriginTarget(target)) {
        throw std::invalid_argument("http/2: invalid request target");
    }
    if (options.timeout.count() < 0) {
        throw std::invalid_argument("http/2 request timeout must not be negative");
    }
    if (options.bodyStream.valid() && !options.body.empty()) {
        throw std::invalid_argument("http/2: set either body or bodyStream, not both");
    }

    // Encode the request header block: pseudo-headers first, then validated user headers.
    encodeScratch_.clear();
    HpackEncoder::encodeHeader(encodeScratch_, ":method", method);
    HpackEncoder::encodeHeader(encodeScratch_, ":scheme", scheme_);
    HpackEncoder::encodeHeader(encodeScratch_, ":authority", std::string_view(authority_));
    HpackEncoder::encodeHeader(encodeScratch_, ":path", target);
    std::pmr::string lowerName(resource_);
    for (const auto& userHeader : options.headers) {
        if (!isValidHttpHeaderName(userHeader.name) || !isValidHttpHeaderValue(userHeader.value)) {
            throw std::invalid_argument("http/2: invalid request header");
        }
        if (isForbiddenH2RequestHeader(userHeader.name)) {
            throw std::invalid_argument("http/2: request header is not allowed over HTTP/2");
        }
        lowerName.clear();
        lowerName.reserve(userHeader.name.size());
        for (const auto ch : userHeader.name) {
            lowerName.push_back(asciiLower(ch));
        }
        HpackEncoder::encodeHeader(encodeScratch_, lowerName, userHeader.value);
    }

    const bool streamedBody = options.bodyStream.valid();
    const bool hasBody = !options.body.empty() || streamedBody;
    const auto id = nextStreamId_;
    nextStreamId_ += 2;

    Stream* stream = constructPmrObject<Stream>(resource_, requestResource);
    stream->id = id;
    stream->streaming = streaming;
    stream->responseBodyAllowed = !httpAsciiEqualsIgnoreCase(method, "HEAD");
    stream->requestResource = requestResource;
    stream->maxBodyBytes = streaming ? 0 : config_.maxResponseBodyBytes;
    stream->flow.setSendWindow(peerSettings_.initialWindowSize());
    stream->pendingBody = options.body;  // empty when streaming from bodyStream
    // A streamed request/response is inherently long-lived; only fully-buffered fetches get a deadline.
    const auto requestTimeout = options.timeout.count() > 0 ? options.timeout : config_.requestTimeout;
    if (!streaming && !streamedBody && requestTimeout.count() > 0) {
        stream->deadline = std::chrono::steady_clock::now() + requestTimeout;
        stream->hasDeadline = true;
    }
    try {
        streams_.emplace(id, stream);
    } catch (...) {
        destroyPmrObject(stream, resource_);
        throw;
    }

    queueHeaders(id, encodeScratch_, !hasBody);
    if (!hasBody) {
        stream->localEndSent = true;
        wakeFlusher();
    } else if (streamedBody) {
        wakeFlusher();  // flush the HEADERS, then pull + send the body concurrently with the read loop
        co_await streamRequestBody(id, options.bodyStream);
    } else {
        sendPendingData(*stream);
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

        if (hopsRemaining == 0 || !isHttpClientRedirectStatus(response.statusCode)) {
            co_return response;
        }
        if (!canReplayHttpClientRedirectRequest(currentOptions, response.statusCode)) {
            co_return response;
        }
        const auto location = findHttpClientResponseHeader(response, "location");
        if (location.empty() ||
            !resolveHttpClientSameOriginRedirect(config_, location, redirectTarget)) {
            co_return response;
        }
        currentPath = redirectTarget;
        applyHttpClientRedirectMethod(currentOptions, response.statusCode);
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

    // Hand the status + headers to the source; the stream keeps buffering DATA for readChunk().
    auto* source = constructPmrObject<Http2StreamSource>(
        requestResource, this, id, stream->response.statusCode,
        std::move(stream->response.headers), requestResource);
    co_return FetchResponseStream(
        std::unique_ptr<FetchStreamSource, FetchStreamSourceDeleter>(source));
}

Task<std::pmr::string> Http2ClientSession::streamReadChunk(std::uint32_t streamId) {
    for (;;) {
        Stream* stream = findStream(streamId);
        if (stream == nullptr) {
            co_return std::pmr::string(resource_);  // stream already released → end of stream
        }
        if (stream->failed) {
            auto* const chunkResource = stream->requestResource;
            destroyStream(streamId);
            (void)chunkResource;
            throw std::runtime_error("http/2 stream failed");
        }
        if (!stream->response.body.empty()) {
            std::pmr::string chunk(stream->requestResource);
            chunk.swap(stream->response.body);
            if (stream->flowDebt > 0) {
                stream->flow.restoreReceive(stream->flowDebt);
                // Once the stream is closed, only the connection window needs crediting; a
                // stream-scoped WINDOW_UPDATE on a closed stream can trip a strict peer.
                queueWindowUpdate(
                    streamId, static_cast<std::uint32_t>(stream->flowDebt), !stream->remoteEnded);
                stream->flowDebt = 0;
                wakeFlusher();
            }
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
    if (!stream->remoteEnded && !stream->failed && state_ != State::kClosed) {
        // Tell the peer to stop and return any withheld flow-control credit to the connection.
        sendRstStream(streamId, Http2ErrorCode::kCancel);
        if (stream->flowDebt > 0) {
            queueWindowUpdate(0, static_cast<std::uint32_t>(stream->flowDebt), false);
            stream->flowDebt = 0;
            wakeFlusher();
        }
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

bool Http2ClientSession::hasAnyTimeout() const noexcept {
    return config_.connectTimeout.count() > 0 || config_.requestTimeout.count() > 0;
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
    // Reset any stream whose per-request deadline has elapsed; failStream resumes its fetch.
    for (auto& [id, stream] : streams_) {
        if (stream->hasDeadline && !stream->completed && now > stream->deadline) {
            sendRstStream(stream->id, Http2ErrorCode::kCancel);
            failStream(*stream, Http2ErrorCode::kCancel);
        }
    }
}

}  // namespace ruvia::detail

#endif  // RUVIA_ENABLE_HTTP_CLIENT
