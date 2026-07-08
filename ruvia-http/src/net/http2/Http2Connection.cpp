#include "Http2Connection.h"

#include <array>
#include <utility>

#include "Http2BodyState.h"
#include "Http2FlowControl.h"
#include "Http2FrameCodec.h"
#include "Http2FramePayload.h"
#include "Http2HeaderBlock.h"
#include "Http2RequestHeaders.h"
#include "Http2WindowUpdate.h"

namespace ruvia::detail {

Http2Connection::Http2Connection(std::pmr::memory_resource* resource, Http2CoreConfig config)
    : resource_(resource),
      config_(config),
      input_(resource),
      outBuffer_(resource),
      streams_(resource),
      decoder_(resource),
      events_(resource),
      blockedStreams_(resource),
      unblockedStreams_(resource),
      localMaxFrameSize_(config.maxFrameSize),
      connectionSendWindow_(static_cast<std::int32_t>(config.initialSendWindow)),
      connectionReceiveWindow_(static_cast<std::int32_t>(config.initialReceiveWindow)) {
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
    return std::span<const std::uint32_t>(unblockedStreams_.data(), unblockedStreams_.size());
}

// =============================================================================
// TODO(sans-io phase 2): the following are ported incrementally from the pure
// logic currently embedded in the Http2ServerSession*.inl coroutine loop. Each
// keeps the protocol logic 1:1 and replaces (a) inline async_write -> append to
// outBuffer_, (b) coroutine resume -> event / unblockedStreams_ marking.
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

void Http2Connection::appendGoaway(Http2ErrorCode error, std::string_view debug) {
    std::array<char, 8> payload;
    auto* out = http2WriteGoawayPayload(payload.data(), lastStreamId_, error);
    closing_ = true;
    appendFrame(
        Http2FrameType::kGoaway, 0, 0,
        std::string_view(payload.data(), static_cast<std::size_t>(out - payload.data())), debug);
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
        receivedFirstSettings_ = true;
        return true;
    }
    if (!applySettingsPayload(payload)) {
        return false;
    }
    receivedFirstSettings_ = true;
    appendFrame(Http2FrameType::kSettings, kHttp2FlagAck, 0, {});
    // TODO(next): flow-control window may have opened -> mark unblocked streams.
    return true;
}

void Http2Connection::appendRstStream(std::uint32_t streamId, Http2ErrorCode error) {
    std::array<char, 4> payload;
    auto* out = http2Write32(payload.data(), static_cast<std::uint32_t>(error));
    appendFrame(
        Http2FrameType::kRstStream, 0, streamId,
        std::string_view(payload.data(), static_cast<std::size_t>(out - payload.data())));
}

void Http2Connection::markSendWindowOpened() noexcept {
    for (const auto id : blockedStreams_) {
        unblockedStreams_.push_back(id);
    }
    blockedStreams_.clear();
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
        if (http2IsIdleStream(header.streamId, lastStreamId_)) {
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
            stream->markReset();
            markSendWindowOpened();
            return true;
        case Http2WindowUpdateResult::kOverflow:
            appendRstStream(header.streamId, Http2ErrorCode::kFlowControlError);
            stream->markReset();
            markSendWindowOpened();
            return true;
    }
    return true;
}

