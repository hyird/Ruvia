#include "ruvia/http/detail/http2/Http2Connection.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <utility>

#include "ruvia/http/detail/HttpResponseBodyAccess.h"
#include "ruvia/http/detail/HttpResponseFileAccess.h"
#include "ruvia/http/detail/http2/Http2BodyQueue.h"
#include "ruvia/http/detail/http2/Http2BodyState.h"
#include "ruvia/http/detail/http2/Http2FlowControl.h"
#include "ruvia/http/detail/http2/Http2FrameCodec.h"
#include "ruvia/http/detail/http2/Http2FramePayload.h"
#include "ruvia/http/detail/http2/Http2HeaderBlock.h"
#include "ruvia/http/detail/http2/Http2RequestHeaders.h"
#include "ruvia/http/detail/http2/Http2HeaderRules.h"
#include "ruvia/http/detail/http2/Http2ResponseHeaders.h"
#include "ruvia/http/detail/http2/Http2WebSocketHandshake.h"
#include "ruvia/http/detail/http2/Http2WindowUpdate.h"
#include "ruvia/http/detail/server/HttpFinalResponseControlPlan.h"

namespace ruvia::detail {

namespace {
// Defense-in-depth budgets for the sans-I/O h2 core. The core has no clock, so these are
// per-connection counters (not rates); both trip GOAWAY(ENHANCE_YOUR_CALM).
//
// Rapid-reset (CVE-2023-44487): a peer can open a stream, let HEADERS complete so the
// owner spawns a handler, then RST it to free the concurrency slot -- repeating forever
// without ever tripping the 128-stream cap. We allow the peer to reset up to this many
// MORE streams than it has let run to completion; a flood that never lets a response
// finish climbs to the cap and trips, while legitimate cancels interleaved with
// completed responses keep refilling the budget and never trip.
constexpr std::uint32_t kHttp2MaxUnprocessedResets = 1000;
// PING flood: every non-ACK PING is echoed as an ACK into the outbound buffer. Cap the
// number of inbound PINGs seen since the owner last drained output; the counter resets on
// every output drain (consumeOutput/takeOutput = ACKs flushed), so healthy keepalive
// never trips and only a peer piling on PINGs faster than we can flush the ACKs does.
constexpr std::uint32_t kHttp2MaxUndrainedPings = 1000;

[[nodiscard]] bool http2IsValidOutboundMethod(std::string_view method) noexcept {
    return method != "CONNECT" && isValidHttpMethodToken(method);
}

[[nodiscard]] bool http2AreValidOutboundRequestHeaders(
    std::string_view authority,
    std::uint16_t defaultPort,
    std::span<const HttpHeaderView> headers,
    bool allowHost,
    bool allowTrailers) noexcept {
    std::uint32_t singletonHeaders = 0;
    bool hostSeen = false;
    for (const auto& header : headers) {
        if (!http2IsValidRegularHeader(header.name(), header.value())) {
            return false;
        }
        const auto kind = classifyRequestHeader(header.name());
        // Content-Length belongs exclusively to Http2RequestContent, even when a raw
        // value happens to match. Accepting both would restore two framing truths.
        if (kind == RequestHeaderKind::kContentLength) {
            return false;
        }
        if (!allowTrailers &&
            (header.name() == "te" || header.name() == "trailer")) {
            return false;
        }
        if (kind == RequestHeaderKind::kHost) {
            if (!allowHost || hostSeen || !isValidHostHeader(header.value()) ||
                !authorityMatchesHost(authority, header.value(), defaultPort)) {
                return false;
            }
            hostSeen = true;
        }
        if (const auto bit = singletonRequestHeaderBit(kind); bit != 0) {
            if ((singletonHeaders & bit) != 0) {
                return false;
            }
            singletonHeaders |= bit;
        }
    }
    return true;
}

[[nodiscard]] bool http2IsValidOutboundRegularRequestHead(
    std::string_view method,
    std::string_view scheme,
    std::string_view authority,
    std::string_view path,
    std::span<const HttpHeaderView> headers) noexcept {
    if (!http2IsValidOutboundMethod(method) ||
        (scheme != "http" && scheme != "https") ||
        !isValidHostHeader(authority) ||
        !isValidOriginFormTarget(path)) {
        return false;
    }
    const auto defaultPort = static_cast<std::uint16_t>(scheme == "https" ? 443 : 80);
    return http2AreValidOutboundRequestHeaders(
        authority, defaultPort, headers, /*allowHost=*/true, /*allowTrailers=*/true);
}

[[nodiscard]] bool http2IsValidWebSocketConnectHeaders(
    std::span<const HttpHeaderView> headers) noexcept {
    bool sawVersion = false;
    for (const auto& header : headers) {
        if (header.name() == "host" ||
            header.name() == "sec-websocket-key" ||
            header.name() == "sec-websocket-accept") {
            return false;
        }
        if (header.name() == "sec-websocket-version") {
            if (sawVersion || header.value() != "13") {
                return false;
            }
            sawVersion = true;
        }
    }
    return sawVersion;
}

[[nodiscard]] bool http2IsValidConnectResponseHead(
    const HttpResponse& response) noexcept {
    if (response.status() < 200 || response.status() >= 300 ||
        responseBodySize(response) != 0 || responseHasFileBody(response)) {
        return false;
    }
    for (const auto& header : response.headers()) {
        const auto known = responseHeaderKnownBit(header);
        if (known == kResponseHeaderContentLength ||
            known == kResponseHeaderTransferEncoding ||
            httpAsciiEqualsIgnoreCase(header.name(), "content-length") ||
            httpAsciiEqualsIgnoreCase(header.name(), "transfer-encoding")) {
            return false;
        }
    }
    return true;
}
}  // namespace

Http2Connection::Http2Connection(
    std::pmr::memory_resource* resource,
    Http2Role role,
    Http2ConnectionLimits limits)
    : resource_(resource),
      limits_(limits),
      input_(resource),
      outBuffer_(resource),
      streams_(resource),
      decoder_(resource),
      peerSettings_(role),
      events_(resource),
      pendingSends_(resource),
      drainedDataStreams_(resource),
      takenDrainedDataStreams_(resource),
      pinnedStreams_(resource),
      role_(role),
      connectionSendWindow_(kHttp2DefaultInitialWindowSize),
      connectionReceiveWindow_(
          static_cast<std::int32_t>(Http2LocalSettings::kInitialWindowSize)) {
    decoder_.setMaxDynamicTableSize(Http2LocalSettings::kHeaderTableSize);
}

// --- outbound byte buffer (batched writes) ------------------------------------

std::string_view Http2Connection::pendingOutput() const noexcept {
    return std::string_view(outBuffer_).substr(outOffset_);
}

void Http2Connection::consumeOutput(std::size_t n) noexcept {
    outOffset_ += n;
    if (outOffset_ >= outBuffer_.size()) {
        outBuffer_.clear();
        outOffset_ = 0;
        consecutivePings_ = 0;  // outbound (incl. PING ACKs) fully flushed
    }
}

void Http2Connection::takeOutput(std::pmr::string& into) {
    consecutivePings_ = 0;  // outbound (incl. PING ACKs) is being flushed
    if (outOffset_ == 0 && into.get_allocator() == outBuffer_.get_allocator()) {
        into.swap(outBuffer_);   // copy-free; outBuffer_ inherits into's old capacity
        outBuffer_.clear();
    } else {
        into.assign(outBuffer_.data() + outOffset_, outBuffer_.size() - outOffset_);
        outBuffer_.clear();
        outOffset_ = 0;
    }
}

// --- event queue --------------------------------------------------------------

std::optional<Http2Event> Http2Connection::nextEvent() {
    if (eventOffset_ < events_.size()) {
        return events_[eventOffset_++];
    }
    events_.clear();
    eventOffset_ = 0;
    return std::nullopt;
}

std::span<const std::uint32_t> Http2Connection::takeDrainedDataStreams() noexcept {
    // Swap-and-clear so each drain is reported exactly once; the returned span stays
    // valid until the next call (double buffer, no allocation churn).
    takenDrainedDataStreams_.swap(drainedDataStreams_);
    drainedDataStreams_.clear();
    return std::span<const std::uint32_t>(
        takenDrainedDataStreams_.data(), takenDrainedDataStreams_.size());
}

// =============================================================================
// Frame processing below is a 1:1 port of the retired coroutine session's pure
// logic: inline async_write became append-to-outBuffer_, coroutine resume became
// events / drained-data marking. The coroutine stack itself is deleted; this
// core is the single h2 implementation for both server and client roles.
// =============================================================================

void Http2Connection::appendFrame(
    Http2FrameType type, std::uint8_t flags, std::uint32_t streamId,
    std::string_view first, std::string_view second) {
    std::array<char, kHttp2FrameHeaderBytes> header;
    http2EncodeFrameHeader(
        header.data(),
        static_cast<std::uint32_t>(first.size() + second.size()),
        type, flags, streamId);
    outBuffer_.append(header.data(), kHttp2FrameHeaderBytes);
    outBuffer_.append(first.data(), first.size());
    if (!second.empty()) {
        outBuffer_.append(second.data(), second.size());
    }
}

void Http2Connection::appendGoawayFrame(
    std::uint32_t lastStreamId, Http2ErrorCode error, std::string_view debug) {
    std::array<char, 8> payload;
    auto* out = http2WriteGoawayPayload(payload.data(), lastStreamId, error);
    appendFrame(
        Http2FrameType::kGoaway, 0, 0,
        std::string_view(payload.data(), static_cast<std::size_t>(out - payload.data())), debug);
}

void Http2Connection::appendGoaway(Http2ErrorCode error, std::string_view debug) {
    connectionError_ = error;
    appendGoawayFrame(lastStreamId_, error, debug);
}

void Http2Connection::beginDrain() {
    // Graceful drain (RFC 9113 §6.8): advertise GOAWAY(NO_ERROR) at the last accepted
    // stream id WITHOUT a connection error -- established streams keep running, and HEADERS
    // for a stream above the advertised id are refused in processHeaders.
    if (draining_ || connectionError_) {
        return;
    }
    draining_ = true;
    goawayLastStreamId_ = lastStreamId_;
    appendGoawayFrame(lastStreamId_, Http2ErrorCode::kNoError, "connection draining");
}

bool Http2Connection::applySettingsPayload(std::string_view payload) {
    if (!http2SettingsPayloadSizeValid(payload)) {
        appendGoaway(Http2ErrorCode::kFrameSizeError, "invalid SETTINGS size");
        return false;
    }
    for (std::size_t offset = 0; offset < payload.size(); offset += 6) {
        const auto entry = http2ReadSettingEntry(payload, offset);
        const auto result = peerSettings_.apply(entry.id, entry.value);
        if (const auto* failure = result.failure()) {
            appendGoaway(
                http2PeerSettingErrorCode(failure->error()),
                http2PeerSettingErrorMessage(failure->error()));
            return false;
        }
        const auto* initialWindowChange = result.initialWindowChange();
        if (initialWindowChange &&
            !http2ApplyStreamSendWindowDelta(streams_, initialWindowChange->delta())) {
            appendGoaway(Http2ErrorCode::kFlowControlError, "stream window overflow");
            return false;
        }
    }
    return true;
}

bool Http2Connection::processSettings(const Http2FrameHeader& header, std::string_view payload) {
    if (header.streamId != 0) {
        appendGoaway(Http2ErrorCode::kProtocolError, "SETTINGS stream id must be zero");
        return false;
    }
    if ((header.flags & kHttp2FlagAck) != 0) {
        if (!payload.empty()) {
            appendGoaway(Http2ErrorCode::kFrameSizeError, "SETTINGS ack payload");
            return false;
        }
        if (prefacePhase_ == PrefacePhase::kAwaitingPeerSettings) {
            // Each peer preface must start with a (possibly empty) non-ACK SETTINGS
            // frame (RFC 9113 §3.4); an ACK alone does not satisfy either role.
            appendGoaway(Http2ErrorCode::kProtocolError, "SETTINGS ACK before SETTINGS");
            return false;
        }
        return true;
    }
    if (!applySettingsPayload(payload)) {
        return false;
    }
    if (prefacePhase_ == PrefacePhase::kAwaitingPeerSettings) {
        prefacePhase_ = PrefacePhase::kReady;
    }
    appendFrame(Http2FrameType::kSettings, kHttp2FlagAck, 0, {});
    // SETTINGS_INITIAL_WINDOW_SIZE may have opened send windows: drain deferred DATA
    // and report streams whose core-owned remainder completed.
    markSendWindowOpened();
    return true;
}

void Http2Connection::appendRstStream(std::uint32_t streamId, Http2ErrorCode error) {
    std::array<char, 4> payload;
    auto* out = http2Write32(payload.data(), static_cast<std::uint32_t>(error));
    appendFrame(
        Http2FrameType::kRstStream, 0, streamId,
        std::string_view(payload.data(), static_cast<std::size_t>(out - payload.data())));
}

std::size_t Http2Connection::sendDataUpToWindow(
    Http2StreamState& stream,
    std::string_view data,
    std::size_t offset,
    Http2EndStream endStream) {
    const auto total = data.size();
    while (offset < total) {
        const auto available = http2AvailableSendWindow(connectionSendWindow_, stream);
        if (available == 0) {
            break;  // window exhausted; caller buffers the remainder
        }
        const auto chunk = std::min<std::size_t>(
            {total - offset, available, peerSettings_.maxFrameSize()});
        const bool last = offset + chunk == total;
        http2ConsumeSendWindow(connectionSendWindow_, stream, chunk);
        appendFrame(
            Http2FrameType::kData,
            static_cast<std::uint8_t>(
                http2EndsStream(endStream) && last ? kHttp2FlagEndStream : 0),
            stream.id(), data.substr(offset, chunk));
        stream.commitLocalContent(chunk);
        offset += chunk;
    }
    return offset;
}

void Http2Connection::markSendWindowOpened() {
    // Drain core-owned DATA remainders now that a window may have opened. Completion
    // reports that the owner may submit the stream's next source chunk.
    for (std::size_t i = 0; i < pendingSends_.size();) {
        auto& pending = pendingSends_[i];
        auto* stream = findStream(pending.streamId);
        if (stream == nullptr || stream->isReset()) {
            pendingSends_.erase(pendingSends_.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        pending.offset = sendDataUpToWindow(
            *stream, std::string_view(pending.bytes.data(), pending.bytes.size()),
            pending.offset, pending.endStream);
        if (pending.offset >= pending.bytes.size()) {
            // The body fully drained. If a trailer block was queued behind it, emit it
            // now as the terminal HEADERS(END_STREAM) -- strictly AFTER all the DATA.
            if (!pending.trailerBlock.empty() && !stream->isReset()) {
                appendResponseHeaderFrames(
                    *stream,
                    std::string_view(pending.trailerBlock.data(), pending.trailerBlock.size()),
                    Http2EndStream::kEndStream);
            }
            if (http2EndsStream(pending.endStream) || !pending.trailerBlock.empty()) {
                stream->markLocalEndStreamCommitted();
                releaseLocalRequestStreamIfClosed(*stream);
            }
            drainedDataStreams_.push_back(pending.streamId);
            pendingSends_.erase(pendingSends_.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
            ++i;  // still window-blocked; keep the remainder for the next opening
        }
    }
}

bool Http2Connection::processWindowUpdate(const Http2FrameHeader& header, std::string_view payload) {
    if (payload.size() != 4) {
        appendGoaway(Http2ErrorCode::kFrameSizeError, "invalid WINDOW_UPDATE");
        return false;
    }
    const auto increment = http2WindowUpdateIncrement(payload);
    if (header.streamId == 0) {
        switch (http2ApplyWindowUpdate(connectionSendWindow_, increment)) {
            case Http2WindowUpdateResult::kOk:
                markSendWindowOpened();
                return true;
            case Http2WindowUpdateResult::kZeroIncrement:
                appendGoaway(Http2ErrorCode::kProtocolError, "zero connection WINDOW_UPDATE");
                return false;
            case Http2WindowUpdateResult::kOverflow:
                appendGoaway(Http2ErrorCode::kFlowControlError, "connection window overflow");
                return false;
        }
        return true;
    }
    auto* stream = streams_.find(header.streamId);
    if (stream == nullptr) {
        if (isIdleStreamId(header.streamId)) {
            appendGoaway(Http2ErrorCode::kProtocolError, "WINDOW_UPDATE on idle stream");
            return false;
        }
        return true;
    }
    switch (http2ApplyStreamWindowUpdate(*stream, increment)) {
        case Http2WindowUpdateResult::kOk:
            markSendWindowOpened();
            return true;
        case Http2WindowUpdateResult::kZeroIncrement:
            appendRstStream(header.streamId, Http2ErrorCode::kProtocolError);
            closeStream(
                header.streamId,
                Http2StreamCloseSource::kLocal,
                Http2ErrorCode::kProtocolError);
            markSendWindowOpened();
            return true;
        case Http2WindowUpdateResult::kOverflow:
            appendRstStream(header.streamId, Http2ErrorCode::kFlowControlError);
            closeStream(
                header.streamId,
                Http2StreamCloseSource::kLocal,
                Http2ErrorCode::kFlowControlError);
            markSendWindowOpened();
            return true;
    }
    return true;
}

bool Http2Connection::isPinned(std::uint32_t streamId) const noexcept {
    return std::find(pinnedStreams_.begin(), pinnedStreams_.end(), streamId) != pinnedStreams_.end();
}

void Http2Connection::pinStream(std::uint32_t streamId) {
    if (!isPinned(streamId)) {
        pinnedStreams_.push_back(streamId);
    }
}

void Http2Connection::flushWindowDebt(Http2StreamState& stream) {
    // A streaming consumer's banked receive-window credit (deferStreamWindowRelease)
    // must return to the CONNECTION window when the stream is removed, or the owner's
    // releaseStreamWindow-after-removal is a silent no-op and the connection window
    // shrinks permanently (nghttp2 self-heals unconsumed data at stream close the same
    // way). Connection-scope only: a stream-scope WINDOW_UPDATE on a gone stream is a
    // peer protocol error.
    const auto debt = stream.takeWindowDebt();
    if (debt == 0) {
        return;
    }
    http2CreditConnectionReceiveWindow(
        connectionReceiveWindow_, static_cast<std::int32_t>(debt));
    char buf[kHttp2WindowUpdateFrameBytes];
    http2WriteWindowUpdate(buf, 0, debt);
    outBuffer_.append(buf, sizeof(buf));
}

void Http2Connection::detachActiveHeaderBlock(Http2StreamState& stream) {
    if (!headerContinuation_.matches(stream.id())) {
        return;
    }
    if (headerContinuation_.kind() == Http2HeaderBlockKind::kDiscarded) {
        // An owner-side terminal transition won the race while an already-invalid
        // block was still awaiting CONTINUATION. The RST just emitted is now the final
        // local frame; finish decoding the peer block later without emitting another.
        discardedHeaderAction_ = DiscardedHeaderAction::kIgnore;
        return;
    }
    if (discardedHeaderStream_) {
        appendGoaway(Http2ErrorCode::kProtocolError, "overlapping detached HEADERS state");
        return;
    }
    discardedHeaderStream_.emplace(stream.id(), resource_);
    // Both strings use the connection resource, so the partial compressed block moves
    // without allocation. Future CONTINUATION fragments complete it in detached state.
    discardedHeaderStream_->requestHeaderBlock().swap(stream.requestHeaderBlock());
    discardedHeaderAction_ = DiscardedHeaderAction::kIgnore;
    headerContinuation_.start(stream.id(), Http2HeaderBlockKind::kDiscarded);
}

void Http2Connection::unpinStream(std::uint32_t streamId) {
    pinnedStreams_.erase(
        std::remove(pinnedStreams_.begin(), pinnedStreams_.end(), streamId), pinnedStreams_.end());
    auto* stream = streams_.find(streamId);
    if (stream == nullptr) {
        return;  // never created, or already removed
    }
    if (stream->isReset()) {
        // The abnormal terminal transition already returned flow-control debt and
        // discarded deferred sends. The pin only kept request-view storage alive.
        releaseLocalRequestStream(*stream);
        streams_.remove(streamId);
        return;
    }

    if (stream->peerEndStream() && stream->localEndStreamCommitted()) {
        // Both protocol halves are closed and the owner lease is gone: normal
        // completion can finally release storage and refill the rapid-reset budget.
        detachActiveHeaderBlock(*stream);
        discardDeferredStreamState(streamId);
        flushWindowDebt(*stream);
        readyQueue_.remove(streamId);
        closedStreams_.remember(streamId, Http2StreamCloseSource::kLocal);
        ++completedResponses_;
        releaseLocalRequestStreamIfClosed(*stream);
        streams_.remove(streamId);
        return;
    }

    // Releasing the last owner while either protocol half is still open must not
    // silently erase the stream. Abort it explicitly so the peer observes a legal
    // terminal transition and the table cannot leak an ownerless half-open stream.
    const auto error = stream->localEndStreamCommitted()
        ? Http2ErrorCode::kNoError
        : Http2ErrorCode::kCancel;
    (void)submitReset(streamId, error);
}

void Http2Connection::discardDeferredStreamState(std::uint32_t streamId) {
    pendingSends_.erase(
        std::remove_if(
            pendingSends_.begin(), pendingSends_.end(),
            [streamId](const Http2PendingSend& pending) {
                return pending.streamId == streamId;
            }),
        pendingSends_.end());
    drainedDataStreams_.erase(
        std::remove(drainedDataStreams_.begin(), drainedDataStreams_.end(), streamId),
        drainedDataStreams_.end());
    if (auto* stream = streams_.find(streamId); stream != nullptr) {
        http2ReleaseResponseHeaderBlock(*stream);
        http2ReleaseResponseTrailerBlock(*stream);
    }
}

bool Http2Connection::closeStreamImpl(
    std::uint32_t streamId,
    Http2StreamCloseSource source,
    Http2ErrorCode error,
    CloseNotification notification) {
    auto* stream = streams_.find(streamId);
    if (stream != nullptr && !stream->isReset()) {
        detachActiveHeaderBlock(*stream);
    }
    readyQueue_.remove(streamId);
    discardDeferredStreamState(streamId);
    if (stream == nullptr || stream->isReset()) {
        return false;
    }

    releaseLocalRequestStream(*stream);
    stream->markClosed(source);
    if (notification == CloseNotification::kEmitEvent) {
        events_.push_back(Http2Event::streamClosed(streamId, source, error));
    }
    // A pin protects request views only. Protocol resources and connection-level
    // flow credit are released immediately even if a sleeping handler retains storage.
    flushWindowDebt(*stream);
    closedStreams_.remember(streamId, source);
    if (!isPinned(streamId)) {
        streams_.remove(streamId);
    }
    return true;
}

bool Http2Connection::closeStream(
    std::uint32_t streamId,
    Http2StreamCloseSource source,
    Http2ErrorCode error) {
    return closeStreamImpl(
        streamId, source, error, CloseNotification::kEmitEvent);
}

bool Http2Connection::closeStreamByOwner(std::uint32_t streamId) {
    return closeStreamImpl(
        streamId,
        Http2StreamCloseSource::kLocal,
        Http2ErrorCode::kNoError,
        CloseNotification::kOwnerAlreadyKnows);
}

bool Http2Connection::processRstStream(const Http2FrameHeader& header, std::string_view payload) {
    if (payload.size() != 4) {
        appendGoaway(Http2ErrorCode::kFrameSizeError, "invalid RST_STREAM");
        return false;
    }
    if (header.streamId == 0) {
        appendGoaway(Http2ErrorCode::kProtocolError, "RST_STREAM stream id must be nonzero");
        return false;
    }
    if (streams_.find(header.streamId) == nullptr &&
        isIdleStreamId(header.streamId)) {
        appendGoaway(Http2ErrorCode::kProtocolError, "RST_STREAM on idle stream");
        return false;
    }
    if (closedStreams_.source(header.streamId) ==
        Http2StreamCloseSource::kPeerGoaway) {
        // The peer already declared this request unprocessed. A trailing reset has no
        // stream lifecycle left to mutate and must not consume the rapid-reset budget.
        return true;
    }
    const auto error = static_cast<Http2ErrorCode>(http2Read32(
        reinterpret_cast<const unsigned char*>(payload.data())));
    closeStream(header.streamId, Http2StreamCloseSource::kPeer, error);
    // Rapid-reset budget (CVE-2023-44487): count peer resets and trip if they run too far
    // ahead of the responses this connection has actually let complete.
    ++peerResetStreams_;
    if (peerResetStreams_ > completedResponses_ + kHttp2MaxUnprocessedResets) {
        appendGoaway(Http2ErrorCode::kEnhanceYourCalm, "excessive stream resets");
        return false;
    }
    return true;
}

bool Http2Connection::processPriority(const Http2FrameHeader& header, std::string_view payload) {
    if (payload.size() != 5) {
        appendGoaway(Http2ErrorCode::kFrameSizeError, "invalid PRIORITY");
        return false;
    }
    if (header.streamId == 0) {
        appendGoaway(Http2ErrorCode::kProtocolError, "PRIORITY stream id must be nonzero");
        return false;
    }
    // RFC 9113 deprecates the RFC 7540 priority tree. Retain frame-shape validation,
    // then ignore the advisory dependency and weight on streams in every state.
    return true;
}

bool Http2Connection::processGoaway(
    const Http2FrameHeader& header,
    std::string_view payload) {
    if (header.streamId != 0 || payload.size() < 8) {
        appendGoaway(Http2ErrorCode::kProtocolError, "malformed GOAWAY");
        return false;
    }

    const Http2PeerGoaway goaway(
        http2Read31(reinterpret_cast<const unsigned char*>(payload.data())),
        static_cast<Http2ErrorCode>(http2Read32(
            reinterpret_cast<const unsigned char*>(payload.data() + 4))));
    if (peerGoaway_ &&
        goaway.lastStreamId() > peerGoaway_->lastStreamId()) {
        // RFC 9113 §6.8: increasing this value can make an already retried request
        // ambiguous, so the peer is not allowed to widen it on a later GOAWAY.
        appendGoaway(Http2ErrorCode::kProtocolError, "GOAWAY last stream id increased");
        return false;
    }

    std::array<std::uint32_t, Http2LocalSettings::kMaxConcurrentStreams>
        unprocessedStreamIds{};
    std::size_t unprocessedCount = 0;
    if (role_ == Http2Role::kClient) {
        bool responseStartedAboveLast = false;
        streams_.forEach([&](Http2StreamState& stream) {
            if (stream.id() <= goaway.lastStreamId() || stream.isReset()) {
                return;
            }
            // A response head proves that the peer acted on this request. Claiming
            // otherwise would make replay unsafe, so reject the contradictory GOAWAY.
            if (stream.headersDecoded()) {
                responseStartedAboveLast = true;
                return;
            }
            unprocessedStreamIds[unprocessedCount++] = stream.id();
        });
        if (responseStartedAboveLast) {
            appendGoaway(
                Http2ErrorCode::kProtocolError,
                "GOAWAY excludes a started response");
            return false;
        }
        std::sort(
            unprocessedStreamIds.begin(),
            unprocessedStreamIds.begin() +
                static_cast<std::ptrdiff_t>(unprocessedCount));
    }

    peerGoaway_ = goaway;
    events_.push_back(Http2Event::goaway(goaway));

    // A valid peer GOAWAY is graceful shutdown state, not a local connection error.
    // Send our directional GOAWAY through the same idempotent drain path, then keep
    // processing streams within each advertised boundary until they finish.
    beginDrain();

    if (role_ == Http2Role::kServer) {
        return true;
    }

    // Higher locally initiated streams were never processed and can be replayed on a
    // new connection. Close them inside the protocol core so flow-control debt,
    // deferred DATA, stream-table storage, and peer concurrency slots cannot leak.
    for (std::size_t i = 0; i < unprocessedCount; ++i) {
        const auto streamId = unprocessedStreamIds[i];
        if (!closeStreamImpl(
                streamId,
                Http2StreamCloseSource::kPeerGoaway,
                goaway.error(),
                CloseNotification::kOwnerAlreadyKnows)) {
            continue;
        }
        events_.push_back(Http2Event::requestUnprocessed(streamId));
    }
    return true;
}

bool Http2Connection::processPing(const Http2FrameHeader& header, std::string_view payload) {
    if (payload.size() != 8) {
        appendGoaway(Http2ErrorCode::kFrameSizeError, "invalid PING");
        return false;
    }
    if (header.streamId != 0) {
        appendGoaway(Http2ErrorCode::kProtocolError, "PING stream id must be zero");
        return false;
    }
    if ((header.flags & kHttp2FlagAck) != 0) {
        return true;  // our own ping's ack; ignore
    }
    // PING-flood budget: bound inbound PINGs seen since the owner last flushed output
    // (see kHttp2MaxUndrainedPings). A peer echoing keepalive normally lets us drain the
    // ACKs and resets the counter; one flooding PINGs without reading the ACKs trips.
    if (++consecutivePings_ > kHttp2MaxUndrainedPings) {
        appendGoaway(Http2ErrorCode::kEnhanceYourCalm, "excessive PING frames");
        return false;
    }
    appendFrame(Http2FrameType::kPing, kHttp2FlagAck, 0, payload);  // echo back
    return true;
}

Http2StreamState* Http2Connection::findStream(std::uint32_t streamId) noexcept {
    return streams_.find(streamId);
}

Http2StreamState* Http2Connection::createStream(std::uint32_t streamId) {
    return streams_.create(streamId, peerSettings_.initialWindowSize());
}

std::optional<Http2RequestHeadSubmitError>
Http2Connection::localRequestAdmissionError() const noexcept {
    if (role_ != Http2Role::kClient) {
        return Http2RequestHeadSubmitError::kInvalidState;
    }
    if (prefacePhase_ == PrefacePhase::kNotStarted) {
        return Http2RequestHeadSubmitError::kConnectionNotStarted;
    }
    if (connectionError_ || peerGoaway_.has_value() || nextLocalStreamId_ > 0x7fffffffU) {
        return Http2RequestHeadSubmitError::kConnectionUnavailable;
    }
    if (activeLocalRequestStreams_ >= peerSettings_.maxConcurrentStreams()) {
        return Http2RequestHeadSubmitError::kPeerStreamLimitReached;
    }
    return std::nullopt;
}

Http2StreamState* Http2Connection::admitLocalRequestStream() {
    auto* stream = createStream(nextLocalStreamId_);
    if (stream != nullptr) {
        nextLocalStreamId_ += 2;
    }
    return stream;
}

void Http2Connection::activateLocalRequestStream(Http2StreamState& stream) noexcept {
    if (stream.holdPeerConcurrencySlot()) {
        ++activeLocalRequestStreams_;
    }
}

void Http2Connection::releaseLocalRequestStream(Http2StreamState& stream) noexcept {
    if (stream.releasePeerConcurrencySlot() && activeLocalRequestStreams_ != 0) {
        --activeLocalRequestStreams_;
    }
}

void Http2Connection::releaseLocalRequestStreamIfClosed(Http2StreamState& stream) noexcept {
    if (stream.isReset() ||
        (stream.peerEndStream() && stream.localEndStreamCommitted())) {
        releaseLocalRequestStream(stream);
    }
}

bool Http2Connection::isIdleStreamId(std::uint32_t streamId) const noexcept {
    if (role_ == Http2Role::kClient) {
        // No server-initiated streams exist (push is never enabled), so every even id
        // is idle, as is any odd id this endpoint has not opened yet.
        return (streamId & 1U) == 0 || streamId >= nextLocalStreamId_;
    }
    return http2IsIdleStream(streamId, lastStreamId_);
}

void Http2Connection::deferStreamWindowRelease(std::uint32_t streamId) {
    if (auto* stream = findStream(streamId)) {
        stream->setDeferWindowRelease();
    }
}

void Http2Connection::releaseStreamWindow(std::uint32_t streamId) {
    auto* stream = findStream(streamId);
    if (stream == nullptr) {
        return;  // debt (if any) died with the stream; nothing left to credit
    }
    const auto debt = stream->takeWindowDebt();
    if (debt == 0) {
        return;
    }
    http2CreditConnectionReceiveWindow(
        connectionReceiveWindow_, static_cast<std::int32_t>(debt));
    // Re-advertise the stream window only while the peer can still send on it; a
    // stream-scoped WINDOW_UPDATE on an ended/reset stream can trip a strict peer.
    if (!stream->bodyEnded() && !stream->isReset()) {
        http2CreditStreamReceiveWindow(*stream, static_cast<std::int32_t>(debt));
        char buf[kHttp2WindowUpdateFrameBytes * 2];
        auto* out = http2WriteDataWindowUpdates(buf, streamId, debt);
        outBuffer_.append(buf, static_cast<std::size_t>(out - buf));
    } else {
        char buf[kHttp2WindowUpdateFrameBytes];
        http2WriteWindowUpdate(buf, 0, debt);
        outBuffer_.append(buf, sizeof(buf));
    }
}

bool Http2Connection::hasQueuedData(std::uint32_t streamId) const noexcept {
    for (const auto& pending : pendingSends_) {
        if (pending.streamId == streamId) {
            return true;
        }
    }
    return false;
}

Http2RequestHeadSubmitResult Http2Connection::submitRegularRequestHead(
    std::string_view method,
    std::string_view scheme,
    std::string_view authority,
    std::string_view path,
    std::span<const HttpHeaderView> headers,
    Http2RequestContent content) {
    if (const auto error = localRequestAdmissionError()) {
        return Http2RequestHeadSubmitResult::makeFailure(*error);
    }
    // Validate the entire semantic head before touching HPACK storage, outbound
    // bytes, the stream method, local-content accounting, or lifecycle state.
    if (!http2IsValidOutboundRegularRequestHead(method, scheme, authority, path, headers)) {
        return Http2RequestHeadSubmitResult::makeFailure(
            Http2RequestHeadSubmitError::kInvalidMessage);
    }

    Http2EndStream endStream = Http2EndStream::kKeepOpen;
    std::array<char, 20> lengthBuffer{};
    std::size_t lengthBytes = 0;
    const bool withoutContent = content.withoutContent() != nullptr;
    const auto* knownLengthContent = content.knownLengthContent();
    const bool streamingContent = content.streamingContent() != nullptr;
    if (!withoutContent && knownLengthContent == nullptr && !streamingContent) {
        return Http2RequestHeadSubmitResult::makeFailure(
            Http2RequestHeadSubmitError::kInvalidMessage);
    }
    if (withoutContent) {
        endStream = Http2EndStream::kEndStream;
    } else if (knownLengthContent != nullptr) {
        endStream = knownLengthContent->length() == 0
            ? Http2EndStream::kEndStream
            : Http2EndStream::kKeepOpen;
        // 20 bytes always hold the canonical decimal form of uint64_t. Do this
        // before mutating stream/HPACK state so every rejection is transactional.
        if (const auto [end, ec] = std::to_chars(
                lengthBuffer.data(),
                lengthBuffer.data() + lengthBuffer.size(),
                knownLengthContent->length());
            ec == std::errc{}) {
            lengthBytes = static_cast<std::size_t>(end - lengthBuffer.data());
        } else {
            return Http2RequestHeadSubmitResult::makeFailure(
                Http2RequestHeadSubmitError::kInvalidMessage);
        }
    }

    auto* stream = admitLocalRequestStream();
    if (stream == nullptr) {
        return Http2RequestHeadSubmitResult::makeFailure(
            Http2RequestHeadSubmitError::kLocalStreamCapacityReached);
    }
    const auto streamId = stream->id();

    stream->assignRequestMethod(method);
    auto& block = stream->responseHeaderBlock();
    block.clear();
    HpackEncoder::encodeHeader(block, ":method", method);
    HpackEncoder::encodeHeader(block, ":scheme", scheme);
    HpackEncoder::encodeHeader(block, ":authority", authority);
    HpackEncoder::encodeHeader(block, ":path", path);
    for (const auto& header : headers) {
        HpackEncoder::encodeHeader(block, header.name(), header.value());
    }
    if (knownLengthContent != nullptr) {
        HpackEncoder::encodeHeaderWithNameIndex(
            block,
            HpackStaticIndex::kContentLength,
            std::string_view(lengthBuffer.data(), lengthBytes));
    }
    appendResponseHeaderFrames(
        *stream, std::string_view(block.data(), block.size()), endStream);
    if (withoutContent) {
        stream->beginLocalContentForbidden();
    } else if (knownLengthContent != nullptr) {
        stream->beginLocalContentKnownLength(knownLengthContent->length());
    } else if (streamingContent) {
        stream->beginLocalContentUnbounded();
    }
    stream->markLocalHeadSubmitted(
        Http2LocalMessageKind::kRequest,
        http2EndsStream(endStream));
    activateLocalRequestStream(*stream);
    http2ReleaseResponseHeaderBlock(*stream);
    return Http2RequestHeadSubmitResult::makeSubmitted(streamId);
}

Http2RequestHeadSubmitResult Http2Connection::submitConnectRequestHead(
    std::string_view authority,
    std::span<const HttpHeaderView> headers) {
    if (const auto error = localRequestAdmissionError()) {
        return Http2RequestHeadSubmitResult::makeFailure(*error);
    }

    RequestTargetView target;
    if (!parseRequestTarget(HttpKnownMethod::kConnect, authority, target) ||
        !http2AreValidOutboundRequestHeaders(
            authority,
            0,
            headers,
            /*allowHost=*/false,
            /*allowTrailers=*/false)) {
        return Http2RequestHeadSubmitResult::makeFailure(
            Http2RequestHeadSubmitError::kInvalidMessage);
    }

    auto* stream = admitLocalRequestStream();
    if (stream == nullptr) {
        return Http2RequestHeadSubmitResult::makeFailure(
            Http2RequestHeadSubmitError::kLocalStreamCapacityReached);
    }
    const auto streamId = stream->id();
    (void)stream->beginStandardConnect();

    auto& block = stream->responseHeaderBlock();
    block.clear();
    HpackEncoder::encodeHeader(block, ":method", "CONNECT");
    HpackEncoder::encodeHeader(block, ":authority", authority);
    for (const auto& header : headers) {
        HpackEncoder::encodeHeader(block, header.name(), header.value());
    }
    appendResponseHeaderFrames(
        *stream,
        std::string_view(block.data(), block.size()),
        Http2EndStream::kKeepOpen);
    stream->assignRequestMethod("CONNECT");
    stream->beginLocalContentForbidden();
    stream->markLocalConnectRequestSubmitted();
    activateLocalRequestStream(*stream);
    http2ReleaseResponseHeaderBlock(*stream);
    return Http2RequestHeadSubmitResult::makeSubmitted(streamId);
}

Http2RequestHeadSubmitResult Http2Connection::submitExtendedConnectRequestHead(
    std::string_view protocol,
    std::string_view scheme,
    std::string_view authority,
    std::string_view path,
    std::span<const HttpHeaderView> headers) {
    if (const auto error = localRequestAdmissionError()) {
        return Http2RequestHeadSubmitResult::makeFailure(*error);
    }
    if (!peerSettings_.enableConnectProtocol()) {
        return Http2RequestHeadSubmitResult::makeFailure(
            Http2RequestHeadSubmitError::kPeerCapabilityUnavailable);
    }

    const bool websocket = httpAsciiEqualsIgnoreCase(protocol, "websocket");
    if (!isValidHttpHeaderName(protocol) ||
        (scheme != "http" && scheme != "https") ||
        !isValidHostHeader(authority) ||
        !isValidOriginFormTarget(path) ||
        !http2AreValidOutboundRequestHeaders(
            authority,
            static_cast<std::uint16_t>(scheme == "https" ? 443 : 80),
            headers,
            /*allowHost=*/!websocket,
            /*allowTrailers=*/false) ||
        (websocket && !http2IsValidWebSocketConnectHeaders(headers))) {
        return Http2RequestHeadSubmitResult::makeFailure(
            Http2RequestHeadSubmitError::kInvalidMessage);
    }

    auto* stream = admitLocalRequestStream();
    if (stream == nullptr) {
        return Http2RequestHeadSubmitResult::makeFailure(
            Http2RequestHeadSubmitError::kLocalStreamCapacityReached);
    }
    const auto streamId = stream->id();
    (void)stream->beginExtendedConnect();

    auto& block = stream->responseHeaderBlock();
    block.clear();
    HpackEncoder::encodeHeader(block, ":method", "CONNECT");
    HpackEncoder::encodeHeader(block, ":protocol", protocol);
    HpackEncoder::encodeHeader(block, ":scheme", scheme);
    HpackEncoder::encodeHeader(block, ":authority", authority);
    HpackEncoder::encodeHeader(block, ":path", path);
    for (const auto& header : headers) {
        HpackEncoder::encodeHeader(block, header.name(), header.value());
    }
    appendResponseHeaderFrames(
        *stream,
        std::string_view(block.data(), block.size()),
        Http2EndStream::kKeepOpen);
    stream->assignRequestMethod("CONNECT");
    stream->setProtocol(protocol);
    stream->beginLocalContentForbidden();
    stream->markLocalConnectRequestSubmitted();
    activateLocalRequestStream(*stream);
    http2ReleaseResponseHeaderBlock(*stream);
    return Http2RequestHeadSubmitResult::makeSubmitted(streamId);
}

HeaderDecodeStatus Http2Connection::decodeHeaderBlock(Http2StreamState& stream) {
    Http2HeaderDecodeContext context{stream};
    const auto result = decoder_.decode(
        stream.requestHeaderBlock(), &context,
        [](void* target, std::string_view name, std::string_view value) {
            return http2OnDecodedInitialHeader(
                *static_cast<Http2HeaderDecodeContext*>(target), name, value);
        });
    http2ResetHeaderBlock(stream);
    if (const auto status = http2ClassifyHeaderDecodeResult(result); status != HeaderDecodeStatus::kOk) {
        return status;
    }
    if (!stream.hasMethod()) {
        return HeaderDecodeStatus::kProtocolError;
    }
    if (stream.hasProtocol()) {
        if (prefacePhase_ != PrefacePhase::kReady ||
            stream.requestKnownMethod() != HttpKnownMethod::kConnect ||
            !stream.hasScheme() ||
            !stream.hasPath() ||
            !stream.hasAuthority() ||
            stream.remoteContent().knownLength() != nullptr) {
            return HeaderDecodeStatus::kProtocolError;
        }
        if (!stream.beginExtendedConnect()) {
            return HeaderDecodeStatus::kProtocolError;
        }
    } else if (stream.requestKnownMethod() == HttpKnownMethod::kConnect) {
        RequestTargetView connectTarget;
        if (!stream.hasAuthority() || stream.hasScheme() || stream.hasPath() ||
            stream.remoteContent().knownLength() != nullptr ||
            !parseRequestTarget(
                HttpKnownMethod::kConnect,
                stream.requestAuthority(),
                connectTarget)) {
            return HeaderDecodeStatus::kProtocolError;
        }
        if (!stream.beginStandardConnect()) {
            return HeaderDecodeStatus::kProtocolError;
        }
    } else if (!stream.hasScheme() || !stream.hasPath()) {
        return HeaderDecodeStatus::kProtocolError;
    }
    stream.markHeadersDecoded();
    // NOTE (sans-I/O): resolveStreamRoute is deliberately NOT called here -- route
    // resolution and body-mode selection are application policy the owner applies
    // after pulling the kMessageHead event.
    return HeaderDecodeStatus::kOk;
}

namespace {

// RFC 9113 §8.1: at most this many 1xx interim heads before the final response head
// (DoS bound; mirrors the retired client session's limit).
constexpr std::uint8_t kMaxHttp2InterimResponses = 8;

struct Http2ResponseDecodeContext final {
    Http2HeaderDecodeContext base;
    std::uint16_t status{0};
    bool sawStatus{false};
    bool sawRegular{false};
};

// Client-role response head decode: ':status' once and first, then validated regular
// headers into the stream's header table (1xx heads are validated but not stored).
bool http2OnDecodedResponseHeader(void* target, std::string_view name, std::string_view value) {
    auto* context = static_cast<Http2ResponseDecodeContext*>(target);
    if (!http2AccumulateHeaderListBytes(context->base, name, value)) {
        return false;
    }
    auto& stream = context->base.stream;
    if (name.empty() || stream.requestHeadersFull()) {
        return false;
    }
    if (name.front() == ':') {
        if (name != ":status" || context->sawStatus || context->sawRegular) {
            return false;
        }
        int status = 0;
        const auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), status);
        if (value.size() != 3 || ec != std::errc{} || ptr != value.data() + value.size() ||
            status < 100 || status > 999 || status == 101) {
            return false;
        }
        context->status = static_cast<std::uint16_t>(status);
        context->sawStatus = true;
        return true;
    }
    if (!context->sawStatus || !http2IsValidRegularHeader(name, value)) {
        return false;
    }
    context->sawRegular = true;
    if (context->status < 200) {
        return true;  // interim head: validate only, never stored
    }
    const auto kind = classifyRequestHeader(name);
    const bool successfulConnect = stream.tunnel().pending() != nullptr &&
        context->status >= 200 && context->status < 300;
    if (kind == RequestHeaderKind::kContentLength && successfulConnect) {
        // RFC 9110 9.3.6: a client ignores Content-Length on a successful CONNECT
        // response. It describes neither HTTP content nor the following tunnel DATA.
        return true;
    }
    if (const auto singletonBit = singletonRequestHeaderBit(kind); singletonBit != 0) {
        if (!stream.markSingletonRequestHeader(singletonBit)) {
            return false;
        }
    }
    if (kind == RequestHeaderKind::kContentLength) {
        std::size_t parsed = 0;
        const auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), parsed);
        if (ec != std::errc{} || ptr != value.data() + value.size()) {
            return false;
        }
        if (!stream.declareRemoteContentLength(parsed)) {
            return false;
        }
    }
    return stream.appendRequestHeader(name, value, kind);
}

}  // namespace

