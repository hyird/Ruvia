#include "ruvia/http/detail/http2/Http2Connection.h"

#include <algorithm>
#include <array>

#include "ruvia/http/detail/HttpResponseContentSemantics.h"
#include "ruvia/http/detail/http2/Http2FlowControl.h"
#include "ruvia/http/detail/http2/Http2FrameCodec.h"
#include "ruvia/http/detail/http2/Http2FramePayload.h"
#include "ruvia/http/detail/http2/Http2HeaderBlock.h"
#include "ruvia/http/detail/http2/Http2RequestHeaders.h"
#include "ruvia/http/detail/http2/Http2HeaderRules.h"
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

[[nodiscard]] bool http2RemoteFinalHeadDecoded(
    const Http2StreamState& stream) noexcept {
    const auto& remote = stream.remoteReceive();
    return remote.headPending() == nullptr &&
        remote.headEndStreamPending() == nullptr;
}

[[nodiscard]] bool http2RemotePeerHalfClosed(
    const Http2StreamState& stream) noexcept {
    const auto& remote = stream.remoteReceive();
    return remote.headEndStreamPending() != nullptr ||
        remote.connectPendingEndStream() != nullptr ||
        remote.endStream() != nullptr ||
        remote.aborted() != nullptr;
}

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
    }
    return status;
}

