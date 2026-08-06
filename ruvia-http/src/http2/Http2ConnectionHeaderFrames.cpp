#include "ruvia/http/detail/http2/Http2Connection.h"

#include <cstdint>
#include <optional>
#include <string_view>

#include "ruvia/http/detail/http2/frame/Http2FramePayload.h"
#include "ruvia/http/detail/http2/hpack/Http2HeaderBlock.h"
#include "ruvia/http/detail/http2/message/Http2HeaderRules.h"
#include "ruvia/http/detail/http2/message/Http2RemoteReceiveSemantics.h"
#include "ruvia/http/detail/http2/message/Http2RequestHeaders.h"

// HEADERS and CONTINUATION as they arrive: padding and priority framing, whether
// this stream may open at all, and keeping a header block contiguous until its
// END_HEADERS.

namespace ruvia::detail {

bool Http2Connection::processHeaders(const Http2FrameHeader& header, std::string_view payload) {
    if (header.streamId == 0) {
        appendGoaway(Http2ErrorCode::kProtocolError, "HEADERS stream id must be nonzero");
        return false;
    }
    if ((header.streamId & 1U) == 0) {
        appendGoaway(Http2ErrorCode::kProtocolError, role_ == Http2Role::kClient ? "HEADERS on even stream id" : "invalid client stream id");
        return false;
    }

    std::string_view fragment;
    switch (http2DecodeHeadersPayload(header, payload, fragment)) {
        case Http2FramePayloadStatus::kDecoded:
            break;
        case Http2FramePayloadStatus::kInvalidPadding:
            appendGoaway(Http2ErrorCode::kProtocolError, "invalid HEADERS padding");
            return false;
        case Http2FramePayloadStatus::kMissingPriorityFields:
            // HEADERS carries a field block and can alter HPACK state, so a payload
            // too short for its mandatory fields is a connection FRAME_SIZE_ERROR
            // (RFC 9113 §4.2), unlike the stream-scoped standalone PRIORITY frame.
            appendGoaway(Http2ErrorCode::kFrameSizeError, "HEADERS priority fields are incomplete");
            return false;
    }

    Http2StreamState* stream = nullptr;
    bool newPeerStream = false;
    bool createdPeerStream = false;
    std::optional<DiscardedHeaderAction> discardedAction;
    if (auto* existing = findStream(header.streamId); existing != nullptr) {
        if (existing->isAborted()) {
            if (existing->localSend().aborted()->source() == Http2StreamCloseSource::kPeer) {
                // Frames sent after the peer's own RST are not an in-flight race: the
                // connection byte stream orders them after that terminal signal.
                appendGoaway(Http2ErrorCode::kStreamClosed, "HEADERS after peer RST_STREAM");
                return false;
            }
            // After WE sent RST_STREAM, any peer frames already in flight must be
            // minimally processed and discarded. Do not send a second RST.
            discardedAction = DiscardedHeaderAction::kIgnore;
        } else if (http2StreamIsClosed(*existing)) {
            // A pin can retain normally completed request storage after both
            // protocol halves have closed. Decode for HPACK synchronization,
            // but never make storage retention authorize another stream frame.
            discardedAction = DiscardedHeaderAction::kIgnore;
        } else if (http2RemoteFinalHeadDecoded(*existing) && (existing->tunnel().open() != nullptr || (role_ == Http2Role::kServer && existing->tunnel().pending() != nullptr))) {
            // CONNECT has no request trailers, and an accepted connected stream only
            // permits DATA/RST_STREAM/WINDOW_UPDATE/PRIORITY. Decode the complete
            // field block for HPACK synchronization, then reset this stream.
            return startDiscardedHeaderBlock(header, fragment, DiscardedHeaderAction::kResetProtocolError);
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
                appendGoaway(Http2ErrorCode::kProtocolError, "new peer stream id is not increasing");
                return false;
            }
            // A stream explicitly closed by this endpoint can still have an in-flight
            // field block. Minimally decode and discard it so HPACK remains synchronized
            // as permitted by RFC 9113 5.1.
            discardedAction = DiscardedHeaderAction::kIgnore;
        } else {
            // Publish a genuinely new peer stream ID only after the first HEADERS
            // fragment has been accepted. A throwing stream allocation or header
            // buffer append must leave the complete frame retryable; advancing the
            // high-water mark before that point would turn the retry into a false
            // "stream id is not increasing" connection error.
            newPeerStream = true;
            const auto* gracefulDrain = localConnectionState_.gracefulDrain();
            const bool drainRefused = gracefulDrain != nullptr && header.streamId > gracefulDrain->lastStreamId();
            stream = drainRefused ? nullptr : createStream(header.streamId);
            createdPeerStream = stream != nullptr;
            if (stream == nullptr) {
                discardedAction = DiscardedHeaderAction::kRefuseStream;
            }
        }
    }

    if (discardedAction) {
        const auto result = startDiscardedHeaderBlock(header, fragment, *discardedAction);
        if (newPeerStream && result) {
            lastStreamId_ = header.streamId;
        }
        return result;
    }

    // Buffering the compressed fragment is the first fallible operation. Do it
    // before publishing END_STREAM in the remote lifecycle; if the PMR rejects the
    // append, a retry sees the original head-pending state. A newly created stream
    // is likewise removed on this failure because it has not become observable yet.
    bool startedHeaderBlock = false;
    try {
        startedHeaderBlock = http2StartHeaderBlock(*stream, fragment);
    } catch (...) {
        if (createdPeerStream) {
            streams_.remove(header.streamId);
        }
        throw;
    }
    if (!startedHeaderBlock) {
        if (createdPeerStream) {
            streams_.remove(header.streamId);
        }
        // The block exceeds the header buffer cap. We cannot decode a block we could
        // not fully buffer, and skipping it would desync the connection-global HPACK
        // dynamic table for every later block (RFC 9113 §4.3) -- so this is a
        // COMPRESSION_ERROR, not a survivable stream reset.
        appendGoaway(Http2ErrorCode::kCompressionError, "field block not decompressed");
        return false;
    }

