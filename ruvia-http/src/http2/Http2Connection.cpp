#include "ruvia/http/detail/http2/Http2Connection.h"

#include <algorithm>
#include <array>

#include "ruvia/http/detail/http2/Http2FlowControl.h"
#include "ruvia/http/detail/http2/Http2FrameCodec.h"
#include "ruvia/http/detail/http2/Http2FramePayload.h"
#include "ruvia/http/detail/http2/Http2RemoteReceiveSemantics.h"
#include "ruvia/http/detail/http2/Http2ResponseHeaders.h"
#include "ruvia/http/detail/http2/Http2WindowUpdate.h"

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
// SETTINGS flood (CVE-2019-9515): every non-ACK SETTINGS frame is likewise echoed as a
// SETTINGS ACK into the outbound buffer, so an unread peer piling on empty SETTINGS grows
// output unboundedly exactly as a PING flood would. Same undrained-count budget, reset on
// the same output drains; a peer legitimately re-tuning SETTINGS mid-connection sends only
// a handful and never trips.
constexpr std::uint32_t kHttp2MaxUndrainedSettings = 1000;
}  // namespace

Http2Connection::Http2Connection(
    std::pmr::memory_resource* resource,
    Http2Role role)
    : resource_(resource),
      input_(resource),
      output_(resource),
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
    return output_.pending();
}

Http2OutputConsumeStatus Http2Connection::consumeOutput(
    std::size_t bytes) noexcept {
    const auto status = output_.consume(bytes);
    if (status == Http2OutputConsumeStatus::kDrained) {
        consecutivePings_ = 0;  // outbound (incl. PING ACKs) fully flushed
        consecutiveSettings_ = 0;  // outbound (incl. SETTINGS ACKs) fully flushed
    }
    return status;
}

