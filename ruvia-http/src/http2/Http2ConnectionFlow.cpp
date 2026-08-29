#include "ruvia/http/detail/http2/Http2Connection.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

#include "ruvia/http/detail/http2/flow/Http2FlowControl.h"
#include "ruvia/http/detail/http2/frame/Http2FrameCodec.h"
#include "ruvia/http/detail/http2/message/Http2RemoteReceiveSemantics.h"
#include "ruvia/http/detail/http2/flow/Http2WindowUpdate.h"

// Connection- and stream-level flow control: how much of a queued body may go out
// under the current send window, what a WINDOW_UPDATE reopens, and the receive
// credit an owner's consumption returns to the peer.

namespace ruvia::detail {
namespace {

[[nodiscard]] std::size_t http2DataFrameEncodedBytes(
    std::size_t dataBytes, std::size_t maxFrameSize) {
    if (dataBytes == 0) {
        return 0;
    }
    const auto frameCount = dataBytes / maxFrameSize + (dataBytes % maxFrameSize == 0 ? 0 : 1);
    if (frameCount >
        (std::numeric_limits<std::size_t>::max() - dataBytes) / kHttp2FrameHeaderBytes) {
        throw std::length_error("HTTP/2 DATA output size overflow");
    }
    return dataBytes + frameCount * kHttp2FrameHeaderBytes;
}

// The largest HPACK integer encoding for a dynamic-table update is six bytes
// (the value is a uint32 with a five-bit prefix). The drain preflight uses this
// upper bound; appendResponseHeaderFrames computes the exact prefix afterwards.
[[nodiscard]] std::size_t http2HeaderFrameEncodedBytes(
    std::size_t headerBytes, std::size_t maxFrameSize, std::size_t firstPrefixBytes) {
    if (firstPrefixBytes > maxFrameSize) {
        throw std::length_error("HTTP/2 header prefix exceeds frame size");
    }
    std::size_t encodedBytes = 0;
    std::size_t offset = 0;
    bool first = true;
    while (offset < headerBytes || (first && firstPrefixBytes != 0)) {
        const auto prefixBytes = first ? firstPrefixBytes : std::size_t{0};
        const auto chunk = std::min(headerBytes - offset, maxFrameSize - prefixBytes);
        const auto frameBytes = kHttp2FrameHeaderBytes + prefixBytes + chunk;
        if (frameBytes > std::numeric_limits<std::size_t>::max() - encodedBytes) {
            throw std::length_error("HTTP/2 header output size overflow");
        }
        encodedBytes += frameBytes;
        offset += chunk;
        first = false;
    }
    return encodedBytes;
}

[[nodiscard]] std::size_t checkedOutputBytesAdd(std::size_t current, std::size_t additional) {
    if (additional > std::numeric_limits<std::size_t>::max() - current) {
        throw std::length_error("HTTP/2 deferred output size overflow");
    }
    return current + additional;
}

}  // namespace

std::size_t Http2Connection::sendDataUpToWindow(
    Http2StreamState& stream, std::string_view data, std::size_t offset, Http2EndStream endStream) {
    const auto total = data.size();
    while (offset < total) {
        const auto available = http2AvailableSendWindow(connectionSendWindow_, stream);
        if (available == 0) {
            break;  // window exhausted; caller buffers the remainder
        }
        const auto chunk =
            std::min<std::size_t>({total - offset, available, peerSettings_.maxFrameSize()});
        const bool last = offset + chunk == total;
        http2ConsumeSendWindow(connectionSendWindow_, stream, chunk);
        output_.appendFrame(Http2FrameType::kData,
            static_cast<std::uint8_t>(http2EndsStream(endStream) && last ? kHttp2FlagEndStream : 0),
            stream.id(), data.substr(offset, chunk));
        stream.commitLocalContent(chunk);
        offset += chunk;
    }
    return offset;
}

void Http2Connection::markSendWindowOpened() {
    // Drain is a wire/state transaction: a throwing PMR resource must not leave
    // a queued body with a smaller window and no corresponding bytes. Simulate
    // the complete drain first and reserve both outbound bytes and the
    // completion notification vector before touching any stream state.
    std::size_t requiredOutputBytes = 0;
    std::size_t drainedCount = 0;
    auto simulatedConnectionWindow = connectionSendWindow_;
    bool simulatedTableUpdatePending = encoderTableSizeUpdatePending_;
    const auto maxFrame = peerSettings_.maxFrameSize();
    constexpr std::size_t kMaxDynamicTableUpdateBytes = 6;
    for (const auto& pending : pendingSends_) {
        auto* stream = findStream(pending.streamId);
        if (stream == nullptr || stream->isAborted()) {
            continue;
        }
        if (pending.offset > pending.bytes.size()) {
            throw std::logic_error("HTTP/2 deferred DATA offset is invalid");
        }
        const auto remaining = pending.bytes.size() - pending.offset;
        const auto available = http2AvailableSendWindow(simulatedConnectionWindow, *stream);
        const auto immediate = std::min(remaining, available);
        requiredOutputBytes = checkedOutputBytesAdd(
            requiredOutputBytes, http2DataFrameEncodedBytes(immediate, maxFrame));
        simulatedConnectionWindow -= static_cast<std::int32_t>(immediate);
        if (immediate != remaining) {
            continue;
        }

        ++drainedCount;
        if (!pending.trailerBlock.empty()) {
            requiredOutputBytes = checkedOutputBytesAdd(requiredOutputBytes,
                http2HeaderFrameEncodedBytes(pending.trailerBlock.size(), maxFrame,
                    simulatedTableUpdatePending ? kMaxDynamicTableUpdateBytes : 0));
            simulatedTableUpdatePending = false;
        }
    }
    output_.reserveAdditional(requiredOutputBytes);
    if (drainedCount > drainedDataStreams_.max_size() - drainedDataStreams_.size()) {
        throw std::length_error("HTTP/2 drained stream notification size overflow");
    }
    drainedDataStreams_.reserve(drainedDataStreams_.size() + drainedCount);

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
            *stream, std::string_view(pending.bytes), pending.offset, pending.endStream);
        if (pending.offset >= pending.bytes.size()) {
            // The body fully drained. If a trailer block was queued behind it, emit it
            // now as the terminal HEADERS(END_STREAM) -- strictly AFTER all the DATA.
            if (!pending.trailerBlock.empty() && !stream->isAborted()) {
                appendResponseHeaderFrames(
                    *stream, std::string_view(pending.trailerBlock), Http2EndStream::kEndStream);
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

bool Http2Connection::processWindowUpdate(
    const Http2FrameHeader& header, std::string_view payload) {
    if (payload.size() != 4) {
        appendGoaway(Http2ErrorCode::kFrameSizeError, "invalid WINDOW_UPDATE");
        return false;
    }
    const auto increment = http2WindowUpdateIncrement(payload);
    if (header.streamId == 0) {
        switch (http2ApplyWindowUpdate(connectionSendWindow_, increment)) {
            case Http2WindowUpdateResult::kOk:
                try {
                    markSendWindowOpened();
                } catch (...) {
                    connectionSendWindow_ -= static_cast<std::int32_t>(increment);
                    throw;
                }
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
            appendGoaway(Http2ErrorCode::kProtocolError, "zero WINDOW_UPDATE on closed stream");
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
                Http2ErrorCode::kProtocolError, "zero WINDOW_UPDATE on released closed stream");
            return false;
        }
        return true;
    }
    const auto resetStreamForWindowUpdateError = [&](Http2ErrorCode error) {
        const auto outputCheckpoint = output_.checkpoint();
        try {
            output_.appendRstStream(header.streamId, error);
            closeStream(header.streamId, Http2StreamCloseSource::kLocal, error);
            markSendWindowOpened();
        } catch (...) {
            output_.rollbackTo(outputCheckpoint);
            throw;
        }
    };
    switch (http2ApplyStreamWindowUpdate(*stream, increment)) {
        case Http2WindowUpdateResult::kOk:
            try {
                markSendWindowOpened();
            } catch (...) {
                (void)stream->addSendWindow(-static_cast<std::int64_t>(increment));
                throw;
            }
            return true;
        case Http2WindowUpdateResult::kZeroIncrement:
            resetStreamForWindowUpdateError(Http2ErrorCode::kProtocolError);
            return true;
        case Http2WindowUpdateResult::kOverflow:
            resetStreamForWindowUpdateError(Http2ErrorCode::kFlowControlError);
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
    const auto streamCredit = stream.receiveWindowCredit().take();
    const auto debt = stream.takeWindowDebt();
    if (debt == 0) {
        return;
    }
    try {
        queueConsumedDataCredit(nullptr, debt);
    } catch (...) {
        stream.receiveWindowCredit().add(streamCredit);
        stream.addWindowDebt(debt);
        throw;
    }
}

bool Http2Connection::releaseReceivedData(std::uint32_t streamId, std::uint32_t bytes) {
    auto* stream = findStream(streamId);
    if (stream == nullptr) {
        return false;  // debt (if any) died with the stream; nothing left to credit
    }
    if (!stream->takeWindowDebt(bytes)) {
        return false;
    }
    try {
        queueConsumedDataCredit(stream, bytes);
    } catch (...) {
        stream->addWindowDebt(bytes);
        throw;
    }
    return true;
}

void Http2Connection::releaseAllReceivedData(std::uint32_t streamId) {
    auto* stream = findStream(streamId);
    if (stream == nullptr) {
        return;
    }
    const auto debt = stream->takeWindowDebt();
    if (debt == 0) {
        return;
    }
    try {
        queueConsumedDataCredit(stream, debt);
    } catch (...) {
        stream->addWindowDebt(debt);
        throw;
    }
}

bool Http2Connection::hasQueuedData(std::uint32_t streamId) const noexcept {
    return std::ranges::find(pendingSends_, streamId, &Http2PendingSend::streamId) !=
           pendingSends_.end();
}

void Http2Connection::queueConsumedDataCredit(Http2StreamState* stream, std::uint32_t bytes) {
    if (bytes == 0) {
        return;
    }

    const bool streamCanReceive =
        stream != nullptr && !http2RemotePeerHalfClosed(*stream) && !stream->isAborted();
    const auto connectionReady = connectionReceiveCredit_.readyAfter(bytes);
    const auto streamReady = streamCanReceive && stream->receiveWindowCredit().readyAfter(bytes);
    const auto frameCount =
        static_cast<std::size_t>(connectionReady) + static_cast<std::size_t>(streamReady);
    if (frameCount != 0) {
        output_.reserveAdditional(frameCount * kHttp2WindowUpdateFrameBytes);
    }

    connectionReceiveCredit_.add(bytes);
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
        http2CreditStreamReceiveWindow(*stream, static_cast<std::int32_t>(increment));
        out = http2WriteWindowUpdate(out, stream->id(), increment);
    }
    if (out != buffer) {
        output_.appendBytes(std::string_view(buffer, static_cast<std::size_t>(out - buffer)));
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