HeaderDecodeStatus Http2Connection::decodeResponseHeaderBlock(Http2StreamState& stream) {
    Http2ResponseDecodeContext context{Http2HeaderDecodeContext{stream}};
    const auto result = decoder_.decode(
        stream.requestHeaderBlock(), &context,
        [](void* target, std::string_view name, std::string_view value) {
            return http2OnDecodedResponseHeader(target, name, value);
        });
    http2ResetHeaderBlock(stream);
    if (const auto status = http2ClassifyHeaderDecodeResult(result); status != HeaderDecodeStatus::kOk) {
        return status;
    }
    if (!context.sawStatus) {
        return HeaderDecodeStatus::kProtocolError;
    }
    if (context.status < 200) {
        // 1xx interim head: cannot end the stream, bounded in count; the stream stays
        // NOT headersDecoded so the next HEADERS block decodes as the (next) head.
        if (stream.peerEndStream()) {
            return HeaderDecodeStatus::kProtocolError;
        }
        stream.countInterimResponse();
        if (stream.interimResponseCount() > kMaxHttp2InterimResponses) {
            return HeaderDecodeStatus::kProtocolError;
        }
        return HeaderDecodeStatus::kOk;
    }
    stream.setResponseStatus(context.status);
    if (stream.tunnel().pending() != nullptr) {
        if (context.status >= 200 && context.status < 300) {
            (void)stream.acceptConnect();
            stream.beginLocalContentUnbounded();
            (void)stream.openLocalConnectTunnel();
        } else {
            (void)stream.rejectConnect();
            appendFrame(Http2FrameType::kData, kHttp2FlagEndStream, stream.id(), {});
            (void)stream.rejectLocalConnect();
        }
    }
    stream.markHeadersDecoded();
    return HeaderDecodeStatus::kOk;
}

