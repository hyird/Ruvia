#include "Http2Connection.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <utility>

#include "HttpResponseBodyAccess.h"
#include "HttpResponseFileAccess.h"
#include "Http2BodyQueue.h"
#include "Http2BodyState.h"
#include "Http2FlowControl.h"
#include "Http2FrameCodec.h"
#include "Http2FramePayload.h"
#include "Http2HeaderBlock.h"
#include "Http2RequestHeaders.h"
#include "Http2HeaderRules.h"
#include "Http2ResponseHeaders.h"
#include "Http2WebSocketHandshake.h"
#include "Http2WindowUpdate.h"
#include "HttpParserInternal.h"

namespace ruvia::detail {

Http2Connection::Http2Connection(
    std::pmr::memory_resource* resource, Http2CoreConfig config, Http2Role role)
    : resource_(resource),
      config_(config),
      input_(resource),
      outBuffer_(resource),
      streams_(resource),
      decoder_(resource),
      events_(resource),
      pendingSends_(resource),
      unblockedStreams_(resource),
      takenUnblockedStreams_(resource),
      pinnedStreams_(resource),
      localMaxFrameSize_(config.maxFrameSize),
      connectionSendWindow_(static_cast<std::int32_t>(config.initialSendWindow)),
      connectionReceiveWindow_(static_cast<std::int32_t>(config.initialReceiveWindow)),
      role_(role) {
    decoder_.setMaxDynamicTableSize(4096);
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
    }
}

void Http2Connection::takeOutput(std::pmr::string& into) {
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

Http2Event Http2Connection::nextEvent() {
    if (eventOffset_ < events_.size()) {
        return events_[eventOffset_++];
    }
    events_.clear();
    eventOffset_ = 0;
    return {};
}

std::span<const std::uint32_t> Http2Connection::takeUnblockedStreams() noexcept {
    // Swap-and-clear so each unblock is reported exactly once; the returned span stays
    // valid until the next call (double buffer, no allocation churn).
    takenUnblockedStreams_.swap(unblockedStreams_);
    unblockedStreams_.clear();
    return std::span<const std::uint32_t>(takenUnblockedStreams_.data(), takenUnblockedStreams_.size());
}

// =============================================================================
// Frame processing below is a 1:1 port of the retired coroutine session's pure
// logic: inline async_write became append-to-outBuffer_, coroutine resume became
// events / unblockedStreams_ marking. The coroutine stack itself is deleted; this
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
    closing_ = true;
    appendGoawayFrame(lastStreamId_, error, debug);
}

void Http2Connection::beginDrain() {
    // Graceful drain (RFC 9113 §6.8): advertise GOAWAY(NO_ERROR) at the last accepted
    // stream id WITHOUT closing -- streams already started keep running, and HEADERS
    // for a stream above the advertised id are refused in processHeaders.
    if (draining_ || closing_) {
        return;
    }
    draining_ = true;
    goawayLastStreamId_ = lastStreamId_;
    appendGoawayFrame(lastStreamId_, Http2ErrorCode::kNoError, "server draining");
}

bool Http2Connection::applySettingsPayload(std::string_view payload) {
    if (!http2SettingsPayloadSizeValid(payload)) {
        appendGoaway(Http2ErrorCode::kFrameSizeError, "invalid SETTINGS size");
        return false;
    }
    for (std::size_t offset = 0; offset < payload.size(); offset += 6) {
        const auto entry = http2ReadSettingEntry(payload, offset);
        const auto result = peerSettings_.apply(entry.id, entry.value);
        if (result.status != Http2PeerSettingsStatus::kOk) {
            appendGoaway(http2PeerSettingsErrorCode(result.status), http2PeerSettingsErrorMessage(result.status));
            return false;
        }
        if (result.initialWindowChanged &&
            !http2ApplyStreamSendWindowDelta(streams_, result.initialWindowDelta)) {
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
        if (role_ == Http2Role::kClient && !receivedFirstSettings_) {
            // The server preface must be a (possibly empty) non-ACK SETTINGS frame
            // (RFC 9113 §3.4); an ACK alone does not satisfy it.
            appendGoaway(Http2ErrorCode::kProtocolError, "SETTINGS ACK before SETTINGS");
            return false;
        }
        receivedFirstSettings_ = true;
        return true;
    }
    if (!applySettingsPayload(payload)) {
        return false;
    }
    receivedFirstSettings_ = true;
    appendFrame(Http2FrameType::kSettings, kHttp2FlagAck, 0, {});
    // SETTINGS_INITIAL_WINDOW_SIZE may have opened send windows: drain deferred bodies
    // and report the unblocked streams to the owner.
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
    Http2StreamState& stream, std::string_view data, std::size_t offset, bool endStream) {
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
            static_cast<std::uint8_t>(endStream && last ? kHttp2FlagEndStream : 0),
            stream.id(), data.substr(offset, chunk));
        offset += chunk;
    }
    return offset;
}

