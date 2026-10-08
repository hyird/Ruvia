#include <array>
#include <exception>
#include <utility>

#include "http3/Http3ServerConnection.h"

namespace ruvia::detail {
namespace {
std::uint64_t mixStreamId(std::uint64_t streamId) noexcept {
    streamId ^= streamId >> 30;
    streamId *= 0xbf58476d1ce4e5b9ULL;
    streamId ^= streamId >> 27;
    streamId *= 0x94d049bb133111ebULL;
    streamId ^= streamId >> 31;
    return streamId;
}
}  // namespace

Http3ServerConnection::request_index::request_index(std::pmr::memory_resource* resource, std::size_t capacity)
    : capacity_(capacity),
      slots_(resource) {
    slots_.resize(indexCapacity(capacity));
}

Http3ServerConnection::RequestIndexSlot*
Http3ServerConnection::request_index::find_or_create(
    std::uint64_t streamId, bool& created) noexcept {
    created = false;
    const auto mask = slots_.size() - 1;
    auto index = static_cast<std::size_t>(mixStreamId(streamId)) & mask;
    for (std::size_t probes = 0; probes < slots_.size(); ++probes) {
        auto& slot = slots_[index];
        if (!slot.occupied) {
            if (tracked_ >= capacity_) {
                return nullptr;
            }
            slot.streamId = streamId;
            slot.entry = nullptr;
            slot.queuePrevious = nullptr;
            slot.queueNext = nullptr;
            slot.queue = QueueKind::kNone;
            slot.status = RequestStatus::kUnknown;
            slot.rejection = Session::Rejection::kNone;
            slot.runStatus.reset();
            slot.resetIntentPrevious = kNoIntentSlot;
            slot.resetIntentNext = kNoIntentSlot;
            slot.resetIntentSequence = 0;
            slot.resetIntentErrorCode = Http3ConnectionErrorCode::kRequestCancelled;
            slot.resetIntentOrigin = ResetIntentOrigin::kLocalCancellation;
            slot.resetIntentPending = false;
            slot.occupied = true;
            ++tracked_;
            created = true;
            return &slot;
        }
        if (slot.streamId == streamId) {
            return &slot;
        }
        index = (index + 1) & mask;
    }
    return nullptr;
}

Http3ServerConnection::RequestIndexSlot*
Http3ServerConnection::request_index::find(std::uint64_t streamId) noexcept {
    return const_cast<RequestIndexSlot*>(
        std::as_const(*this).find(streamId));
}

const Http3ServerConnection::RequestIndexSlot*
Http3ServerConnection::request_index::find(std::uint64_t streamId) const noexcept {
    if (slots_.empty()) {
        return nullptr;
    }
    const auto mask = slots_.size() - 1;
    auto index = static_cast<std::size_t>(mixStreamId(streamId)) & mask;
    for (std::size_t probes = 0; probes < slots_.size(); ++probes) {
        const auto& slot = slots_[index];
        if (!slot.occupied) {
            return nullptr;
        }
        if (slot.streamId == streamId) {
            return &slot;
        }
        index = (index + 1) & mask;
    }
    return nullptr;
}

Http3ServerConnection::IntrusiveQueue&
Http3ServerConnection::output_scheduler::queue(QueueKind kind) noexcept {
    switch (kind) {
        case QueueKind::kDataRunnable:
            return data_runnable_;
        case QueueKind::kControlRunnable:
            return control_runnable_;
        case QueueKind::kLocalRunnable:
            return local_runnable_;
        case QueueKind::kDataBlocked:
            return data_blocked_;
        case QueueKind::kControlBlocked:
            return control_blocked_;
        case QueueKind::kNone:
            break;
    }
    std::terminate();
}

const Http3ServerConnection::IntrusiveQueue&
Http3ServerConnection::output_scheduler::queue(QueueKind kind) const noexcept {
    return const_cast<output_scheduler*>(this)->queue(kind);
}

void Http3ServerConnection::output_scheduler::remove(RequestIndexSlot& slot) noexcept {
    if (slot.queue == QueueKind::kNone) {
        return;
    }
    auto& queue = this->queue(slot.queue);
    if (slot.queuePrevious != nullptr) {
        slot.queuePrevious->queueNext = slot.queueNext;
    } else {
        queue.head = slot.queueNext;
    }
    if (slot.queueNext != nullptr) {
        slot.queueNext->queuePrevious = slot.queuePrevious;
    } else {
        queue.tail = slot.queuePrevious;
    }
    const bool wasBlocked = slot.queue == QueueKind::kDataBlocked ||
                            slot.queue == QueueKind::kControlBlocked;
    slot.queuePrevious = nullptr;
    slot.queueNext = nullptr;
    slot.queue = QueueKind::kNone;
    auto& count = wasBlocked ? blocked_ : ready_;
    if (count == 0) {
        std::terminate();
    }
    --count;
}

Http3ServerConnection::RequestIndexSlot*
Http3ServerConnection::output_scheduler::select(WorkLanes eligibleLanes) const noexcept {
    std::array<WorkLane, 3> lanes{};
    switch (next_lane_) {
        case WorkLane::kData:
            lanes = {WorkLane::kData, WorkLane::kControl, WorkLane::kLocal};
            break;
        case WorkLane::kControl:
            lanes = {WorkLane::kControl, WorkLane::kLocal, WorkLane::kData};
            break;
        case WorkLane::kLocal:
            lanes = {WorkLane::kLocal, WorkLane::kData, WorkLane::kControl};
            break;
    }
    for (const auto lane : lanes) {
        if (!eligibleLanes.contains(lane)) {
            continue;
        }
        const auto kind = lane == WorkLane::kData      ? QueueKind::kDataRunnable
                          : lane == WorkLane::kControl ? QueueKind::kControlRunnable
                                                       : QueueKind::kLocalRunnable;
        if (const auto* queue = &this->queue(kind); queue->head != nullptr) {
            return queue->head;
        }
    }
    return nullptr;
}

void Http3ServerConnection::request_index::attach(RequestIndexSlot& slot, RequestEntry& entry) noexcept {
    if (slot.entry != nullptr || slot.rejectionEntry != nullptr) {
        std::terminate();
    }
    slot.entry = &entry;
    ++active_;
}
void Http3ServerConnection::request_index::attach_rejection(RequestIndexSlot& slot, RejectionEntry& entry) noexcept {
    if (slot.entry != nullptr || slot.rejectionEntry != nullptr) {
        std::terminate();
    }
    slot.rejectionEntry = &entry;
    ++rejections_;
}
void Http3ServerConnection::request_index::retire(RequestIndexSlot& slot, RequestEntry& entry) noexcept {
    if (slot.entry != &entry || active_ == 0 || slot.queue != QueueKind::kNone) {
        std::terminate();
    }
    slot.entry = nullptr;
    --active_;
}
void Http3ServerConnection::request_index::retire_rejection(RequestIndexSlot& slot, RejectionEntry& entry) noexcept {
    if (slot.rejectionEntry != &entry || rejections_ == 0 || slot.queue != QueueKind::kNone) {
        std::terminate();
    }
    slot.rejectionEntry = nullptr;
    --rejections_;
}
bool Http3ServerConnection::output_scheduler::enqueue(RequestIndexSlot& slot, QueueKind kind) noexcept {
    if (slot.queue != QueueKind::kNone || kind == QueueKind::kNone) {
        std::terminate();
    }
    auto& selected = queue(kind);
    const bool was_empty = selected.head == nullptr;
    slot.queuePrevious = selected.tail;
    slot.queueNext = nullptr;
    if (selected.tail != nullptr) {
        selected.tail->queueNext = &slot;
    } else {
        selected.head = &slot;
    }
    selected.tail = &slot;
    slot.queue = kind;
    if (kind == QueueKind::kDataBlocked || kind == QueueKind::kControlBlocked) {
        ++blocked_;
    } else {
        ++ready_;
    }
    return was_empty;
}
void Http3ServerConnection::output_scheduler::advance() noexcept {
    switch (next_lane_) {
        case WorkLane::kData:
            next_lane_ = WorkLane::kControl;
            break;
        case WorkLane::kControl:
            next_lane_ = WorkLane::kLocal;
            break;
        case WorkLane::kLocal:
            next_lane_ = WorkLane::kData;
            break;
    }
}

Http3ServerConnection::transport_retirement::reset_result Http3ServerConnection::transport_retirement::enqueue_reset(
    RequestIndexSlot& slot, Http3ConnectionErrorCode errorCode,
    ResetIntentOrigin origin) noexcept {
    if (!slot.occupied) {
        return reset_result::capacity_exhausted;
    }
    if (retired_ || taken_over_ || handed_off_) {
        return reset_result::unavailable;
    }
    if (slot.resetIntentPending) {
        // Identical codes are idempotent. The first protocol error wins, and a
        // local H3_REQUEST_CANCELLED never downgrades an existing protocol code;
        // only a protocol error may supersede a pending local cancellation.
        if (slot.resetIntentErrorCode == errorCode) {
            if (origin == ResetIntentOrigin::kStreamProtocolError) {
                slot.resetIntentOrigin = origin;
            }
            return reset_result::queued;
        }
        if (slot.resetIntentOrigin == ResetIntentOrigin::kStreamProtocolError ||
            origin != ResetIntentOrigin::kStreamProtocolError) {
            return reset_result::queued;
        }
        if (next_sequence_ == 0 ||
            next_sequence_ == kReservedCloseIntentSequence) {
            return reset_result::sequence_exhausted;
        }
        // A protocol error supersedes only a local cancellation. Give the new
        // payload a new token so an ack for the old code cannot clear it.
        // Reinsert at the tail so channel publication remains in sequence
        // order across sibling resets and push-open intents.
        unlink_reset(slot);
        return enqueue_reset(slot, errorCode, origin);
    }
    if (reset_count_ >= capacity_) {
        return reset_result::capacity_exhausted;
    }
    if (next_sequence_ == 0 ||
        next_sequence_ == kReservedCloseIntentSequence) {
        return reset_result::sequence_exhausted;
    }

    const auto slotIndex = static_cast<std::size_t>(&slot - requests_.slots().data());
    if (slotIndex >= requests_.slots().size()) {
        return reset_result::capacity_exhausted;
    }
    slot.resetIntentSequence = next_sequence_++;
    slot.resetIntentErrorCode = errorCode;
    slot.resetIntentOrigin = origin;
    slot.resetIntentPending = true;
    slot.resetIntentPrevious = reset_tail_;
    slot.resetIntentNext = kNoIntentSlot;
    if (reset_tail_ != kNoIntentSlot) {
        if (reset_tail_ >= requests_.slots().size()) {
            std::terminate();
        }
        requests_.slots()[reset_tail_].resetIntentNext = slotIndex;
    } else {
        reset_head_ = slotIndex;
    }
    reset_tail_ = slotIndex;
    ++reset_count_;
    return reset_result::queued;
}

void Http3ServerConnection::transport_retirement::unlink_reset(RequestIndexSlot& slot) noexcept {
    if (!slot.resetIntentPending) {
        std::terminate();
    }
    if (slot.resetIntentPrevious != kNoIntentSlot) {
        if (slot.resetIntentPrevious >= requests_.slots().size()) {
            std::terminate();
        }
        requests_.slots()[slot.resetIntentPrevious].resetIntentNext = slot.resetIntentNext;
    } else {
        reset_head_ = slot.resetIntentNext;
    }
    if (slot.resetIntentNext != kNoIntentSlot) {
        if (slot.resetIntentNext >= requests_.slots().size()) {
            std::terminate();
        }
        requests_.slots()[slot.resetIntentNext].resetIntentPrevious = slot.resetIntentPrevious;
    } else {
        reset_tail_ = slot.resetIntentPrevious;
    }
    slot.resetIntentPrevious = kNoIntentSlot;
    slot.resetIntentNext = kNoIntentSlot;
    slot.resetIntentSequence = 0;
    slot.resetIntentErrorCode = Http3ConnectionErrorCode::kRequestCancelled;
    slot.resetIntentOrigin = ResetIntentOrigin::kLocalCancellation;
    slot.resetIntentPending = false;
    if (reset_count_ == 0) {
        std::terminate();
    }
    --reset_count_;
}

std::optional<Http3ServerConnection::TransportIntent>
Http3ServerConnection::transport_retirement::next_intent() const noexcept {
    if (close_pending_) {
        return TransportIntent{
            .token = {.kind = TransportIntentKind::kConnectionClose,
                .id = {epoch_, generation_, 0},
                .sequence = kReservedCloseIntentSequence},
            .closeReason = close_reason_,
            .connectionErrorCode = close_code_};
    }
    std::optional<TransportIntent> selected;
    if (reset_head_ != kNoIntentSlot) {
        if (reset_head_ >= requests_.slots().size()) {
            std::terminate();
        }
        const auto& slot = requests_.slots()[reset_head_];
        if (!slot.occupied || !slot.resetIntentPending) {
            std::terminate();
        }
        selected = TransportIntent{
            .token = {.kind = TransportIntentKind::kStreamReset,
                .id = {epoch_, generation_, slot.streamId, slot.pushId},
                .sequence = slot.resetIntentSequence},
            .streamResetErrorCode = slot.resetIntentErrorCode};
    }

    return selected;
}

Http3ServerConnection::transport_retirement::transport_retirement(request_index& requests, std::size_t capacity, std::uint64_t epoch, std::uint64_t generation) noexcept
    : requests_(requests),
      capacity_(capacity),
      epoch_(epoch),
      generation_(generation) {}
bool Http3ServerConnection::transport_retirement::sequence_available() const noexcept {
    return next_sequence_ != 0 && next_sequence_ != kReservedCloseIntentSequence;
}
std::uint64_t Http3ServerConnection::transport_retirement::allocate_sequence() noexcept {
    if (!sequence_available()) {
        std::terminate();
    }
    return next_sequence_++;
}
bool Http3ServerConnection::transport_retirement::acknowledge(const TransportIntentToken& token) noexcept {
    if (token.id.epoch != epoch_ || token.id.connection_generation != generation_) {
        return false;
    }
    if (token.kind == TransportIntentKind::kConnectionClose) {
        if (token.id.push_id || !close_pending_ || token.id.stream_id != 0 || token.sequence != kReservedCloseIntentSequence) {
            return false;
        }
        close_pending_ = false;
        handed_off_ = true;
        return true;
    }
    if (token.kind != TransportIntentKind::kStreamReset) {
        return false;
    }
    auto* slot = requests_.find(token.id.stream_id);
    if (slot == nullptr || slot->pushId != token.id.push_id || !slot->resetIntentPending || token.sequence == 0 || token.sequence > slot->resetIntentSequence) {
        return false;
    }
    // A superseded token settles only the earlier publication; the newer debt stays.
    if (token.sequence == slot->resetIntentSequence) {
        unlink_reset(*slot);
    }
    return true;
}
void Http3ServerConnection::transport_retirement::require_close(TransportCloseReason reason, std::optional<Http3ConnectionErrorCode> code) noexcept {
    if (reason == TransportCloseReason::kNone) {
        std::terminate();
    }
    close_required_ = true;
    if (!close_pending_ && !handed_off_ && !retired_ && !taken_over_) {
        close_reason_ = reason;
        close_code_ = code;
        close_pending_ = true;
    }
}
bool Http3ServerConnection::transport_retirement::take_over() noexcept {
    if (!close_pending_ || handed_off_ || retired_ || taken_over_) {
        return false;
    }
    taken_over_ = true;
    return true;
}
bool Http3ServerConnection::transport_retirement::confirm() noexcept {
    if (retired_) {
        return false;
    }
    retired_ = true;
    return true;
}

}  // namespace ruvia::detail
