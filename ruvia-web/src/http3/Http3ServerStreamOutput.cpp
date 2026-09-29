#include "ruvia/web/detail/http3/Http3ServerStreamOutput.h"

#include <bit>
#include <exception>
#include <limits>
#include <stdexcept>

#include "ruvia/core/memory/MemoryPool.h"
#include "ruvia/core/memory/PmrResource.h"
#include "ruvia/http/Http3PeerStreams.h"
#include "ruvia/http/Http3VarInt.h"

namespace ruvia::detail {
namespace {

std::uint64_t hashStreamId(std::uint64_t value) noexcept {
    value ^= value >> 30;
    value *= 0xbf58476d1ce4e5b9ULL;
    value ^= value >> 27;
    value *= 0x94d049bb133111ebULL;
    value ^= value >> 31;
    return value;
}

}  // namespace

Http3ServerStreamOutput::Http3ServerStreamOutput(Http3QuicServerTransport& transport,
    ConnectionId connectionId, WorkerMemory& worker, std::uint64_t epoch,
    std::uint64_t connectionGeneration, Http3ServerStreamOutputConfig config)
    : Http3ServerStreamOutput(transport, connectionId, worker.resource(), epoch,
          connectionGeneration, config) {}

Http3ServerStreamOutput::Http3ServerStreamOutput(Http3QuicServerTransport& transport,
    ConnectionId connectionId, std::pmr::memory_resource* resource, std::uint64_t epoch,
    std::uint64_t connectionGeneration, Http3ServerStreamOutputConfig config)
    : transport_(transport),
      connectionId_(connectionId),
      epoch_(epoch),
      connectionGeneration_(connectionGeneration),
      maxTrackedStreams_(config.maxTrackedStreams),
      maxQueuedBlocks_(config.maxQueuedBlocks),
      maxDriveWorkItems_(config.maxDriveWorkItems),
      writeTimeout_(config.writeTimeout),
      ownerThread_(std::this_thread::get_id()),
      streams_(pmrResourceOrDefault(resource)),
      nodes_(pmrResourceOrDefault(resource)) {
    if (maxTrackedStreams_ == 0 || maxQueuedBlocks_ == 0 ||
        maxQueuedBlocks_ >= kNoNode || maxDriveWorkItems_ == 0 ||
        (writeTimeout_ && writeTimeout_->count() <= 0)) {
        throw std::invalid_argument("HTTP/3 server output capacities must be nonzero and representable");
    }
    if (!transport_.connectionInfo(connectionId_)) {
        throw std::invalid_argument("HTTP/3 server output requires a live transport connection");
    }
    streams_.resize(tableCapacity(maxTrackedStreams_));
    nodes_.resize(maxQueuedBlocks_);
    for (std::size_t i = 0; i < nodes_.size(); ++i) {
        nodes_[i].next = i + 1 == nodes_.size()
                             ? kNoNode
                             : static_cast<std::uint32_t>(i + 1);
    }
    freeNode_ = 0;
}

Http3ServerStreamOutput::~Http3ServerStreamOutput() {
    if (std::this_thread::get_id() != ownerThread_) {
        std::terminate();
    }
    if (retirementFailed_ || queuedBlockCount_ != 0) {
        std::terminate();
    }
    for (const auto& slot : streams_) {
        if (slot.occupied &&
            (slot.info.state == StreamState::kStopping || !isTerminal(slot.info.state))) {
            std::terminate();
        }
    }
}

void Http3ServerStreamOutput::requireOwnerThread() const {
    if (std::this_thread::get_id() != ownerThread_) {
        throw std::logic_error("HTTP/3 server output used outside its server network thread");
    }
}

Http3ServerStreamOutput::IdentityStatus Http3ServerStreamOutput::identityStatus(
    const Http3StreamMessageId& id) const noexcept {
    if (id.epoch != epoch_) {
        return IdentityStatus::kForeignEpoch;
    }
    if (id.connectionGeneration != connectionGeneration_) {
        return IdentityStatus::kStaleConnection;
    }
    return IdentityStatus::kMatch;
}

bool Http3ServerStreamOutput::validRequestStreamId(StreamId streamId) const noexcept {
    return isHttp3RequestStreamId(streamId);
}

Http3ServerStreamOutput::StreamSlot* Http3ServerStreamOutput::findStream(StreamId streamId) noexcept {
    const auto mask = streams_.size() - 1;
    auto index = static_cast<std::size_t>(hashStreamId(streamId)) & mask;
    for (std::size_t probes = 0; probes < streams_.size(); ++probes) {
        auto& slot = streams_[index];
        if (!slot.occupied) {
            return nullptr;
        }
        if (slot.info.streamId == streamId) {
            return &slot;
        }
        index = (index + 1) & mask;
    }
    return nullptr;
}

const Http3ServerStreamOutput::StreamSlot*
Http3ServerStreamOutput::findStream(StreamId streamId) const noexcept {
    const auto mask = streams_.size() - 1;
    auto index = static_cast<std::size_t>(hashStreamId(streamId)) & mask;
    for (std::size_t probes = 0; probes < streams_.size(); ++probes) {
        const auto& slot = streams_[index];
        if (!slot.occupied) {
            return nullptr;
        }
        if (slot.info.streamId == streamId) {
            return &slot;
        }
        index = (index + 1) & mask;
    }
    return nullptr;
}

Http3ServerStreamOutput::StreamSlot*
Http3ServerStreamOutput::findOrCreateStream(StreamId streamId) {
    const auto mask = streams_.size() - 1;
    auto index = static_cast<std::size_t>(hashStreamId(streamId)) & mask;
    for (std::size_t probes = 0; probes < streams_.size(); ++probes) {
        auto& slot = streams_[index];
        if (!slot.occupied) {
            if (trackedStreamCount_ >= maxTrackedStreams_) {
                return nullptr;
            }
            slot.info = StreamInfo{.streamId = streamId};
            slot.head = kNoNode;
            slot.tail = kNoNode;
            slot.occupied = true;
            ++trackedStreamCount_;
            return &slot;
        }
        if (slot.info.streamId == streamId) {
            return &slot;
        }
        index = (index + 1) & mask;
    }
    return nullptr;
}

bool Http3ServerStreamOutput::isTerminal(StreamState state) const noexcept {
    return state == StreamState::kStopping || state == StreamState::kFinished ||
           state == StreamState::kReset || state == StreamState::kCancelled ||
           state == StreamState::kFailed || state == StreamState::kConnectionClosed;
}

Http3ServerStreamOutput::Result Http3ServerStreamOutput::acceptData(
    Http3StreamMailbox::BorrowedBlock& block) {
    requireOwnerThread();
    if (!block) {
        return {.status = Status::kInvalidInput};
    }
    const auto identity = identityStatus(block.id());
    if (identity == IdentityStatus::kForeignEpoch) {
        return {.status = Status::kForeignEpoch};
    }
    if (identity == IdentityStatus::kStaleConnection) {
        return {.status = Status::kStaleConnection};
    }
    if (stopped_) {
        return {.status = Status::kStopped};
    }
    const StreamId streamId = block.id().streamId;
    if (!validRequestStreamId(streamId)) {
        return failConnection(Status::kInvalidStreamId);
    }
    if (const auto* existing = findStream(streamId);
        existing != nullptr &&
        (existing->info.state == StreamState::kCancelled ||
            existing->info.state == StreamState::kReset ||
            existing->info.state == StreamState::kFailed ||
            existing->info.state == StreamState::kConnectionClosed)) {
        return {.status = Status::kClosedStream};
    }
    if (freeNode_ == kNoNode) {
        return {.status = Status::kBackpressured};
    }

    auto* slot = findOrCreateStream(streamId);
    if (slot == nullptr) {
        return failConnection(Status::kCapacityExhausted);
    }
    const auto bytes = block.bytes();
    if (slot->info.sendFinAccepted) {
        return bytes.empty() ? Result{.status = Status::kClosedStream}
                             : failConnection(Status::kFinalSizeError);
    }
    if (slot->info.state == StreamState::kFinished && !bytes.empty()) {
        return failConnection(Status::kFinalSizeError);
    }
    if (isTerminal(slot->info.state)) {
        return {.status = Status::kClosedStream};
    }
    if (bytes.size() > std::numeric_limits<std::uint64_t>::max() - slot->info.receivedWireBytes) {
        return failConnection(Status::kFinalSizeError);
    }
    const auto newReceived = slot->info.receivedWireBytes + bytes.size();
    if (newReceived > kHttp3VarIntMax ||
        (slot->info.finalWireBytes && newReceived > *slot->info.finalWireBytes)) {
        return failConnection(Status::kFinalSizeError);
    }
    // A newly queued block is the first real pending output (or new write
    // activity); a deferred FIN alone must not start the timeout clock.
    slot->lastWriteActivity = Http3QuicServerTransport::Clock::now();

    const auto nodeIndex = freeNode_;
    auto& node = nodes_[nodeIndex];
    freeNode_ = node.next;
    node.next = kNoNode;
    node.offset = 0;
    node.block = std::move(block);
    if (slot->tail == kNoNode) {
        slot->head = nodeIndex;
    } else {
        nodes_[slot->tail].next = nodeIndex;
    }
    slot->tail = nodeIndex;
    slot->info.receivedWireBytes = newReceived;
    slot->info.queuedWireBytes += bytes.size();
    ++slot->info.queuedBlocks;
    ++queuedBlockCount_;
    if (slot->info.finalWireBytes) {
        slot->info.state = StreamState::kFinPending;
    }
    requestScan();
    return {.status = Status::kAccepted};
}

Http3ServerStreamOutput::Result Http3ServerStreamOutput::acceptControl(
    const Http3StreamControl& control) {
    requireOwnerThread();
    const auto identity = identityStatus(control.id);
    if (identity == IdentityStatus::kForeignEpoch) {
        return {.status = Status::kForeignEpoch};
    }
    if (identity == IdentityStatus::kStaleConnection) {
        return {.status = Status::kStaleConnection};
    }
    if (control.kind == Http3StreamControl::Kind::kConnectionClosed) {
        if (stopped_ && stopComplete_) {
            return {.status = Status::kConnectionClosed};
        }
        stopped_ = true;
        for (auto& slot : streams_) {
            if (slot.occupied && !isTerminal(slot.info.state)) {
                slot.info.state = StreamState::kConnectionClosed;
            }
        }
        const bool retired = closeConnectionAndRelease();
        if (!retired) {
            return {.status = Status::kUnsafeToRelease};
        }
        stopComplete_ = true;
        return {.status = Status::kConnectionClosed};
    }
    if (stopped_) {
        return {.status = Status::kStopped};
    }
    if (!validRequestStreamId(control.id.streamId)) {
        return failConnection(Status::kInvalidStreamId);
    }
    auto* slot = findOrCreateStream(control.id.streamId);
    if (slot == nullptr) {
        return failConnection(Status::kCapacityExhausted);
    }
    if (isTerminal(slot->info.state)) {
        if (slot->info.state == StreamState::kFinished &&
            control.kind == Http3StreamControl::Kind::kStreamFin &&
            slot->info.finalWireBytes && *slot->info.finalWireBytes == control.value) {
            return {.status = Status::kDuplicateFin};
        }
        if (slot->info.state == StreamState::kFinished &&
            control.kind == Http3StreamControl::Kind::kStreamFin &&
            slot->info.finalWireBytes && *slot->info.finalWireBytes != control.value) {
            return failConnection(Status::kFinalSizeError);
        }
        return {.status = Status::kClosedStream};
    }

    switch (control.kind) {
        case Http3StreamControl::Kind::kConnectionClosed:
            break;
        case Http3StreamControl::Kind::kStreamReset: {
            slot->info.state = StreamState::kStopping;
            const bool retired = retireStream(*slot, StreamState::kReset,
                static_cast<std::uint64_t>(control.streamResetErrorCode));
            if (!retired) {
                return {.status = Status::kUnsafeToRelease,
                    .termination = slot->info.termination};
            }
            return {.status = connectionRetired_ ? Status::kConnectionClosed : Status::kReset,
                .termination = slot->info.termination};
        }
        case Http3StreamControl::Kind::kWritable:
            notifyTransportActivity();
            return {.status = Status::kWritable};
        case Http3StreamControl::Kind::kTunnelEstablished:
            return {.status = Status::kInvalidInput};
        case Http3StreamControl::Kind::kStreamFin:
            if (control.value > kHttp3VarIntMax || control.value < slot->info.receivedWireBytes) {
                return failConnection(Status::kFinalSizeError);
            }
            if (slot->info.finalWireBytes) {
                if (*slot->info.finalWireBytes == control.value) {
                    return {.status = Status::kDuplicateFin};
                }
                return failConnection(Status::kFinalSizeError);
            }
            slot->info.finalWireBytes = control.value;
            slot->info.state = StreamState::kFinPending;
            if (control.value == slot->info.acceptedWireBytes &&
                slot->info.queuedWireBytes == 0 && slot->head == kNoNode) {
                slot->lastWriteActivity = Http3QuicServerTransport::Clock::now();
            }
            requestScan();
            const bool finishReady = control.value == slot->info.acceptedWireBytes &&
                                     slot->info.queuedWireBytes == 0 && slot->head == kNoNode;
            return {.status = finishReady ? Status::kAccepted : Status::kFinDeferred};
    }
    return {.status = Status::kClosedStream};
}

Http3ServerStreamOutput::Result Http3ServerStreamOutput::cancelStream(
    StreamId streamId, std::uint64_t errorCode) {
    requireOwnerThread();
    if (stopped_) {
        return {.status = Status::kStopped};
    }
    if (!validRequestStreamId(streamId)) {
        return {.status = Status::kInvalidStreamId};
    }
    if (errorCode > kHttp3VarIntMax) {
        return {.status = Status::kInvalidInput};
    }
    auto* slot = findOrCreateStream(streamId);
    if (slot == nullptr) {
        return failConnection(Status::kCapacityExhausted);
    }
    if (isTerminal(slot->info.state)) {
        return {.status = Status::kClosedStream,
            .termination = slot->info.termination};
    }
    slot->info.state = StreamState::kStopping;
    const bool retired = retireStream(*slot, StreamState::kCancelled, errorCode);
    if (!retired) {
        return {.status = Status::kUnsafeToRelease,
            .termination = slot->info.termination};
    }
    return {.status = connectionRetired_ ? Status::kConnectionClosed : Status::kCancelled,
        .termination = slot->info.termination};
}

Http3ServerStreamOutput::DriveResult Http3ServerStreamOutput::drive() {
    requireOwnerThread();
    DriveResult result;
    if (stopped_) {
        result.status = connectionRetired_ ? Status::kConnectionClosed : Status::kStopped;
        return result;
    }
    if (retirementFailed_) {
        result.status = Status::kUnsafeToRelease;
        return result;
    }
    if (roundRemainingSlots_ == 0) {
        if (!writeTimeout_ || liveStreamCount() == 0) {
            return result;
        }
        roundRemainingSlots_ = streams_.size();
        roundMadeProgress_ = false;
        scanAgain_ = false;
    }

    while (roundRemainingSlots_ != 0 && result.operations < maxDriveWorkItems_) {
        const auto index = roundRobinSlot_;
        roundRobinSlot_ = (roundRobinSlot_ + 1) % streams_.size();
        --roundRemainingSlots_;
        ++result.scannedSlots;
        auto& slot = streams_[index];
        if (!slot.occupied || isTerminal(slot.info.state)) {
            continue;
        }

        if (writeTimedOut(slot, Http3QuicServerTransport::Clock::now())) {
            result.lastStreamId = slot.info.streamId;
            slot.info.timedOut = true;
            ++result.operations;
            const bool retired = retireStream(slot, StreamState::kCancelled,
                static_cast<std::uint64_t>(Http3ConnectionErrorCode::kRequestCancelled));
            if (!retired) {
                result.status = Status::kUnsafeToRelease;
                result.errorStreamId = slot.info.streamId;
                result.transportError = slot.info.termination.close;
                result.termination = slot.info.termination;
                break;
            }
            if (connectionRetired_) {
                result.status = Status::kConnectionClosed;
                result.termination = slot.info.termination;
                break;
            }
            ++result.timedOutStreams;
            result.madeProgress = true;
            roundMadeProgress_ = true;
            continue;
        }

        if (slot.head != kNoNode) {
            auto& node = nodes_[slot.head];
            const auto bytes = node.block.bytes();
            if (node.offset > bytes.size()) {
                result.lastStreamId = slot.info.streamId;
                ++result.operations;
                const auto failure = handleWriteFailure(slot, StreamWrite::Status::kFatal);
                result.status = failure.status;
                result.errorStreamId = slot.info.streamId;
                result.writeStatus = StreamWrite::Status::kFatal;
                result.termination = failure.termination;
                result.transportError = failure.transportError;
                result.madeProgress = slot.info.state != StreamState::kStopping;
                roundMadeProgress_ = roundMadeProgress_ || result.madeProgress;
                break;
            }
            const auto remaining = bytes.subspan(node.offset);
            if (remaining.empty()) {
                result.lastStreamId = slot.info.streamId;
                const auto indexToRelease = slot.head;
                slot.head = node.next;
                if (slot.head == kNoNode) {
                    slot.tail = kNoNode;
                }
                --slot.info.queuedBlocks;
                releaseNode(indexToRelease);
                ++result.operations;
                result.madeProgress = true;
                roundMadeProgress_ = true;
                continue;
            }

            const auto input = std::span<const char>(
                reinterpret_cast<const char*>(remaining.data()), remaining.size());
            result.lastStreamId = slot.info.streamId;
            const auto written = transport_.writeStream(connectionId_, slot.info.streamId, input);
            ++result.operations;
            ++result.writeCalls;
            slot.info.lastWriteStatus = written.status;
            if (written.status == StreamWrite::Status::kWouldBlock) {
                ++result.wouldBlockWrites;
                continue;
            }
            if (written.status != StreamWrite::Status::kAccepted || written.bytes == 0 ||
                written.bytes > remaining.size() ||
                written.bytes > std::numeric_limits<std::uint64_t>::max() - slot.info.acceptedWireBytes ||
                written.bytes > slot.info.queuedWireBytes) {
                const auto failure = handleWriteFailure(slot, written.status);
                result.status = failure.status;
                result.errorStreamId = slot.info.streamId;
                result.writeStatus = written.status;
                result.termination = failure.termination;
                result.transportError = failure.transportError;
                result.madeProgress = slot.info.state != StreamState::kStopping;
                roundMadeProgress_ = roundMadeProgress_ || result.madeProgress;
                break;
            }
            slot.info.acceptedWireBytes += written.bytes;
            slot.info.queuedWireBytes -= written.bytes;
            slot.lastWriteActivity = Http3QuicServerTransport::Clock::now();
            node.offset += written.bytes;
            result.acceptedBytes += written.bytes;
            result.madeProgress = true;
            roundMadeProgress_ = true;
            if (node.offset == bytes.size()) {
                const auto indexToRelease = slot.head;
                slot.head = node.next;
                if (slot.head == kNoNode) {
                    slot.tail = kNoNode;
                }
                --slot.info.queuedBlocks;
                releaseNode(indexToRelease);
            }
            continue;
        }

        if (slot.info.finalWireBytes && slot.info.state == StreamState::kFinPending &&
            slot.info.queuedWireBytes == 0 &&
            slot.info.acceptedWireBytes == *slot.info.finalWireBytes) {
            result.lastStreamId = slot.info.streamId;
            ++result.operations;
            const bool sendFinWasAccepted = slot.info.sendFinAccepted;
            TransportError error = TransportError::kNone;
            if (!finishStream(slot, error)) {
                result.status = connectionRetired_  ? Status::kConnectionClosed
                                : retirementFailed_ ? Status::kUnsafeToRelease
                                                    : Status::kTransportError;
                result.errorStreamId = slot.info.streamId;
                result.transportError = error;
                result.madeProgress = slot.info.state != StreamState::kStopping;
                roundMadeProgress_ = roundMadeProgress_ || result.madeProgress;
                break;
            }
            const bool completed = slot.info.state == StreamState::kFinished;
            result.madeProgress = result.madeProgress || !sendFinWasAccepted || completed ||
                                  connectionRetired_;
            roundMadeProgress_ = roundMadeProgress_ || result.madeProgress;
            if (connectionRetired_) {
                result.status = Status::kConnectionClosed;
                result.transportError = error;
                break;
            }
            if (completed) {
                ++result.finishedStreams;
            }
        }
    }

    if (result.status == Status::kIdle && result.madeProgress) {
        result.status = result.finishedStreams != 0 ? Status::kFinished : Status::kProgress;
    }
    if (stopped_ || retirementFailed_) {
        return result;
    }
    if (roundRemainingSlots_ != 0) {
        result.needsReschedule = true;
    } else if (roundMadeProgress_ || scanAgain_) {
        roundRemainingSlots_ = streams_.size();
        roundMadeProgress_ = false;
        scanAgain_ = false;
        result.needsReschedule = true;
    }
    return result;
}

void Http3ServerStreamOutput::notifyTransportActivity() {
    requireOwnerThread();
    requestScan();
}

Http3ServerStreamOutput::Result Http3ServerStreamOutput::stop() {
    requireOwnerThread();
    if (stopComplete_) {
        return {.status = connectionRetired_ ? Status::kConnectionClosed : Status::kStopped};
    }
    stopped_ = true;
    if (connectionRetired_) {
        releaseAllQueues();
        stopComplete_ = true;
        return {.status = Status::kConnectionClosed};
    }
    if (retirementFailed_) {
        if (!closeConnectionAndRelease()) {
            return {.status = Status::kUnsafeToRelease};
        }
        stopComplete_ = true;
        return {.status = Status::kConnectionClosed};
    }

    for (auto& slot : streams_) {
        if (slot.occupied && !isTerminal(slot.info.state)) {
            slot.info.state = StreamState::kStopping;
        }
    }
    for (auto& slot : streams_) {
        if (!slot.occupied || slot.info.state != StreamState::kStopping) {
            continue;
        }
        if (!retireStream(slot, StreamState::kCancelled,
                static_cast<std::uint64_t>(Http3ConnectionErrorCode::kRequestCancelled))) {
            return {.status = Status::kUnsafeToRelease,
                .termination = slot.info.termination};
        }
        if (connectionRetired_) {
            stopComplete_ = true;
            return {.status = Status::kConnectionClosed};
        }
    }
    stopComplete_ = true;
    return {.status = Status::kStopped};
}

std::optional<Http3ServerStreamOutput::StreamInfo>
Http3ServerStreamOutput::streamInfo(StreamId streamId) const {
    requireOwnerThread();
    const auto* const slot = findStream(streamId);
    if (slot == nullptr) {
        return std::nullopt;
    }
    return slot->info;
}

std::size_t Http3ServerStreamOutput::trackedStreamCount() const {
    requireOwnerThread();
    return trackedStreamCount_;
}

std::size_t Http3ServerStreamOutput::liveStreamCount() const {
    requireOwnerThread();
    std::size_t count{};
    for (const auto& slot : streams_) {
        if (slot.occupied && slot.lastWriteActivity && !slot.info.sendFinAccepted &&
            !isTerminal(slot.info.state)) {
            ++count;
        }
    }
    return count;
}

std::size_t Http3ServerStreamOutput::pendingStreamCount() const {
    requireOwnerThread();
    std::size_t count{};
    for (const auto& slot : streams_) {
        if (!slot.occupied || !slot.lastWriteActivity || slot.info.sendFinAccepted ||
            isTerminal(slot.info.state)) {
            continue;
        }
        const bool dataPending = slot.head != kNoNode;
        const bool finReady = slot.info.finalWireBytes && slot.info.queuedWireBytes == 0 &&
                              slot.info.acceptedWireBytes == *slot.info.finalWireBytes;
        if (dataPending || finReady) {
            ++count;
        }
    }
    return count;
}

std::size_t Http3ServerStreamOutput::queuedBlockCount() const {
    requireOwnerThread();
    return queuedBlockCount_;
}

bool Http3ServerStreamOutput::stopped() const {
    requireOwnerThread();
    return stopped_;
}

bool Http3ServerStreamOutput::connectionRetired() const {
    requireOwnerThread();
    return connectionRetired_;
}

bool Http3ServerStreamOutput::connectionIdentityGone() {
    return !transport_.connectionInfo(connectionId_).has_value();
}

bool Http3ServerStreamOutput::closeConnectionAndRelease() {
    if (connectionRetired_) {
        stopped_ = true;
        stopComplete_ = true;
        markConnectionClosed();
        releaseAllQueues();
        return true;
    }
    const auto close = transport_.retireConnectionLocally(connectionId_);
    if (close == TransportError::kNone ||
        (close == TransportError::kNoConnection && connectionIdentityGone())) {
        connectionRetired_ = true;
        stopped_ = true;
        stopComplete_ = true;
        markConnectionClosed();
        releaseAllQueues();
        retirementFailed_ = false;
        return true;
    }
    retirementFailed_ = true;
    return false;
}

bool Http3ServerStreamOutput::retireStream(StreamSlot& slot, StreamState terminalState,
    std::uint64_t errorCode) {
    slot.info.state = StreamState::kStopping;
    slot.info.termination = transport_.terminateBidirectionalStream(
        connectionId_, slot.info.streamId, errorCode);
    const auto directionRetired = [](TransportError error) noexcept {
        return error == TransportError::kNone || error == TransportError::kClosed ||
               error == TransportError::kNoStream;
    };
    if (directionRetired(slot.info.termination.send) &&
        directionRetired(slot.info.termination.close)) {
        slot.info.state = terminalState;
        releaseStreamQueue(slot);
        requestScan();
        return true;
    }
    if (slot.info.termination.close == TransportError::kNoConnection &&
        connectionIdentityGone()) {
        connectionRetired_ = true;
        stopped_ = true;
        stopComplete_ = true;
        markConnectionClosed();
        releaseAllQueues();
        return true;
    }
    if (closeConnectionAndRelease()) {
        return true;
    }
    return false;
}

void Http3ServerStreamOutput::releaseNode(std::uint32_t index) noexcept {
    auto& node = nodes_[index];
    node.block.release();
    node.offset = 0;
    node.next = freeNode_;
    freeNode_ = index;
    if (queuedBlockCount_ == 0) {
        std::terminate();
    }
    --queuedBlockCount_;
}

void Http3ServerStreamOutput::releaseStreamQueue(StreamSlot& slot) noexcept {
    auto index = slot.head;
    while (index != kNoNode) {
        const auto next = nodes_[index].next;
        releaseNode(index);
        index = next;
    }
    slot.head = kNoNode;
    slot.tail = kNoNode;
    slot.info.queuedBlocks = 0;
    slot.info.queuedWireBytes = 0;
}

void Http3ServerStreamOutput::releaseAllQueues() noexcept {
    for (auto& slot : streams_) {
        if (slot.occupied) {
            releaseStreamQueue(slot);
        }
    }
}

void Http3ServerStreamOutput::markConnectionClosed() noexcept {
    for (auto& slot : streams_) {
        if (slot.occupied && slot.info.state != StreamState::kFinished &&
            slot.info.state != StreamState::kReset && slot.info.state != StreamState::kCancelled &&
            slot.info.state != StreamState::kFailed) {
            slot.info.state = StreamState::kConnectionClosed;
        }
    }
}

Http3ServerStreamOutput::Result Http3ServerStreamOutput::failConnection(
    Status status, TransportError error) {
    stopped_ = true;
    for (auto& slot : streams_) {
        if (slot.occupied && !isTerminal(slot.info.state)) {
            slot.info.state = StreamState::kFailed;
        }
    }
    if (!closeConnectionAndRelease()) {
        return {.status = Status::kUnsafeToRelease, .transportError = error};
    }
    stopComplete_ = true;
    return {.status = status, .transportError = error};
}

Http3ServerStreamOutput::Result Http3ServerStreamOutput::handleWriteFailure(
    StreamSlot& slot, StreamWrite::Status writeStatus) {
    slot.info.lastWriteStatus = writeStatus;
    slot.info.state = StreamState::kStopping;
    const bool retired = retireStream(slot, StreamState::kFailed,
        static_cast<std::uint64_t>(Http3ConnectionErrorCode::kInternalError));
    if (!retired) {
        return {.status = Status::kUnsafeToRelease,
            .writeStatus = writeStatus,
            .termination = slot.info.termination};
    }
    return {.status = connectionRetired_ ? Status::kConnectionClosed : Status::kTransportError,
        .transportError = slot.info.termination.send,
        .writeStatus = writeStatus,
        .termination = slot.info.termination};
}

bool Http3ServerStreamOutput::finishStream(StreamSlot& slot, TransportError& error) {
    if (!slot.info.sendFinAccepted) {
        error = transport_.finishStream(connectionId_, slot.info.streamId);
        slot.info.finishError = error;
        if (error != TransportError::kNone) {
            slot.info.state = StreamState::kStopping;
            const bool retired = retireStream(slot, StreamState::kFailed,
                static_cast<std::uint64_t>(Http3ConnectionErrorCode::kInternalError));
            if (!retired) {
                retirementFailed_ = true;
            }
            return false;
        }
        slot.info.sendFinAccepted = true;
    }

    const auto retired = transport_.retireCompletedBidirectionalStream(
        connectionId_, slot.info.streamId);
    if (retired == TransportError::kWouldBlock) {
        error = TransportError::kNone;
        return true;
    }
    if (retired == TransportError::kNone) {
        slot.info.state = StreamState::kFinished;
        slot.info.finishError = TransportError::kNone;
        return true;
    }
    error = retired;
    slot.info.finishError = retired;
    if (retired == TransportError::kNoConnection && connectionIdentityGone()) {
        connectionRetired_ = true;
        stopped_ = true;
        stopComplete_ = true;
        markConnectionClosed();
        releaseAllQueues();
        return true;
    }
    if (closeConnectionAndRelease()) {
        return true;
    }
    retirementFailed_ = true;
    return false;
}

bool Http3ServerStreamOutput::writeTimedOut(const StreamSlot& slot,
    Http3QuicServerTransport::Clock::time_point now) const noexcept {
    const bool outputPending = slot.head != kNoNode ||
                               (slot.info.finalWireBytes &&
                                   slot.info.queuedWireBytes == 0 &&
                                   slot.info.acceptedWireBytes == *slot.info.finalWireBytes);
    if (!writeTimeout_ || !slot.lastWriteActivity || !outputPending ||
        slot.info.sendFinAccepted || now < *slot.lastWriteActivity) {
        return false;
    }
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               now - *slot.lastWriteActivity) >= *writeTimeout_;
}

void Http3ServerStreamOutput::requestScan() noexcept {
    if (stopped_) {
        return;
    }
    if (roundRemainingSlots_ == 0) {
        roundRemainingSlots_ = streams_.size();
        roundMadeProgress_ = false;
        scanAgain_ = false;
    } else {
        scanAgain_ = true;
    }
}

std::size_t Http3ServerStreamOutput::tableCapacity(std::size_t maxTrackedStreams) {
    if (maxTrackedStreams > std::numeric_limits<std::size_t>::max() / 2) {
        throw std::length_error("HTTP/3 server output stream capacity is too large");
    }
    const auto needed = maxTrackedStreams * 2;
    const auto maxPowerOfTwo = std::size_t{1} << (std::numeric_limits<std::size_t>::digits - 1);
    if (needed > maxPowerOfTwo) {
        throw std::length_error("HTTP/3 server output stream capacity is too large");
    }
    return std::bit_ceil(needed);
}

}  // namespace ruvia::detail