HeaderDecodeStatus Http2Connection::decodeInitialHeaderBlock(Http2StreamState& stream) {
    return role_ == Http2Role::kClient ? decodeResponseHeaderBlock(stream) : decodeHeaderBlock(stream);
}

HeaderDecodeStatus Http2Connection::decodeRefusedHeaderBlock(Http2StreamState& stream) {
    Http2HeaderDecodeContext context{stream};
    const auto result = decoder_.decode(
        stream.requestHeaderBlock(), &context,
        [](void* target, std::string_view name, std::string_view value) {
            return http2OnDecodedInitialHeader(
                *static_cast<Http2HeaderDecodeContext*>(target), name, value);
        });
    http2ResetHeaderBlock(stream);
    return http2ClassifyHeaderDecodeResult(result);
}

HeaderDecodeStatus Http2Connection::decodeDiscardedHeaderBlock(Http2StreamState& stream) {
    // Even a block whose HTTP semantics are no longer observable must be decoded in
    // full because HPACK's dynamic table is connection-scoped. Keep the decompressed
    // field-list budget, but deliberately avoid mutating request/response state.
    Http2HeaderDecodeContext context{stream};
    const auto result = decoder_.decode(
        stream.requestHeaderBlock(), &context,
        [](void* target, std::string_view name, std::string_view value) {
            return http2AccumulateHeaderListBytes(
                *static_cast<Http2HeaderDecodeContext*>(target), name, value);
        });
    http2ResetHeaderBlock(stream);
    return http2ClassifyHeaderDecodeResult(result);
}

