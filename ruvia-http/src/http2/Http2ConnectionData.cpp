#include "ruvia/http/detail/http2/Http2Connection.h"

#include <algorithm>
#include <exception>
#include <stdexcept>
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
    // Feed retries the complete frame when a PMR allocation throws. Do not debit the
    // connection window until the selected branch has reserved all of its output and
    // event capacity; otherwise a retry would observe a half-processed DATA frame.
    if (flowBytes > connectionReceiveWindow_) {
        appendGoaway(Http2ErrorCode::kFlowControlError, "connection flow-control window exceeded");
        return false;
    }

    const bool endStream = (header.flags & kHttp2FlagEndStream) != 0;
    const auto reserveEvents = [this](std::size_t count) {
        reserveEventSlots(count);
    };
    const auto reserveConsumedCredit = [this](Http2StreamState* creditStream, std::uint32_t bytes) {
        if (bytes == 0) {
            return;
        }
        const bool streamCanReceive = creditStream != nullptr && !http2RemotePeerHalfClosed(*creditStream) && !creditStream->isAborted();
        const auto connectionReady = connectionReceiveCredit_.readyAfter(bytes);
        const auto streamReady = streamCanReceive && creditStream->receiveWindowCredit().readyAfter(bytes);
        const auto frameCount = static_cast<std::size_t>(connectionReady) + static_cast<std::size_t>(streamReady);
        if (frameCount != 0) {
            output_.reserveAdditional(frameCount * kHttp2WindowUpdateFrameBytes);
        }
    };
    const auto reserveStreamError = [this, &reserveEvents](std::size_t extraEvents = 0) {
        reserveEvents(1 + extraEvents);
        // RST_STREAM plus at most one WINDOW_UPDATE from closeStream's flushed
        // stream debt and one from this frame's dropped connection credit.
        output_.reserveAdditional(kHttp2FrameHeaderBytes + 4 + 2 * kHttp2WindowUpdateFrameBytes);
    };
    const auto debitConnection = [this, flowBytes]() noexcept {
        connectionReceiveWindow_ -= flowBytes;
    };

    auto* stream = findStream(header.streamId);
    if (wasClosedByPeerReset(header.streamId, stream)) {
        // This peer's RST_STREAM and later DATA are ordered on the same connection.
        // Unlike DATA that was already in flight when we sent a reset, this cannot be
        // a state-view race; keep the strict closed-state verdict.
        appendGoaway(Http2ErrorCode::kStreamClosed, "DATA after peer RST_STREAM");
        return false;
    }
    if (stream == nullptr) {
        const auto closeSource = closedStreams_.source(header.streamId);
        if (closeSource == Http2StreamCloseSource::kPeerGoaway || !isIdleStreamId(header.streamId)) {
            // A dropped DATA frame still consumes connection flow control, but has no
            // stream window or application event to publish.
            reserveConsumedCredit(nullptr, static_cast<std::uint32_t>(flowBytes));
            debitConnection();
            releaseDroppedDataConnectionWindow(flowBytes);
            return true;
        }
        appendGoaway(Http2ErrorCode::kProtocolError, "DATA before HEADERS");
        return false;
    }
    if (http2StreamIsClosed(*stream)) {
        reserveConsumedCredit(nullptr, static_cast<std::uint32_t>(flowBytes));
        debitConnection();
        releaseDroppedDataConnectionWindow(flowBytes);
        return true;
    }

    const auto resetStream = [&](Http2ErrorCode error, bool debitStreamWindow) {
        reserveStreamError();
        debitConnection();
        if (debitStreamWindow) {
            (void)stream->consumeReceiveWindow(flowBytes);
        }
        output_.appendRstStream(header.streamId, error);
        closeStream(header.streamId, Http2StreamCloseSource::kLocal, error);
        releaseDroppedDataConnectionWindow(flowBytes);
    };

    const auto& remote = stream->remoteReceive();
    if (remote.headPending() != nullptr || remote.headEndStreamPending() != nullptr) {
        resetStream(Http2ErrorCode::kProtocolError, false);
        return true;
    }
    if (remote.endStream() != nullptr || remote.connectPendingEndStream() != nullptr) {
        // END_STREAM closes only the peer's send half. Another DATA frame from this
        // peer is nevertheless a frame on a half-closed (remote) stream.
        resetStream(Http2ErrorCode::kStreamClosed, false);
        return true;
    }

    const bool pendingConnectControl = remote.connectPending() != nullptr;
    const bool rejectedConnectTerminal = remote.connectRejectedAwaitingEndStream() != nullptr;
    const bool tunnelData = remote.tunnelOpen() != nullptr;
    const bool contentData = remote.contentOpen() != nullptr;
    const bool metadataOnlyContent = contentData && (stream->remoteContent().metadataOnlyWithoutLength() != nullptr || stream->remoteContent().metadataOnlyKnownLength() != nullptr);
    if (!pendingConnectControl && !rejectedConnectTerminal && !tunnelData && !contentData) {
        resetStream(Http2ErrorCode::kStreamClosed, false);
        return true;
    }

    // Check the stream window without mutating it so a flow-control reset can be
    // fully reserved before either receive window is consumed.
    if (flowBytes > stream->receiveWindow()) {
        resetStream(Http2ErrorCode::kFlowControlError, false);
        return true;
    }

    if (pendingConnectControl || rejectedConnectTerminal) {
        // CONNECT has no request content. Empty DATA remains framing-only until its
        // END_STREAM, while padding still consumes both flow-control windows.
        if (!data.empty()) {
            resetStream(Http2ErrorCode::kProtocolError, true);
            return true;
        }
        reserveConsumedCredit(endStream ? nullptr : stream, static_cast<std::uint32_t>(flowBytes));
        debitConnection();
        (void)stream->consumeReceiveWindow(flowBytes);
        if (flowBytes > 0) {
            queueConsumedDataCredit(endStream ? nullptr : stream, static_cast<std::uint32_t>(flowBytes));
        }
        if (!endStream) {
            return true;
        }
        const bool remoteFinished = pendingConnectControl ? stream->finishRemotePendingConnect() : stream->finishRemoteRejectedConnect();
        if (!remoteFinished) {
            // The selected remote alternative is exclusive and the transition is
            // noexcept; reaching this branch means an internal invariant failed.
            std::terminate();
        }
        releaseLocalRequestStreamIfClosed(*stream);
        return true;
    }

    // Evaluate content accounting on a detached state first. A rejected DATA frame
    // must reserve its reset before the live content counter is changed.
    Http2RemoteContentAccountingResult contentAccounting = Http2RemoteContentAccountingResult::kAccepted;
    Http2RemoteContentState candidateContent = stream->remoteContent();
    if (contentData) {
        contentAccounting = candidateContent.account(data.size());
        if (contentAccounting == Http2RemoteContentAccountingResult::kCounterOverflow) {
            resetStream(Http2ErrorCode::kCancel, true);
            return true;
        }
        if (contentAccounting != Http2RemoteContentAccountingResult::kAccepted) {
            resetStream(Http2ErrorCode::kProtocolError, true);
            return true;
        }
        if (endStream && !candidateContent.terminalLengthValid()) {
            // Preserve the already-received body chunk before reporting the
            // Content-Length mismatch. The body event and the close event are one
            // pre-reserved publication batch; the close path flushes the matching
            // receive debt, so this branch must not return the same bytes again via
            // releaseDroppedDataConnectionWindow().
            reserveStreamError(data.empty() ? 0 : 1);
            if (data.empty() && flowBytes > 0) {
                reserveConsumedCredit(nullptr, static_cast<std::uint32_t>(flowBytes));
            }
            debitConnection();
            (void)stream->consumeReceiveWindow(flowBytes);
            (void)stream->accountRemoteContent(data.size());
            if (flowBytes > 0) {
                if (!data.empty()) {
                    stream->addWindowDebt(static_cast<std::uint32_t>(flowBytes));
                    events_.push_back(Http2Event::messageBodyChunk(header.streamId, data, static_cast<std::uint32_t>(flowBytes)));
                } else {
                    queueConsumedDataCredit(nullptr, static_cast<std::uint32_t>(flowBytes));
                }
            }
            output_.appendRstStream(header.streamId, Http2ErrorCode::kProtocolError);
            closeStream(header.streamId, Http2StreamCloseSource::kLocal, Http2ErrorCode::kProtocolError);
            return true;
        }
    }

    const bool deliverData = !metadataOnlyContent && !data.empty();
    if (deliverData) {
        reserveEvents(endStream ? 2 : 1);
    } else if (endStream) {
        reserveEvents(1);
    }
    if (!deliverData && flowBytes > 0) {
        reserveConsumedCredit(endStream ? nullptr : stream, static_cast<std::uint32_t>(flowBytes));
    }

    debitConnection();
    (void)stream->consumeReceiveWindow(flowBytes);
    if (contentData) {
        (void)stream->accountRemoteContent(data.size());
    }
    if (flowBytes > 0) {
        if (deliverData) {
            // Advertise new capacity only after the owner has consumed or copied the
            // borrowed event bytes; this preserves backpressure for content and tunnel
            // data alike.
            stream->addWindowDebt(static_cast<std::uint32_t>(flowBytes));
        } else {
            // Empty/padding-only DATA has no application event, so its credit is
            // returned immediately in the same bounded batch.
            queueConsumedDataCredit(endStream ? nullptr : stream, static_cast<std::uint32_t>(flowBytes));
        }
    }

    // Hand only actual body bytes to the owner. Empty and padding-only DATA still
    // participate in framing, END_STREAM, and flow control without creating a
    // no-progress event for every tiny wire frame.
    if (deliverData) {
        events_.push_back(tunnelData ? Http2Event::tunnelData(header.streamId, data, static_cast<std::uint32_t>(flowBytes)) : Http2Event::messageBodyChunk(header.streamId, data, static_cast<std::uint32_t>(flowBytes)));
    }
    if (endStream) {
        const bool remoteFinished = tunnelData ? stream->finishRemoteTunnel() : stream->finishRemoteContent();
        if (!remoteFinished) {
            std::terminate();
        }
        events_.push_back(tunnelData ? Http2Event::tunnelEnd(header.streamId) : Http2Event::messageEnd(header.streamId));
        releaseLocalRequestStreamIfClosed(*stream);
    }
    return true;
}

}  // namespace ruvia::detail
