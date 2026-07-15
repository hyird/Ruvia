#include "ruvia/http/detail/http2/Http2Connection.h"

#include <charconv>
#include <cstdint>
#include <optional>
#include <string_view>

#include "ruvia/http/detail/HttpResponseContentSemantics.h"
#include "ruvia/http/detail/http2/Http2FramePayload.h"
#include "ruvia/http/detail/http2/Http2HeaderBlock.h"
#include "ruvia/http/detail/http2/Http2HeaderRules.h"
#include "ruvia/http/detail/http2/Http2RemoteReceiveSemantics.h"
#include "ruvia/http/detail/http2/Http2RequestHeaders.h"

namespace ruvia::detail {

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
    const auto responseContentSemantics = httpResponseContentSemantics(
        stream.requestKnownMethod(), *context->status);
    const bool successfulConnect =
        responseContentSemantics ==
        HttpResponseContentSemantics::kConnectTunnel;
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
    if (contentSemantics == HttpResponseContentSemantics::kWithoutContent &&
        !stream.selectRemoteContentMetadataOnly()) {
        return HeaderDecodeStatus::kProtocolError;
    }
    if (stream.tunnel().pending() != nullptr) {
        if (contentSemantics ==
            HttpResponseContentSemantics::kConnectTunnel) {
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
            if (!source.has_value()) {
                // HEADERS is the only frame that could establish this peer stream, but
                // a newly established identifier must be greater than every identifier
                // the peer already opened (RFC 9113 5.1.1). A skipped lower identifier
                // cannot be reopened as a new request.
                appendGoaway(
                    Http2ErrorCode::kProtocolError,
                    "new peer stream id is not increasing");
                return false;
            }
            // A stream explicitly closed by this endpoint can still have an in-flight
            // field block. Minimally decode and discard it so HPACK remains synchronized
            // as permitted by RFC 9113 5.1.
            discardedAction = DiscardedHeaderAction::kIgnore;
        } else {
            // Record every genuinely new peer stream ID, even when it is malformed or
            // refused, so a later lower ID cannot be reopened as idle.
            lastStreamId_ = header.streamId;
            const auto* gracefulDrain =
                localConnectionState_.gracefulDrain();
            const bool drainRefused = gracefulDrain != nullptr &&
                header.streamId > gracefulDrain->lastStreamId();
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
    // Bound the CONTINUATION count per header block. Empty CONTINUATION frames add
    // no bytes and so never trip the accumulated-block size cap; without this an
    // endless stream of them keeps the block "in progress" forever (RFC 9113 §6.10,
    // CVE-2024-27316 CONTINUATION flood).
    if (!headerContinuation_.recordContinuationFrame()) {
        appendGoaway(Http2ErrorCode::kEnhanceYourCalm, "CONTINUATION flood");
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

}  // namespace ruvia::detail
