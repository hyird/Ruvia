#include "ruvia/http/detail/http2/Http2Connection.h"

#include <algorithm>
#include <utility>

#include "ruvia/http/detail/http2/flow/Http2FlowControl.h"
#include "ruvia/http/detail/http2/frame/Http2FrameCodec.h"
#include "ruvia/http/detail/http2/frame/Http2FramePayload.h"
#include "ruvia/http/detail/http2/message/Http2RemoteReceiveSemantics.h"

// Inbound DATA: framing and padding validation, the receive-window accounting a
// payload consumes, and delivering the bytes to the stream that owns them.

namespace ruvia::detail {

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
            queueConsumedDataCredit(
                (header.flags & kHttp2FlagEndStream) == 0
                    ? stream
                    : nullptr,
                static_cast<std::uint32_t>(flowBytes));
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
            // retain. Metadata-only empty frames likewise need no application ack,
            // but their credit is still batched to prevent per-frame amplification.
            queueConsumedDataCredit(
                (header.flags & kHttp2FlagEndStream) == 0
                    ? stream
                    : nullptr,
                static_cast<std::uint32_t>(flowBytes));
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

}  // namespace ruvia::detail