bool Http2Connection::startDiscardedHeaderBlock(
    const Http2FrameHeader& header,
    std::string_view fragment,
    DiscardedHeaderAction action) {
    if (discardedHeaderStream_) {
        appendGoaway(Http2ErrorCode::kProtocolError, "overlapping discarded HEADERS block");
        return false;
    }
    discardedHeaderStream_.emplace(header.streamId, resource_);
    discardedHeaderAction_ = action;
    if (!http2StartHeaderBlock(*discardedHeaderStream_, fragment)) {
        discardedHeaderStream_.reset();
        discardedHeaderAction_ = DiscardedHeaderAction::kIgnore;
        appendGoaway(Http2ErrorCode::kCompressionError, "field block not decompressed");
        return false;
    }
    if ((header.flags & kHttp2FlagEndHeaders) != 0) {
        return finishDiscardedHeaderBlock();
    }
    headerContinuation_.start(header.streamId, Http2HeaderBlockKind::kDiscarded);
    return true;
}

bool Http2Connection::finishDiscardedHeaderBlock() {
    if (!discardedHeaderStream_) {
        appendGoaway(Http2ErrorCode::kProtocolError, "missing discarded HEADERS state");
        return false;
    }
    const auto streamId = discardedHeaderStream_->id();
    const auto action = discardedHeaderAction_;
    const auto status = action == DiscardedHeaderAction::kRefuseStream
        ? decodeRefusedHeaderBlock(*discardedHeaderStream_)
        : decodeDiscardedHeaderBlock(*discardedHeaderStream_);
    discardedHeaderStream_.reset();
    discardedHeaderAction_ = DiscardedHeaderAction::kIgnore;

    if (status == HeaderDecodeStatus::kCompressionError) {
        appendGoaway(Http2ErrorCode::kCompressionError, "invalid HPACK block");
        return false;
    }
    if (action == DiscardedHeaderAction::kIgnore) {
        return true;
    }

    auto error = Http2ErrorCode::kProtocolError;
    if (action == DiscardedHeaderAction::kResetStreamClosed) {
        error = Http2ErrorCode::kStreamClosed;
    } else if (action == DiscardedHeaderAction::kRefuseStream &&
               status == HeaderDecodeStatus::kOk) {
        error = Http2ErrorCode::kRefusedStream;
    }
    appendRstStream(streamId, error);
    if (findStream(streamId) != nullptr) {
        closeStream(streamId, Http2StreamCloseSource::kLocal, error);
    } else {
        closedStreams_.remember(streamId, Http2StreamCloseSource::kLocal);
    }
    return true;
}