void Http2Connection::takeOutput(std::pmr::string& into) {
    consecutivePings_ = 0;  // outbound (incl. PING ACKs) is being flushed
    consecutiveSettings_ = 0;  // outbound (incl. SETTINGS ACKs) is being flushed
    output_.take(into);
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
// logic: inline async_write became append-to-output_, coroutine resume became
// events / drained-data marking. The coroutine stack itself is deleted; this
// core is the single h2 implementation for both server and client roles.
// =============================================================================

void Http2Connection::appendGoaway(Http2ErrorCode error, std::string_view debug) {
    // RFC 9113 §6.8: "Endpoints MUST NOT increase the value they send in the last
    // stream identifier". lastStreamId_ doubles as the idle-stream high-water mark
    // (§5.1.1), so it keeps climbing for streams a graceful drain already refused --
    // those are exactly the ids the peer was told it may retry elsewhere. Clamp to the
    // advertised drain boundary; read it before fail() replaces the drain state.
    auto advertised = lastStreamId_;
    if (const auto* drain = localConnectionState_.gracefulDrain()) {
        advertised = std::min(advertised, drain->lastStreamId());
    }
    localConnectionState_.fail(error);
    output_.appendGoawayFrame(advertised, error, debug);
}

void Http2Connection::beginDrain() {
    // Graceful drain (RFC 9113 §6.8): advertise GOAWAY(NO_ERROR) at the last accepted
    // stream id WITHOUT a connection error -- established streams keep running, and HEADERS
    // for a stream above the advertised id are refused in processHeaders.
    if (!localConnectionState_.beginGracefulDrain(lastStreamId_)) {
        return;
    }
    output_.appendGoawayFrame(
        lastStreamId_, Http2ErrorCode::kNoError, "connection draining");
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
    // SETTINGS flood budget (CVE-2019-9515): bound non-ACK SETTINGS seen since output
    // was last drained, exactly like the PING flood, since each appends an ACK below.
    if (++consecutiveSettings_ > kHttp2MaxUndrainedSettings) {
        appendGoaway(Http2ErrorCode::kEnhanceYourCalm, "excessive SETTINGS");
        return false;
    }
    output_.appendFrame(Http2FrameType::kSettings, kHttp2FlagAck, 0, {});
    // SETTINGS_INITIAL_WINDOW_SIZE may have opened send windows: drain deferred DATA
    // and report streams whose core-owned remainder completed.
    markSendWindowOpened();
    return true;
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
        output_.appendFrame(
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
        if (stream == nullptr || stream->isAborted()) {
            pendingSends_.erase(pendingSends_.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        pending.offset = sendDataUpToWindow(
            *stream, std::string_view(pending.bytes.data(), pending.bytes.size()),
            pending.offset, pending.endStream);
        if (pending.offset >= pending.bytes.size()) {
            // The body fully drained. If a trailer block was queued behind it, emit it
            // now as the terminal HEADERS(END_STREAM) -- strictly AFTER all the DATA.
            if (!pending.trailerBlock.empty() && !stream->isAborted()) {
                appendResponseHeaderFrames(
                    *stream,
                    std::string_view(pending.trailerBlock.data(), pending.trailerBlock.size()),
                    Http2EndStream::kEndStream);
            }
            if (http2EndsStream(pending.endStream) || !pending.trailerBlock.empty()) {
                (void)stream->commitLocalEndStream();
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
    if (stream != nullptr && http2StreamIsClosed(*stream)) {
        // RFC 9113 section 6.9 permits a valid WINDOW_UPDATE on a closed
        // stream. A zero increment remains a stream PROTOCOL_ERROR, but no
        // RST_STREAM can legally be emitted after protocol closure.
        if (increment == 0) {
            appendGoaway(
                Http2ErrorCode::kProtocolError,
                "zero WINDOW_UPDATE on closed stream");
            return false;
        }
        return true;
    }
    if (stream == nullptr) {
        if (isIdleStreamId(header.streamId)) {
            appendGoaway(Http2ErrorCode::kProtocolError, "WINDOW_UPDATE on idle stream");
            return false;
        }
        if (increment == 0) {
            // A skipped/released identifier is closed, not idle. Promote the
            // mandatory stream error because RST_STREAM is forbidden there.
            appendGoaway(
                Http2ErrorCode::kProtocolError,
                "zero WINDOW_UPDATE on released closed stream");
            return false;
        }
        return true;
    }
    switch (http2ApplyStreamWindowUpdate(*stream, increment)) {
        case Http2WindowUpdateResult::kOk:
            markSendWindowOpened();
            return true;
        case Http2WindowUpdateResult::kZeroIncrement:
            output_.appendRstStream(header.streamId, Http2ErrorCode::kProtocolError);
            closeStream(
                header.streamId,
                Http2StreamCloseSource::kLocal,
                Http2ErrorCode::kProtocolError);
            markSendWindowOpened();
            return true;
        case Http2WindowUpdateResult::kOverflow:
            output_.appendRstStream(header.streamId, Http2ErrorCode::kFlowControlError);
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
    // Unreleased event credit must return to the CONNECTION window when a stream is
    // removed, or an owner that stops consuming after reset would permanently shrink
    // the shared window. Connection-scope only: a stream WINDOW_UPDATE on a gone
    // stream is a peer protocol error.
    const auto debt = stream.takeWindowDebt();
    if (debt == 0) {
        return;
    }
    http2CreditConnectionReceiveWindow(
        connectionReceiveWindow_, static_cast<std::int32_t>(debt));
    char buf[kHttp2WindowUpdateFrameBytes];
    http2WriteWindowUpdate(buf, 0, debt);
    output_.appendBytes(std::string_view(buf, sizeof(buf)));
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
    if (stream->isAborted()) {
        // The abnormal terminal transition already returned flow-control debt and
        // discarded deferred sends. The pin only kept request-view storage alive.
        releaseLocalRequestStream(*stream);
        streams_.remove(streamId);
        return;
    }

    if (http2StreamIsClosed(*stream)) {
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
    const auto error = stream->localSend().endStreamCommitted() != nullptr
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
    }
}

bool Http2Connection::closeStreamImpl(
    std::uint32_t streamId,
    Http2StreamCloseSource source,
    Http2ErrorCode error,
    CloseNotification notification) {
    auto* stream = streams_.find(streamId);
    if (stream != nullptr && !stream->isAborted()) {
        detachActiveHeaderBlock(*stream);
    }
    readyQueue_.remove(streamId);
    discardDeferredStreamState(streamId);
    if (stream == nullptr || stream->isAborted()) {
        return false;
    }

    releaseLocalRequestStream(*stream);
    (void)stream->abort(source);
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

bool Http2Connection::wasClosedByPeerReset(
    std::uint32_t streamId,
    const Http2StreamState* retainedStream) const noexcept {
    if (retainedStream != nullptr) {
        return retainedStream->isAborted() &&
            retainedStream->localSend().aborted()->source() ==
                Http2StreamCloseSource::kPeer;
    }
    return closedStreams_.source(streamId) ==
        Http2StreamCloseSource::kPeer;
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
    auto* const stream = streams_.find(header.streamId);
    if (stream == nullptr &&
        isIdleStreamId(header.streamId)) {
        appendGoaway(Http2ErrorCode::kProtocolError, "RST_STREAM on idle stream");
        return false;
    }
    if (wasClosedByPeerReset(header.streamId, stream)) {
        // This peer's two resets are ordered on the same connection, so the
        // second cannot have been sent before it knew that it had terminated
        // the stream. Enforce the same peer-reset finality as DATA/HEADERS and
        // avoid counting a duplicate as another rapid-reset lifecycle.
        appendGoaway(
            Http2ErrorCode::kStreamClosed,
            "RST_STREAM after peer RST_STREAM");
        return false;
    }
    if (stream != nullptr && http2StreamIsClosed(*stream)) {
        // RST_STREAM can race with END_STREAM or a reset sent by this endpoint.
        // The stream was already terminal, so minimally process without charging
        // another peer-reset lifecycle.
        return true;
    }
    if (closedStreams_.source(header.streamId) ==
        Http2StreamCloseSource::kPeerGoaway) {
        // The peer already declared this request unprocessed. A trailing reset has no
        // stream lifecycle left to mutate and must not consume the rapid-reset budget.
        return true;
    }
    const auto error = static_cast<Http2ErrorCode>(http2Read32(
        reinterpret_cast<const unsigned char*>(payload.data())));
    if (!closeStream(header.streamId, Http2StreamCloseSource::kPeer, error)) {
        // A reset can race with one sent by this endpoint. RFC 9113 requires
        // minimal processing in that state, but the no-op neither opened a
        // handler slot nor freed one and therefore must not spend the
        // rapid-reset lifecycle budget.
        return true;
    }
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
    if (header.streamId == 0) {
        appendGoaway(Http2ErrorCode::kProtocolError, "PRIORITY stream id must be nonzero");
        return false;
    }
    if (payload.size() != 5) {
        auto* const stream = findStream(header.streamId);
        if (stream == nullptr || http2StreamIsClosed(*stream)) {
            // PRIORITY is allowed in every state, but RST_STREAM is not legal
            // on an idle or closed stream. Promote its mandatory stream
            // FRAME_SIZE_ERROR to the connection instead.
            appendGoaway(
                Http2ErrorCode::kFrameSizeError,
                "invalid PRIORITY without active stream");
            return false;
        }
        // RFC 9113 section 6.3 makes malformed PRIORITY length a stream error,
        // unlike most fixed-size connection-control frames. Do not terminate
        // unrelated multiplexed streams for one bad advisory frame.
        output_.appendRstStream(
            header.streamId, Http2ErrorCode::kFrameSizeError);
        if (stream != nullptr) {
            closeStream(
                header.streamId,
                Http2StreamCloseSource::kLocal,
                Http2ErrorCode::kFrameSizeError);
        }
        return true;
    }
    // RFC 9113 deprecates the RFC 7540 priority tree. Retain frame-shape validation,
    // then ignore the advisory dependency and weight on streams in every state.
    return true;
}

bool Http2Connection::processGoaway(
    const Http2FrameHeader& header,
    std::string_view payload) {
    // RFC 9113 §6.8 gives these two malformations distinct codes: a nonzero stream id is
    // a PROTOCOL_ERROR, while a payload short of the 8-octet fixed fields is a
    // FRAME_SIZE_ERROR. Both are connection errors, but conformance suites read the code.
    if (header.streamId != 0) {
        appendGoaway(Http2ErrorCode::kProtocolError, "GOAWAY stream id must be zero");
        return false;
    }
    if (payload.size() < 8) {
        appendGoaway(Http2ErrorCode::kFrameSizeError, "invalid GOAWAY size");
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
            if (stream.id() <= goaway.lastStreamId() || stream.isAborted()) {
                return;
            }
            // A response head proves that the peer acted on this request. Claiming
            // otherwise would make replay unsafe, so reject the contradictory GOAWAY.
            if (http2RemoteFinalHeadDecoded(stream) ||
                stream.interimResponseCount() != 0) {
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
    output_.appendFrame(Http2FrameType::kPing, kHttp2FlagAck, 0, payload);  // echo back
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
    if (localConnectionState_.fatalFailure() != nullptr ||
        peerGoaway_.has_value() || nextLocalStreamId_ > 0x7fffffffU) {
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
    if (http2StreamIsClosed(stream)) {
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

void Http2Connection::releaseReceivedData(std::uint32_t streamId) {
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
    if (!http2RemotePeerHalfClosed(*stream)) {
        http2CreditStreamReceiveWindow(*stream, static_cast<std::int32_t>(debt));
        char buf[kHttp2WindowUpdateFrameBytes * 2];
        auto* out = http2WriteDataWindowUpdates(buf, streamId, debt);
        output_.appendBytes(std::string_view(
            buf, static_cast<std::size_t>(out - buf)));
    } else {
        char buf[kHttp2WindowUpdateFrameBytes];
        http2WriteWindowUpdate(buf, 0, debt);
        output_.appendBytes(std::string_view(buf, sizeof(buf)));
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
    output_.appendBytes(std::string_view(buf, sizeof(buf)));
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
    if (wasClosedByPeerReset(header.streamId, stream)) {
        // This peer's RST_STREAM and later DATA are ordered on the same
        // connection. Unlike DATA that was already in flight when WE sent a
        // reset, this cannot be a state-view race. Do not answer with another
        // stream frame; use the same strict closed-state verdict for retained
        // (pinned) and already-released storage.
        appendGoaway(
            Http2ErrorCode::kStreamClosed,
            "DATA after peer RST_STREAM");
        return false;
    }
    if (stream == nullptr) {
        const auto closeSource = closedStreams_.source(header.streamId);
        if (closeSource == Http2StreamCloseSource::kPeerGoaway) {
            releaseDroppedDataConnectionWindow(flowBytes);
            return true;
        }
        if (!isIdleStreamId(header.streamId)) {
            // A locally reset stream can receive DATA that was already in flight.
            // Minimal processing still debits connection flow control, then drops
            // it without manufacturing an illegal second stream frame. Applying
            // this tolerant rule to released closed streams also avoids coupling
            // wire behavior to the bounded close-history lifetime.
            releaseDroppedDataConnectionWindow(flowBytes);
            return true;
        }
        appendGoaway(Http2ErrorCode::kProtocolError, "DATA before HEADERS");
        return false;
    }
    if (http2StreamIsClosed(*stream)) {
        releaseDroppedDataConnectionWindow(flowBytes);
        return true;
    }
    const auto& remote = stream->remoteReceive();
    if (remote.headPending() != nullptr ||
        remote.headEndStreamPending() != nullptr) {
        output_.appendRstStream(header.streamId, Http2ErrorCode::kProtocolError);
        closeStream(
            header.streamId,
            Http2StreamCloseSource::kLocal,
            Http2ErrorCode::kProtocolError);
        releaseDroppedDataConnectionWindow(flowBytes);
        return true;
    }
    if (remote.endStream() != nullptr ||
        remote.connectPendingEndStream() != nullptr) {
        // END_STREAM closes only the peer's send half. The opposite half of an
        // accepted CONNECT tunnel remains usable, but another DATA frame from this
        // peer is a frame on a half-closed (remote) stream (RFC 9113 5.1/8.5).
        output_.appendRstStream(header.streamId, Http2ErrorCode::kStreamClosed);
        closeStream(
            header.streamId,
            Http2StreamCloseSource::kLocal,
            Http2ErrorCode::kStreamClosed);
        releaseDroppedDataConnectionWindow(flowBytes);
        return true;
    }
    const bool pendingConnectControl = remote.connectPending() != nullptr;
    const bool rejectedConnectTerminal =
        remote.connectRejectedAwaitingEndStream() != nullptr;
    const bool tunnelData = remote.tunnelOpen() != nullptr;
    const bool contentData = remote.contentOpen() != nullptr;
    const bool metadataOnlyContent = contentData &&
        (stream->remoteContent().metadataOnlyWithoutLength() != nullptr ||
         stream->remoteContent().metadataOnlyKnownLength() != nullptr);
    if (!pendingConnectControl && !rejectedConnectTerminal &&
        !tunnelData && !contentData) {
        output_.appendRstStream(header.streamId, Http2ErrorCode::kStreamClosed);
        closeStream(
            header.streamId,
            Http2StreamCloseSource::kLocal,
            Http2ErrorCode::kStreamClosed);
        releaseDroppedDataConnectionWindow(flowBytes);
        return true;
    }

    if (http2DebitStreamReceiveWindow(*stream, flowBytes) ==
        Http2ReceiveWindowDebitStatus::kExceeded) {
        output_.appendRstStream(header.streamId, Http2ErrorCode::kFlowControlError);
        closeStream(
            header.streamId,
            Http2StreamCloseSource::kLocal,
            Http2ErrorCode::kFlowControlError);
        releaseDroppedDataConnectionWindow(flowBytes);
        return true;
    }

    if (pendingConnectControl || rejectedConnectTerminal) {
        // RFC 9110 §9.3.6 says CONNECT has no request content, while RFC 9113 §8.1
        // still requires a final frame carrying END_STREAM. Empty DATA is therefore
        // framing-only both before a decision and after rejection; never surface it
        // as tunnel/content bytes. Padding remains flow-controlled.
        if (!data.empty()) {
            output_.appendRstStream(header.streamId, Http2ErrorCode::kProtocolError);
            closeStream(
                header.streamId,
                Http2StreamCloseSource::kLocal,
                Http2ErrorCode::kProtocolError);
            releaseDroppedDataConnectionWindow(flowBytes);
            return true;
        }
        if (flowBytes > 0) {
            const auto increment = static_cast<std::uint32_t>(flowBytes);
            http2CreditConnectionReceiveWindow(connectionReceiveWindow_, flowBytes);
            http2CreditStreamReceiveWindow(*stream, flowBytes);
            char buf[kHttp2WindowUpdateFrameBytes * 2];
            auto* out = http2WriteDataWindowUpdates(
                buf, header.streamId, increment);
            output_.appendBytes(std::string_view(
                buf, static_cast<std::size_t>(out - buf)));
        }
        if ((header.flags & kHttp2FlagEndStream) == 0) {
            return true;
        }
        const bool remoteFinished = pendingConnectControl
            ? stream->finishRemotePendingConnect()
            : stream->finishRemoteRejectedConnect();
        if (!remoteFinished) {
            output_.appendRstStream(header.streamId, Http2ErrorCode::kProtocolError);
            closeStream(
                header.streamId,
                Http2StreamCloseSource::kLocal,
                Http2ErrorCode::kProtocolError);
            return true;
        }
        releaseLocalRequestStreamIfClosed(*stream);
        return true;
    }

    if (contentData) {
        switch (stream->accountRemoteContent(data.size())) {
            case Http2RemoteContentAccountingResult::kAccepted:
                break;
            case Http2RemoteContentAccountingResult::kCounterOverflow:
                output_.appendRstStream(header.streamId, Http2ErrorCode::kCancel);
                closeStream(
                    header.streamId,
                    Http2StreamCloseSource::kLocal,
                    Http2ErrorCode::kCancel);
                releaseDroppedDataConnectionWindow(flowBytes);
                return true;
            case Http2RemoteContentAccountingResult::kDeclaredLengthExceeded:
            case Http2RemoteContentAccountingResult::kContentForbidden:
                output_.appendRstStream(header.streamId, Http2ErrorCode::kProtocolError);
                closeStream(
                    header.streamId,
                    Http2StreamCloseSource::kLocal,
                    Http2ErrorCode::kProtocolError);
                releaseDroppedDataConnectionWindow(flowBytes);
                return true;
        }
    }
    const bool deliverData = !metadataOnlyContent && !data.empty();
    if (flowBytes > 0) {
        if (deliverData) {
            // The receiver advertises new capacity only after the event owner has
            // consumed or copied these bytes. This applies equally to HTTP content
            // and CONNECT tunnel DATA; immediate credit would disable backpressure
            // before the external runtime can select its storage policy.
            stream->addWindowDebt(static_cast<std::uint32_t>(flowBytes));
        } else {
            // Empty DATA (including padding-only DATA) gives the owner no content to
            // retain. Metadata-only empty frames likewise need no application ack.
            const auto increment = static_cast<std::uint32_t>(flowBytes);
            http2CreditConnectionReceiveWindow(connectionReceiveWindow_, flowBytes);
            http2CreditStreamReceiveWindow(*stream, flowBytes);
            char buf[kHttp2WindowUpdateFrameBytes * 2];
            auto* out = http2WriteDataWindowUpdates(buf, header.streamId, increment);
            output_.appendBytes(std::string_view(
                buf, static_cast<std::size_t>(out - buf)));
        }
    }

    // sans-I/O: hand only actual body bytes to the owner. Empty and padding-only
    // DATA frames still participate in framing, END_STREAM, and flow control, but
    // exposing them as empty chunks would create no-progress queue wakeups and let
    // a frame flood allocate one application event per nine wire octets.
    // Buffered vs streaming delivery and product size limits remain owner policy.
    if (deliverData) {
        events_.push_back(tunnelData
            ? Http2Event::tunnelData(header.streamId, data)
            : Http2Event::messageBodyChunk(header.streamId, data));
    }
    if ((header.flags & kHttp2FlagEndStream) != 0) {
        if (contentData &&
            !stream->remoteContent().terminalLengthValid()) {
            output_.appendRstStream(header.streamId, Http2ErrorCode::kProtocolError);
            closeStream(
                header.streamId,
                Http2StreamCloseSource::kLocal,
                Http2ErrorCode::kProtocolError);
            return true;
        }
        const bool remoteFinished = tunnelData
            ? stream->finishRemoteTunnel()
            : stream->finishRemoteContent();
        if (!remoteFinished) {
            output_.appendRstStream(header.streamId, Http2ErrorCode::kProtocolError);
            closeStream(
                header.streamId,
                Http2StreamCloseSource::kLocal,
                Http2ErrorCode::kProtocolError);
            return true;
        }
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
                    stream != nullptr && !http2StreamIsClosed(*stream) &&
                    stream->tunnel().open() != nullptr) {
                    // RFC 9113 8.5 narrows connected streams to DATA and the three
                    // stream-management frame types, even though unknown frames are
                    // normally ignored elsewhere.
                    output_.appendRstStream(header.streamId, Http2ErrorCode::kProtocolError);
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
        if (localConnectionState_.fatalFailure() != nullptr) {
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
    if (localConnectionState_.fatalFailure() != nullptr) {
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
        if (localConnectionState_.fatalFailure() != nullptr) {
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
    if (localConnectionState_.fatalFailure() != nullptr) {
        return Http2FeedResult::kProtocolFailure;
    }
    return inputOffset_ < input_.size()
        ? Http2FeedResult::kNeedInput
        : Http2FeedResult::kAccepted;
}

Http2StreamState* Http2Connection::stream(std::uint32_t streamId) noexcept {
    return streams_.find(streamId);
}


void Http2Connection::beginConnection() {
    if (prefacePhase_ != PrefacePhase::kNotStarted) {
        return;
    }
    if (role_ == Http2Role::kClient) {
        output_.appendBytes(kHttp2ClientPreface);
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
    output_.appendBytes(std::string_view(
        buffer.data(), static_cast<std::size_t>(out - buffer.data())));
}

}  // namespace ruvia::detail