void Http2Connection::closeStream(std::uint32_t streamId, Http2StreamCloseSource source) {
    auto* stream = streams_.find(streamId);
    readyQueue_.remove(streamId);
    if (stream != nullptr) {
        stream->markClosed(source);
        events_.push_back(Http2Event{Http2Event::Kind::kStreamClosed, streamId, {}});
        streams_.remove(streamId);
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
        http2IsIdleStream(header.streamId, lastStreamId_)) {
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
        // A stream that depends on itself is a protocol error on that stream.
        appendRstStream(header.streamId, Http2ErrorCode::kProtocolError);
        closeStream(header.streamId, Http2StreamCloseSource::kLocal);
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
    // after pulling the kRequestHeaders event.
    return HeaderDecodeStatus::kOk;
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
    events_.push_back(Http2Event{Http2Event::Kind::kRequestEnd, stream.id(), {}});
    return HeaderDecodeStatus::kOk;
}

bool Http2Connection::handleHeaderDecodeFailure(Http2StreamState& stream, HeaderDecodeStatus status) {
    if (status == HeaderDecodeStatus::kCompressionError) {
        appendGoaway(Http2ErrorCode::kCompressionError, "invalid HPACK block");
        return false;
    }
    appendRstStream(stream.id(), Http2ErrorCode::kProtocolError);
    stream.markReset();
    return true;
}

void Http2Connection::emitRequestHeaders(Http2StreamState& stream) {
    // RFC 9113 §8.1.1: a declared content-length must equal the summed DATA payload.
    // A body-less HEADERS (END_STREAM set) with a nonzero content-length can never be
    // satisfied -- reject it here so both END_STREAM routes stay consistent.
    if (stream.peerEndStream() && !http2BodyLengthComplete(stream)) {
        appendRstStream(stream.id(), Http2ErrorCode::kProtocolError);
        stream.markReset();
        return;
    }
    events_.push_back(Http2Event{Http2Event::Kind::kRequestHeaders, stream.id(), {}});
    if (stream.peerEndStream() || stream.standardConnect()) {
        stream.markBodyEnded();
        events_.push_back(Http2Event{Http2Event::Kind::kRequestEnd, stream.id(), {}});
    }
}

bool Http2Connection::processHeaders(const Http2FrameHeader& header, std::string_view payload) {
    if (header.streamId == 0) {
        appendGoaway(Http2ErrorCode::kProtocolError, "HEADERS stream id must be nonzero");
        return false;
    }
    if ((header.streamId & 1U) == 0) {
        appendGoaway(Http2ErrorCode::kProtocolError, "invalid client stream id");
        return false;
    }

    std::uint32_t dependency = 0;
    if (!http2HeadersPriorityDependency(header, payload, dependency)) {
        appendGoaway(Http2ErrorCode::kProtocolError, "invalid HEADERS priority");
        return false;
    }
    if ((header.flags & kHttp2FlagPriority) != 0 && dependency == header.streamId) {
        if (header.streamId > lastStreamId_) {
            lastStreamId_ = header.streamId;
        }
        appendRstStream(header.streamId, Http2ErrorCode::kProtocolError);
        closedStreams_.remember(header.streamId, Http2StreamCloseSource::kLocal);
        return true;
    }

    if (auto* existing = findStream(header.streamId); existing != nullptr) {
        if (existing->headersDecoded() && !existing->isReset()) {
            return processTrailerHeaders(*existing, header, payload);
        }
        if (existing->isReset()) {
            if (existing->closeSource() == Http2StreamCloseSource::kPeer) {
                appendRstStream(header.streamId, Http2ErrorCode::kStreamClosed);
                return true;
            }
            appendGoaway(Http2ErrorCode::kStreamClosed, "HEADERS on closed stream");
            return false;
        }
        appendRstStream(header.streamId, Http2ErrorCode::kProtocolError);
        existing->markReset();
        return true;
    }
    if (header.streamId <= lastStreamId_) {
        const auto source = closedStreams_.source(header.streamId);
        if (source == Http2StreamCloseSource::kPeer) {
            appendRstStream(header.streamId, Http2ErrorCode::kStreamClosed);
            return true;
        }
        if (source == Http2StreamCloseSource::kLocal) {
            appendGoaway(Http2ErrorCode::kStreamClosed, "HEADERS on closed stream");
            return false;
        }
        appendGoaway(Http2ErrorCode::kProtocolError, "new stream id lower than previous");
        return false;
    }

    // TODO(sans-io): graceful-drain refused-stream handling depends on beginGoaway
    // wiring (draining_/goawayLastStreamId_), ported when the submit path lands.
    auto* stream = createStream(header.streamId);
    lastStreamId_ = header.streamId;
    const bool refusedStream = stream == nullptr;
    if (refusedStream) {
        refusedHeaderStream_.emplace(header.streamId, resource_);
        stream = &*refusedHeaderStream_;
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
        appendRstStream(stream->id(), Http2ErrorCode::kEnhanceYourCalm);
        stream->markReset();
        return true;
    }

    if ((header.flags & kHttp2FlagEndHeaders) != 0) {
        const auto status = refusedStream ? decodeRefusedHeaderBlock(*stream) : decodeHeaderBlock(*stream);
        if (status != HeaderDecodeStatus::kOk) {
            if (refusedStream) {
                refusedHeaderStream_.reset();
            }
            return handleHeaderDecodeFailure(*stream, status);
        }
        if (refusedStream) {
            appendRstStream(header.streamId, Http2ErrorCode::kRefusedStream);
            closedStreams_.remember(header.streamId, Http2StreamCloseSource::kLocal);
            refusedHeaderStream_.reset();
        } else {
            emitRequestHeaders(*stream);
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
        stream.markReset();
        return true;
    }
    if ((header.flags & kHttp2FlagEndStream) == 0) {
        appendRstStream(stream.id(), Http2ErrorCode::kProtocolError);
        stream.markReset();
        return true;
    }

    std::string_view fragment;
    if (!http2DecodeHeadersPayload(header, payload, fragment)) {
        appendGoaway(Http2ErrorCode::kProtocolError, "invalid trailer padding");
        return false;
    }
    if (!http2StartHeaderBlock(stream, fragment)) {
        appendRstStream(stream.id(), Http2ErrorCode::kEnhanceYourCalm);
        stream.markReset();
        return true;
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
        appendRstStream(stream->id(), Http2ErrorCode::kEnhanceYourCalm);
        stream->markReset();
        headerContinuation_.reset();
        if (refusedHeaderStream_ && refusedHeaderStream_->id() == stream->id()) {
            refusedHeaderStream_.reset();
        }
        return true;
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
            const auto status = refusedStream ? decodeRefusedHeaderBlock(*stream) : decodeHeaderBlock(*stream);
            if (status != HeaderDecodeStatus::kOk) {
                if (refusedStream) {
                    refusedHeaderStream_.reset();
                }
                return handleHeaderDecodeFailure(*stream, status);
            }
            if (refusedStream) {
                appendRstStream(stream->id(), Http2ErrorCode::kRefusedStream);
                closedStreams_.remember(stream->id(), Http2StreamCloseSource::kLocal);
                refusedHeaderStream_.reset();
            } else {
                emitRequestHeaders(*stream);
            }
        }
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
        case Http2FrameType::kGoaway:
            closing_ = true;
            return true;
        // TODO(sans-io): kData is ported next (body -> kRequestBodyChunk / kRequestEnd
        // with receive-window flow control).
        default:
            appendGoaway(Http2ErrorCode::kInternalError, "frame type not yet ported");
            return false;
    }
}

Http2FeedResult Http2Connection::feed(std::string_view in) {
    // Buffer all fed bytes (nghttp2_session_mem_recv semantics: the caller may drop
    // its buffer after feed). Then consume as many complete frames as are available.
    input_.append(in.data(), in.size());
    for (;;) {
        const std::size_t available = input_.size() - inputOffset_;
        if (available < kHttp2FrameHeaderBytes) {
            break;
        }
        const auto header = http2ParseFrameHeader(
            std::string_view(input_.data() + inputOffset_, kHttp2FrameHeaderBytes));
        if (header.length > kHttp2MaxFrameSizeLimit || header.length > localMaxFrameSize_) {
            appendGoaway(Http2ErrorCode::kFrameSizeError, "frame too large");
            return {in.size(), Http2FeedStatus::kError};
        }
        if (available < kHttp2FrameHeaderBytes + header.length) {
            break;  // partial frame; wait for the next feed
        }
        const auto payload = std::string_view(
            input_.data() + inputOffset_ + kHttp2FrameHeaderBytes, header.length);
        if (!processFrame(header, payload)) {
            return {in.size(), Http2FeedStatus::kError};
        }
        inputOffset_ += kHttp2FrameHeaderBytes + header.length;
        if (closing_) {
            break;
        }
    }
    // Reclaim the consumed prefix so input_ does not grow unbounded.
    if (inputOffset_ > 0) {
        input_.erase(0, inputOffset_);
        inputOffset_ = 0;
    }
    return {in.size(), Http2FeedStatus::kOk};
}

Http2StreamState* Http2Connection::stream(std::uint32_t streamId) noexcept {
    return streams_.find(streamId);
}

void Http2Connection::submitResponseHead(
    std::uint32_t /*streamId*/, const HttpResponse& /*response*/, bool /*bodyForbidden*/) {
    // TODO: port appendHttp2ResponseHeaders -> outBuffer_ (writeHeaders atomic
    // HEADERS+CONTINUATION sequence).
}

Http2SubmitResult Http2Connection::submitData(
    std::uint32_t /*streamId*/, std::string_view /*chunk*/, bool /*endStream*/) {
    // TODO: port writeData: split by min(window, peerMaxFrameSize); consume send
    // window; on closed window record blockedStreams_ and return kBlocked.
    return Http2SubmitResult::kOk;
}

void Http2Connection::submitReset(std::uint32_t /*streamId*/, std::uint32_t /*errorCode*/) {
    // TODO: port sendRstStream -> outBuffer_.
}

void Http2Connection::beginGoaway(std::uint32_t /*errorCode*/) {
    // TODO: port sendGoaway -> outBuffer_; set closing_.
    closing_ = true;
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