HeaderDecodeStatus Http2Connection::finishTrailerBlock(Http2StreamState& stream) {
    Http2HeaderDecodeContext context{stream};
    const auto result = decoder_.decode(
        stream.requestHeaderBlock(), &context,
        [](void* target, std::string_view name, std::string_view value) {
            return http2OnDecodedTrailer(
                *static_cast<Http2HeaderDecodeContext*>(target), name, value);
        });
    http2ResetHeaderBlock(stream);
    if (const auto status = http2ClassifyHeaderDecodeResult(result); status != HeaderDecodeStatus::kOk) {
        return status;
    }
    if (!http2RemoteContentTerminalValid(stream, role_)) {
        return HeaderDecodeStatus::kProtocolError;
    }
    stream.markPeerEndStream();
    stream.markBodyEnded();
    events_.push_back(Http2Event::messageEnd(stream.id()));
    releaseLocalRequestStreamIfClosed(stream);
    return HeaderDecodeStatus::kOk;
}

bool Http2Connection::handleHeaderDecodeFailure(Http2StreamState& stream, HeaderDecodeStatus status) {
    if (status == HeaderDecodeStatus::kCompressionError) {
        appendGoaway(Http2ErrorCode::kCompressionError, "invalid HPACK block");
        return false;
    }
    appendRstStream(stream.id(), Http2ErrorCode::kProtocolError);
    if (findStream(stream.id()) == &stream) {
        closeStream(
            stream.id(),
            Http2StreamCloseSource::kLocal,
            Http2ErrorCode::kProtocolError);  // emits a typed stream-closed event
    } else {
        stream.markReset();  // refused-stream scratch, not in the table
    }
    return true;
}

void Http2Connection::emitRequestHeaders(Http2StreamState& stream) {
    // RFC 9113 §8.1.1: a declared content-length must equal the summed DATA payload.
    // A body-less HEADERS (END_STREAM set) with a nonzero content-length can never be
    // satisfied -- reject it here so both END_STREAM routes stay consistent. Client
    // role: HEAD responses and 204/304 are exempt (their content-length describes the
    // body a GET would have had, RFC 9110 §8.6).
    if (stream.peerEndStream() &&
        !http2RemoteContentTerminalValid(stream, role_)) {
        appendRstStream(stream.id(), Http2ErrorCode::kProtocolError);
        closeStream(
            stream.id(),
            Http2StreamCloseSource::kLocal,
            Http2ErrorCode::kProtocolError);  // remove, don't leak the slot
        return;
    }
    events_.push_back(Http2Event::messageHead(stream.id()));
    if (role_ == Http2Role::kServer &&
        stream.tunnel().pending() != nullptr) {
        // CONNECT has no request content, but the peer's wire half remains open for
        // tunnel DATA after a successful response. Route/accept decisions start from
        // kMessageHead; kMessageEnd would incorrectly imply END_STREAM was received.
        stream.markBodyEnded();
        return;
    }
    if (role_ == Http2Role::kClient &&
        stream.tunnel().open() != nullptr) {
        if (stream.peerEndStream()) {
            stream.markBodyEnded();
            events_.push_back(Http2Event::tunnelEnd(stream.id()));
        }
        releaseLocalRequestStreamIfClosed(stream);
        return;
    }
    if (stream.peerEndStream()) {
        stream.markBodyEnded();
        events_.push_back(Http2Event::messageEnd(stream.id()));
    }
    releaseLocalRequestStreamIfClosed(stream);
}

bool Http2Connection::processHeaders(const Http2FrameHeader& header, std::string_view payload) {
    if (header.streamId == 0) {
        appendGoaway(Http2ErrorCode::kProtocolError, "HEADERS stream id must be nonzero");
        return false;
    }
    if ((header.streamId & 1U) == 0) {
        appendGoaway(
            Http2ErrorCode::kProtocolError,
            role_ == Http2Role::kClient ? "HEADERS on even stream id" : "invalid client stream id");
        return false;
    }

    std::string_view fragment;
    if (!http2DecodeHeadersPayload(header, payload, fragment)) {
        appendGoaway(Http2ErrorCode::kProtocolError, "invalid HEADERS priority");
        return false;
    }

    Http2StreamState* stream = nullptr;
    std::optional<DiscardedHeaderAction> discardedAction;
    if (auto* existing = findStream(header.streamId); existing != nullptr) {
        if (existing->isReset()) {
            if (existing->closeSource() == Http2StreamCloseSource::kPeer) {
                // Frames sent after the peer's own RST are not an in-flight race: the
                // connection byte stream orders them after that terminal signal.
                appendGoaway(Http2ErrorCode::kStreamClosed, "HEADERS after peer RST_STREAM");
                return false;
            }
            // After WE sent RST_STREAM, any peer frames already in flight must be
            // minimally processed and discarded. Do not send a second RST.
            discardedAction = DiscardedHeaderAction::kIgnore;
        } else if (existing->headersDecoded() &&
                   (existing->tunnel().open() != nullptr ||
                    (role_ == Http2Role::kServer &&
                     existing->tunnel().pending() != nullptr))) {
            // CONNECT has no request trailers, and an accepted connected stream only
            // permits DATA/RST_STREAM/WINDOW_UPDATE/PRIORITY. Decode the complete
            // field block for HPACK synchronization, then reset this stream.
            return startDiscardedHeaderBlock(
                header, fragment, DiscardedHeaderAction::kResetProtocolError);
        } else if (existing->headersDecoded()) {
            return processTrailerHeaders(*existing, header, fragment);
        } else if (role_ == Http2Role::kClient) {
            // A 1xx interim head was decoded on this stream; this block is the next
            // (possibly final) response head -- decode it through the shared tail.
            stream = existing;
        } else {
            // A second initial request head is a stream error. Decode its complete
            // field block before applying the reset so HPACK remains synchronized.
            discardedAction = DiscardedHeaderAction::kResetProtocolError;
        }
    } else if (role_ == Http2Role::kClient) {
        if (isIdleStreamId(header.streamId)) {
            appendGoaway(Http2ErrorCode::kProtocolError, "HEADERS on idle stream");
            return false;
        }
        if (closedStreams_.source(header.streamId) == Http2StreamCloseSource::kPeer) {
            appendGoaway(Http2ErrorCode::kStreamClosed, "HEADERS after peer RST_STREAM");
            return false;
        }
        // A locally cancelled/completed or GOAWAY-rejected client stream no longer has
        // storage. Decode any late block into scratch so HPACK stays synchronized.
        discardedAction = DiscardedHeaderAction::kIgnore;
    } else {
        if (header.streamId <= lastStreamId_) {
            const auto source = closedStreams_.source(header.streamId);
            if (source == Http2StreamCloseSource::kPeer) {
                appendGoaway(Http2ErrorCode::kStreamClosed, "HEADERS after peer RST_STREAM");
                return false;
            }
            // Opening a higher peer stream implicitly closes every lower unused ID.
            // Whether this one was explicitly locally closed or aged out of bounded
            // history, it is not idle; tolerate the closed-stream frame by decoding and
            // discarding it so HPACK can never desynchronize.
            discardedAction = DiscardedHeaderAction::kIgnore;
        } else {
            // Record every genuinely new peer stream ID, even when it is malformed or
            // refused, so a later lower ID cannot be reopened as idle.
            lastStreamId_ = header.streamId;
            const bool drainRefused = draining_ && header.streamId > goawayLastStreamId_;
            stream = drainRefused ? nullptr : createStream(header.streamId);
            if (stream == nullptr) {
                discardedAction = DiscardedHeaderAction::kRefuseStream;
            }
        }
    }

    if (discardedAction) {
        return startDiscardedHeaderBlock(header, fragment, *discardedAction);
    }

    if ((header.flags & kHttp2FlagEndStream) != 0) {
        stream->markPeerEndStream();
    }
    if (!http2StartHeaderBlock(*stream, fragment)) {
        // The block exceeds the header buffer cap. We cannot decode a block we could
        // not fully buffer, and skipping it would desync the connection-global HPACK
        // dynamic table for every later block (RFC 9113 §4.3) -- so this is a
        // COMPRESSION_ERROR, not a survivable stream reset.
        appendGoaway(Http2ErrorCode::kCompressionError, "field block not decompressed");
        return false;
    }

    if ((header.flags & kHttp2FlagEndHeaders) != 0) {
        const auto status = decodeInitialHeaderBlock(*stream);
        if (status != HeaderDecodeStatus::kOk) {
            return handleHeaderDecodeFailure(*stream, status);
        }
        if (stream->headersDecoded()) {
            emitRequestHeaders(*stream);  // not yet decoded = a 1xx interim head (client)
        }
    } else {
        headerContinuation_.start(stream->id(), Http2HeaderBlockKind::kInitial);
    }
    return true;
}

bool Http2Connection::processTrailerHeaders(
    Http2StreamState& stream,
    const Http2FrameHeader& header,
    std::string_view fragment) {
    if (stream.bodyEnded()) {
        return startDiscardedHeaderBlock(
            header, fragment, DiscardedHeaderAction::kResetStreamClosed);
    }
    if ((header.flags & kHttp2FlagEndStream) == 0) {
        return startDiscardedHeaderBlock(
            header, fragment, DiscardedHeaderAction::kResetProtocolError);
    }
    if (!http2StartHeaderBlock(stream, fragment)) {
        // Un-bufferable trailer block: same HPACK-consistency reasoning as HEADERS --
        // COMPRESSION_ERROR rather than a survivable stream reset.
        appendGoaway(Http2ErrorCode::kCompressionError, "field block not decompressed");
        return false;
    }

    if ((header.flags & kHttp2FlagEndHeaders) != 0) {
        const auto status = finishTrailerBlock(stream);
        if (status != HeaderDecodeStatus::kOk) {
            return handleHeaderDecodeFailure(stream, status);
        }
    } else {
        headerContinuation_.start(stream.id(), Http2HeaderBlockKind::kTrailers);
    }
    return true;
}