void Http2Connection::takeOutput(std::pmr::string& into) {
    consecutivePings_ = 0;  // outbound (incl. PING ACKs) is being flushed
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
    connectionError_ = error;
    output_.appendGoawayFrame(lastStreamId_, error, debug);
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

    if (http2RemotePeerHalfClosed(*stream) &&
        stream->localSend().endStreamCommitted() != nullptr) {
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
    if (stream.isAborted() ||
        (http2RemotePeerHalfClosed(stream) &&
         stream.localSend().endStreamCommitted() != nullptr)) {
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
            stream.remoteContent().allowedKnownLength() != nullptr) {
            return HeaderDecodeStatus::kProtocolError;
        }
        if (!stream.beginExtendedConnect()) {
            return HeaderDecodeStatus::kProtocolError;
        }
    } else if (stream.requestKnownMethod() == HttpKnownMethod::kConnect) {
        RequestTargetView connectTarget;
        if (!stream.hasAuthority() || stream.hasScheme() || stream.hasPath() ||
            stream.remoteContent().allowedKnownLength() != nullptr ||
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
    const bool remoteHeadFinalized = stream.tunnel().pending() != nullptr
        ? stream.finalizeRemoteConnectHead()
        : stream.finalizeRemoteContentHead();
    if (!remoteHeadFinalized) {
        return HeaderDecodeStatus::kProtocolError;
    }
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
    std::optional<std::uint16_t> status;
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
        if (name != ":status" || context->status || context->sawRegular) {
            return false;
        }
        int parsedStatus = 0;
        const auto [ptr, ec] = std::from_chars(
            value.data(), value.data() + value.size(), parsedStatus);
        if (value.size() != 3 || ec != std::errc{} || ptr != value.data() + value.size() ||
            parsedStatus < 100 || parsedStatus > 999 || parsedStatus == 101) {
            return false;
        }
        context->status = static_cast<std::uint16_t>(parsedStatus);
        return true;
    }
    if (!context->status || !http2IsValidRegularHeader(name, value)) {
        return false;
    }
    context->sawRegular = true;
    if (*context->status < 200) {
        return true;  // interim head: validate only, never stored
    }
    const auto kind = classifyRequestHeader(name);
    const bool successfulConnect =
        httpResponseContentSemantics(
            stream.requestKnownMethod(), *context->status)
            .connectTunnel() != nullptr;
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
    Http2ResponseDecodeContext context{
        Http2HeaderDecodeContext{stream}, std::nullopt, false};
    const auto result = decoder_.decode(
        stream.requestHeaderBlock(), &context,
        [](void* target, std::string_view name, std::string_view value) {
            return http2OnDecodedResponseHeader(target, name, value);
        });
    http2ResetHeaderBlock(stream);
    if (const auto status = http2ClassifyHeaderDecodeResult(result); status != HeaderDecodeStatus::kOk) {
        return status;
    }
    if (!context.status) {
        return HeaderDecodeStatus::kProtocolError;
    }
    if (*context.status < 200) {
        // 1xx interim head cannot carry END_STREAM. Without it, the remote receive
        // state remains head-pending so the next HEADERS is decoded as another head.
        if (stream.remoteReceive().headEndStreamPending() != nullptr) {
            return HeaderDecodeStatus::kProtocolError;
        }
        stream.countInterimResponse();
        if (stream.interimResponseCount() > kMaxHttp2InterimResponses) {
            return HeaderDecodeStatus::kProtocolError;
        }
        return HeaderDecodeStatus::kOk;
    }
    if (!stream.setResponseStatus(*context.status)) {
        return HeaderDecodeStatus::kProtocolError;
    }
    const auto contentSemantics = httpResponseContentSemantics(
        stream.requestKnownMethod(), *context.status);
    if (contentSemantics.withoutContent() != nullptr &&
        !stream.selectRemoteContentMetadataOnly()) {
        return HeaderDecodeStatus::kProtocolError;
    }
    if (stream.tunnel().pending() != nullptr) {
        if (contentSemantics.connectTunnel() != nullptr) {
            if (!stream.acceptConnect()) {
                return HeaderDecodeStatus::kProtocolError;
            }
            stream.beginLocalContentUnbounded();
            (void)stream.openLocalConnectTunnel();
        } else {
            if (!stream.rejectConnect()) {
                return HeaderDecodeStatus::kProtocolError;
            }
            output_.appendFrame(Http2FrameType::kData, kHttp2FlagEndStream, stream.id(), {});
            (void)stream.rejectLocalConnect();
        }
    } else if (!stream.finalizeRemoteContentHead()) {
        return HeaderDecodeStatus::kProtocolError;
    }
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
    output_.appendRstStream(streamId, error);
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
    if (!stream.remoteContent().terminalLengthValid()) {
        return HeaderDecodeStatus::kProtocolError;
    }
    if (!stream.finishRemoteContent()) {
        return HeaderDecodeStatus::kProtocolError;
    }
    events_.push_back(Http2Event::messageEnd(stream.id()));
    releaseLocalRequestStreamIfClosed(stream);
    return HeaderDecodeStatus::kOk;
}

bool Http2Connection::handleHeaderDecodeFailure(Http2StreamState& stream, HeaderDecodeStatus status) {
    if (status == HeaderDecodeStatus::kCompressionError) {
        appendGoaway(Http2ErrorCode::kCompressionError, "invalid HPACK block");
        return false;
    }
    output_.appendRstStream(stream.id(), Http2ErrorCode::kProtocolError);
    if (findStream(stream.id()) == &stream) {
        closeStream(
            stream.id(),
            Http2StreamCloseSource::kLocal,
            Http2ErrorCode::kProtocolError);  // emits a typed stream-closed event
    } else {
        (void)stream.abort(Http2StreamCloseSource::kLocal);
        // Refused-stream scratch is not in the table, but still models the same
        // whole-stream terminal transition as a live stream.
    }
    return true;
}

void Http2Connection::emitRequestHeaders(Http2StreamState& stream) {
    // RFC 9113 §8.1.1: a declared content-length must equal the summed DATA payload.
    // A body-less HEADERS (END_STREAM set) with a nonzero content-length can never be
    // satisfied -- reject it here so both END_STREAM routes stay consistent. Client
    // role: HEAD responses and 204/304 are exempt (their content-length describes the
    // body a GET would have had, RFC 9110 §8.6).
    if (http2RemotePeerHalfClosed(stream) &&
        !stream.remoteContent().terminalLengthValid()) {
        output_.appendRstStream(stream.id(), Http2ErrorCode::kProtocolError);
        closeStream(
            stream.id(),
            Http2StreamCloseSource::kLocal,
            Http2ErrorCode::kProtocolError);  // remove, don't leak the slot
        return;
    }
    events_.push_back(Http2Event::messageHead(stream.id()));
    if (role_ == Http2Role::kServer &&
        stream.tunnel().pending() != nullptr) {
        // CONNECT has no request content, but that fact alone says nothing about the
        // peer send half: it can remain open for a tunnel or already carry END_STREAM.
        // Route/accept decisions start from kMessageHead; the typed remote state owns
        // any later tunnel-end signal, so a generic kMessageEnd would be misleading.
        return;
    }
    if (role_ == Http2Role::kClient &&
        stream.tunnel().open() != nullptr) {
        if (http2RemotePeerHalfClosed(stream)) {
            events_.push_back(Http2Event::tunnelEnd(stream.id()));
        }
        releaseLocalRequestStreamIfClosed(stream);
        return;
    }
    if (http2RemotePeerHalfClosed(stream)) {
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
        if (existing->isAborted()) {
            if (existing->localSend().aborted()->source() ==
                Http2StreamCloseSource::kPeer) {
                // Frames sent after the peer's own RST are not an in-flight race: the
                // connection byte stream orders them after that terminal signal.
                appendGoaway(Http2ErrorCode::kStreamClosed, "HEADERS after peer RST_STREAM");
                return false;
            }
            // After WE sent RST_STREAM, any peer frames already in flight must be
            // minimally processed and discarded. Do not send a second RST.
            discardedAction = DiscardedHeaderAction::kIgnore;
        } else if (http2RemoteFinalHeadDecoded(*existing) &&
                   (existing->tunnel().open() != nullptr ||
                    (role_ == Http2Role::kServer &&
                     existing->tunnel().pending() != nullptr))) {
            // CONNECT has no request trailers, and an accepted connected stream only
            // permits DATA/RST_STREAM/WINDOW_UPDATE/PRIORITY. Decode the complete
            // field block for HPACK synchronization, then reset this stream.
            return startDiscardedHeaderBlock(
                header, fragment, DiscardedHeaderAction::kResetProtocolError);
        } else if (http2RemoteFinalHeadDecoded(*existing)) {
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
        if (!stream->recordRemoteHeadEndStream()) {
            output_.appendRstStream(header.streamId, Http2ErrorCode::kProtocolError);
            closeStream(
                header.streamId,
                Http2StreamCloseSource::kLocal,
                Http2ErrorCode::kProtocolError);
            return true;
        }
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
        if (http2RemoteFinalHeadDecoded(*stream)) {
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
    if (http2RemotePeerHalfClosed(stream)) {
        return startDiscardedHeaderBlock(
            header, fragment, DiscardedHeaderAction::kResetStreamClosed);
    }
    if (stream.remoteReceive().contentOpen() == nullptr) {
        return startDiscardedHeaderBlock(
            header, fragment, DiscardedHeaderAction::kResetProtocolError);
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
        if (stream == nullptr || stream->isAborted()) {
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
            if (http2RemoteFinalHeadDecoded(*stream)) {
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
    if (stream == nullptr) {
        if (closedStreams_.source(header.streamId) ==
            Http2StreamCloseSource::kPeerGoaway) {
            releaseDroppedDataConnectionWindow(flowBytes);
            return true;
        }
        if (!isIdleStreamId(header.streamId)) {
            output_.appendRstStream(header.streamId, Http2ErrorCode::kStreamClosed);
            releaseDroppedDataConnectionWindow(flowBytes);
            return true;
        }
        appendGoaway(Http2ErrorCode::kProtocolError, "DATA before HEADERS");
        return false;
    }
    if (stream->isAborted()) {
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

    // sans-I/O: hand the body to the owner as an event; the core does not buffer it
    // (buffered vs streaming delivery and product size limits are owner policy).
    // Content-Length was accounted above. A non-empty event retains receive-window
    // debt until releaseReceivedData(); the view remains valid until the next feed.
    if (!metadataOnlyContent) {
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
                    stream != nullptr &&
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
