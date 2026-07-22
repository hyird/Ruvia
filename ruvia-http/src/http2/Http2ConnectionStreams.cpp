#include "ruvia/http/detail/http2/Http2Connection.h"

#include <utility>

#include "ruvia/http/detail/http2/Http2FrameCodec.h"
#include "ruvia/http/detail/http2/Http2FramePayload.h"
#include "ruvia/http/detail/http2/Http2RemoteReceiveSemantics.h"
#include "ruvia/http/detail/http2/Http2ResponseHeaders.h"

// The stream table and a stream's life: creating and finding one, pinning it
// while the owner still holds a reference, closing it, and the RST_STREAM
// handling that includes the rapid-reset budget.

namespace ruvia::detail {

namespace {

// Rapid-reset (CVE-2023-44487): a peer can open a stream, let HEADERS complete so the
// owner spawns a handler, then RST it to free the concurrency slot -- repeating forever
// without ever tripping the 128-stream cap. We allow the peer to reset up to this many
// MORE streams than it has let run to completion; a flood that never lets a response
// finish climbs to the cap and trips, while legitimate cancels interleaved with
// completed responses keep refilling the budget and never trip.
constexpr std::uint32_t kHttp2MaxUnprocessedResets = 1000;

}  // namespace

bool Http2Connection::isPinned(std::uint32_t streamId) const noexcept {
    return std::ranges::find(pinnedStreams_, streamId) != pinnedStreams_.end();
}

void Http2Connection::pinStream(std::uint32_t streamId) {
    if (!isPinned(streamId)) {
        pinnedStreams_.push_back(streamId);
    }
}

void Http2Connection::detachActiveHeaderBlock(Http2StreamState& stream) {
    if (!headerContinuation_.matches(stream.id())) {
        return;
    }
    if (headerContinuation_.kind() == Http2HeaderBlockKind::kDiscarded) {
        // An owner-side terminal transition won the race while an already-invalid
        // block was still awaiting CONTINUATION. The RST just emitted is now the final
        // local frame; finish decoding the peer block later without emitting another.
        discardedHeaderAction_ = DiscardedHeaderAction::kIgnore;
        return;
    }
    if (discardedHeaderStream_) {
        appendGoaway(Http2ErrorCode::kProtocolError, "overlapping detached HEADERS state");
        return;
    }
    discardedHeaderStream_.emplace(stream.id(), resource_);
    // Both strings use the connection resource, so the partial compressed block moves
    // without allocation. Future CONTINUATION fragments complete it in detached state.
    discardedHeaderStream_->requestHeaderBlock().swap(stream.requestHeaderBlock());
    discardedHeaderAction_ = DiscardedHeaderAction::kIgnore;
    headerContinuation_.start(stream.id(), Http2HeaderBlockKind::kDiscarded);
}

void Http2Connection::unpinStream(std::uint32_t streamId) {
    std::erase(pinnedStreams_, streamId);
    auto* stream = streams_.find(streamId);
    if (stream == nullptr) {
        return;  // never created, or already removed
    }
    if (stream->isAborted()) {
        // The abnormal terminal transition already returned flow-control debt and
        // discarded deferred sends. The pin only kept request-view storage alive.
        releaseLocalRequestStream(*stream);
        streams_.remove(streamId);
        return;
    }

    if (http2StreamIsClosed(*stream)) {
        // Both protocol halves are closed and the owner lease is gone: normal
        // completion can finally release storage and refill the rapid-reset budget.
        detachActiveHeaderBlock(*stream);
        discardDeferredStreamState(streamId);
        flushWindowDebt(*stream);
        readyQueue_.remove(streamId);
        closedStreams_.remember(streamId, Http2StreamCloseSource::kLocal);
        ++completedResponses_;
        releaseLocalRequestStreamIfClosed(*stream);
        streams_.remove(streamId);
        return;
    }

    // Releasing the last owner while either protocol half is still open must not
    // silently erase the stream. Abort it explicitly so the peer observes a legal
    // terminal transition and the table cannot leak an ownerless half-open stream.
    const auto error = stream->localSend().endStreamCommitted() != nullptr
        ? Http2ErrorCode::kNoError
        : Http2ErrorCode::kCancel;
    (void)submitReset(streamId, error);
}

void Http2Connection::discardDeferredStreamState(std::uint32_t streamId) {
    std::erase_if(
        pendingSends_,
        [streamId](const Http2PendingSend& pending) {
            return pending.streamId == streamId;
        });
    std::erase(drainedDataStreams_, streamId);
    if (auto* stream = streams_.find(streamId); stream != nullptr) {
        http2ReleaseResponseHeaderBlock(*stream);
    }
}

bool Http2Connection::closeStreamImpl(
    std::uint32_t streamId,
    Http2StreamCloseSource source,
    Http2ErrorCode error,
    CloseNotification notification) {
    auto* stream = streams_.find(streamId);
    if (stream != nullptr && !stream->isAborted()) {
        detachActiveHeaderBlock(*stream);
    }
    readyQueue_.remove(streamId);
    discardDeferredStreamState(streamId);
    if (stream == nullptr || stream->isAborted()) {
        return false;
    }

    releaseLocalRequestStream(*stream);
    (void)stream->abort(source);
    if (notification == CloseNotification::kEmitEvent) {
        events_.push_back(Http2Event::streamClosed(streamId, source, error));
    }
    // A pin protects request views only. Protocol resources and connection-level
    // flow credit are released immediately even if a sleeping handler retains storage.
    flushWindowDebt(*stream);
    closedStreams_.remember(streamId, source);
    if (!isPinned(streamId)) {
        streams_.remove(streamId);
    }
    return true;
}

bool Http2Connection::closeStream(
    std::uint32_t streamId,
    Http2StreamCloseSource source,
    Http2ErrorCode error) {
    return closeStreamImpl(
        streamId, source, error, CloseNotification::kEmitEvent);
}

bool Http2Connection::closeStreamByOwner(std::uint32_t streamId) {
    return closeStreamImpl(
        streamId,
        Http2StreamCloseSource::kLocal,
        Http2ErrorCode::kNoError,
        CloseNotification::kOwnerAlreadyKnows);
}

bool Http2Connection::wasClosedByPeerReset(
    std::uint32_t streamId,
    const Http2StreamState* retainedStream) const noexcept {
    if (retainedStream != nullptr) {
        return retainedStream->isAborted() &&
            retainedStream->localSend().aborted()->source() ==
                Http2StreamCloseSource::kPeer;
    }
    return closedStreams_.source(streamId) ==
        Http2StreamCloseSource::kPeer;
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
    auto* const stream = streams_.find(header.streamId);
    if (stream == nullptr &&
        isIdleStreamId(header.streamId)) {
        appendGoaway(Http2ErrorCode::kProtocolError, "RST_STREAM on idle stream");
        return false;
    }
    if (wasClosedByPeerReset(header.streamId, stream)) {
        // This peer's two resets are ordered on the same connection, so the
        // second cannot have been sent before it knew that it had terminated
        // the stream. Enforce the same peer-reset finality as DATA/HEADERS and
        // avoid counting a duplicate as another rapid-reset lifecycle.
        appendGoaway(
            Http2ErrorCode::kStreamClosed,
            "RST_STREAM after peer RST_STREAM");
        return false;
    }
    if (stream != nullptr && http2StreamIsClosed(*stream)) {
        // RST_STREAM can race with END_STREAM or a reset sent by this endpoint.
        // The stream was already terminal, so minimally process without charging
        // another peer-reset lifecycle.
        return true;
    }
    if (closedStreams_.source(header.streamId) ==
        Http2StreamCloseSource::kPeerGoaway) {
        // The peer already declared this request unprocessed. A trailing reset has no
        // stream lifecycle left to mutate and must not consume the rapid-reset budget.
        return true;
    }
    const auto error = static_cast<Http2ErrorCode>(http2Read32(
        reinterpret_cast<const unsigned char*>(payload.data())));
    if (!closeStream(header.streamId, Http2StreamCloseSource::kPeer, error)) {
        // A reset can race with one sent by this endpoint. RFC 9113 requires
        // minimal processing in that state, but the no-op neither opened a
        // handler slot nor freed one and therefore must not spend the
        // rapid-reset lifecycle budget.
        return true;
    }
    // Rapid-reset budget (CVE-2023-44487): count peer resets and trip if they run too far
    // ahead of the responses this connection has actually let complete.
    ++peerResetStreams_;
    if (peerResetStreams_ > completedResponses_ + kHttp2MaxUnprocessedResets) {
        appendGoaway(Http2ErrorCode::kEnhanceYourCalm, "excessive stream resets");
        return false;
    }
    return true;
}

Http2StreamState* Http2Connection::findStream(std::uint32_t streamId) noexcept {
    return streams_.find(streamId);
}

Http2StreamState* Http2Connection::createStream(std::uint32_t streamId) {
    return streams_.create(streamId, peerSettings_.initialWindowSize());
}

std::optional<Http2RequestHeadSubmitError>
Http2Connection::localRequestAdmissionError() const noexcept {
    if (role_ != Http2Role::kClient) {
        return Http2RequestHeadSubmitError::kInvalidState;
    }
    if (prefacePhase_ == PrefacePhase::kNotStarted) {
        return Http2RequestHeadSubmitError::kConnectionNotStarted;
    }
    if (localConnectionState_.fatalFailure() != nullptr ||
        peerGoaway_.has_value() || nextLocalStreamId_ > 0x7fffffffU) {
        return Http2RequestHeadSubmitError::kConnectionUnavailable;
    }
    if (activeLocalRequestStreams_ >= peerSettings_.maxConcurrentStreams()) {
        return Http2RequestHeadSubmitError::kPeerStreamLimitReached;
    }
    return std::nullopt;
}

Http2StreamState* Http2Connection::admitLocalRequestStream() {
    auto* stream = createStream(nextLocalStreamId_);
    if (stream != nullptr) {
        nextLocalStreamId_ += 2;
    }
    return stream;
}

void Http2Connection::activateLocalRequestStream(Http2StreamState& stream) noexcept {
    if (stream.holdPeerConcurrencySlot()) {
        ++activeLocalRequestStreams_;
    }
}

void Http2Connection::releaseLocalRequestStream(Http2StreamState& stream) noexcept {
    if (stream.releasePeerConcurrencySlot() && activeLocalRequestStreams_ != 0) {
        --activeLocalRequestStreams_;
    }
}

void Http2Connection::releaseLocalRequestStreamIfClosed(Http2StreamState& stream) noexcept {
    if (http2StreamIsClosed(stream)) {
        releaseLocalRequestStream(stream);
    }
}

bool Http2Connection::isIdleStreamId(std::uint32_t streamId) const noexcept {
    if (role_ == Http2Role::kClient) {
        // No server-initiated streams exist (push is never enabled), so every even id
        // is idle, as is any odd id this endpoint has not opened yet.
        return (streamId & 1U) == 0 || streamId >= nextLocalStreamId_;
    }
    return http2IsIdleStream(streamId, lastStreamId_);
}

Http2StreamState* Http2Connection::stream(
    std::uint32_t streamId) & noexcept {
    return streams_.find(streamId);
}

}  // namespace ruvia::detail