bool Http2Connection::processContinuation(const Http2FrameHeader& header, std::string_view payload) {
    if (!headerContinuation_.matches(header.streamId)) {
        appendGoaway(Http2ErrorCode::kProtocolError, "invalid CONTINUATION");
        return false;
    }
    const auto kind = headerContinuation_.kind();
    Http2StreamState* stream = nullptr;
    if (kind == Http2HeaderBlockKind::kDiscarded) {
        if (!discardedHeaderStream_ || discardedHeaderStream_->id() != header.streamId) {
            appendGoaway(Http2ErrorCode::kProtocolError, "missing discarded CONTINUATION state");
            return false;
        }
        // Prefer detached storage even when a pinned reset stream with the same ID is
        // still present. Its request views are owner-held and must remain immutable.
        stream = &*discardedHeaderStream_;
    } else {
        stream = findStream(header.streamId);
        if (stream == nullptr || stream->isReset()) {
            appendGoaway(Http2ErrorCode::kProtocolError, "missing live CONTINUATION stream");
            return false;
        }
    }
    if (!http2AppendHeaderBlock(*stream, payload)) {
        // The accumulated HEADERS+CONTINUATION block overflowed the buffer cap; the
        // partial block cannot be decoded, so skipping it would desync HPACK for the
        // whole connection -- COMPRESSION_ERROR (RFC 9113 §4.3), not a stream reset.
        appendGoaway(Http2ErrorCode::kCompressionError, "field block not decompressed");
        return false;
    }
    if ((header.flags & kHttp2FlagEndHeaders) != 0) {
        const auto completedKind = headerContinuation_.finishKind();
        if (completedKind == Http2HeaderBlockKind::kDiscarded) {
            return finishDiscardedHeaderBlock();
        }
        if (completedKind == Http2HeaderBlockKind::kTrailers) {
            const auto status = finishTrailerBlock(*stream);
            if (status != HeaderDecodeStatus::kOk) {
                return handleHeaderDecodeFailure(*stream, status);
            }
        } else {
            const auto status = decodeInitialHeaderBlock(*stream);
            if (status != HeaderDecodeStatus::kOk) {
                return handleHeaderDecodeFailure(*stream, status);
            }
            if (stream->headersDecoded()) {
                emitRequestHeaders(*stream);
            }
        }
    }
    return true;
}

void Http2Connection::releaseDroppedDataConnectionWindow(std::int32_t flowBytes) {
    // Every structurally valid DATA frame reached this path only after the shared
    // connection debit succeeded. Return exactly that credit while keeping the
    // connection; no stream window survives an abandoned stream.
    http2CreditConnectionReceiveWindow(connectionReceiveWindow_, flowBytes);
    if (flowBytes == 0) {
        return;
    }
    char buf[kHttp2WindowUpdateFrameBytes];
    http2WriteWindowUpdate(buf, 0, static_cast<std::uint32_t>(flowBytes));
    outBuffer_.append(buf, sizeof(buf));
}

bool Http2Connection::processData(const Http2FrameHeader& header, std::string_view payload) {
    if (header.streamId == 0) {
        appendGoaway(Http2ErrorCode::kProtocolError, "DATA stream id must be nonzero");
        return false;
    }
    if ((header.streamId & 1U) == 0) {
        appendGoaway(Http2ErrorCode::kProtocolError, "DATA on invalid client stream id");
        return false;
    }
    std::string_view data;
    if (!http2DecodeDataPayload(header, payload, data)) {
        // Invalid padding is a frame-structure error for the connection. Validate it
        // before any stream-phase shortcut (closed body, pending CONNECT, reset) can
        // incorrectly downgrade it to a stream error.
        appendGoaway(Http2ErrorCode::kProtocolError, "invalid DATA padding");
        return false;
    }
    const auto flowBytes = static_cast<std::int32_t>(payload.size());
    if (http2DebitConnectionReceiveWindow(connectionReceiveWindow_, flowBytes) ==
        Http2ReceiveWindowDebitStatus::kExceeded) {
        appendGoaway(
            Http2ErrorCode::kFlowControlError,
            "connection flow-control window exceeded");
        return false;
    }

    auto* stream = findStream(header.streamId);
    if (stream == nullptr) {
        if (closedStreams_.source(header.streamId) ==
            Http2StreamCloseSource::kPeerGoaway) {
            releaseDroppedDataConnectionWindow(flowBytes);
            return true;
        }
        if (!isIdleStreamId(header.streamId)) {
            appendRstStream(header.streamId, Http2ErrorCode::kStreamClosed);
            releaseDroppedDataConnectionWindow(flowBytes);
            return true;
        }
        appendGoaway(Http2ErrorCode::kProtocolError, "DATA before HEADERS");
        return false;
    }
    if (stream->isReset()) {
        releaseDroppedDataConnectionWindow(flowBytes);
        return true;
    }
    if (!stream->headersDecoded()) {
        appendRstStream(header.streamId, Http2ErrorCode::kProtocolError);
        closeStream(
            header.streamId,
            Http2StreamCloseSource::kLocal,
            Http2ErrorCode::kProtocolError);
        releaseDroppedDataConnectionWindow(flowBytes);
        return true;
    }
    if (stream->peerEndStream()) {
        // END_STREAM closes only the peer's send half. The opposite half of an
        // accepted CONNECT tunnel remains usable, but another DATA frame from this
        // peer is a frame on a half-closed (remote) stream (RFC 9113 5.1/8.5).
        appendRstStream(header.streamId, Http2ErrorCode::kStreamClosed);
        closeStream(
            header.streamId,
            Http2StreamCloseSource::kLocal,
            Http2ErrorCode::kStreamClosed);
        releaseDroppedDataConnectionWindow(flowBytes);
        return true;
    }
    if (role_ == Http2Role::kServer &&
        stream->tunnel().pending() != nullptr) {
        appendRstStream(header.streamId, Http2ErrorCode::kProtocolError);
        closeStream(
            header.streamId,
            Http2StreamCloseSource::kLocal,
            Http2ErrorCode::kProtocolError);
        releaseDroppedDataConnectionWindow(flowBytes);
        return true;
    }
    const bool tunnelData = stream->tunnel().open() != nullptr;
    if (!tunnelData && stream->bodyEnded()) {
        appendRstStream(header.streamId, Http2ErrorCode::kStreamClosed);
        closeStream(
            header.streamId,
            Http2StreamCloseSource::kLocal,
            Http2ErrorCode::kStreamClosed);
        releaseDroppedDataConnectionWindow(flowBytes);
        return true;
    }

    if (http2DebitStreamReceiveWindow(*stream, flowBytes) ==
        Http2ReceiveWindowDebitStatus::kExceeded) {
        appendRstStream(header.streamId, Http2ErrorCode::kFlowControlError);
        closeStream(
            header.streamId,
            Http2StreamCloseSource::kLocal,
            Http2ErrorCode::kFlowControlError);
        releaseDroppedDataConnectionWindow(flowBytes);
        return true;
    }

    if (!tunnelData) {
        switch (http2AccountDataBody(
            *stream,
            data.size(),
            limits_.maxStreamBodyBytes,
            limits_.maxBufferedBodyBytes)) {
            case Http2BodyAccountingResult::kOk:
                break;
            case Http2BodyAccountingResult::kTooLarge:
                appendRstStream(header.streamId, Http2ErrorCode::kCancel);
                closeStream(
                    header.streamId,
                    Http2StreamCloseSource::kLocal,
                    Http2ErrorCode::kCancel);
                releaseDroppedDataConnectionWindow(flowBytes);
                return true;
            case Http2BodyAccountingResult::kContentLengthExceeded:
                appendRstStream(header.streamId, Http2ErrorCode::kProtocolError);
                closeStream(
                    header.streamId,
                    Http2StreamCloseSource::kLocal,
                    Http2ErrorCode::kProtocolError);
                releaseDroppedDataConnectionWindow(flowBytes);
                return true;
        }
    }
    if (flowBytes > 0) {
        if (stream->deferWindowRelease()) {
            // Streaming consumer: bank the credit; releaseStreamWindow() re-advertises
            // it as the owner drains, so a slow reader stalls the peer (backpressure)
            // instead of growing the buffered response without bound.
            stream->addWindowDebt(static_cast<std::uint32_t>(flowBytes));
        } else {
            const auto increment = static_cast<std::uint32_t>(flowBytes);
            http2CreditConnectionReceiveWindow(connectionReceiveWindow_, flowBytes);
            http2CreditStreamReceiveWindow(*stream, flowBytes);
            char buf[kHttp2WindowUpdateFrameBytes * 2];
            auto* out = http2WriteDataWindowUpdates(buf, header.streamId, increment);
            outBuffer_.append(buf, static_cast<std::size_t>(out - buf));
        }
    }

    // sans-I/O: hand the body to the owner as an event; the core does not buffer it
    // (buffered vs streaming delivery is application policy). Content-length and size caps
    // were already enforced by http2AccountDataBody above. The view is valid until the
    // next feed (input_ is only reclaimed at the start of the following feed).
    events_.push_back(tunnelData
        ? Http2Event::tunnelData(header.streamId, data)
        : Http2Event::messageBodyChunk(header.streamId, data));
    if ((header.flags & kHttp2FlagEndStream) != 0) {
        if (!tunnelData &&
            !http2RemoteContentTerminalValid(*stream, role_)) {
            appendRstStream(header.streamId, Http2ErrorCode::kProtocolError);
            closeStream(
                header.streamId,
                Http2StreamCloseSource::kLocal,
                Http2ErrorCode::kProtocolError);
            return true;
        }
        http2MarkBodyEnded(*stream);
        events_.push_back(tunnelData
            ? Http2Event::tunnelEnd(header.streamId)
            : Http2Event::messageEnd(header.streamId));
        releaseLocalRequestStreamIfClosed(*stream);
    }
    return true;
}

bool Http2Connection::processFrame(const Http2FrameHeader& header, std::string_view payload) {
    if (prefacePhase_ == PrefacePhase::kAwaitingPeerSettings &&
        header.type != static_cast<std::uint8_t>(Http2FrameType::kSettings)) {
        appendGoaway(Http2ErrorCode::kProtocolError, "first frame must be SETTINGS");
        return false;
    }
    if (!headerContinuation_.expectsFrameType(header.type)) {
        appendGoaway(Http2ErrorCode::kProtocolError, "expected CONTINUATION");
        return false;
    }
    switch (static_cast<Http2FrameType>(header.type)) {
        case Http2FrameType::kSettings:
            return processSettings(header, payload);
        case Http2FrameType::kPing:
            return processPing(header, payload);
        case Http2FrameType::kWindowUpdate:
            return processWindowUpdate(header, payload);
        case Http2FrameType::kRstStream:
            return processRstStream(header, payload);
        case Http2FrameType::kPriority:
            return processPriority(header, payload);
        case Http2FrameType::kHeaders:
            return processHeaders(header, payload);
        case Http2FrameType::kContinuation:
            return processContinuation(header, payload);
        case Http2FrameType::kData:
            return processData(header, payload);
        case Http2FrameType::kGoaway:
            return processGoaway(header, payload);
        case Http2FrameType::kPushPromise:
            // Server: clients can never push. Client: we advertise ENABLE_PUSH=0.
            appendGoaway(Http2ErrorCode::kProtocolError, "unexpected PUSH_PROMISE");
            return false;
        default:
            if (header.streamId != 0) {
                if (auto* stream = findStream(header.streamId);
                    stream != nullptr &&
                    stream->tunnel().open() != nullptr) {
                    // RFC 9113 8.5 narrows connected streams to DATA and the three
                    // stream-management frame types, even though unknown frames are
                    // normally ignored elsewhere.
                    appendRstStream(header.streamId, Http2ErrorCode::kProtocolError);
                    closeStream(
                        header.streamId,
                        Http2StreamCloseSource::kLocal,
                        Http2ErrorCode::kProtocolError);
                }
            }
            return true;
    }
}

bool Http2Connection::consumeFrames(std::string_view buffer, std::size_t& offset) {
    for (;;) {
        const std::size_t available = buffer.size() - offset;
        if (available < kHttp2FrameHeaderBytes) {
            break;
        }
        const auto header = http2ParseFrameHeader(buffer.substr(offset, kHttp2FrameHeaderBytes));
        if (header.length > kHttp2MaxFrameSizeLimit || header.length > localMaxFrameSize_) {
            appendGoaway(Http2ErrorCode::kFrameSizeError, "frame too large");
            return false;
        }
        if (available < kHttp2FrameHeaderBytes + header.length) {
            break;  // partial frame; wait for the next feed
        }
        const auto payload = buffer.substr(offset + kHttp2FrameHeaderBytes, header.length);
        if (!processFrame(header, payload)) {
            return false;
        }
        offset += kHttp2FrameHeaderBytes + header.length;
        if (connectionError_) {
            break;
        }
    }
    return true;
}