void Http2Connection::markSendWindowOpened() {
    // Drain buffered response bodies now that the window (may have) opened. A body that
    // fully drains reports its stream as unblocked so the owner pulls the next chunk.
    for (std::size_t i = 0; i < pendingSends_.size();) {
        auto& pending = pendingSends_[i];
        auto* stream = findStream(pending.streamId);
        if (stream == nullptr || stream->isReset()) {
            unblockedStreams_.push_back(pending.streamId);
            pendingSends_.erase(pendingSends_.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        pending.offset = sendDataUpToWindow(
            *stream, std::string_view(pending.bytes.data(), pending.bytes.size()),
            pending.offset, pending.endStream);
        if (pending.offset >= pending.bytes.size()) {
            unblockedStreams_.push_back(pending.streamId);
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
            closeStream(header.streamId, Http2StreamCloseSource::kLocal);
            markSendWindowOpened();
            return true;
        case Http2WindowUpdateResult::kOverflow:
            appendRstStream(header.streamId, Http2ErrorCode::kFlowControlError);
            closeStream(header.streamId, Http2StreamCloseSource::kLocal);
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
    connectionReceiveWindow_ += static_cast<std::int32_t>(debt);
    char buf[kHttp2WindowUpdateFrameBytes];
    http2WriteWindowUpdate(buf, 0, debt);
    outBuffer_.append(buf, sizeof(buf));
}

void Http2Connection::unpinStream(std::uint32_t streamId) {
    pinnedStreams_.erase(
        std::remove(pinnedStreams_.begin(), pinnedStreams_.end(), streamId), pinnedStreams_.end());
    auto* stream = streams_.find(streamId);
    if (stream == nullptr) {
        return;  // never created, or already removed
    }
    flushWindowDebt(*stream);
    readyQueue_.remove(streamId);
    if (!stream->isReset()) {
        // Normal completion (no RST arrived while pinned): remember it as locally closed
        // so any late frame on this id is treated as closed, not idle.
        closedStreams_.remember(streamId, Http2StreamCloseSource::kLocal);
    }
    // An RST that arrived while pinned already recorded closedStreams_ in closeStream.
    streams_.remove(streamId);
}

void Http2Connection::closeStream(std::uint32_t streamId, Http2StreamCloseSource source) {
    auto* stream = streams_.find(streamId);
    readyQueue_.remove(streamId);
    if (stream != nullptr) {
        stream->markClosed(source);
        events_.push_back(Http2Event{Http2Event::Kind::kStreamClosed, streamId, {}});
        if (isPinned(streamId)) {
            // A handler still holds views into this stream's decoded storage. Keep it in
            // the table (so those views stay valid) but mark it reset so later frames are
            // dropped; unpinStream frees it (and flushes its window debt) once the
            // handler finishes. Preserve the close SOURCE (markReset defaults to kLocal,
            // which would clobber a peer RST -- the owner reads closeSource() to tell a
            // legitimate peer abort from a local reject, e.g. keeping a complete
            // response on peer RST_STREAM).
            stream->markReset(source);
        } else {
            flushWindowDebt(*stream);  // return banked receive credit to the connection
            streams_.remove(streamId);
        }
    }
    closedStreams_.remember(streamId, source);
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
    closeStream(header.streamId, Http2StreamCloseSource::kPeer);
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
    const auto dependency = http2Read31(reinterpret_cast<const unsigned char*>(payload.data()));
    if (dependency == header.streamId) {
        // A stream that depends on itself is a protocol error (RFC 9113 §5.3.1). On a
        // live stream that is a stream error -> RST_STREAM. On an IDLE stream (one never
        // opened) we must NOT RST -- an RST_STREAM on an idle stream is itself something
        // the peer MUST treat as a connection error, so we would be provoking a teardown
        // over a deprecated, purely-advisory PRIORITY frame. Ignore it instead.
        if (findStream(header.streamId) != nullptr) {
            appendRstStream(header.streamId, Http2ErrorCode::kProtocolError);
            closeStream(header.streamId, Http2StreamCloseSource::kLocal);
        }
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
    appendFrame(Http2FrameType::kPing, kHttp2FlagAck, 0, payload);  // echo back
    return true;
}

Http2StreamState* Http2Connection::findStream(std::uint32_t streamId) noexcept {
    return streams_.find(streamId);
}

Http2StreamState* Http2Connection::createStream(std::uint32_t streamId) {
    return streams_.create(streamId, peerSettings_.initialWindowSize());
}

bool Http2Connection::isIdleStreamId(std::uint32_t streamId) const noexcept {
    if (role_ == Http2Role::kClient) {
        // No server-initiated streams exist (push is never enabled), so every even id
        // is idle, as is any odd id this endpoint has not opened yet.
        return (streamId & 1U) == 0 || streamId >= nextLocalStreamId_;
    }
    return http2IsIdleStream(streamId, lastStreamId_);
}

void Http2Connection::queueClientPreface() {
    outBuffer_.append(kHttp2ClientPreface.data(), kHttp2ClientPreface.size());
    queueLocalSettings();
}

std::uint32_t Http2Connection::openLocalStream() {
    if (role_ != Http2Role::kClient || closing_ || peerGoaway_ || nextLocalStreamId_ > 0x7fffffffU) {
        return 0;
    }
    auto* stream = createStream(nextLocalStreamId_);
    if (stream == nullptr) {
        return 0;
    }
    const auto streamId = nextLocalStreamId_;
    nextLocalStreamId_ += 2;
    return streamId;
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
    connectionReceiveWindow_ += static_cast<std::int32_t>(debt);
    // Re-advertise the stream window only while the peer can still send on it; a
    // stream-scoped WINDOW_UPDATE on an ended/reset stream can trip a strict peer.
    if (!stream->bodyEnded() && !stream->isReset()) {
        stream->restoreReceiveWindow(static_cast<std::int32_t>(debt));
        char buf[kHttp2WindowUpdateFrameBytes * 2];
        auto* out = http2WriteDataWindowUpdates(buf, streamId, debt);
        outBuffer_.append(buf, static_cast<std::size_t>(out - buf));
    } else {
        char buf[kHttp2WindowUpdateFrameBytes];
        http2WriteWindowUpdate(buf, 0, debt);
        outBuffer_.append(buf, sizeof(buf));
    }
}

bool Http2Connection::hasBlockedSend(std::uint32_t streamId) const noexcept {
    for (const auto& pending : pendingSends_) {
        if (pending.streamId == streamId) {
            return true;
        }
    }
    return false;
}

std::uint32_t Http2Connection::peerMaxConcurrentStreams() const noexcept {
    return peerSettings_.maxConcurrentStreams();
}

void Http2Connection::submitRequestHead(
    std::uint32_t streamId,
    std::string_view method,
    std::string_view scheme,
    std::string_view authority,
    std::string_view path,
    std::span<const HttpHeaderView> headers,
    bool endStream) {
    auto* stream = findStream(streamId);
    if (stream == nullptr || stream->isReset()) {
        return;
    }
    if (const auto parsed = parseMethod(method); parsed != HttpMethod::kUnknown) {
        stream->setRequestMethod(parsed);  // enables the HEAD content-length exemption
    }
    auto& block = stream->responseHeaderBlock();
    block.clear();
    HpackEncoder::encodeHeader(block, ":method", method);
    HpackEncoder::encodeHeader(block, ":scheme", scheme);
    HpackEncoder::encodeHeader(block, ":authority", authority);
    HpackEncoder::encodeHeader(block, ":path", path);
    for (const auto& header : headers) {
        HpackEncoder::encodeHeader(block, header.name(), header.value());
    }
    appendResponseHeaderFrames(
        *stream, std::string_view(block.data(), block.size()), endStream);
    http2ReleaseResponseHeaderBlock(*stream);
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
        if (stream.requestMethod() != HttpMethod::kConnect ||
            !stream.protocolIsWebSocket() ||
            !stream.hasScheme() ||
            !stream.hasPath() ||
            !stream.hasAuthority()) {
            return HeaderDecodeStatus::kProtocolError;
        }
        stream.markExtendedConnectWebSocket();
    } else if (stream.requestMethod() == HttpMethod::kConnect) {
        if (!stream.hasAuthority() || stream.hasScheme() || stream.hasPath()) {
            return HeaderDecodeStatus::kProtocolError;
        }
        stream.markStandardConnect();
    } else if (!stream.hasScheme() || !stream.hasPath()) {
        return HeaderDecodeStatus::kProtocolError;
    }
    if (stream.bufferedBodyExceedsContentLength()) {
        return HeaderDecodeStatus::kProtocolError;
    }
    stream.markHeadersDecoded();
    // NOTE (sans-I/O): resolveStreamRoute is deliberately NOT called here -- route
    // resolution and body-mode selection are ruvia-web/edge policy the owner applies
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
        if (!stream.setContentLength(parsed)) {
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
    if (!stream.bodyLengthComplete()) {
        return HeaderDecodeStatus::kProtocolError;
    }
    stream.markPeerEndStream();
    stream.markBodyEnded();
    events_.push_back(Http2Event{Http2Event::Kind::kMessageEnd, stream.id(), {}});
    return HeaderDecodeStatus::kOk;
}

bool Http2Connection::handleHeaderDecodeFailure(Http2StreamState& stream, HeaderDecodeStatus status) {
    if (status == HeaderDecodeStatus::kCompressionError) {
        appendGoaway(Http2ErrorCode::kCompressionError, "invalid HPACK block");
        return false;
    }
    appendRstStream(stream.id(), Http2ErrorCode::kProtocolError);
    if (findStream(stream.id()) == &stream) {
        closeStream(stream.id(), Http2StreamCloseSource::kLocal);  // emits kStreamClosed
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
    const bool lengthExempt = role_ == Http2Role::kClient &&
        (stream.requestMethod() == HttpMethod::kHead ||
         stream.responseStatus() == 204 || stream.responseStatus() == 304);
    if (stream.peerEndStream() && !lengthExempt && !http2BodyLengthComplete(stream)) {
        appendRstStream(stream.id(), Http2ErrorCode::kProtocolError);
        closeStream(stream.id(), Http2StreamCloseSource::kLocal);  // remove, don't leak the slot
        return;
    }
    events_.push_back(Http2Event{Http2Event::Kind::kMessageHead, stream.id(), {}});
    if (stream.peerEndStream() || stream.standardConnect()) {
        stream.markBodyEnded();
        events_.push_back(Http2Event{Http2Event::Kind::kMessageEnd, stream.id(), {}});
    }
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

    std::uint32_t dependency = 0;
    if (!http2HeadersPriorityDependency(header, payload, dependency)) {
        appendGoaway(Http2ErrorCode::kProtocolError, "invalid HEADERS priority");
        return false;
    }
    if ((header.flags & kHttp2FlagPriority) != 0 && dependency == header.streamId) {
        if (role_ == Http2Role::kServer && header.streamId > lastStreamId_) {
            lastStreamId_ = header.streamId;
        }
        appendRstStream(header.streamId, Http2ErrorCode::kProtocolError);
        if (findStream(header.streamId) != nullptr) {
            closeStream(header.streamId, Http2StreamCloseSource::kLocal);  // owner observes the abort
        } else {
            closedStreams_.remember(header.streamId, Http2StreamCloseSource::kLocal);
        }
        return true;
    }

    Http2StreamState* stream = nullptr;
    bool refusedStream = false;
    // A HEADERS block on a stream WE locally closed: decode it (HPACK sync) but discard
    // the result and RST(STREAM_CLOSED) rather than tearing the connection down.
    bool discardIntoStream = false;
    if (auto* existing = findStream(header.streamId); existing != nullptr) {
        if (existing->headersDecoded() && !existing->isReset()) {
            return processTrailerHeaders(*existing, header, payload);
        }
        if (existing->isReset()) {
            if (existing->closeSource() == Http2StreamCloseSource::kPeer) {
                appendRstStream(header.streamId, Http2ErrorCode::kStreamClosed);
                return true;
            }
            // Locally closed (we RST'd on a body cap / content-length mismatch, or the
            // handler finished): the peer's in-flight HEADERS/trailers are legal (RFC
            // 9113 §5.1 -- a peer may send frames before observing our RST). Do NOT
            // GOAWAY the whole connection; decode-and-discard the block into this reset
            // stream's buffer to keep the connection-global HPACK table in sync, then
            // RST(STREAM_CLOSED). Handled by the discardIntoStream path below.
            discardIntoStream = true;
            stream = existing;
        }
        if (role_ == Http2Role::kClient) {
            // A 1xx interim head was decoded on this stream; this block is the next
            // (possibly final) response head -- decode it through the shared tail.
            stream = existing;
        } else {
            appendRstStream(header.streamId, Http2ErrorCode::kProtocolError);
            closeStream(header.streamId, Http2StreamCloseSource::kLocal);  // remove, don't leak
            return true;
        }
    } else if (role_ == Http2Role::kClient) {
        if (isIdleStreamId(header.streamId)) {
            appendGoaway(Http2ErrorCode::kProtocolError, "HEADERS on idle stream");
            return false;
        }
        // A head for a stream this endpoint already closed (e.g. cancelled): decode
        // and discard to keep HPACK in sync; the close was already signalled, so no
        // RST_STREAM is added on top.
        refusedStream = true;
        refusedHeaderStream_.emplace(header.streamId, resource_);
        stream = &*refusedHeaderStream_;
    } else {
        if (header.streamId <= lastStreamId_) {
            const auto source = closedStreams_.source(header.streamId);
            if (source == Http2StreamCloseSource::kPeer) {
                appendRstStream(header.streamId, Http2ErrorCode::kStreamClosed);
                return true;
            }
            if (source == Http2StreamCloseSource::kLocal) {
                // We locally closed this stream (early response, then the peer's
                // trailers are still in flight): decode-and-discard through the refused
                // path to keep HPACK synced, then RST(STREAM_CLOSED) -- NOT GOAWAY,
                // which would kill every other multiplexed stream in a benign race.
                refusedStream = true;
                refusedHeaderStream_.emplace(header.streamId, resource_);
                stream = &*refusedHeaderStream_;
            } else {
                appendGoaway(Http2ErrorCode::kProtocolError, "new stream id lower than previous");
                return false;
            }
        }

        // A genuinely-new stream (not the locally-closed decode-and-discard case above,
        // which already resolved `stream`). While draining, a stream above the id we
        // advertised in GOAWAY must not be processed (RFC 9113 §6.8); route it through
        // the refused-stream path so its block is still decoded (HPACK sync) then RST'd.
        if (stream == nullptr) {
            const bool drainRefused = draining_ && header.streamId > goawayLastStreamId_;
            stream = drainRefused ? nullptr : createStream(header.streamId);
            lastStreamId_ = header.streamId;
            refusedStream = stream == nullptr;
            if (refusedStream) {
                refusedHeaderStream_.emplace(header.streamId, resource_);
                stream = &*refusedHeaderStream_;
            }
        }
    }
    if (discardIntoStream) {
        // A HEADERS block on the in-table stream we locally reset (case a): decode it
        // for HPACK consistency, discard the result, and RST(STREAM_CLOSED). A
        // multi-frame block into a still-pinned reset stream would need continuation-
        // mode tracking on the live stream, which we do not have -- fall back to the
        // (safe) connection close only for that rare race; the common single-frame
        // trailer survives.
        if ((header.flags & kHttp2FlagEndHeaders) == 0) {
            appendGoaway(Http2ErrorCode::kStreamClosed, "multi-frame HEADERS on closed stream");
            return false;
        }
        std::string_view fragment;
        if (!http2DecodeHeadersPayload(header, payload, fragment)) {
            appendGoaway(Http2ErrorCode::kProtocolError, "invalid HEADERS padding");
            return false;
        }
        if (!http2StartHeaderBlock(*stream, fragment)) {
            appendGoaway(Http2ErrorCode::kEnhanceYourCalm, "header block too large");
            return false;
        }
        if (decodeRefusedHeaderBlock(*stream) == HeaderDecodeStatus::kCompressionError) {
            appendGoaway(Http2ErrorCode::kCompressionError, "invalid HPACK block");
            return false;
        }
        appendRstStream(header.streamId, Http2ErrorCode::kStreamClosed);
        return true;
    }

    if ((header.flags & kHttp2FlagEndStream) != 0) {
        stream->markPeerEndStream();
    }

    std::string_view fragment;
    if (!http2DecodeHeadersPayload(header, payload, fragment)) {
        appendGoaway(Http2ErrorCode::kProtocolError, "invalid HEADERS padding");
        return false;
    }
    if (!http2StartHeaderBlock(*stream, fragment)) {
        // The block exceeds the header buffer cap. We cannot decode a block we could
        // not fully buffer, and skipping it would desync the connection-global HPACK
        // dynamic table for every later block (RFC 9113 §4.3) -- so this is a
        // CONNECTION error, not a survivable stream reset.
        appendGoaway(Http2ErrorCode::kEnhanceYourCalm, "header block too large");
        return false;
    }

    if ((header.flags & kHttp2FlagEndHeaders) != 0) {
        const auto status = refusedStream ? decodeRefusedHeaderBlock(*stream) : decodeInitialHeaderBlock(*stream);
        if (status != HeaderDecodeStatus::kOk) {
            // Handle the failure BEFORE dropping the refused-stream storage `stream`
            // may point into (using it after reset() would be use-after-destroy).
            const bool keepConnection = handleHeaderDecodeFailure(*stream, status);
            if (refusedStream) {
                refusedHeaderStream_.reset();
            }
            return keepConnection;
        }
        if (refusedStream) {
            if (role_ == Http2Role::kServer) {
                appendRstStream(header.streamId, Http2ErrorCode::kRefusedStream);
                closedStreams_.remember(header.streamId, Http2StreamCloseSource::kLocal);
            }
            refusedHeaderStream_.reset();
        } else if (stream->headersDecoded()) {
            emitRequestHeaders(*stream);  // not yet decoded = a 1xx interim head (client)
        }
    } else {
        headerContinuation_.start(stream->id(), false);
    }
    return true;
}

bool Http2Connection::processTrailerHeaders(
    Http2StreamState& stream, const Http2FrameHeader& header, std::string_view payload) {
    if (stream.bodyEnded()) {
        appendRstStream(stream.id(), Http2ErrorCode::kStreamClosed);
        closeStream(stream.id(), Http2StreamCloseSource::kLocal);
        return true;
    }
    if ((header.flags & kHttp2FlagEndStream) == 0) {
        appendRstStream(stream.id(), Http2ErrorCode::kProtocolError);
        closeStream(stream.id(), Http2StreamCloseSource::kLocal);
        return true;
    }

    std::string_view fragment;
    if (!http2DecodeHeadersPayload(header, payload, fragment)) {
        appendGoaway(Http2ErrorCode::kProtocolError, "invalid trailer padding");
        return false;
    }
    if (!http2StartHeaderBlock(stream, fragment)) {
        // Un-bufferable trailer block: same HPACK-consistency reasoning as HEADERS --
        // connection error rather than a survivable stream reset.
        appendGoaway(Http2ErrorCode::kEnhanceYourCalm, "trailer block too large");
        return false;
    }

    if ((header.flags & kHttp2FlagEndHeaders) != 0) {
        const auto status = finishTrailerBlock(stream);
        if (status != HeaderDecodeStatus::kOk) {
            return handleHeaderDecodeFailure(stream, status);
        }
    } else {
        headerContinuation_.start(stream.id(), true);
    }
    return true;
}

bool Http2Connection::processContinuation(const Http2FrameHeader& header, std::string_view payload) {
    if (!headerContinuation_.matches(header.streamId)) {
        appendGoaway(Http2ErrorCode::kProtocolError, "invalid CONTINUATION");
        return false;
    }
    auto* stream = findStream(header.streamId);
    if (stream == nullptr) {
        if (!refusedHeaderStream_ || refusedHeaderStream_->id() != header.streamId) {
            appendGoaway(Http2ErrorCode::kProtocolError, "missing CONTINUATION stream");
            return false;
        }
        stream = &*refusedHeaderStream_;
    }
    if (!http2AppendHeaderBlock(*stream, payload)) {
        // The accumulated HEADERS+CONTINUATION block overflowed the buffer cap; the
        // partial block cannot be decoded, so skipping it would desync HPACK for the
        // whole connection -- a CONNECTION error (RFC 9113 §4.3), not a stream reset.
        appendGoaway(Http2ErrorCode::kEnhanceYourCalm, "header block too large");
        return false;
    }
    if ((header.flags & kHttp2FlagEndHeaders) != 0) {
        const bool trailers = headerContinuation_.finishWasTrailers();
        if (trailers) {
            const auto status = finishTrailerBlock(*stream);
            if (status != HeaderDecodeStatus::kOk) {
                return handleHeaderDecodeFailure(*stream, status);
            }
        } else {
            const bool refusedStream = refusedHeaderStream_ && refusedHeaderStream_->id() == stream->id();
            const auto status = refusedStream ? decodeRefusedHeaderBlock(*stream) : decodeInitialHeaderBlock(*stream);
            if (status != HeaderDecodeStatus::kOk) {
                const bool keepConnection = handleHeaderDecodeFailure(*stream, status);
                if (refusedStream) {
                    refusedHeaderStream_.reset();
                }
                return keepConnection;
            }
            if (refusedStream) {
                const auto refusedId = stream->id();
                if (role_ == Http2Role::kServer) {
                    appendRstStream(refusedId, Http2ErrorCode::kRefusedStream);
                    closedStreams_.remember(refusedId, Http2StreamCloseSource::kLocal);
                }
                refusedHeaderStream_.reset();
            } else if (stream->headersDecoded()) {
                emitRequestHeaders(*stream);
            }
        }
    }
    return true;
}

void Http2Connection::dropDataFrame(std::size_t flowBytes, bool windowConsumed) {
    // RFC 9113 §6.9.1: DATA counts against the peer's connection send window even when
    // the frame is in error. Keeping the connection means we must return that credit or
    // the peer's window drains to zero and stalls. Only the connection window is
    // re-advertised; the stream is being abandoned.
    if (windowConsumed) {
        connectionReceiveWindow_ += static_cast<std::int32_t>(flowBytes);
    }
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
    auto* stream = findStream(header.streamId);
    if (stream == nullptr) {
        if (!isIdleStreamId(header.streamId)) {
            appendRstStream(header.streamId, Http2ErrorCode::kStreamClosed);
            dropDataFrame(payload.size(), /*windowConsumed=*/false);
            return true;
        }
        appendGoaway(Http2ErrorCode::kProtocolError, "DATA before HEADERS");
        return false;
    }
    if (stream->isReset()) {
        dropDataFrame(payload.size(), /*windowConsumed=*/false);
        return true;
    }
    if (!stream->headersDecoded()) {
        appendRstStream(header.streamId, Http2ErrorCode::kProtocolError);
        closeStream(header.streamId, Http2StreamCloseSource::kLocal);
        dropDataFrame(payload.size(), /*windowConsumed=*/false);
        return true;
    }
    if (stream->bodyEnded()) {
        appendRstStream(header.streamId, Http2ErrorCode::kStreamClosed);
        closeStream(header.streamId, Http2StreamCloseSource::kLocal);
        dropDataFrame(payload.size(), /*windowConsumed=*/false);
        return true;
    }

    const auto flowBytes = static_cast<std::int32_t>(payload.size());
    switch (http2ConsumeReceiveWindows(connectionReceiveWindow_, *stream, flowBytes)) {
        case Http2ReceiveWindowResult::kOk:
            break;
        case Http2ReceiveWindowResult::kConnectionExceeded:
            appendGoaway(Http2ErrorCode::kFlowControlError, "connection flow-control window exceeded");
            return false;
        case Http2ReceiveWindowResult::kStreamExceeded:
            // The stream debit was rejected, so the connection window was not touched:
            // credit the peer back without restoring it locally.
            appendRstStream(header.streamId, Http2ErrorCode::kFlowControlError);
            closeStream(header.streamId, Http2StreamCloseSource::kLocal);
            dropDataFrame(payload.size(), /*windowConsumed=*/false);
            return true;
    }

    std::string_view data;
    if (!http2DecodeDataPayload(header, payload, data)) {
        appendGoaway(Http2ErrorCode::kProtocolError, "invalid DATA padding");
        return false;
    }

    switch (http2AccountDataBody(
        *stream, data.size(), config_.maxStreamBodyBytes, config_.maxBufferedBodyBytes)) {
        case Http2BodyAccountingResult::kOk:
            break;
        case Http2BodyAccountingResult::kTooLarge:
            appendRstStream(header.streamId, Http2ErrorCode::kCancel);
            closeStream(header.streamId, Http2StreamCloseSource::kLocal);
            dropDataFrame(payload.size(), /*windowConsumed=*/true);
            return true;
        case Http2BodyAccountingResult::kContentLengthExceeded:
            appendRstStream(header.streamId, Http2ErrorCode::kProtocolError);
            closeStream(header.streamId, Http2StreamCloseSource::kLocal);
            dropDataFrame(payload.size(), /*windowConsumed=*/true);
            return true;
    }
    if (flowBytes > 0) {
        if (stream->deferWindowRelease()) {
            // Streaming consumer: bank the credit; releaseStreamWindow() re-advertises
            // it as the owner drains, so a slow reader stalls the peer (backpressure)
            // instead of growing the buffered response without bound.
            stream->addWindowDebt(static_cast<std::uint32_t>(flowBytes));
        } else {
            const auto increment = static_cast<std::uint32_t>(flowBytes);
            http2RestoreReceiveWindows(connectionReceiveWindow_, *stream, flowBytes);
            char buf[kHttp2WindowUpdateFrameBytes * 2];
            auto* out = http2WriteDataWindowUpdates(buf, header.streamId, increment);
            outBuffer_.append(buf, static_cast<std::size_t>(out - buf));
        }
    }

    // sans-I/O: hand the body to the owner as an event; the core does not buffer it
    // (buffered vs streaming delivery is web/edge policy). Content-length and size caps
    // were already enforced by http2AccountDataBody above. The view is valid until the
    // next feed (input_ is only reclaimed at the start of the following feed).
    events_.push_back(Http2Event{Http2Event::Kind::kMessageBodyChunk, header.streamId, data});
    if ((header.flags & kHttp2FlagEndStream) != 0) {
        if (!http2BodyLengthComplete(*stream)) {
            appendRstStream(header.streamId, Http2ErrorCode::kProtocolError);
            closeStream(header.streamId, Http2StreamCloseSource::kLocal);
            return true;
        }
        http2MarkBodyEnded(*stream);
        events_.push_back(Http2Event{Http2Event::Kind::kMessageEnd, header.streamId, {}});
    }
    return true;
}

bool Http2Connection::processFrame(const Http2FrameHeader& header, std::string_view payload) {
    if (!receivedFirstSettings_ &&
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
        case Http2FrameType::kGoaway: {
            if (header.streamId != 0 || payload.size() < 8) {
                appendGoaway(Http2ErrorCode::kProtocolError, "malformed GOAWAY");
                return false;
            }
            peerGoaway_ = true;
            const auto lastProcessed = http2Read31(
                reinterpret_cast<const unsigned char*>(payload.data()));
            // The owner reacts (client: fail streams above lastProcessed, stop opening
            // new ones; streams at or below it keep running to completion).
            events_.push_back(Http2Event{Http2Event::Kind::kGoaway, lastProcessed, {}});
            if (role_ == Http2Role::kServer) {
                closing_ = true;  // a departing client: drain and stop reading
            }
            return true;
        }
        case Http2FrameType::kPushPromise:
            // Server: clients can never push. Client: we advertise ENABLE_PUSH=0.
            appendGoaway(Http2ErrorCode::kProtocolError, "unexpected PUSH_PROMISE");
            return false;
        default:
            return true;  // RFC 9113 §4.1: unknown frame types MUST be ignored
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
        if (closing_) {
            break;
        }
    }
    return true;
}

Http2FeedResult Http2Connection::feed(std::string_view in) {
    // Reclaim the prefix consumed by the PREVIOUS feed now, at the start of this one --
    // not at the end of that feed. A kMessageBodyChunk event carries a view INTO input_
    // (or into the caller's `in` on the fast path), and reclaiming shifts the buffer;
    // deferring the reclaim keeps those views valid until this next feed, matching the
    // documented contract. The prior feed's events reference now-stale bytes, so reset
    // the event queue too (the owner must drain events after each feed).
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
    if (input_.empty() && !awaitingClientPreface_) {
        std::size_t offset = 0;
        if (!consumeFrames(in, offset)) {
            return {in.size(), Http2FeedStatus::kError};
        }
        if (offset < in.size()) {
            input_.append(in.data() + offset, in.size() - offset);  // partial-frame tail
        }
        return {in.size(), Http2FeedStatus::kOk};
    }

    // SLOW PATH: a buffered partial-frame tail and/or the connection preface is pending.
    // Buffer all fed bytes (nghttp2_session_mem_recv semantics) then consume frames.
    input_.append(in.data(), in.size());

    // Server mode: the 24-byte client connection preface precedes the first frame
    // (RFC 9113 §3.4). Consume + validate it before any frame parsing.
    if (awaitingClientPreface_) {
        constexpr std::string_view kClientPreface = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
        if (input_.size() - inputOffset_ < kClientPreface.size()) {
            return {in.size(), Http2FeedStatus::kNeedMore};  // wait for the full preface
        }
        if (std::string_view(input_.data() + inputOffset_, kClientPreface.size()) != kClientPreface) {
            appendGoaway(Http2ErrorCode::kProtocolError, "invalid connection preface");
            return {in.size(), Http2FeedStatus::kError};
        }
        inputOffset_ += kClientPreface.size();
        awaitingClientPreface_ = false;
    }

    if (!consumeFrames(std::string_view(input_.data(), input_.size()), inputOffset_)) {
        return {in.size(), Http2FeedStatus::kError};
    }
    // NOTE: the consumed prefix is reclaimed at the START of the next feed (see above),
    // so body-chunk views handed out via events stay valid until then.
    return {in.size(), Http2FeedStatus::kOk};
}

Http2StreamState* Http2Connection::stream(std::uint32_t streamId) noexcept {
    return streams_.find(streamId);
}

void Http2Connection::appendResponseHeaderFrames(
    Http2StreamState& stream, std::string_view headerBlock, bool endStream) {
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
            (last ? kHttp2FlagEndHeaders : 0) | (endStream && last ? kHttp2FlagEndStream : 0));
        appendFrame(
            first ? Http2FrameType::kHeaders : Http2FrameType::kContinuation,
            flags, stream.id(), headerBlock.substr(offset, chunk));
        offset += chunk;
        first = false;
    }
}

void Http2Connection::submitResponseHead(
    std::uint32_t streamId, const HttpResponse& response, bool bodyForbidden) {
    auto* stream = findStream(streamId);
    if (stream == nullptr || stream->isReset()) {
        return;
    }
    const auto policy = responseWritePolicy(response.status());
    const bool bodyAllowed = policy.bodyAllowed();
    const bool sendBody = bodyAllowed && !bodyForbidden;
    const bool streamBody = responseHasStreamBody(response);

    // Mirror writeResponse's framing decision: the writer owns an auto Content-Length
    // only for a buffered (non-streaming) body; a streaming body sends no length.
    std::uint64_t contentLength = 0;
    if (bodyAllowed && !streamBody) {
        contentLength = responseHasFileBody(response)
            ? responseFileBody(response).length
            : responseBodySize(response);
    }
    appendHttp2ResponseHeaders(*stream, response, contentLength, !streamBody);
    const bool endStream = !sendBody || (!streamBody && contentLength == 0);
    appendResponseHeaderFrames(
        *stream,
        std::string_view(stream->responseHeaderBlock().data(), stream->responseHeaderBlock().size()),
        endStream);
    http2ReleaseResponseHeaderBlock(*stream);
}

void Http2Connection::submitStreamingResponseHead(
    std::uint32_t streamId, const HttpResponse& head, bool bodyForbidden) {
    auto* stream = findStream(streamId);
    if (stream == nullptr || stream->isReset()) {
        return;
    }
    // No auto Content-Length for a streaming body (length unknown); mirror the coroutine
    // sink's commit(). END_STREAM only when the status/method forbids a body.
    appendHttp2ResponseHeaders(*stream, head, 0, /*emitAutoContentLength=*/false);
    appendResponseHeaderFrames(
        *stream,
        std::string_view(stream->responseHeaderBlock().data(), stream->responseHeaderBlock().size()),
        bodyForbidden);
    http2ReleaseResponseHeaderBlock(*stream);
}

Http2SubmitResult Http2Connection::submitData(
    std::uint32_t streamId, std::string_view chunk, bool endStream) {
    auto* stream = findStream(streamId);
    if (stream == nullptr || stream->isReset()) {
        return Http2SubmitResult::kClosed;
    }
    // If this stream still has a window-blocked body queued, appending preserves DATA
    // ordering; it drains (with the accumulated END_STREAM) when the window reopens.
    for (auto& pending : pendingSends_) {
        if (pending.streamId == streamId) {
            pending.bytes.append(chunk.data(), chunk.size());
            if (endStream) {
                pending.endStream = true;
            }
            return Http2SubmitResult::kBlocked;
        }
    }
    if (chunk.empty()) {
        if (endStream) {
            appendFrame(Http2FrameType::kData, kHttp2FlagEndStream, streamId, {});
        }
        return Http2SubmitResult::kOk;
    }
    const auto consumed = sendDataUpToWindow(*stream, chunk, 0, endStream);
    if (consumed < chunk.size()) {
        std::pmr::string remainder(resource_);
        remainder.append(chunk.data() + consumed, chunk.size() - consumed);
        pendingSends_.push_back(Http2PendingSend{streamId, std::move(remainder), 0, endStream});
        return Http2SubmitResult::kBlocked;
    }
    return Http2SubmitResult::kOk;
}

void Http2Connection::submitWebSocketHandshake(
    std::uint32_t streamId, std::string_view subprotocol, std::string_view extensions) {
    auto* stream = findStream(streamId);
    if (stream == nullptr || stream->isReset()) {
        return;
    }
    http2EncodeWebSocketHandshakeHeaders(stream->responseHeaderBlock(), subprotocol, extensions);
    appendResponseHeaderFrames(
        *stream,
        std::string_view(stream->responseHeaderBlock().data(), stream->responseHeaderBlock().size()),
        /*endStream=*/false);
    http2ReleaseResponseHeaderBlock(*stream);
}

void Http2Connection::submitTrailers(std::uint32_t streamId, std::string_view headerBlock) {
    auto* stream = findStream(streamId);
    if (stream == nullptr || stream->isReset() || headerBlock.empty()) {
        return;
    }
    appendResponseHeaderFrames(*stream, headerBlock, /*endStream=*/true);
}

void Http2Connection::submitReset(std::uint32_t streamId, std::uint32_t errorCode) {
    appendRstStream(streamId, static_cast<Http2ErrorCode>(errorCode));
    if (auto* stream = findStream(streamId); stream != nullptr) {
        stream->markReset();
    }
    // Drop any buffered body for the aborted stream.
    pendingSends_.erase(
        std::remove_if(
            pendingSends_.begin(), pendingSends_.end(),
            [streamId](const Http2PendingSend& p) { return p.streamId == streamId; }),
        pendingSends_.end());
}

void Http2Connection::beginGoaway(std::uint32_t errorCode) {
    appendGoaway(static_cast<Http2ErrorCode>(errorCode));
    closing_ = true;
}

bool Http2Connection::seedUpgradedStream(const HttpServerParseResult& parsed, std::string_view body) {
    // 1:1 port of the coroutine session's seedUpgradedStream, with queueReady replaced
    // by emitRequestHeaders (the owner dispatches off the events).
    auto* stream = createStream(1);
    if (stream == nullptr) {
        return false;
    }
    lastStreamId_ = 1;
    stream->setRequestMethod(parsed.request.method());
    stream->assignRequestPath(parsed.request.target());
    const auto host = requestKnownHeader(parsed.request, RequestKnownHeader::kHost);
    if (!host.empty()) {
        stream->assignRequestAuthority(host);
        stream->markAuthority();
        stream->markHost();
    }
    stream->markMethod();
    stream->markScheme(80);
    stream->markPath();
    if (!parsed.chunked && parsed.contentLength != 0) {
        if (body.size() != parsed.contentLength) {
            return false;
        }
        if (!stream->setContentLength(parsed.contentLength)) {
            return false;
        }
    }
    if (!body.empty()) {
        stream->assignRequestBody(body);
        stream->setReceivedBodyBytes(body.size());
    }
    for (const auto& header : parsed.request.headers()) {
        if (http2IsForbiddenUpgradedRequestHeader(header.name())) {
            continue;
        }
        if (!stream->appendRequestHeader(
            header.name(),
            header.value(),
            classifyRequestHeader(header.name()))) {
            return false;
        }
    }
    stream->markHeadersDecoded();
    stream->markPeerEndStream();
    stream->markBodyEnded();
    emitRequestHeaders(*stream);
    return true;
}

bool Http2Connection::beginUpgraded(
    const HttpServerParseResult& parsed, std::string_view settingsPayload, std::string_view body) {
    // h2c upgrade (RFC 7540 §3.2), mirroring the coroutine runUpgraded: apply the
    // HTTP2-Settings payload as the peer's initial SETTINGS (the real first SETTINGS
    // still arrives after the preface, so receivedFirstSettings_ stays false), seed
    // stream 1 from the upgraded h1 request, answer with our SETTINGS + a SETTINGS
    // ACK for the upgrade payload, and expect the client connection preface next.
    if (!applySettingsPayload(settingsPayload)) {
        return false;  // GOAWAY queued, closing_ set by appendGoaway
    }
    if (!seedUpgradedStream(parsed, body)) {
        appendGoaway(Http2ErrorCode::kProtocolError, "invalid upgraded request");
        return false;
    }
    queueLocalSettings();
    // Deliberately NO SETTINGS ACK for the HTTP2-Settings payload: RFC 7540 §3.2.1 --
    // the 101 response is the implicit acknowledgement. An unsolicited ACK breaks
    // clients that did not register those settings as ACK-pending (e.g. curl/nghttp2
    // fails the whole connection on an ACK it never expected). The coroutine session
    // sent one; that was a latent bug.
    expectClientPreface();
    return true;
}

void Http2Connection::queueLocalSettings() {
    // 1:1 port of Http2ServerSession::sendLocalSettings, with the socket write
    // replaced by an append to the outbound buffer. The frame encoders are pure.
    std::array<char, kHttp2LocalSettingsFrameBytes + kHttp2WindowUpdateFrameBytes> buffer;
    auto* out = http2WriteLocalSettingsFrame(buffer.data());
    if constexpr (kHttp2LocalInitialWindowSize > kHttp2DefaultInitialWindowSize) {
        out = http2WriteWindowUpdate(
            out,
            0,
            kHttp2LocalInitialWindowSize - static_cast<std::uint32_t>(kHttp2DefaultInitialWindowSize));
    }
    outBuffer_.append(buffer.data(), static_cast<std::size_t>(out - buffer.data()));
}

}  // namespace ruvia::detail
