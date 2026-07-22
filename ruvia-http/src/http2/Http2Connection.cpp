#include "ruvia/http/detail/http2/Http2Connection.h"

#include <algorithm>
#include <array>
#include <utility>

#include "ruvia/http/detail/http2/frame/Http2FrameCodec.h"
#include "ruvia/http/detail/http2/frame/Http2FramePayload.h"
#include "ruvia/http/detail/http2/message/Http2RemoteReceiveSemantics.h"
#include "ruvia/http/detail/http2/flow/Http2WindowUpdate.h"

namespace ruvia::detail {

namespace {
// Defense-in-depth budgets for the sans-I/O h2 core. The core has no clock, so these are
// per-connection counters (not rates); both trip GOAWAY(ENHANCE_YOUR_CALM).
//
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

std::string_view Http2Connection::pendingOutput() const & noexcept {
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

std::span<const std::uint32_t>
Http2Connection::takeDrainedDataStreams() & noexcept {
    // Swap-and-clear so each drain is reported exactly once; the returned span stays
    // valid until the next call (double buffer, no allocation churn).
    takenDrainedDataStreams_.swap(drainedDataStreams_);
    drainedDataStreams_.clear();
    return takenDrainedDataStreams_;
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
        if (entry.id == Http2SettingId::kHeaderTableSize &&
            entry.value < encoderDynamicTableSize_) {
            // RFC 9113 §4.3.1 requires the next field block after our SETTINGS ACK
            // to begin with a conformant table-size update. This encoder never uses
            // dynamic entries, so permanently selecting zero is both exact and avoids
            // carrying a fictitious compression capacity through later SETTINGS.
            encoderDynamicTableSize_ = 0;
            encoderTableSizeUpdatePending_ = true;
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
        std::ranges::sort(
            std::span(unprocessedStreamIds).first(unprocessedCount));
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

    if (!consumeFrames(std::string_view(input_), inputOffset_)) {
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