Http2FeedResult Http2Connection::feed(std::string_view in) {
    // Starting the role-specific preface is an explicit ownership boundary. In
    // particular, do not clear prior events, reclaim input, or copy caller bytes when
    // the driver forgot beginConnection(); the exact same span remains retryable.
    if (prefacePhase_ == PrefacePhase::kNotStarted) {
        return Http2FeedResult::kConnectionNotStarted;
    }
    // Event delivery is part of input ownership, not a best-effort side channel. An
    // unread event may carry a zero-copy view into the previous input, so accepting
    // another span here would both discard the event and invalidate its bytes.
    if (eventOffset_ < events_.size()) {
        return Http2FeedResult::kEventsPending;
    }
    if (connectionError_) {
        return Http2FeedResult::kProtocolFailure;
    }

    // Reclaim the prefix consumed by the PREVIOUS feed now, at the start of this one --
    // not at the end of that feed. A kMessageBodyChunk event carries a view INTO input_
    // (or into the caller's `in` on the fast path), and reclaiming shifts the buffer;
    // deferring the reclaim keeps those views valid until this next feed, matching the
    // documented contract. All prior events have been pulled (enforced above), so the
    // exhausted queue and its now-stale views can be reset.
    if (inputOffset_ > 0) {
        input_.erase(0, inputOffset_);
        inputOffset_ = 0;
    }
    events_.clear();
    eventOffset_ = 0;

    // FAST PATH: nothing buffered and no preface pending -> parse the complete frames
    // DIRECTLY over the caller's `in`, buffering only the unconsumed partial-frame tail.
    // This avoids copying the whole read (up to the driver's read-chunk size) into
    // input_ on the common case where each read delivers whole frames. Event body views
    // then point into `in`, valid until the next feed -- the same lifetime input_ views
    // have, and both in-tree drivers keep their read buffer alive across a feed.
    if (input_.empty() && prefacePhase_ != PrefacePhase::kAwaitingClientMagic) {
        std::size_t offset = 0;
        if (!consumeFrames(in, offset)) {
            return Http2FeedResult::kProtocolFailure;
        }
        if (offset < in.size()) {
            input_.append(in.data() + offset, in.size() - offset);  // partial-frame tail
        }
        if (connectionError_) {
            return Http2FeedResult::kProtocolFailure;
        }
        return input_.empty()
            ? Http2FeedResult::kAccepted
            : Http2FeedResult::kNeedInput;
    }

    // SLOW PATH: a buffered partial-frame tail and/or the connection preface is pending.
    // Buffer all fed bytes (nghttp2_session_mem_recv semantics) then consume frames.
    input_.append(in.data(), in.size());

    // Server mode: the 24-byte client connection preface precedes the first frame
    // (RFC 9113 §3.4). Consume + validate it before any frame parsing.
    if (prefacePhase_ == PrefacePhase::kAwaitingClientMagic) {
        if (input_.size() - inputOffset_ < kHttp2ClientPreface.size()) {
            return Http2FeedResult::kNeedInput;  // wait for the full preface
        }
        if (std::string_view(
                input_.data() + inputOffset_, kHttp2ClientPreface.size()) !=
            kHttp2ClientPreface) {
            appendGoaway(Http2ErrorCode::kProtocolError, "invalid connection preface");
            return Http2FeedResult::kProtocolFailure;
        }
        inputOffset_ += kHttp2ClientPreface.size();
        prefacePhase_ = PrefacePhase::kAwaitingPeerSettings;
    }

    if (!consumeFrames(std::string_view(input_.data(), input_.size()), inputOffset_)) {
        return Http2FeedResult::kProtocolFailure;
    }
    // NOTE: the consumed prefix is reclaimed at the START of the next feed (see above),
    // so body-chunk views handed out via events stay valid until then.
    if (connectionError_) {
        return Http2FeedResult::kProtocolFailure;
    }
    return inputOffset_ < input_.size()
        ? Http2FeedResult::kNeedInput
        : Http2FeedResult::kAccepted;
}

Http2StreamState* Http2Connection::stream(std::uint32_t streamId) noexcept {
    return streams_.find(streamId);
}

void Http2Connection::appendResponseHeaderFrames(
    Http2StreamState& stream,
    std::string_view headerBlock,
    Http2EndStream endStream) {
    // A HEADERS + CONTINUATION run must be an uninterrupted frame sequence for the same
    // stream (RFC 9113 §6.10). Appending them contiguously to the single outbound buffer
    // guarantees that ordering (replacing the coroutine writeHeaders' atomic write).
    const std::size_t maxFrame = peerSettings_.maxFrameSize();
    std::size_t offset = 0;
    bool first = true;
    while (offset < headerBlock.size()) {
        const auto chunk = std::min<std::size_t>(headerBlock.size() - offset, maxFrame);
        const bool last = offset + chunk == headerBlock.size();
        const auto flags = static_cast<std::uint8_t>(
            (last ? kHttp2FlagEndHeaders : 0) |
            (first && http2EndsStream(endStream) ? kHttp2FlagEndStream : 0));
        appendFrame(
            first ? Http2FrameType::kHeaders : Http2FrameType::kContinuation,
            flags, stream.id(), headerBlock.substr(offset, chunk));
        offset += chunk;
        first = false;
    }
}

Http2BufferedResponseHeadSubmitResult Http2Connection::submitResponseHead(
    std::uint32_t streamId, const HttpResponse& response) {
    auto* stream = findStream(streamId);
    if (stream == nullptr || stream->isReset()) {
        return Http2BufferedResponseHeadSubmitResult::makeFailure(
            Http2ResponseHeadSubmitError::kClosed);
    }
    const bool successfulConnect =
        response.status() >= 200 && response.status() < 300 &&
        stream->tunnel().pending() != nullptr;
    if (role_ != Http2Role::kServer || !stream->headersDecoded() ||
        !stream->canSubmitLocalHead() || successfulConnect) {
        return Http2BufferedResponseHeadSubmitResult::makeFailure(
            Http2ResponseHeadSubmitError::kInvalidState);
    }
    const auto controlPlan = httpFinalResponseControlPlan(
        response,
        HttpProtocolVersion::kHttp2);
    if (!controlPlan.accepted()) {
        return Http2BufferedResponseHeadSubmitResult::makeFailure(
            Http2ResponseHeadSubmitError::kInvalidMessage);
    }

    // A complete HttpResponse is buffered or file-backed, so its size is known.
    // Explicit response streaming uses submitStreamingResponseHead() instead.
    auto writePlan = httpBufferedResponseWritePlan(
        stream->requestKnownMethod(), response);
    if (appendHttp2ResponseHeaders(*stream, response, writePlan.contentLength(), true) !=
        Http2ResponseHeaderEncodeStatus::kOk) {
        return Http2BufferedResponseHeadSubmitResult::makeFailure(
            Http2ResponseHeadSubmitError::kInvalidMessage);
    }
    const auto endStream = writePlan.sendBody()
        ? Http2EndStream::kKeepOpen
        : Http2EndStream::kEndStream;
    if (writePlan.bodySuppressed()) {
        stream->beginLocalContentForbidden();
    } else {
        stream->beginLocalContentKnownLength(writePlan.contentLength());
    }
    appendResponseHeaderFrames(
        *stream,
        std::string_view(stream->responseHeaderBlock().data(), stream->responseHeaderBlock().size()),
        endStream);
    stream->markLocalHeadSubmitted(
        Http2LocalMessageKind::kResponse,
        http2EndsStream(endStream));
    if (stream->tunnel().pending() != nullptr) {
        (void)stream->rejectConnect();
    }
    http2ReleaseResponseHeaderBlock(*stream);
    return Http2BufferedResponseHeadSubmitResult::makeSubmitted(
        std::move(writePlan));
}

Http2StreamingResponseHeadSubmitResult Http2Connection::submitStreamingResponseHead(
    std::uint32_t streamId,
    HttpResponse head,
    ResponseStreamKind kind,
    ResponseTrailerIntent trailerIntent) {
    auto* stream = findStream(streamId);
    if (stream == nullptr || stream->isReset()) {
        return Http2StreamingResponseHeadSubmitResult::makeFailure(
            Http2ResponseHeadSubmitError::kClosed);
    }
    const bool successfulConnect =
        head.status() >= 200 && head.status() < 300 &&
        stream->tunnel().pending() != nullptr;
    if (role_ != Http2Role::kServer || !stream->headersDecoded() ||
        !stream->canSubmitLocalHead() || successfulConnect) {
        return Http2StreamingResponseHeadSubmitResult::makeFailure(
            Http2ResponseHeadSubmitError::kInvalidState);
    }
    const auto controlPlan = httpFinalResponseControlPlan(
        head,
        HttpProtocolVersion::kHttp2);
    if (!controlPlan.accepted()) {
        return Http2StreamingResponseHeadSubmitResult::makeFailure(
            Http2ResponseHeadSubmitError::kInvalidMessage);
    }
    auto bodyPlan = httpResponseBodyPlan(
        stream->requestKnownMethod(), head.status());
    auto streamHead = prepareResponseStreamHead(
        std::move(head),
        kind,
        ResponseStreamFraming::kHttp2Frames,
        bodyPlan,
        trailerIntent);
    const auto& commitPlan = streamHead.commitPlan();
    // Streaming never invents a length: an explicit value is validated and becomes
    // the exact DATA contract; absence remains unbounded. A content-forbidden
    // response either ends on this HEADERS block or enters the explicit
    // trailers-only phase; it never becomes DATA-open.
    const auto explicitContentLength =
        http2ExplicitResponseContentLength(streamHead.response());
    if (appendHttp2ResponseHeaders(
            *stream, streamHead.response(), 0, /*emitAutoContentLength=*/false) !=
        Http2ResponseHeaderEncodeStatus::kOk) {
        return Http2StreamingResponseHeadSubmitResult::makeFailure(
            Http2ResponseHeadSubmitError::kInvalidMessage);
    }
    const auto endStream =
        commitPlan.headDisposition() == ResponseStreamHeadDisposition::kMessageEnded
        ? Http2EndStream::kEndStream
        : Http2EndStream::kKeepOpen;
    if (bodyPlan.bodySuppressed()) {
        stream->beginLocalContentForbidden();
    } else if (explicitContentLength.status ==
               Http2ExplicitContentLengthStatus::kValid) {
        stream->beginLocalContentKnownLength(explicitContentLength.value);
    } else {
        stream->beginLocalContentUnbounded();
    }
    appendResponseHeaderFrames(
        *stream,
        std::string_view(stream->responseHeaderBlock().data(), stream->responseHeaderBlock().size()),
        endStream);
    if (commitPlan.headDisposition() ==
        ResponseStreamHeadDisposition::kTrailersOnly) {
        stream->markLocalTrailersOnlyHeadSubmitted(
            Http2LocalMessageKind::kResponse);
    } else {
        stream->markLocalHeadSubmitted(
            Http2LocalMessageKind::kResponse,
            http2EndsStream(endStream));
    }
    if (stream->tunnel().pending() != nullptr) {
        (void)stream->rejectConnect();
    }
    http2ReleaseResponseHeaderBlock(*stream);
    return Http2StreamingResponseHeadSubmitResult::makeSubmitted(commitPlan);
}

Http2SubmitStatus Http2Connection::submitInterimResponseHead(
    std::uint32_t streamId,
    const HttpInterimResponseHead& response) {
    auto* stream = findStream(streamId);
    if (stream == nullptr || stream->isReset()) {
        return Http2SubmitStatus::kClosed;
    }
    if (role_ != Http2Role::kServer || !stream->headersDecoded() ||
        !stream->canSubmitLocalHead()) {
        return Http2SubmitStatus::kInvalidState;
    }
    if (appendHttp2InterimResponseHeaders(*stream, response) !=
        Http2ResponseHeaderEncodeStatus::kOk) {
        return Http2SubmitStatus::kInvalidMessage;
    }
    appendResponseHeaderFrames(
        *stream,
        std::string_view(stream->responseHeaderBlock().data(), stream->responseHeaderBlock().size()),
        Http2EndStream::kKeepOpen);
    http2ReleaseResponseHeaderBlock(*stream);
    return Http2SubmitStatus::kAccepted;
}

