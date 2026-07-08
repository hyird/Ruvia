#include "Http2Connection.h"

#include <utility>

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

Http2FeedResult Http2Connection::feed(std::string_view /*in*/) {
    // TODO: port readFrame + processFrame here (frame codec + HPACK + stream state
    // are already pure; append control-frame output to outBuffer_, push kRequest*
    // events, and mark unblocked streams instead of resuming coroutines).
    return {0, Http2FeedStatus::kNeedMore};
}

Http2StreamState* Http2Connection::stream(std::uint32_t /*streamId*/) noexcept {
    // TODO: return streams_.find(streamId)
    return nullptr;
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
    // TODO: port sendLocalSettings -> outBuffer_.
}

}  // namespace ruvia::detail
