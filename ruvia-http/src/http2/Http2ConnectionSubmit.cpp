#include "ruvia/http/detail/http2/Http2Connection.h"

#include <algorithm>
#include <utility>

#include "ruvia/http/detail/field/HttpHeaderSectionSize.h"
#include "ruvia/http/detail/coding/HttpRequestContentSemantics.h"
#include "ruvia/http/detail/response/HttpResponseBodyAccess.h"
#include "ruvia/http/detail/coding/HttpResponseContentSemantics.h"
#include "ruvia/http/detail/http2/flow/Http2FlowControl.h"
#include "ruvia/http/detail/http2/message/Http2HeaderRules.h"
#include "ruvia/http/detail/http2/message/Http2RemoteReceiveSemantics.h"
#include "ruvia/http/detail/http2/message/Http2ResponseHeaders.h"
#include "ruvia/http/detail/http2/message/Http2WebSocketHandshake.h"
#include "ruvia/http/detail/server/HttpFinalResponseControlPlan.h"
#include "ruvia/http/detail/server/HttpResponseTrailers.h"

namespace ruvia::detail {
namespace {

[[nodiscard]] bool http2IsValidConnectResponseHead(const HttpResponse& response) noexcept {
    const auto& body = responseBody(response);
    if (!response.status().isSuccessful() || body.size() != 0 || body.file().has_value()) {
        return false;
    }
    for (const auto& header : response.headers()) {
        const auto known = responseHeaderKnownBit(header);
        if (known == kResponseHeaderContentLength || known == kResponseHeaderTransferEncoding || httpAsciiEqualsIgnoreCase(header.name(), "content-length") || httpAsciiEqualsIgnoreCase(header.name(), "transfer-encoding")) {
            return false;
        }
    }
    return true;
}
}  // namespace

void Http2Connection::appendResponseHeaderFrames(Http2StreamState& stream, std::string_view headerBlock, Http2EndStream endStream) {
    // A HEADERS + CONTINUATION run must be an uninterrupted frame sequence for the same
    // stream (RFC 9113 §6.10). Appending them contiguously to the single outbound buffer
    // guarantees that ordering (replacing the coroutine writeHeaders' atomic write).
    std::pmr::string tableSizeUpdate(resource_);
    if (encoderTableSizeUpdatePending_) {
        HpackEncoder::encodeDynamicTableSizeUpdate(tableSizeUpdate, encoderDynamicTableSize_);
    }

    const std::size_t maxFrame = peerSettings_.maxFrameSize();
    std::size_t offset = 0;
    bool first = true;
    while (offset < headerBlock.size() || (first && !tableSizeUpdate.empty())) {
        const auto prefix = first ? std::string_view(tableSizeUpdate) : std::string_view{};
        const auto chunk = std::min<std::size_t>(headerBlock.size() - offset, maxFrame - prefix.size());
        const bool last = offset + chunk == headerBlock.size();
        const auto flags = static_cast<std::uint8_t>((last ? kHttp2FlagEndHeaders : 0) | (first && http2EndsStream(endStream) ? kHttp2FlagEndStream : 0));
        output_.appendFrame(first ? Http2FrameType::kHeaders : Http2FrameType::kContinuation, flags, stream.id(), prefix, headerBlock.substr(offset, chunk));
        offset += chunk;
        first = false;
    }
    if (!tableSizeUpdate.empty()) {
        encoderTableSizeUpdatePending_ = false;
    }
}

Http2BufferedResponseHeadSubmitResult Http2Connection::submitResponseHead(std::uint32_t streamId, const HttpResponse& response, HttpBufferedResponseWritePlan writePlan) {
    auto* stream = findStream(streamId);
    if (stream == nullptr || stream->isAborted()) {
        return Http2BufferedResponseHeadSubmitResult::makeClosedFailure();
    }
    if (role_ != Http2Role::kServer || !http2RemoteFinalHeadDecoded(*stream) || stream->localSend().headPending() == nullptr) {
        return Http2BufferedResponseHeadSubmitResult::makeInvalidStateFailure();
    }
    if (writePlan.requestMethod() != stream->requestKnownMethod() || !writePlan.matchesResponse(response)) {
        return Http2BufferedResponseHeadSubmitResult::makeResponsePlanMismatchFailure();
    }
    const bool successfulConnect = response.status().isSuccessful() && stream->tunnel().pending() != nullptr;
    if (successfulConnect) {
        return Http2BufferedResponseHeadSubmitResult::makeInvalidStateFailure();
    }
    const auto controlResult = http2FinalResponseControlPlan(response);
    const auto* http2Control = controlResult.control();
    if (http2Control == nullptr) {
        return Http2BufferedResponseHeadSubmitResult::makeInvalidMessageFailure();
    }

    const auto headPlanResult = http2BufferedResponseHeadPlan(writePlan, response);
    const auto* headPlan = headPlanResult.plan();
    if (headPlan == nullptr) {
        const auto error = headPlanResult.failure()->error();
        const bool responsePlanMismatch = error == Http2ResponseHeadPlanError::kResponseStatusMismatch || error == Http2ResponseHeadPlanError::kResponseRepresentationMismatch;
        return responsePlanMismatch ? Http2BufferedResponseHeadSubmitResult::makeResponsePlanMismatchFailure() : Http2BufferedResponseHeadSubmitResult::makeInvalidMessageFailure();
    }
    if (!appendHttp2ResponseHeaders(*stream, response, *headPlan, *http2Control)) {
        return Http2BufferedResponseHeadSubmitResult::makeInvalidMessageFailure();
    }
    const auto endStream = writePlan.sendBody() ? Http2EndStream::kKeepOpen : Http2EndStream::kEndStream;
    if (headPlan->bodyPlan().bodySuppressed()) {
        stream->beginLocalContentForbidden();
    } else {
        stream->beginLocalContentKnownLength(writePlan.contentLength());
    }
    appendResponseHeaderFrames(*stream, std::string_view(stream->responseHeaderBlock()), endStream);
    if (http2EndsStream(endStream)) {
        (void)stream->commitLocalHeadEndStream();
    } else {
        (void)stream->beginLocalResponseContent();
    }
    if (stream->tunnel().pending() != nullptr) {
        (void)stream->rejectConnect();
    }
    http2ReleaseResponseHeaderBlock(*stream);
    return Http2BufferedResponseHeadSubmitResult::makeSubmitted(std::move(writePlan));
}

Http2StreamingResponseHeadSubmitResult Http2Connection::submitStreamingResponseHead(std::uint32_t streamId, HttpResponse head, ResponseStreamKind kind, ResponseTrailerIntent trailerIntent) {
    auto* stream = findStream(streamId);
    if (stream == nullptr || stream->isAborted()) {
        return Http2StreamingResponseHeadSubmitResult::makeClosedFailure();
    }
    const bool successfulConnect = head.status().isSuccessful() && stream->tunnel().pending() != nullptr;
    if (role_ != Http2Role::kServer || !http2RemoteFinalHeadDecoded(*stream) || stream->localSend().headPending() == nullptr || successfulConnect) {
        return Http2StreamingResponseHeadSubmitResult::makeInvalidStateFailure();
    }
    const auto controlResult = http2FinalResponseControlPlan(head);
    const auto* http2Control = controlResult.control();
    if (http2Control == nullptr) {
        return Http2StreamingResponseHeadSubmitResult::makeInvalidMessageFailure();
    }
    auto preparedCommitPlan = httpResponseStreamCommitPlan(ResponseStreamFraming::kHttp2Frames, stream->requestKnownMethod(), head.status(), trailerIntent);
    auto streamHead = prepareResponseStreamHead(std::move(head), kind, std::move(preparedCommitPlan));
    const auto& commitPlan = streamHead.commitPlan();
    // One prepared plan owns both the encoded Content-Length metadata and the
    // local DATA accounting contract. Explicit length is parsed exactly once;
    // absence remains unbounded, while content-forbidden responses never become
    // DATA-open.
    const auto headPlanResult = http2StreamingResponseHeadPlan(commitPlan.bodyPlan(), streamHead.response());
    const auto* headPlan = headPlanResult.plan();
    if (headPlan == nullptr) {
        return Http2StreamingResponseHeadSubmitResult::makeInvalidMessageFailure();
    }
    if (!appendHttp2ResponseHeaders(*stream, streamHead.response(), *headPlan, *http2Control)) {
        return Http2StreamingResponseHeadSubmitResult::makeInvalidMessageFailure();
    }
    const auto endStream = commitPlan.headDisposition() == ResponseStreamHeadDisposition::kMessageEnded ? Http2EndStream::kEndStream : Http2EndStream::kKeepOpen;
    if (headPlan->bodyPlan().bodySuppressed()) {
        stream->beginLocalContentForbidden();
    } else if (const auto contentLength = headPlan->streamingContentLength()) {
        stream->beginLocalContentKnownLength(*contentLength);
    } else {
        stream->beginLocalContentUnbounded();
    }
    appendResponseHeaderFrames(*stream, std::string_view(stream->responseHeaderBlock()), endStream);
    if (commitPlan.headDisposition() == ResponseStreamHeadDisposition::kTrailersOnly) {
        (void)stream->beginLocalResponseTrailersOnly();
    } else {
        if (http2EndsStream(endStream)) {
            (void)stream->commitLocalHeadEndStream();
        } else {
            (void)stream->beginLocalResponseContent();
        }
    }
    if (stream->tunnel().pending() != nullptr) {
        (void)stream->rejectConnect();
    }
    http2ReleaseResponseHeaderBlock(*stream);
    return Http2StreamingResponseHeadSubmitResult::makeSubmitted(commitPlan);
}

Http2SubmitStatus Http2Connection::submitInterimResponseHead(std::uint32_t streamId, const HttpInterimResponseHead& response) {
    auto* stream = findStream(streamId);
    if (stream == nullptr || stream->isAborted()) {
        return Http2SubmitStatus::kClosed;
    }
    if (role_ != Http2Role::kServer || !http2RemoteFinalHeadDecoded(*stream) || stream->localSend().headPending() == nullptr) {
        return Http2SubmitStatus::kInvalidState;
    }
    if (appendHttp2InterimResponseHeaders(*stream, response) != Http2InterimResponseHeaderEncodeStatus::kOk) {
        return Http2SubmitStatus::kInvalidMessage;
    }
    appendResponseHeaderFrames(*stream, std::string_view(stream->responseHeaderBlock()), Http2EndStream::kKeepOpen);
    http2ReleaseResponseHeaderBlock(*stream);
    return Http2SubmitStatus::kAccepted;
}

Http2DataSubmitStatus Http2Connection::submitData(std::uint32_t streamId, std::string_view chunk, Http2EndStream endStream) {
    auto* stream = findStream(streamId);
    if (stream == nullptr || stream->isAborted()) {
        return Http2DataSubmitStatus::kClosed;
    }
    const auto& localSend = stream->localSend();
    if (localSend.requestContentOpen() == nullptr && localSend.responseContentOpen() == nullptr && localSend.tunnelOpen() == nullptr) {
        return Http2DataSubmitStatus::kInvalidState;
    }
    // One queued submission per stream is the hard backpressure boundary. The
    // current input remains caller-owned and can be retried after the prior one drains.
    for (const auto& pending : pendingSends_) {
        if (pending.streamId == streamId) {
            return Http2DataSubmitStatus::kBackpressured;
        }
    }
    switch (stream->checkLocalContentAccept(chunk.size(), http2EndsStream(endStream))) {
        case Http2LocalContentCheck::kAccepted:
            break;
        case Http2LocalContentCheck::kNotStarted:
        case Http2LocalContentCheck::kForbidden:
            return Http2DataSubmitStatus::kInvalidState;
        case Http2LocalContentCheck::kLengthExceeded:
            return Http2DataSubmitStatus::kContentLengthExceeded;
        case Http2LocalContentCheck::kLengthIncomplete:
            return Http2DataSubmitStatus::kContentLengthIncomplete;
    }
    // Prepare every allocation needed for a deferred suffix BEFORE accepting the
    // input or consuming flow-control window. A recoverable allocation failure can
    // therefore never leave a framed prefix without its core-owned remainder.
    std::optional<Http2PendingSend> deferred;
    if (!chunk.empty()) {
        const auto immediateBytes = std::min(chunk.size(), http2AvailableSendWindow(connectionSendWindow_, *stream));
        if (immediateBytes < chunk.size()) {
            std::pmr::string remainder(resource_);
            remainder.append(chunk.data() + immediateBytes, chunk.size() - immediateBytes);
            pendingSends_.reserve(pendingSends_.size() + 1);
            deferred.emplace(Http2PendingSend{streamId, std::move(remainder), 0, endStream, std::pmr::string(resource_)});
        }
    }
    // Accepted means ownership of the WHOLE input, even when flow control below
    // can only materialize a prefix and the prepared suffix becomes pending.
    stream->acceptLocalContent(chunk.size());
    if (chunk.empty()) {
        if (http2EndsStream(endStream)) {
            output_.appendFrame(Http2FrameType::kData, kHttp2FlagEndStream, streamId, {});
            (void)stream->commitLocalEndStream();
            releaseLocalRequestStreamIfClosed(*stream);
        }
        return Http2DataSubmitStatus::kAccepted;
    }
    const auto consumed = sendDataUpToWindow(*stream, chunk, 0, endStream);
    if (consumed < chunk.size()) {
        // immediateBytes above is the exact total that sendDataUpToWindow can
        // consume from the current windows, so a deferred value must exist here.
        pendingSends_.push_back(std::move(*deferred));
        if (http2EndsStream(endStream)) {
            (void)stream->queueLocalEndStream();
        }
        return Http2DataSubmitStatus::kQueued;
    }
    if (http2EndsStream(endStream)) {
        (void)stream->commitLocalEndStream();
        releaseLocalRequestStreamIfClosed(*stream);
    }
    return Http2DataSubmitStatus::kAccepted;
}

Http2SubmitStatus Http2Connection::submitConnectResponseHead(std::uint32_t streamId, const HttpResponse& response) {
    auto* stream = findStream(streamId);
    if (stream == nullptr || stream->isAborted()) {
        return Http2SubmitStatus::kClosed;
    }
    if (role_ != Http2Role::kServer || !http2RemoteFinalHeadDecoded(*stream) || stream->localSend().headPending() == nullptr || stream->tunnel().pending() == nullptr) {
        return Http2SubmitStatus::kInvalidState;
    }
    if (!http2IsValidConnectResponseHead(response)) {
        return Http2SubmitStatus::kInvalidMessage;
    }
    const auto controlResult = http2FinalResponseControlPlan(response);
    const auto* http2Control = controlResult.control();
    if (http2Control == nullptr) {
        return Http2SubmitStatus::kInvalidMessage;
    }
    const auto headPlanResult = http2ConnectResponseHeadPlan(httpResponseBodyPlan(HttpKnownMethod::kConnect, response.status()));
    const auto* headPlan = headPlanResult.plan();
    if (headPlan == nullptr) {
        return Http2SubmitStatus::kInvalidMessage;
    }
    if (!appendHttp2ResponseHeaders(*stream, response, *headPlan, *http2Control)) {
        return Http2SubmitStatus::kInvalidMessage;
    }
    appendResponseHeaderFrames(*stream, std::string_view(stream->responseHeaderBlock()), Http2EndStream::kKeepOpen);
    (void)stream->acceptConnect();
    stream->beginLocalContentUnbounded();
    (void)stream->openLocalConnectTunnel();
    http2ReleaseResponseHeaderBlock(*stream);
    if (http2RemotePeerHalfClosed(*stream)) {
        events_.push_back(Http2Event::tunnelEnd(streamId));
    }
    return Http2SubmitStatus::kAccepted;
}

Http2WebSocketHandshakeSubmitResult Http2Connection::submitWebSocketHandshake(std::uint32_t streamId, WebSocketServerNegotiation&& negotiation) {
    auto* stream = findStream(streamId);
    if (stream == nullptr || stream->isAborted()) {
        return Http2WebSocketHandshakeSubmitResult::makeFailure(Http2WebSocketHandshakeSubmitError::kClosed);
    }
    if (role_ != Http2Role::kServer || !http2RemoteFinalHeadDecoded(*stream) || stream->localSend().headPending() == nullptr || !http2IsPendingWebSocketConnect(*stream)) {
        return Http2WebSocketHandshakeSubmitResult::makeFailure(Http2WebSocketHandshakeSubmitError::kInvalidState);
    }
    http2EncodeWebSocketHandshakeHeaders(stream->responseHeaderBlock(), negotiation);
    appendResponseHeaderFrames(*stream, std::string_view(stream->responseHeaderBlock()), Http2EndStream::kKeepOpen);
    (void)stream->acceptConnect();
    stream->beginLocalContentUnbounded();
    (void)stream->openLocalConnectTunnel();
    http2ReleaseResponseHeaderBlock(*stream);
    if (http2RemotePeerHalfClosed(*stream)) {
        events_.push_back(Http2Event::tunnelEnd(streamId));
    }
    return Http2WebSocketHandshakeSubmitResult::makeSubmitted(std::move(negotiation));
}

Http2FinishSubmitStatus Http2Connection::finishResponse(std::uint32_t streamId, const HttpResponseTrailerSection& trailers) {
    auto* stream = findStream(streamId);
    if (stream == nullptr || stream->isAborted()) {
        return Http2FinishSubmitStatus::kClosed;
    }
    if (stream->localSend().responseContentOpen() == nullptr && stream->localSend().responseTrailersOnly() == nullptr) {
        return Http2FinishSubmitStatus::kInvalidState;
    }
    if (!stream->localContent().lengthComplete()) {
        return Http2FinishSubmitStatus::kContentLengthIncomplete;
    }
    if (stream->localSend().responseTrailersOnly() != nullptr && trailers.empty()) {
        // A trailers-only response cannot fall back to DATA(END_STREAM): its
        // method/status explicitly forbids DATA, including an empty terminal frame.
        return Http2FinishSubmitStatus::kInvalidState;
    }
    // The entire semantic trailer section was validated before the initial head
    // commit and is encoded in detached
    // storage before output, pending DATA, or stream phase changes. It either joins
    // the terminal transaction whole or leaves no per-stream staged side channel.
    std::pmr::string trailerBlock(resource_);
    appendHttp2ResponseTrailers(trailerBlock, trailers);
    // If the body still has a window-blocked remainder, the trailer HEADERS must NOT
    // jump ahead of that queued DATA. Stash it on the pending entry and move END_STREAM
    // from the body to the trailer (markSendWindowOpened emits it once the body drains).
    for (auto& pending : pendingSends_) {
        if (pending.streamId == streamId) {
            if (trailerBlock.empty()) {
                pending.endStream = Http2EndStream::kEndStream;
            } else {
                pending.endStream = Http2EndStream::kKeepOpen;
                pending.trailerBlock.swap(trailerBlock);
            }
            (void)stream->queueLocalEndStream();
            return Http2FinishSubmitStatus::kQueued;
        }
    }
    if (trailerBlock.empty()) {
        output_.appendFrame(Http2FrameType::kData, kHttp2FlagEndStream, streamId, {});
        (void)stream->commitLocalEndStream();
        return Http2FinishSubmitStatus::kAccepted;
    }
    appendResponseHeaderFrames(*stream, std::string_view(trailerBlock), Http2EndStream::kEndStream);
    (void)stream->commitLocalEndStream();
    return Http2FinishSubmitStatus::kAccepted;
}

Http2SubmitStatus Http2Connection::submitReset(std::uint32_t streamId, Http2ErrorCode error) {
    if (streamId == 0) {
        return Http2SubmitStatus::kInvalidState;
    }
    auto* stream = findStream(streamId);
    if (stream == nullptr) {
        return closedStreams_.source(streamId).has_value() ? Http2SubmitStatus::kClosed : Http2SubmitStatus::kInvalidState;
    }
    if (stream->isAborted()) {
        return Http2SubmitStatus::kClosed;
    }
    // A client-created stream is still RFC-idle until its request HEADERS are
    // submitted; RST_STREAM on that state would make the peer close the connection.
    // A server owner does not own a peer stream until its initial header block has
    // decoded; rejecting an early reset also preserves the mandatory CONTINUATION run.
    if ((role_ == Http2Role::kClient && stream->localSend().headPending() != nullptr) || (role_ == Http2Role::kServer && !http2RemoteFinalHeadDecoded(*stream)) || http2StreamIsClosed(*stream)) {
        return Http2SubmitStatus::kInvalidState;
    }
    output_.appendRstStream(streamId, error);
    return closeStreamByOwner(streamId) ? Http2SubmitStatus::kAccepted : Http2SubmitStatus::kClosed;
}

}  // namespace ruvia::detail