Http2DataSubmitStatus Http2Connection::submitData(
    std::uint32_t streamId,
    std::string_view chunk,
    Http2EndStream endStream) {
    auto* stream = findStream(streamId);
    if (stream == nullptr || stream->isReset()) {
        return Http2DataSubmitStatus::kClosed;
    }
    if (!stream->localBodyOpen()) {
        return Http2DataSubmitStatus::kInvalidState;
    }
    // One queued submission per stream is the hard backpressure boundary. The
    // current input remains caller-owned and can be retried after the prior one drains.
    for (const auto& pending : pendingSends_) {
        if (pending.streamId == streamId) {
            return Http2DataSubmitStatus::kBackpressured;
        }
    }
    if (http2EndsStream(endStream) &&
        stream->localMessageKind() == Http2LocalMessageKind::kResponse &&
        !stream->responseTrailerBlock().empty()) {
        // A direct terminal DATA would strand already accepted semantic trailers.
        // The caller must submit keep-open DATA and use finishResponse(), which
        // atomically places the terminal trailer section after all DATA.
        return Http2DataSubmitStatus::kInvalidState;
    }
    switch (stream->checkLocalContentAccept(chunk.size(), http2EndsStream(endStream))) {
        case Http2LocalContentCheck::kAccepted:
            break;
        case Http2LocalContentCheck::kNotStarted:
        case Http2LocalContentCheck::kForbidden:
            return Http2DataSubmitStatus::kInvalidState;
        case Http2LocalContentCheck::kLengthExceeded:
            return Http2DataSubmitStatus::kContentLengthExceeded;
        case Http2LocalContentCheck::kLengthIncomplete:
            return Http2DataSubmitStatus::kContentLengthIncomplete;
    }
    // Prepare every allocation needed for a deferred suffix BEFORE accepting the
    // input or consuming flow-control window. A recoverable allocation failure can
    // therefore never leave a framed prefix without its core-owned remainder.
    std::optional<Http2PendingSend> deferred;
    if (!chunk.empty()) {
        const auto immediateBytes = std::min(
            chunk.size(), http2AvailableSendWindow(connectionSendWindow_, *stream));
        if (immediateBytes < chunk.size()) {
            std::pmr::string remainder(resource_);
            remainder.append(
                chunk.data() + immediateBytes, chunk.size() - immediateBytes);
            pendingSends_.reserve(pendingSends_.size() + 1);
            deferred.emplace(Http2PendingSend{
                streamId,
                std::move(remainder),
                0,
                endStream,
                std::pmr::string(resource_)});
        }
    }
    // Accepted means ownership of the WHOLE input, even when flow control below
    // can only materialize a prefix and the prepared suffix becomes pending.
    stream->acceptLocalContent(chunk.size());
    if (chunk.empty()) {
        if (http2EndsStream(endStream)) {
            appendFrame(Http2FrameType::kData, kHttp2FlagEndStream, streamId, {});
            stream->markLocalEndStreamCommitted();
            releaseLocalRequestStreamIfClosed(*stream);
        }
        return Http2DataSubmitStatus::kAccepted;
    }
    const auto consumed = sendDataUpToWindow(*stream, chunk, 0, endStream);
    if (consumed < chunk.size()) {
        // immediateBytes above is the exact total that sendDataUpToWindow can
        // consume from the current windows, so a deferred value must exist here.
        pendingSends_.push_back(std::move(*deferred));
        if (http2EndsStream(endStream)) {
            stream->markLocalEndStreamQueued();
        }
        return Http2DataSubmitStatus::kQueued;
    }
    if (http2EndsStream(endStream)) {
        stream->markLocalEndStreamCommitted();
        releaseLocalRequestStreamIfClosed(*stream);
    }
    return Http2DataSubmitStatus::kAccepted;
}

Http2SubmitStatus Http2Connection::submitConnectResponseHead(
    std::uint32_t streamId,
    const HttpResponse& response) {
    auto* stream = findStream(streamId);
    if (stream == nullptr || stream->isReset()) {
        return Http2SubmitStatus::kClosed;
    }
    if (role_ != Http2Role::kServer || !stream->headersDecoded() ||
        !stream->canSubmitLocalHead() ||
        stream->tunnel().pending() == nullptr ||
        !stream->responseTrailerBlock().empty()) {
        return Http2SubmitStatus::kInvalidState;
    }
    if (!http2IsValidConnectResponseHead(response)) {
        return Http2SubmitStatus::kInvalidMessage;
    }
    if (appendHttp2ResponseHeaders(
            *stream, response, 0, /*emitAutoContentLength=*/false) !=
        Http2ResponseHeaderEncodeStatus::kOk) {
        return Http2SubmitStatus::kInvalidMessage;
    }
    appendResponseHeaderFrames(
        *stream,
        std::string_view(
            stream->responseHeaderBlock().data(),
            stream->responseHeaderBlock().size()),
        Http2EndStream::kKeepOpen);
    (void)stream->acceptConnect();
    stream->beginLocalContentUnbounded();
    stream->markLocalHeadSubmitted(
        Http2LocalMessageKind::kConnectTunnel,
        /*endStream=*/false);
    http2ReleaseResponseHeaderBlock(*stream);
    if (stream->peerEndStream()) {
        events_.push_back(Http2Event::tunnelEnd(streamId));
    }
    return Http2SubmitStatus::kAccepted;
}

Http2SubmitStatus Http2Connection::submitWebSocketHandshake(
    std::uint32_t streamId, std::string_view subprotocol, std::string_view extensions) {
    auto* stream = findStream(streamId);
    if (stream == nullptr || stream->isReset()) {
        return Http2SubmitStatus::kClosed;
    }
    if (role_ != Http2Role::kServer || !stream->headersDecoded() ||
        !stream->canSubmitLocalHead() ||
        !http2IsPendingWebSocketConnect(*stream) ||
        !stream->responseTrailerBlock().empty()) {
        return Http2SubmitStatus::kInvalidState;
    }
    http2EncodeWebSocketHandshakeHeaders(stream->responseHeaderBlock(), subprotocol, extensions);
    appendResponseHeaderFrames(
        *stream,
        std::string_view(stream->responseHeaderBlock().data(), stream->responseHeaderBlock().size()),
        Http2EndStream::kKeepOpen);
    (void)stream->acceptConnect();
    stream->beginLocalContentUnbounded();
    stream->markLocalHeadSubmitted(
        Http2LocalMessageKind::kConnectTunnel,
        /*endStream=*/false);
    http2ReleaseResponseHeaderBlock(*stream);
    if (stream->peerEndStream()) {
        events_.push_back(Http2Event::tunnelEnd(streamId));
    }
    return Http2SubmitStatus::kAccepted;
}

Http2ResponseTrailerSubmitStatus Http2Connection::submitResponseTrailerSection(
    std::uint32_t streamId,
    std::span<const HttpHeaderView> trailers) {
    auto* stream = findStream(streamId);
    if (stream == nullptr || stream->isReset()) {
        return Http2ResponseTrailerSubmitStatus::kClosed;
    }
    if (role_ != Http2Role::kServer || !stream->headersDecoded() ||
        (!stream->localBodyOpen() && !stream->localTrailersOnly()) ||
        stream->localMessageKind() != Http2LocalMessageKind::kResponse ||
        !stream->responseTrailerBlock().empty()) {
        return Http2ResponseTrailerSubmitStatus::kInvalidState;
    }
    if (trailers.empty()) {
        return Http2ResponseTrailerSubmitStatus::kEmpty;
    }
    if (!responseTrailerSectionValid(trailers)) {
        return Http2ResponseTrailerSubmitStatus::kInvalidField;
    }
    if (!stream->localContent().lengthComplete()) {
        return Http2ResponseTrailerSubmitStatus::kContentLengthIncomplete;
    }

    // Encode into detached storage so a later field/allocation failure can never
    // attach a prefix of the semantic section to the live stream. All protocol
    // validation above completes before either HPACK bytes or stream state mutate.
    std::pmr::string encoded(resource_);
    for (const auto& trailer : trailers) {
        appendHttp2ResponseTrailer(encoded, trailer.name(), trailer.value());
    }
    stream->responseTrailerBlock().swap(encoded);
    return Http2ResponseTrailerSubmitStatus::kAccepted;
}

Http2FinishSubmitStatus Http2Connection::finishResponse(std::uint32_t streamId) {
    auto* stream = findStream(streamId);
    if (stream == nullptr || stream->isReset()) {
        return Http2FinishSubmitStatus::kClosed;
    }
    if ((!stream->localBodyOpen() && !stream->localTrailersOnly()) ||
        stream->localMessageKind() != Http2LocalMessageKind::kResponse) {
        return Http2FinishSubmitStatus::kInvalidState;
    }
    if (!stream->localContent().lengthComplete()) {
        return Http2FinishSubmitStatus::kContentLengthIncomplete;
    }
    auto& headerBlock = stream->responseTrailerBlock();
    if (stream->localTrailersOnly() && headerBlock.empty()) {
        // A trailers-only response cannot fall back to DATA(END_STREAM): its
        // method/status explicitly forbids DATA, including an empty terminal frame.
        return Http2FinishSubmitStatus::kInvalidState;
    }
    // If the body still has a window-blocked remainder, the trailer HEADERS must NOT
    // jump ahead of that queued DATA. Stash it on the pending entry and move END_STREAM
    // from the body to the trailer (markSendWindowOpened emits it once the body drains).
    for (auto& pending : pendingSends_) {
        if (pending.streamId == streamId) {
            if (headerBlock.empty()) {
                pending.endStream = Http2EndStream::kEndStream;
            } else {
                pending.endStream = Http2EndStream::kKeepOpen;
                pending.trailerBlock.assign(headerBlock.data(), headerBlock.size());
            }
            http2ReleaseResponseTrailerBlock(*stream);
            stream->markLocalEndStreamQueued();
            return Http2FinishSubmitStatus::kQueued;
        }
    }
    if (headerBlock.empty()) {
        appendFrame(Http2FrameType::kData, kHttp2FlagEndStream, streamId, {});
        stream->markLocalEndStreamCommitted();
        return Http2FinishSubmitStatus::kAccepted;
    }
    appendResponseHeaderFrames(
        *stream,
        std::string_view(headerBlock.data(), headerBlock.size()),
        Http2EndStream::kEndStream);
    http2ReleaseResponseTrailerBlock(*stream);
    stream->markLocalEndStreamCommitted();
    return Http2FinishSubmitStatus::kAccepted;
}

Http2SubmitStatus Http2Connection::submitReset(
    std::uint32_t streamId,
    Http2ErrorCode error) {
    if (streamId == 0) {
        return Http2SubmitStatus::kInvalidState;
    }
    auto* stream = findStream(streamId);
    if (stream == nullptr) {
        return closedStreams_.source(streamId) == Http2StreamCloseSource::kNone
            ? Http2SubmitStatus::kInvalidState
            : Http2SubmitStatus::kClosed;
    }
    if (stream->isReset()) {
        return Http2SubmitStatus::kClosed;
    }
    // A client-created stream is still RFC-idle until its request HEADERS are
    // submitted; RST_STREAM on that state would make the peer close the connection.
    // A server owner does not own a peer stream until its initial header block has
    // decoded; rejecting an early reset also preserves the mandatory CONTINUATION run.
    if ((role_ == Http2Role::kClient && stream->canSubmitLocalHead()) ||
        (role_ == Http2Role::kServer && !stream->headersDecoded()) ||
        (stream->peerEndStream() && stream->localEndStreamCommitted())) {
        return Http2SubmitStatus::kInvalidState;
    }
    appendRstStream(streamId, error);
    return closeStreamByOwner(streamId)
        ? Http2SubmitStatus::kAccepted
        : Http2SubmitStatus::kClosed;
}

void Http2Connection::beginConnection() {
    if (prefacePhase_ != PrefacePhase::kNotStarted) {
        return;
    }
    if (role_ == Http2Role::kClient) {
        outBuffer_.append(kHttp2ClientPreface.data(), kHttp2ClientPreface.size());
        prefacePhase_ = PrefacePhase::kAwaitingPeerSettings;
    } else {
        prefacePhase_ = PrefacePhase::kAwaitingClientMagic;
    }

    std::array<char,
        Http2LocalSettings::kFrameBytes + kHttp2WindowUpdateFrameBytes> buffer;
    auto* out = http2WriteLocalSettingsFrame(buffer.data());
    if constexpr (
        Http2LocalSettings::kInitialWindowSize >
        static_cast<std::uint32_t>(kHttp2DefaultInitialWindowSize)) {
        out = http2WriteWindowUpdate(
            out,
            0,
            Http2LocalSettings::kInitialWindowSize -
                static_cast<std::uint32_t>(kHttp2DefaultInitialWindowSize));
    }
    outBuffer_.append(buffer.data(), static_cast<std::size_t>(out - buffer.data()));
}

}  // namespace ruvia::detail
