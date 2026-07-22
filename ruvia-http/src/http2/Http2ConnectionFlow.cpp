#include "ruvia/http/detail/http2/Http2Connection.h"

#include <algorithm>
#include <utility>

#include "ruvia/http/detail/http2/Http2FlowControl.h"
#include "ruvia/http/detail/http2/Http2FrameCodec.h"
#include "ruvia/http/detail/http2/Http2RemoteReceiveSemantics.h"
#include "ruvia/http/detail/http2/Http2WindowUpdate.h"

// Connection- and stream-level flow control: how much of a queued body may go out
// under the current send window, what a WINDOW_UPDATE reopens, and the receive
// credit an owner's consumption returns to the peer.

namespace ruvia::detail {

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
            *stream, std::string_view(pending.bytes),
            pending.offset, pending.endStream);
        if (pending.offset >= pending.bytes.size()) {
            // The body fully drained. If a trailer block was queued behind it, emit it
            // now as the terminal HEADERS(END_STREAM) -- strictly AFTER all the DATA.
            if (!pending.trailerBlock.empty() && !stream->isAborted()) {
                appendResponseHeaderFrames(
                    *stream,
                    std::string_view(pending.trailerBlock),
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

void Http2Connection::flushWindowDebt(Http2StreamState& stream) {
    // Unreleased event credit must survive stream removal at CONNECTION scope, or an
    // owner that stops consuming after reset would permanently shrink the shared
    // window. It joins the same bounded batch as owner-consumed credit; a stream
    // WINDOW_UPDATE on a gone stream would instead be a peer protocol error.
    // Stream-scoped credit that was consumed but not yet advertised dies with
    // the stream. Its matching connection credit was queued independently.
    (void)stream.receiveWindowCredit().take();
    const auto debt = stream.takeWindowDebt();
    if (debt == 0) {
        return;
    }
    queueConsumedDataCredit(nullptr, debt);
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
    queueConsumedDataCredit(stream, debt);
}

bool Http2Connection::hasQueuedData(std::uint32_t streamId) const noexcept {
    return std::ranges::find(
               pendingSends_, streamId, &Http2PendingSend::streamId) !=
        pendingSends_.end();
}

void Http2Connection::queueConsumedDataCredit(
    Http2StreamState* stream,
    std::uint32_t bytes) {
    if (bytes == 0) {
        return;
    }

    connectionReceiveCredit_.add(bytes);
    const bool streamCanReceive = stream != nullptr &&
        !http2RemotePeerHalfClosed(*stream) && !stream->isAborted();
    if (streamCanReceive) {
        stream->receiveWindowCredit().add(bytes);
    }

    char buffer[kHttp2WindowUpdateFrameBytes * 2];
    auto* out = buffer;
    if (connectionReceiveCredit_.ready()) {
        const auto increment = connectionReceiveCredit_.take();
        http2CreditConnectionReceiveWindow(
            connectionReceiveWindow_, static_cast<std::int32_t>(increment));
        out = http2WriteWindowUpdate(out, 0, increment);
    }
    if (streamCanReceive && stream->receiveWindowCredit().ready()) {
        const auto increment = stream->receiveWindowCredit().take();
        http2CreditStreamReceiveWindow(
            *stream, static_cast<std::int32_t>(increment));
        out = http2WriteWindowUpdate(out, stream->id(), increment);
    }
    if (out != buffer) {
        output_.appendBytes(std::string_view(
            buffer, static_cast<std::size_t>(out - buffer)));
    }
}

void Http2Connection::releaseDroppedDataConnectionWindow(std::int32_t flowBytes) {
    // Every structurally valid DATA frame reached this path only after the shared
    // connection debit succeeded. Return exactly that credit while keeping the
    // connection; no stream window survives an abandoned stream.
    if (flowBytes <= 0) {
        return;
    }
    queueConsumedDataCredit(nullptr, static_cast<std::uint32_t>(flowBytes));
}

}  // namespace ruvia::detail
