#include "ruvia/web/detail/http3/Http3ServerStreamInput.h"

#include <bit>
#include <limits>
#include <stdexcept>
#include <string_view>

#include "ruvia/core/memory/MemoryPool.h"
#include "ruvia/http/Http3PeerStreams.h"
#include "ruvia/http/Http3VarInt.h"

namespace ruvia::detail {
namespace {

bool isProtocolError(const Http3ConnectionResult& result) noexcept {
    return result.scope != Http3ConnectionErrorScope::kNone ||
           result.status == Http3ConnectionStatus::kStreamError ||
           result.status == Http3ConnectionStatus::kConnectionError;
}

}  // namespace

Http3ServerStreamInput::Http3ServerStreamInput(Http3SansIoSessionEngine& session,
    WorkerMemory& worker, std::uint64_t epoch, std::uint64_t connectionGeneration,
    std::size_t maxTrackedStreams)
    : session_(session),
      epoch_(epoch),
      connectionGeneration_(connectionGeneration),
      maxTrackedStreams_(maxTrackedStreams),
      streams_(worker.resource()) {
    if (maxTrackedStreams_ == 0) {
        throw std::invalid_argument("HTTP/3 stream input capacity must be greater than zero");
    }
    const auto capacity = tableCapacity(maxTrackedStreams_);
    streams_.reserve(capacity);
    for (std::size_t i = 0; i < capacity; ++i) {
        streams_.emplace_back(worker.resource());
    }
}

Http3ServerStreamInput::Result Http3ServerStreamInput::acceptData(
    const Http3StreamMailbox::BorrowedBlock& block) noexcept {
    if (!block || block.critical() != nullptr) {
        return {Status::kInvalidInput};
    }
    const auto& id = block.id();
    if (const auto identity = identityStatus(id); identity != Status::kFed) {
        return {identity};
    }
    if (stopped_) {
        return {Status::kStopped};
    }

    Status failure = Status::kFed;
    auto* state = findOrCreate(id.streamId, failure);
    if (state == nullptr) {
        return {failure};
    }
    state->receivedEarlyData = state->receivedEarlyData || id.received_early_data;
    const auto bytes = block.bytes();
    if (state->phase == StreamPhase::kFinished && !bytes.empty()) {
        return finalSizeFailure();
    }
    if (state->phase == StreamPhase::kReset && !bytes.empty()) {
        return finalSizeFailure();
    }
    const bool resetPending = state->phase == StreamPhase::kResetPending;
    if (state->qpackBlocked && !resetPending) {
        return {Status::kInvalidInput};
    }
    if (state->phase != StreamPhase::kOpen && !resetPending) {
        return {Status::kClosedStream};
    }

    if (bytes.size() > std::numeric_limits<std::uint64_t>::max() - state->wireBytes) {
        return finalSizeFailure();
    }
    const auto newWireBytes = state->wireBytes + bytes.size();
    if (newWireBytes > kHttp3VarIntMax ||
        (state->finalSize.has_value() && newWireBytes > *state->finalSize) ||
        (resetPending && (!state->resetPublishedBytes.has_value() ||
                             newWireBytes > *state->resetPublishedBytes))) {
        return finalSizeFailure();
    }
    const bool fin = !resetPending && state->finalSize.has_value() &&
                     newWireBytes == *state->finalSize;
    const auto wire = std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    if (state->qpackBlocked && resetPending) {
        state->wireBytes = newWireBytes;
        if (newWireBytes == *state->resetPublishedBytes) {
            return applyPeerReset(id.streamId, *state);
        }
        return {Status::kDeferredReset};
    }
    auto result = feedSession(id.streamId, wire, fin, *state);
    if (result.status == Status::kFed && fin) {
        result.status = Status::kFinished;
    }
    if (resetPending && result.status == Status::kFed) {
        if (state->wireBytes == *state->resetPublishedBytes) {
            return applyPeerReset(id.streamId, *state);
        }
        result.status = Status::kDeferredReset;
    }
    return result;
}

Http3ServerStreamInput::Result Http3ServerStreamInput::acceptControl(
    const Http3StreamControl& control) noexcept {
    if (const auto identity = identityStatus(control.id); identity != Status::kFed) {
        return {identity};
    }
    if (stopped_) {
        return {Status::kStopped};
    }

    switch (control.kind) {
        case Http3StreamControl::Kind::kConnectionClosed:
            stop();
            return {Status::kConnectionClosed};
        case Http3StreamControl::Kind::kStreamReset: {
            Status failure = Status::kFed;
            auto* state = findOrCreate(control.id.streamId, failure);
            if (state == nullptr) {
                return {failure};
            }
            state->receivedEarlyData = state->receivedEarlyData ||
                                       control.id.received_early_data;
            if (state->resetPublishedBytes.has_value()) {
                if (*state->resetPublishedBytes != control.value ||
                    state->resetErrorCode != control.streamResetErrorCode) {
                    return finalSizeFailure();
                }
                return {state->phase == StreamPhase::kResetPending
                            ? Status::kDeferredReset
                            : Status::kClosedStream};
            }
            if (state->phase != StreamPhase::kOpen && state->phase != StreamPhase::kFinished) {
                return {Status::kClosedStream};
            }
            if ((state->finalSize.has_value() && state->phase != StreamPhase::kFinished &&
                    !(state->qpackBlocked && state->pendingFin && *state->finalSize == state->wireBytes)) ||
                control.value > kHttp3VarIntMax || control.value < state->wireBytes ||
                (state->phase == StreamPhase::kFinished && control.value != state->wireBytes)) {
                return finalSizeFailure();
            }
            state->resetPublishedBytes = control.value;
            state->resetErrorCode = control.streamResetErrorCode;
            if (control.value != state->wireBytes) {
                state->phase = StreamPhase::kResetPending;
                return {Status::kDeferredReset};
            }
            return applyPeerReset(control.id.streamId, *state);
        }
        case Http3StreamControl::Kind::kStreamFin:
            return acceptFin(control);
        case Http3StreamControl::Kind::kWritable:
            return {Status::kIgnoredControl};
        case Http3StreamControl::Kind::kTunnelEstablished:
            return {Status::kInvalidInput};
    }
    return {Status::kInvalidInput};
}

Http3ServerStreamInput::Result Http3ServerStreamInput::cancelRequest(std::uint64_t streamId) noexcept {
    if (stopped_) {
        return {Status::kStopped};
    }
    if (!isHttp3RequestStreamId(streamId)) {
        return {Status::kInvalidInput};
    }
    Status failure = Status::kFed;
    auto* state = findOrCreate(streamId, failure);
    if (state == nullptr) {
        return {failure};
    }
    if (state->phase != StreamPhase::kOpen && state->phase != StreamPhase::kFinished) {
        return {Status::kClosedStream};
    }
    clearQpack(*state);
    state->phase = StreamPhase::kCancelled;
    finishRequestStream(*state);
    // Missing session state is normal before HEADERS or after response release.
    (void)session_.cancelRequest(streamId);
    return {Status::kLocalCancelled};
}

void Http3ServerStreamInput::stop() noexcept {
    if (stopped_) {
        return;
    }
    stopped_ = true;
    for (auto& slot : streams_) {
        if (slot.occupied) {
            clearQpack(slot.state);
            slot.state.phase = StreamPhase::kConnectionClosed;
            finishRequestStream(slot.state);
        }
    }
    session_.stop();
}

std::size_t Http3ServerStreamInput::trackedStreamCount() const noexcept {
    return trackedStreamCount_;
}

std::size_t Http3ServerStreamInput::observedRequestStreamCount() const noexcept {
    return observedRequestStreamCount_;
}

std::size_t Http3ServerStreamInput::activeRequestStreamCount() const noexcept {
    return activeRequestStreamCount_;
}

bool Http3ServerStreamInput::stopped() const noexcept {
    return stopped_;
}

Http3ServerStreamInput::Status Http3ServerStreamInput::identityStatus(
    const Http3StreamMessageId& id) const noexcept {
    if (id.epoch != epoch_) {
        return Status::kForeignEpoch;
    }
    if (id.connectionGeneration != connectionGeneration_) {
        return Status::kStaleConnection;
    }
    return Status::kFed;
}

Http3ServerStreamInput::Result Http3ServerStreamInput::acceptFin(
    const Http3StreamControl& control) noexcept {
    Status failure = Status::kFed;
    auto* state = findOrCreate(control.id.streamId, failure);
    if (state == nullptr) {
        return {failure};
    }
    state->receivedEarlyData = state->receivedEarlyData ||
                               control.id.received_early_data;
    if (state->resetPublishedBytes.has_value()) {
        return finalSizeFailure();
    }

    if (state->phase == StreamPhase::kReset || state->phase == StreamPhase::kCancelled ||
        state->phase == StreamPhase::kFailed ||
        state->phase == StreamPhase::kConnectionClosed) {
        return {Status::kClosedStream};
    }
    if (state->finalSize.has_value()) {
        if (*state->finalSize == control.value) {
            return {Status::kDuplicateFin};
        }
        return finalSizeFailure();
    }
    if (state->phase != StreamPhase::kOpen) {
        return {Status::kClosedStream};
    }
    if (control.value > kHttp3VarIntMax || control.value < state->wireBytes) {
        return finalSizeFailure();
    }

    state->finalSize = control.value;
    if (control.value != state->wireBytes) {
        return {Status::kDeferredFin};
    }
    if (state->qpackBlocked) {
        state->pendingFin = true;
        return {Status::kDeferredQpack};
    }
    return feedSession(control.id.streamId, {}, true, *state);
}

Http3ServerStreamInput::Result Http3ServerStreamInput::applyPeerReset(
    std::uint64_t streamId, StreamState& state) noexcept {
    state.phase = StreamPhase::kReset;
    const auto result = session_.feed(streamId, {}, false, true);
    clearQpack(state);
    if (isProtocolError(result)) {
        if (result.scope == Http3ConnectionErrorScope::kConnection ||
            result.status == Http3ConnectionStatus::kConnectionError) {
            closeForConnectionError();
        } else {
            state.phase = StreamPhase::kFailed;
            finishRequestStream(state);
        }
        return {Status::kProtocolError, result};
    }
    finishRequestStream(state);
    return {Status::kReset, result};
}

Http3ServerStreamInput::Result Http3ServerStreamInput::finalSizeFailure() noexcept {
    // Inconsistent transport/mailbox counts are not peer RESET evidence or
    // HTTP framing errors. Retire locally; the owner must close transport.
    stop();
    return {Status::kFinalSizeError};
}

Http3ServerStreamInput::Result Http3ServerStreamInput::feedSession(std::uint64_t streamId,
    std::string_view bytes, bool fin, StreamState& state) noexcept {
    const auto result = session_.feed(streamId, bytes, fin);
    if (isProtocolError(result) || result.status == Http3ConnectionStatus::kReset) {
        if (result.scope == Http3ConnectionErrorScope::kConnection ||
            result.status == Http3ConnectionStatus::kConnectionError) {
            closeForConnectionError();
        } else {
            state.phase = StreamPhase::kFailed;
            finishRequestStream(state);
        }
        return {Status::kProtocolError, result};
    }
    state.wireBytes += bytes.size();
    if (result.status == Http3ConnectionStatus::kQpackBlocked) {
        if (result.consumedBytes > bytes.size()) {
            return finalSizeFailure();
        }
        try {
            state.pendingBytes.assign(bytes.substr(result.consumedBytes));
        } catch (...) {
            stop();
            return {Status::kCapacityExhausted};
        }
        if (!state.qpackBlocked) {
            state.qpackBlocked = true;
            ++blockedQpackCount_;
        }
        state.pendingFin = fin;
        return {Status::kDeferredQpack, result};
    }
    if (fin) {
        state.phase = StreamPhase::kFinished;
        finishRequestStream(state);
        return {Status::kFinished, result};
    }
    return {Status::kFed, result};
}

std::size_t Http3ServerStreamInput::tableCapacity(std::size_t maxTrackedStreams) {
    if (maxTrackedStreams > std::numeric_limits<std::size_t>::max() / 2) {
        throw std::length_error("HTTP/3 stream input capacity is too large");
    }
    const auto needed = maxTrackedStreams * 2;
    const auto maxPowerOfTwo = std::size_t{1} << (std::numeric_limits<std::size_t>::digits - 1);
    if (needed > maxPowerOfTwo) {
        throw std::length_error("HTTP/3 stream input capacity is too large");
    }
    return std::bit_ceil(needed);
}

Http3ServerStreamInput::StreamState* Http3ServerStreamInput::findOrCreate(
    std::uint64_t streamId, Status& failure) noexcept {
    if (!http3StreamIdType(streamId)) {
        failure = Status::kInvalidInput;
        return nullptr;
    }
    std::uint64_t hash = streamId;
    hash ^= hash >> 30;
    hash *= 0xbf58476d1ce4e5b9ULL;
    hash ^= hash >> 27;
    hash *= 0x94d049bb133111ebULL;
    hash ^= hash >> 31;
    const auto mask = streams_.size() - 1;
    auto index = static_cast<std::size_t>(hash) & mask;
    for (std::size_t probes = 0; probes < streams_.size(); ++probes) {
        auto& slot = streams_[index];
        if (!slot.occupied) {
            if (trackedStreamCount_ >= maxTrackedStreams_) {
                failure = Status::kCapacityExhausted;
                stop();
                return nullptr;
            }
            slot.streamId = streamId;
            slot.state = StreamState(streams_.get_allocator().resource());
            slot.state.requestStream = isHttp3RequestStreamId(streamId);
            slot.state.requestActive = slot.state.requestStream;
            slot.occupied = true;
            ++trackedStreamCount_;
            if (slot.state.requestStream) {
                ++observedRequestStreamCount_;
                ++activeRequestStreamCount_;
            }
            return &slot.state;
        }
        if (slot.streamId == streamId) {
            return &slot.state;
        }
        index = (index + 1) & mask;
    }
    failure = Status::kCapacityExhausted;
    stop();
    return nullptr;
}

void Http3ServerStreamInput::closeForConnectionError() noexcept {
    stopped_ = true;
    for (auto& slot : streams_) {
        if (slot.occupied) {
            clearQpack(slot.state);
            slot.state.phase = StreamPhase::kConnectionClosed;
            finishRequestStream(slot.state);
        }
    }
}

void Http3ServerStreamInput::clearQpack(StreamState& state) noexcept {
    if (state.qpackBlocked) {
        if (blockedQpackCount_ == 0) {
            std::terminate();
        }
        --blockedQpackCount_;
        state.qpackBlocked = false;
    }
    state.pendingFin = false;
    std::pmr::string(state.pendingBytes.get_allocator()).swap(state.pendingBytes);
}

bool Http3ServerStreamInput::receivedEarlyData(std::uint64_t streamId) const noexcept {
    for (const auto& slot : streams_) {
        if (slot.occupied && slot.streamId == streamId) {
            return slot.state.receivedEarlyData;
        }
    }
    return false;
}

bool Http3ServerStreamInput::canAcceptInput(std::uint64_t streamId) const noexcept {
    if (blockedQpackCount_ == 0) {
        return true;
    }
    for (const auto& slot : streams_) {
        if (slot.occupied && slot.streamId == streamId) {
            return !slot.state.qpackBlocked || slot.state.phase == StreamPhase::kResetPending;
        }
    }
    return true;
}

std::optional<Http3ServerStreamInput::ResumedInput> Http3ServerStreamInput::resumeQpack() noexcept {
    if (stopped_ || blockedQpackCount_ == 0) {
        return std::nullopt;
    }
    for (std::size_t count = 0; count < streams_.size(); ++count) {
        auto& slot = streams_[nextQpackResume_];
        nextQpackResume_ = (nextQpackResume_ + 1) % streams_.size();
        auto& state = slot.state;
        if (!slot.occupied || !state.qpackBlocked || !session_.canAcceptInput(slot.streamId, state.pendingBytes.size())) {
            continue;
        }
        const auto result = session_.feed(slot.streamId, state.pendingBytes, state.pendingFin);
        if (result.status == Http3ConnectionStatus::kQpackBlocked) {
            if (result.consumedBytes > state.pendingBytes.size()) {
                return ResumedInput{slot.streamId, finalSizeFailure()};
            }
            if (result.consumedBytes == 0) {
                continue;
            }
            state.pendingBytes.erase(0, result.consumedBytes);
            return ResumedInput{slot.streamId, {Status::kDeferredQpack, result}};
        }
        const bool fin = state.pendingFin;
        clearQpack(state);
        if (isProtocolError(result)) {
            if (result.scope == Http3ConnectionErrorScope::kConnection) {
                closeForConnectionError();
            } else {
                state.phase = StreamPhase::kFailed;
                finishRequestStream(state);
            }
            return ResumedInput{slot.streamId, {Status::kProtocolError, result}};
        }
        if (fin) {
            state.phase = StreamPhase::kFinished;
            finishRequestStream(state);
        }
        return ResumedInput{slot.streamId, {fin ? Status::kFinished : Status::kFed, result}};
    }
    return std::nullopt;
}

void Http3ServerStreamInput::finishRequestStream(StreamState& state) noexcept {
    if (!state.requestActive) {
        return;
    }
    state.requestActive = false;
    if (activeRequestStreamCount_ == 0) {
        std::terminate();
    }
    --activeRequestStreamCount_;
}

}  // namespace ruvia::detail