    if (newPeerStream) {
        lastStreamId_ = header.streamId;
    }

    bool recordedHeadEndStream = false;
    if ((header.flags & kHttp2FlagEndStream) != 0) {
        if (!stream->recordRemoteHeadEndStream()) {
            output_.appendRstStream(header.streamId, Http2ErrorCode::kProtocolError);
            closeStream(header.streamId, Http2StreamCloseSource::kLocal, Http2ErrorCode::kProtocolError);
            return true;
        }
        recordedHeadEndStream = true;
    }

    if ((header.flags & kHttp2FlagEndHeaders) != 0) {
        try {
            Http2StreamHeaderDecodeTransaction transaction{*stream, role_ == Http2Role::kServer};
            auto hpackTransaction = decoder_.beginTransaction();
            const auto status = decodeInitialHeaderBlock(*stream, transaction, hpackTransaction);
            if (status != HeaderDecodeStatus::kOk) {
                transaction.rollback();
                return handleHeaderDecodeFailure(*stream, status, &hpackTransaction);
            }
            if (http2RemoteFinalHeadDecoded(*stream)) {
                emitRequestHeaders(*stream);  // not yet decoded = a 1xx interim head (client)
            }
            transaction.commit();
            hpackTransaction.commit();
            http2ResetHeaderBlock(*stream);
        } catch (...) {
            if (recordedHeadEndStream) {
                (void)stream->rollbackRemoteHeadEndStreamForRetry();
            }
            throw;
        }
    } else {
        headerContinuation_.start(stream->id(), Http2HeaderBlockKind::kInitial);
    }
    return true;
}

bool Http2Connection::processTrailerHeaders(Http2StreamState& stream, const Http2FrameHeader& header, std::string_view fragment) {
    if (http2RemotePeerHalfClosed(stream)) {
        return startDiscardedHeaderBlock(header, fragment, DiscardedHeaderAction::kResetStreamClosed);
    }
    if (stream.remoteReceive().contentOpen() == nullptr) {
        return startDiscardedHeaderBlock(header, fragment, DiscardedHeaderAction::kResetProtocolError);
    }
    if ((header.flags & kHttp2FlagEndStream) == 0) {
        return startDiscardedHeaderBlock(header, fragment, DiscardedHeaderAction::kResetProtocolError);
    }
    if (!http2StartHeaderBlock(stream, fragment)) {
        // Un-bufferable trailer block: same HPACK-consistency reasoning as HEADERS --
        // COMPRESSION_ERROR rather than a survivable stream reset.
        appendGoaway(Http2ErrorCode::kCompressionError, "field block not decompressed");
        return false;
    }

    if ((header.flags & kHttp2FlagEndHeaders) != 0) {
        Http2StreamHeaderDecodeTransaction transaction{stream, false};
        auto hpackTransaction = decoder_.beginTransaction();
        const auto status = finishTrailerBlock(stream, transaction, hpackTransaction);
        if (status != HeaderDecodeStatus::kOk) {
            transaction.rollback();
            return handleHeaderDecodeFailure(stream, status, &hpackTransaction);
        }
        transaction.commit();
        hpackTransaction.commit();
        http2ResetHeaderBlock(stream);
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
    // CVE-2024-27316 CONTINUATION flood). Check without mutating first: buffering
    // the fragment below can allocate, and a failed retry must not spend the
    // frame-count budget twice.
    if (!headerContinuation_.continuationFrameBudgetAvailable()) {
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
    (void)headerContinuation_.recordContinuationFrame();
    if ((header.flags & kHttp2FlagEndHeaders) != 0) {
        const auto continuationCheckpoint = headerContinuation_.checkpoint();
        try {
            const auto completedKind = headerContinuation_.finishKind();
            if (completedKind == Http2HeaderBlockKind::kDiscarded) {
                return finishDiscardedHeaderBlock();
            }
            if (completedKind == Http2HeaderBlockKind::kTrailers) {
                Http2StreamHeaderDecodeTransaction transaction{*stream, false};
                auto hpackTransaction = decoder_.beginTransaction();
                const auto status = finishTrailerBlock(*stream, transaction, hpackTransaction);
                if (status != HeaderDecodeStatus::kOk) {
                    transaction.rollback();
                    return handleHeaderDecodeFailure(*stream, status, &hpackTransaction);
                }
                transaction.commit();
                hpackTransaction.commit();
                http2ResetHeaderBlock(*stream);
            } else {
                Http2StreamHeaderDecodeTransaction transaction{*stream, role_ == Http2Role::kServer};
                auto hpackTransaction = decoder_.beginTransaction();
                const auto status = decodeInitialHeaderBlock(*stream, transaction, hpackTransaction);
                if (status != HeaderDecodeStatus::kOk) {
                    transaction.rollback();
                    return handleHeaderDecodeFailure(*stream, status, &hpackTransaction);
                }
                if (http2RemoteFinalHeadDecoded(*stream)) {
                    emitRequestHeaders(*stream);
                }
                transaction.commit();
                hpackTransaction.commit();
                http2ResetHeaderBlock(*stream);
            }
        } catch (...) {
            // `finishKind()` is a commit of the continuation latch. Decode and
            // subsequent event/output reservation remain fallible, so restore the
            // latch before rethrowing; the caller may retry this exact final frame.
            headerContinuation_.restore(continuationCheckpoint);
            throw;
        }
    }
    return true;
}

}  // namespace ruvia::detail
