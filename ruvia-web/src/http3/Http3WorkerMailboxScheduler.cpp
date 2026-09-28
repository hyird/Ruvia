#include "ruvia/web/detail/http3/Http3WorkerMailboxScheduler.h"

#include <algorithm>
#include <exception>
#include <limits>
#include <stdexcept>
#include <utility>

namespace ruvia::detail {
namespace {

constexpr std::uint8_t kCapacityData = 1;
constexpr std::uint8_t kCapacityControl = 2;

}  // namespace

Http3WorkerMailboxCapacitySignal::Http3WorkerMailboxCapacitySignal(WakeRef wake)
    : wake_(wake) {
    if (!wake_.valid()) {
        throw std::invalid_argument("HTTP/3 mailbox capacity signal requires a stable wake target");
    }
}

Http3StreamMailboxCapacityNotifier
Http3WorkerMailboxCapacitySignal::notifier() noexcept {
    return {.context = this, .notify = &notifyThunk};
}

std::uint64_t Http3WorkerMailboxCapacitySignal::generation() const noexcept {
    return generation_.load(std::memory_order_acquire);
}

std::uint64_t Http3WorkerMailboxCapacitySignal::consume() noexcept {
    pending_.exchange(false, std::memory_order_acq_rel);
    return generation_.load(std::memory_order_acquire);
}

void Http3WorkerMailboxCapacitySignal::notifyThunk(void* context) noexcept {
    if (context == nullptr) {
        std::terminate();
    }
    auto& signal = *static_cast<Http3WorkerMailboxCapacitySignal*>(context);
    signal.generation_.fetch_add(1, std::memory_order_acq_rel);
    if (!signal.pending_.exchange(true, std::memory_order_acq_rel)) {
        signal.wake_.notify(signal.wake_.context);
    }
}

Http3WorkerMailboxScheduler::Http3WorkerMailboxScheduler(const WorkerHandle& worker,
    std::size_t maxConnections, std::pmr::memory_resource* resource,
    Http3WorkerMailboxCapacitySignal* capacitySignal, std::size_t controlBurstLimit)
    : worker_(worker),
      maxConnections_(maxConnections),
      capacitySignal_(capacitySignal),
      controlBurstLimit_(controlBurstLimit),
      slots_(resource == nullptr ? std::pmr::get_default_resource() : resource),
      freeSlots_(resource == nullptr ? std::pmr::get_default_resource() : resource) {
    if (!worker_.valid() || !worker_.isCurrent()) {
        throw std::invalid_argument("HTTP/3 worker mailbox scheduler must be built on its worker");
    }
    if (maxConnections_ == 0 || controlBurstLimit_ == 0) {
        throw std::invalid_argument("HTTP/3 scheduler limits must be positive");
    }
    if (capacitySignal_ != nullptr) {
        consumedCapacityGeneration_ = capacitySignal_->consume();
    }
    slots_.resize(maxConnections_);
    freeSlots_.reserve(maxConnections_);
    for (std::size_t index = maxConnections_; index != 0; --index) {
        auto& slot = slots_[index - 1];
        slot.scheduler = this;
        slot.index = index - 1;
        freeSlots_.push_back(index - 1);
    }
}

Http3WorkerMailboxScheduler::~Http3WorkerMailboxScheduler() {
    if (!worker_.isCurrent() || attachedConnections_ != 0 ||
        std::any_of(queues_.begin(), queues_.end(), [](const SlotQueue& queue) {
            return queue.head != nullptr || queue.tail != nullptr || queue.size != 0;
        }) ||
        capacityWaitArmed_) {
        std::terminate();
    }
    for (const auto& slot : slots_) {
        if (slot.state == SlotState::kReserved || slot.state == SlotState::kActive ||
            slot.state == SlotState::kRetiring) {
            std::terminate();
        }
    }
}

std::optional<Http3WorkerMailboxScheduler::Registration>
Http3WorkerMailboxScheduler::reserve(
    std::uint64_t epoch, std::uint64_t connectionGeneration) noexcept {
    if (!onWorker()) {
        return std::nullopt;
    }
    while (!freeSlots_.empty()) {
        const auto index = freeSlots_.back();
        freeSlots_.pop_back();
        auto& slot = slots_[index];
        if (slot.state != SlotState::kFree || slot.scheduler != this || slot.owner != nullptr) {
            std::terminate();
        }
        if (slot.slotGeneration == std::numeric_limits<std::uint64_t>::max()) {
            slot.state = SlotState::kExhausted;
            continue;
        }
        ++slot.slotGeneration;
        slot.epoch = epoch;
        slot.connectionGeneration = connectionGeneration;
        slot.state = SlotState::kReserved;
        slot.offeredIntent.reset();
        slot.parkedIntent.reset();
        return Registration{
            .token = tokenFor(slot),
            .activation = {.context = &slot,
                .activate = &activationThunk,
                .slotGeneration = slot.slotGeneration}};
    }
    return std::nullopt;
}

bool Http3WorkerMailboxScheduler::attach(ConnectionToken token,
    Connection& connection) noexcept {
    auto* slot = validate(token);
    if (slot == nullptr || slot->state != SlotState::kReserved ||
        !connection.onWorker() || connection.epoch_ != token.epoch ||
        connection.connectionGeneration_ != token.connectionGeneration ||
        connection.activation_.context != slot ||
        connection.activation_.activate != &activationThunk ||
        connection.activation_.slotGeneration != token.slotGeneration) {
        return false;
    }
    slot->owner = &connection;
    slot->state = SlotState::kActive;
    ++attachedConnections_;
    syncOwner(*slot);
    return true;
}

bool Http3WorkerMailboxScheduler::abandon(ConnectionToken token) noexcept {
    auto* slot = validate(token);
    if (slot == nullptr || slot->state != SlotState::kReserved) {
        return false;
    }
    clearSlot(*slot);
    slot->state = SlotState::kFree;
    freeSlots_.push_back(slot->index);
    return true;
}

bool Http3WorkerMailboxScheduler::beginRetirement(ConnectionToken token) noexcept {
    auto* slot = validate(token);
    if (slot == nullptr || slot->state != SlotState::kActive || slot->owner == nullptr ||
        !slot->owner->stopped()) {
        return false;
    }
    slot->state = SlotState::kRetiring;
    setLinked(*slot, QueueId::kDataRunnable, false);
    setLinked(*slot, QueueId::kControlRunnable, false);
    setLinked(*slot, QueueId::kLocalRunnable, false);
    setLinked(*slot, QueueId::kDataBlocked, false);
    setLinked(*slot, QueueId::kControlBlocked, false);
    syncOwner(*slot);
    return true;
}

bool Http3WorkerMailboxScheduler::retire(ConnectionToken token) noexcept {
    auto* slot = validate(token);
    if (slot == nullptr || slot->state != SlotState::kRetiring || slot->owner == nullptr ||
        slot->owner->pendingTransportIntentCount() != 0 ||
        !slot->owner->detachActivationAfterJoin()) {
        return false;
    }
    auto* owner = slot->owner;
    if (!owner->transportRetired_ && !owner->transportRetirementTakenOver_ &&
        !owner->closeIntentHandedOff_) {
        std::terminate();
    }
    clearSlot(*slot);
    slot->state = SlotState::kFree;
    if (attachedConnections_ == 0) {
        std::terminate();
    }
    --attachedConnections_;
    freeSlots_.push_back(slot->index);
    return true;
}

Http3WorkerMailboxScheduler::StepResult Http3WorkerMailboxScheduler::step() noexcept {
    if (!onWorker()) {
        return {.kind = StepKind::kWrongWorker};
    }
    consumeCapacityWake();
    normalizeCapacityPass();
    const bool forcedData = controlBurst_ >= controlBurstLimit_ && hasLaneWork(Lane::kData);
    const auto selected = selectLane();
    if (!selected) {
        return {};
    }

    StepResult result;
    switch (*selected) {
        case Lane::kData: {
            if (hasRecoverable(QueueId::kDataBlocked)) {
                auto* slot = popFront(QueueId::kDataBlocked);
                if (slot == nullptr || slot->owner == nullptr ||
                    !slot->owner->reactivateBlockedOne(Connection::WorkLane::kData)) {
                    if (slot != nullptr && slot->owner != nullptr) {
                        syncOwner(*slot);
                    }
                    result.kind = StepKind::kReconciled;
                    break;
                }
                result = stepPublication(*slot, Lane::kData);
                break;
            }
            auto* slot = popFront(QueueId::kDataRunnable);
            result = slot == nullptr ? StepResult{.kind = StepKind::kReconciled}
                                     : stepPublication(*slot, Lane::kData);
            break;
        }
        case Lane::kControl: {
            bool recoveredIntent = false;
            if (hasRecoverable(QueueId::kControlBlocked) ||
                hasRecoverable(QueueId::kIntentBlocked)) {
                auto* slot = selectRecoveryControl(recoveredIntent);
                if (slot == nullptr) {
                    result.kind = StepKind::kReconciled;
                } else if (recoveredIntent) {
                    const auto current = slot->owner->peekTransportIntent();
                    if (!current || !slot->parkedIntent || current->token != *slot->parkedIntent) {
                        syncOwner(*slot);
                        result.kind = StepKind::kReconciled;
                    } else {
                        slot->parkedIntent.reset();
                        result = stepIntent(*slot);
                    }
                } else if (slot->owner == nullptr ||
                           !slot->owner->reactivateBlockedOne(Connection::WorkLane::kControl)) {
                    if (slot->owner != nullptr) {
                        syncOwner(*slot);
                    }
                    result.kind = StepKind::kReconciled;
                } else {
                    result = stepPublication(*slot, Lane::kControl);
                }
                break;
            }
            auto* slot = popFront(QueueId::kControlRunnable);
            result = slot == nullptr ? StepResult{.kind = StepKind::kReconciled}
                                     : stepPublication(*slot, Lane::kControl);
            break;
        }
        case Lane::kLocal: {
            auto* slot = popFront(QueueId::kLocalRunnable);
            result = slot == nullptr ? StepResult{.kind = StepKind::kReconciled}
                                     : stepPublication(*slot, Lane::kLocal);
            break;
        }
        case Lane::kIntent: {
            auto* slot = popFront(QueueId::kIntentRunnable);
            result = slot == nullptr ? StepResult{.kind = StepKind::kReconciled}
                                     : stepIntent(*slot);
            break;
        }
    }

    if (*selected == Lane::kControl || *selected == Lane::kIntent) {
        if (controlBurst_ < controlBurstLimit_) {
            ++controlBurst_;
        }
    } else {
        controlBurst_ = 0;
    }
    // A forced DATA turn is inserted into the rotation. Advancing the cursor
    // here would skip LOCAL/intent every time the control burst limit is one.
    if (!forcedData) {
        nextLane_ = static_cast<Lane>((static_cast<std::uint8_t>(*selected) + 1U) % 4U);
    }
    normalizeCapacityPass();
    return result;
}

bool Http3WorkerMailboxScheduler::acknowledgeIntent(ConnectionToken connection,
    const Connection::TransportIntentToken& intent) noexcept {
    auto* slot = validate(connection);
    if (slot == nullptr || slot->owner == nullptr || !slot->offeredIntent ||
        *slot->offeredIntent != intent) {
        return false;
    }
    if (!slot->owner->ackTransportIntent(intent)) {
        return false;
    }
    slot->offeredIntent.reset();
    syncOwner(*slot);
    return true;
}

bool Http3WorkerMailboxScheduler::parkIntentForControlCapacity(ConnectionToken connection,
    const Connection::TransportIntentToken& intent) noexcept {
    auto* slot = validate(connection);
    if (slot == nullptr || slot->owner == nullptr || !slot->offeredIntent ||
        *slot->offeredIntent != intent || intent.kind != Connection::TransportIntentKind::kStreamReset) {
        return false;
    }
    const auto current = slot->owner->peekTransportIntent();
    if (!current || current->token != intent) {
        syncOwner(*slot);
        return false;
    }
    slot->offeredIntent.reset();
    slot->parkedIntent = intent;
    setLinked(*slot, QueueId::kIntentRunnable, false);
    setLinked(*slot, QueueId::kIntentBlocked, true);
    return true;
}

bool Http3WorkerMailboxScheduler::bindMailbox(Mailbox& mailbox) noexcept {
    if (!onWorker() || mailbox_ != nullptr || freeSlots_.size() != maxConnections_) {
        return false;
    }
    mailbox_ = &mailbox;
    return true;
}

Http3WorkerMailboxScheduler::CapacityArmResult
Http3WorkerMailboxScheduler::armCapacityWait() noexcept {
    if (!onWorker()) {
        return {.status = CapacityArmStatus::kWrongWorker};
    }
    if (mailbox_ == nullptr || capacitySignal_ == nullptr) {
        return {.status = CapacityArmStatus::kUnavailable};
    }
    normalizeCapacityPass();
    if (capacitySignal_ != nullptr &&
        capacitySignal_->generation() != consumedCapacityGeneration_) {
        return {.status = CapacityArmStatus::kWakePending};
    }
    if (capacityPassLanes_ != 0) {
        return {.status = CapacityArmStatus::kRecoveryPending};
    }

    std::uint8_t lanes = 0;
    if (queues_[queueIndex(QueueId::kDataBlocked)].head != nullptr) {
        lanes |= kCapacityData;
    }
    if (queues_[queueIndex(QueueId::kControlBlocked)].head != nullptr ||
        queues_[queueIndex(QueueId::kIntentBlocked)].head != nullptr) {
        lanes |= kCapacityControl;
    }
    if (lanes == 0) {
        return {.status = CapacityArmStatus::kNoBlockedWork};
    }
    const auto interest = lanes == (kCapacityData | kCapacityControl)
                              ? Mailbox::CapacityInterest::kAny
                          : lanes == kCapacityData
                              ? Mailbox::CapacityInterest::kData
                              : Mailbox::CapacityInterest::kControl;
    if (capacityWaitArmed_ && armedCapacityLanes_ == lanes) {
        return {.status = CapacityArmStatus::kArmed, .interest = interest};
    }
    // Replace the one mailbox registration when the set of blocked lanes
    // changes. Mailbox::armCapacityWait rechecks capacity after publishing the
    // new interest, so a return racing this replacement cannot be missed.
    switch (mailbox_->armCapacityWait(interest)) {
        case Mailbox::CapacityWaitResult::kReady:
            capacityWaitArmed_ = false;
            armedCapacityLanes_ = 0;
            startCapacityPass(lanes);
            return {.status = CapacityArmStatus::kReady, .interest = interest};
        case Mailbox::CapacityWaitResult::kArmed:
            armedCapacityLanes_ = lanes;
            capacityWaitArmed_ = true;
            return {.status = CapacityArmStatus::kArmed, .interest = interest};
        case Mailbox::CapacityWaitResult::kStopped:
            capacityWaitArmed_ = false;
            armedCapacityLanes_ = 0;
            return {.status = CapacityArmStatus::kStopped, .interest = interest};
        case Mailbox::CapacityWaitResult::kUnavailable:
            return {.status = CapacityArmStatus::kUnavailable, .interest = interest};
        case Mailbox::CapacityWaitResult::kInvalidInterest:
            return {.status = CapacityArmStatus::kUnavailable, .interest = interest};
    }
    return {.status = CapacityArmStatus::kUnavailable, .interest = interest};
}

bool Http3WorkerMailboxScheduler::takeLocalWakeObligation() noexcept {
    if (!onWorker()) {
        return false;
    }
    return std::exchange(localWakePending_, false);
}

Http3WorkerMailboxScheduler::Snapshot
Http3WorkerMailboxScheduler::snapshot() const noexcept {
    if (!onWorker()) {
        return {.wrongWorker = true};
    }
    Snapshot result;
    result.runnable = {queues_[queueIndex(QueueId::kDataRunnable)].size,
        queues_[queueIndex(QueueId::kControlRunnable)].size,
        queues_[queueIndex(QueueId::kLocalRunnable)].size,
        queues_[queueIndex(QueueId::kIntentRunnable)].size};
    result.blocked = {queues_[queueIndex(QueueId::kDataBlocked)].size,
        queues_[queueIndex(QueueId::kControlBlocked)].size,
        queues_[queueIndex(QueueId::kIntentBlocked)].size};
    result.attachedConnections = attachedConnections_;
    result.freeConnections = freeSlots_.size();
    result.capacityPassLanes = capacityPassLanes_;
    result.localWakePending = localWakePending_;
    result.capacityWakePending = capacitySignal_ != nullptr &&
                                 capacitySignal_->generation() != consumedCapacityGeneration_;
    result.capacityWaitArmed = capacityWaitArmed_;
    return result;
}

bool Http3WorkerMailboxScheduler::onWorker() const noexcept {
    return worker_.isCurrent();
}

Http3WorkerMailboxScheduler::Slot*
Http3WorkerMailboxScheduler::validate(ConnectionToken token) noexcept {
    if (!onWorker() || token.slot >= slots_.size()) {
        return nullptr;
    }
    auto& slot = slots_[token.slot];
    if (slot.scheduler != this || slot.state == SlotState::kFree ||
        slot.state == SlotState::kExhausted || slot.epoch != token.epoch ||
        slot.connectionGeneration != token.connectionGeneration ||
        slot.slotGeneration != token.slotGeneration) {
        return nullptr;
    }
    return &slot;
}

const Http3WorkerMailboxScheduler::Slot*
Http3WorkerMailboxScheduler::validate(ConnectionToken token) const noexcept {
    return const_cast<Http3WorkerMailboxScheduler*>(this)->validate(token);
}

Http3WorkerMailboxScheduler::ConnectionToken
Http3WorkerMailboxScheduler::tokenFor(const Slot& slot) noexcept {
    return {.slot = slot.index,
        .epoch = slot.epoch,
        .connectionGeneration = slot.connectionGeneration,
        .slotGeneration = slot.slotGeneration};
}

bool Http3WorkerMailboxScheduler::sameIntent(
    const std::optional<Connection::TransportIntentToken>& current,
    const std::optional<Connection::TransportIntentToken>& previous) const noexcept {
    return current.has_value() == previous.has_value() &&
           (!current || *current == *previous);
}

void Http3WorkerMailboxScheduler::setLinked(Slot& slot, QueueId id, bool linked) noexcept {
    auto& link = slot.links[queueIndex(id)];
    if (linked) {
        if (!link.linked) {
            pushBack(slot, id);
        }
    } else if (link.linked) {
        remove(slot, id);
    }
}

void Http3WorkerMailboxScheduler::pushBack(Slot& slot, QueueId id) noexcept {
    auto& link = slot.links[queueIndex(id)];
    if (link.linked) {
        std::terminate();
    }
    if (id == QueueId::kDataBlocked || id == QueueId::kControlBlocked ||
        id == QueueId::kIntentBlocked) {
        if (blockedGeneration_ == std::numeric_limits<std::size_t>::max()) {
            std::terminate();
        }
        link.blockedGeneration = ++blockedGeneration_;
    }
    auto& queue = queues_[queueIndex(id)];
    link.previous = queue.tail;
    link.next = nullptr;
    link.linked = true;
    if (queue.tail != nullptr) {
        queue.tail->links[queueIndex(id)].next = &slot;
    } else {
        queue.head = &slot;
    }
    queue.tail = &slot;
    ++queue.size;
}

void Http3WorkerMailboxScheduler::remove(Slot& slot, QueueId id) noexcept {
    auto& link = slot.links[queueIndex(id)];
    if (!link.linked) {
        std::terminate();
    }
    auto& queue = queues_[queueIndex(id)];
    if (link.previous != nullptr) {
        link.previous->links[queueIndex(id)].next = link.next;
    } else {
        queue.head = link.next;
    }
    if (link.next != nullptr) {
        link.next->links[queueIndex(id)].previous = link.previous;
    } else {
        queue.tail = link.previous;
    }
    link.previous = nullptr;
    link.next = nullptr;
    link.blockedGeneration = 0;
    link.linked = false;
    if (queue.size == 0) {
        std::terminate();
    }
    --queue.size;
}

Http3WorkerMailboxScheduler::Slot*
Http3WorkerMailboxScheduler::popFront(QueueId id) noexcept {
    auto* slot = queues_[queueIndex(id)].head;
    if (slot != nullptr) {
        remove(*slot, id);
    }
    return slot;
}

void Http3WorkerMailboxScheduler::syncSlot(
    Slot& slot, const Connection::WorkerActivation& activation) noexcept {
    if (activation.work.wrongWorker) {
        std::terminate();
    }
    const bool active = slot.state == SlotState::kActive;
    setLinked(slot, QueueId::kDataRunnable, active && activation.work.runnable.data);
    setLinked(slot, QueueId::kControlRunnable, active && activation.work.runnable.control);
    setLinked(slot, QueueId::kLocalRunnable, active && activation.work.runnable.local);
    setLinked(slot, QueueId::kDataBlocked, active && activation.work.blocked.data);
    setLinked(slot, QueueId::kControlBlocked, active && activation.work.blocked.control);

    if (slot.parkedIntent && !sameIntent(activation.transportIntent, slot.parkedIntent)) {
        setLinked(slot, QueueId::kIntentBlocked, false);
        slot.parkedIntent.reset();
    }
    if (slot.offeredIntent && !sameIntent(activation.transportIntent, slot.offeredIntent)) {
        slot.offeredIntent.reset();
    }
    if (!activation.transportIntent) {
        setLinked(slot, QueueId::kIntentRunnable, false);
        setLinked(slot, QueueId::kIntentBlocked, false);
        slot.offeredIntent.reset();
        slot.parkedIntent.reset();
    } else if (slot.parkedIntent && *slot.parkedIntent == *activation.transportIntent) {
        setLinked(slot, QueueId::kIntentRunnable, false);
        setLinked(slot, QueueId::kIntentBlocked, true);
    } else if (slot.offeredIntent && *slot.offeredIntent == *activation.transportIntent) {
        setLinked(slot, QueueId::kIntentRunnable, false);
    } else {
        slot.offeredIntent.reset();
        setLinked(slot, QueueId::kIntentBlocked, false);
        setLinked(slot, QueueId::kIntentRunnable, true);
    }
}

void Http3WorkerMailboxScheduler::syncOwner(Slot& slot) noexcept {
    if (slot.owner == nullptr) {
        return;
    }
    syncSlot(slot, slot.owner->activationSnapshot());
}

void Http3WorkerMailboxScheduler::receiveActivation(Slot& slot, std::uint64_t epoch,
    std::uint64_t connectionGeneration, std::uint64_t slotGeneration,
    const Connection::WorkerActivation& activation) noexcept {
    if (!onWorker()) {
        std::terminate();
    }
    if (slot.scheduler != this || slot.state != SlotState::kActive ||
        slot.epoch != epoch || slot.connectionGeneration != connectionGeneration ||
        slot.slotGeneration != slotGeneration) {
        return;
    }
    syncSlot(slot, activation);
    const auto& work = activation.work;
    if (work.runnable.data || work.runnable.control || work.runnable.local ||
        activation.transportIntent) {
        localWakePending_ = true;
    }
}

void Http3WorkerMailboxScheduler::activationThunk(void* context, std::uint64_t epoch,
    std::uint64_t connectionGeneration, std::uint64_t slotGeneration,
    const Connection::WorkerActivation& activation) noexcept {
    if (context == nullptr) {
        std::terminate();
    }
    auto& slot = *static_cast<Slot*>(context);
    if (slot.scheduler == nullptr) {
        std::terminate();
    }
    slot.scheduler->receiveActivation(
        slot, epoch, connectionGeneration, slotGeneration, activation);
}

void Http3WorkerMailboxScheduler::consumeCapacityWake() noexcept {
    if (capacitySignal_ == nullptr ||
        capacitySignal_->generation() == consumedCapacityGeneration_) {
        return;
    }
    consumedCapacityGeneration_ = capacitySignal_->consume();
    capacityWaitArmed_ = false;
    const auto lanes = std::exchange(armedCapacityLanes_, std::uint8_t{0});
    if (lanes != 0) {
        startCapacityPass(lanes);
    }
}

void Http3WorkerMailboxScheduler::startCapacityPass(std::uint8_t lanes) noexcept {
    capacityPassCutoff_ = blockedGeneration_;
    capacityPassLanes_ |= lanes;
    normalizeCapacityPass();
    if (capacityPassLanes_ != 0) {
        localWakePending_ = true;
    }
}

void Http3WorkerMailboxScheduler::normalizeCapacityPass() noexcept {
    if ((capacityPassLanes_ & kCapacityData) != 0 &&
        !hasRecoverable(QueueId::kDataBlocked)) {
        capacityPassLanes_ &= static_cast<std::uint8_t>(~kCapacityData);
    }
    if ((capacityPassLanes_ & kCapacityControl) != 0 &&
        !hasRecoverable(QueueId::kControlBlocked) &&
        !hasRecoverable(QueueId::kIntentBlocked)) {
        capacityPassLanes_ &= static_cast<std::uint8_t>(~kCapacityControl);
    }
}

bool Http3WorkerMailboxScheduler::hasRecoverable(QueueId id) noexcept {
    if (id != QueueId::kDataBlocked && id != QueueId::kControlBlocked &&
        id != QueueId::kIntentBlocked) {
        return false;
    }
    const auto* head = queues_[queueIndex(id)].head;
    return head != nullptr && head->links[queueIndex(id)].blockedGeneration <= capacityPassCutoff_;
}

bool Http3WorkerMailboxScheduler::hasLaneWork(Lane lane) noexcept {
    switch (lane) {
        case Lane::kData:
            return queues_[queueIndex(QueueId::kDataRunnable)].head != nullptr ||
                   (((capacityPassLanes_ & kCapacityData) != 0) &&
                       hasRecoverable(QueueId::kDataBlocked));
        case Lane::kControl:
            return queues_[queueIndex(QueueId::kControlRunnable)].head != nullptr ||
                   (((capacityPassLanes_ & kCapacityControl) != 0) &&
                       (hasRecoverable(QueueId::kControlBlocked) ||
                           hasRecoverable(QueueId::kIntentBlocked)));
        case Lane::kLocal:
            return queues_[queueIndex(QueueId::kLocalRunnable)].head != nullptr;
        case Lane::kIntent:
            return queues_[queueIndex(QueueId::kIntentRunnable)].head != nullptr;
    }
    return false;
}

std::optional<Http3WorkerMailboxScheduler::Lane>
Http3WorkerMailboxScheduler::selectLane() noexcept {
    const bool dataReady = hasLaneWork(Lane::kData);
    if (controlBurst_ >= controlBurstLimit_ && dataReady) {
        return Lane::kData;
    }
    for (std::uint8_t offset = 0; offset < 4; ++offset) {
        const auto index = (static_cast<std::uint8_t>(nextLane_) + offset) % 4U;
        const auto lane = static_cast<Lane>(index);
        if (hasLaneWork(lane)) {
            return lane;
        }
    }
    return std::nullopt;
}

Http3WorkerMailboxScheduler::StepResult
Http3WorkerMailboxScheduler::stepPublication(Slot& slot, Lane lane) noexcept {
    if (slot.owner == nullptr) {
        return {.kind = StepKind::kReconciled};
    }
    Connection::WorkLanes lanes{};
    switch (lane) {
        case Lane::kData:
            lanes.data = true;
            break;
        case Lane::kControl:
            lanes.control = true;
            break;
        case Lane::kLocal:
            lanes.local = true;
            break;
        case Lane::kIntent:
            return {.kind = StepKind::kReconciled};
    }
    const auto token = tokenFor(slot);
    const auto publication = slot.owner->publishOne(lanes);
    syncOwner(slot);
    if (publication.status == Connection::PublishStatus::kNoReadyRequest) {
        return {.kind = StepKind::kReconciled, .connection = token};
    }
    if (publication.status == Connection::PublishStatus::kWrongWorker) {
        return {.kind = StepKind::kWrongWorker, .connection = token};
    }
    return {.kind = StepKind::kPublication,
        .connection = token,
        .publication = publication,
        .notifyPeer = publication.publication.notifyPeer};
}

Http3WorkerMailboxScheduler::StepResult
Http3WorkerMailboxScheduler::stepIntent(Slot& slot) noexcept {
    if (slot.owner == nullptr) {
        return {.kind = StepKind::kReconciled};
    }
    const auto intent = slot.owner->peekTransportIntent();
    if (!intent) {
        syncOwner(slot);
        return {.kind = StepKind::kReconciled, .connection = tokenFor(slot)};
    }
    slot.offeredIntent = intent->token;
    setLinked(slot, QueueId::kIntentRunnable, false);
    return {.kind = StepKind::kTransportIntent,
        .connection = tokenFor(slot),
        .intent = *intent};
}

Http3WorkerMailboxScheduler::Slot*
Http3WorkerMailboxScheduler::selectRecoveryControl(bool& isIntent) noexcept {
    const bool controlReady = hasRecoverable(QueueId::kControlBlocked);
    const bool intentReady = hasRecoverable(QueueId::kIntentBlocked);
    if (!controlReady && !intentReady) {
        capacityPassLanes_ &= static_cast<std::uint8_t>(~kCapacityControl);
        isIntent = false;
        return nullptr;
    }
    if (intentReady && controlReady) {
        isIntent = preferIntentRecovery_;
        preferIntentRecovery_ = !preferIntentRecovery_;
    } else {
        isIntent = intentReady;
    }
    auto* slot = popFront(isIntent ? QueueId::kIntentBlocked : QueueId::kControlBlocked);
    if (slot != nullptr && isIntent) {
        // The connection activation snapshot remains authoritative; keep the
        // exact blocked token until this slot receives its recovery turn.
        slot->links[queueIndex(QueueId::kIntentBlocked)].blockedGeneration = 0;
    }
    return slot;
}

void Http3WorkerMailboxScheduler::clearSlot(Slot& slot) noexcept {
    for (std::size_t index = 0; index < queueIndex(QueueId::kCount); ++index) {
        if (slot.links[index].linked) {
            remove(slot, static_cast<QueueId>(index));
        }
    }
    slot.owner = nullptr;
    slot.epoch = 0;
    slot.connectionGeneration = 0;
    slot.offeredIntent.reset();
    slot.parkedIntent.reset();
}

}  // namespace ruvia::detail
