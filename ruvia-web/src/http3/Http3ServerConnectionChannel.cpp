#include "ruvia/web/detail/http3/Http3ServerConnectionChannel.h"

#include <algorithm>
#include <exception>
#include <limits>
#include <stdexcept>

namespace ruvia::detail {

Http3ServerConnectionChannel::Http3ServerConnectionChannel(
    Notification networkWake, Notification workerWake)
    : networkWake_(networkWake),
      workerWake_(workerWake),
      networkOwner_(std::this_thread::get_id()) {
    if (networkWake_.context == nullptr || networkWake_.notify == nullptr ||
        workerWake_.context == nullptr || workerWake_.notify == nullptr) {
        throw std::invalid_argument("HTTP/3 server connection channel requires both wake targets");
    }
}

Http3ServerConnectionChannel::~Http3ServerConnectionChannel() {
    if (!readyToDestroy()) {
        std::terminate();
    }
}

Http3ServerConnectionChannel::GrantPublication
Http3ServerConnectionChannel::reserveAndPublishGrant(
    Http3WorkerMailboxScheduler& scheduler, std::uint64_t epoch,
    std::uint64_t connectionGeneration) noexcept {
    if (workerOwnerBound_.load(std::memory_order_acquire) && !onWorkerOwner()) {
        return {.status = Status::kWrongOwner};
    }
    if (workerStopping_.load(std::memory_order_acquire) ||
        workerPublicationsClosed_.load(std::memory_order_acquire) ||
        lifecycle_.load(std::memory_order_acquire) != Lifecycle::kVacant) {
        return {.status = Status::kWrongState};
    }
    if (hasLastIdentity_.load(std::memory_order_acquire) &&
        (epoch < lastEpoch_ ||
            (epoch == lastEpoch_ && connectionGeneration <= lastConnectionGeneration_))) {
        return {.status = Status::kStale};
    }

    const auto registration = scheduler.reserve(epoch, connectionGeneration);
    if (!registration) {
        return {.status = Status::kUnavailable};
    }
    const auto identity = identityOf(registration->token);
    if (!workerOwnerBound_.load(std::memory_order_relaxed)) {
        workerOwner_ = std::this_thread::get_id();
        workerOwnerBound_.store(true, std::memory_order_release);
    }
    if (identity.epoch != epoch || identity.connectionGeneration != connectionGeneration) {
        std::terminate();
    }

    beginNotification();
    identity_ = identity;
    hasLastIdentity_.store(true, std::memory_order_release);
    lastEpoch_ = epoch;
    lastConnectionGeneration_ = connectionGeneration;
    bindTaken_ = false;
    revokeTaken_ = false;
    transportRetiredTaken_ = false;
    networkFinalizedTaken_ = false;
    closingGeneration_ = false;
    attachSucceeded_.store(false, std::memory_order_relaxed);
    hasLastResetSequence_ = false;
    lastResetSequence_ = 0;
    closeIntentAckTaken_ = false;
    closeIntentPublished_.store(false, std::memory_order_relaxed);
    closeIntentAckPublished_.store(false, std::memory_order_relaxed);
    admissionSealed_ = {};
    admissionSealedPublished_.store(false, std::memory_order_relaxed);
    admissionSealedTaken_ = false;
    drainComplete_ = {};
    drainCompletePublished_.store(false, std::memory_order_relaxed);
    drainCompleteTaken_ = false;
    attachResult_ = AttachAck{};
    lifecycle_.store(Lifecycle::kGrantAvailable, std::memory_order_release);
    notifyNetworkBorrowed();
    return {.status = Status::kPublished,
        .identity = identity,
        .registration = *registration};
}

Http3ServerConnectionChannel::Status
Http3ServerConnectionChannel::stopGrantPublication() noexcept {
    if (workerOwnerBound_.load(std::memory_order_acquire)) {
        if (!onWorkerOwner()) {
            return Status::kWrongOwner;
        }
    } else {
        workerOwner_ = std::this_thread::get_id();
        workerOwnerBound_.store(true, std::memory_order_release);
    }
    if (workerStopping_.exchange(true, std::memory_order_acq_rel)) {
        return Status::kWrongState;
    }
    beginNotification();
    notifyNetworkBorrowed();
    return Status::kPublished;
}

Http3ServerConnectionChannel::Status
Http3ServerConnectionChannel::peekGrant(Identity& identity) const noexcept {
    if (!onNetworkOwner()) {
        return Status::kWrongOwner;
    }
    if (networkPublicationsClosed_.load(std::memory_order_acquire)) {
        return Status::kWrongState;
    }
    if (lifecycle_.load(std::memory_order_acquire) != Lifecycle::kGrantAvailable) {
        return Status::kEmpty;
    }
    identity = identity_;
    return Status::kReceived;
}

Http3ServerConnectionChannel::Status
Http3ServerConnectionChannel::commitAccepted(Identity identity) noexcept {
    return commitAccepted(identity, {});
}

Http3ServerConnectionChannel::Status
Http3ServerConnectionChannel::commitAccepted(
    Identity identity, ConnectionMetadataView metadata) noexcept {
    if (!onNetworkOwner()) {
        return Status::kWrongOwner;
    }
    if (networkPublicationsClosed_.load(std::memory_order_acquire)) {
        return Status::kWrongState;
    }
    if (!matches(identity)) {
        return Status::kStale;
    }
    auto expected = Lifecycle::kGrantAvailable;
    if (!lifecycle_.compare_exchange_strong(expected, Lifecycle::kBinding,
            std::memory_order_acq_rel, std::memory_order_acquire)) {
        return Status::kWrongState;
    }
    if (bindPublished_.load(std::memory_order_acquire)) {
        std::terminate();
    }
    beginNotification();
    bind_ = {.identity = identity, .metadata = metadata};
    bindPublished_.store(true, std::memory_order_release);
    notifyWorkerBorrowed();
    return Status::kPublished;
}

Http3ServerConnectionChannel::Status
Http3ServerConnectionChannel::revokeGrant() noexcept {
    if (!onNetworkOwner()) {
        return Status::kWrongOwner;
    }
    if (networkPublicationsClosed_.load(std::memory_order_acquire)) {
        return Status::kWrongState;
    }
    auto expected = Lifecycle::kGrantAvailable;
    if (!lifecycle_.compare_exchange_strong(expected, Lifecycle::kRevokeRequested,
            std::memory_order_acq_rel, std::memory_order_acquire)) {
        return Status::kWrongState;
    }
    beginNotification();
    notifyWorkerBorrowed();
    return Status::kPublished;
}

Http3ServerConnectionChannel::Status
Http3ServerConnectionChannel::receiveRevoke(Identity& identity) noexcept {
    if (!onWorkerOwner()) {
        return Status::kWrongOwner;
    }
    if (workerPublicationsClosed_.load(std::memory_order_acquire)) {
        return Status::kWrongState;
    }
    if (lifecycle_.load(std::memory_order_acquire) != Lifecycle::kRevokeRequested ||
        revokeTaken_) {
        return Status::kEmpty;
    }
    identity = identity_;
    revokeTaken_ = true;
    return Status::kReceived;
}

Http3ServerConnectionChannel::Status
Http3ServerConnectionChannel::acknowledgeRevoke(
    Http3WorkerMailboxScheduler& scheduler,
    const Http3WorkerMailboxScheduler::Registration& registration) noexcept {
    if (!onWorkerOwner()) {
        return Status::kWrongOwner;
    }
    if (workerPublicationsClosed_.load(std::memory_order_acquire) ||
        lifecycle_.load(std::memory_order_acquire) != Lifecycle::kRevokeRequested ||
        !revokeTaken_) {
        return Status::kWrongState;
    }
    if (!registrationMatches(registration, identity_)) {
        return Status::kStale;
    }
    if (revokeAckPublished_.load(std::memory_order_acquire)) {
        return Status::kWrongState;
    }
    if (!scheduler.abandon(registration.token)) {
        return Status::kWrongState;
    }

    beginNotification();
    revokeAck_ = {.identity = identity_};
    revokeAckPublished_.store(true, std::memory_order_release);
    notifyNetworkBorrowed();
    return Status::kPublished;
}

Http3ServerConnectionChannel::Status
Http3ServerConnectionChannel::receiveRevokeAck(RevokeAck& acknowledgement) noexcept {
    if (!onNetworkOwner()) {
        return Status::kWrongOwner;
    }
    if (networkPublicationsClosed_.load(std::memory_order_acquire)) {
        return Status::kWrongState;
    }
    if (!revokeAckPublished_.load(std::memory_order_acquire)) {
        return Status::kEmpty;
    }
    acknowledgement = revokeAck_;
    const bool current = matches(acknowledgement.identity) &&
                         lifecycle_.load(std::memory_order_acquire) ==
                             Lifecycle::kRevokeRequested;
    beginNotification();
    revokeAckPublished_.store(false, std::memory_order_release);
    if (current) {
        lifecycle_.store(Lifecycle::kRevoked, std::memory_order_release);
    }
    notifyWorkerBorrowed();
    return current ? Status::kReceived : Status::kStale;
}

Http3ServerConnectionChannel::Status
Http3ServerConnectionChannel::receiveBind(Bind& bind) noexcept {
    if (!onWorkerOwner()) {
        return Status::kWrongOwner;
    }
    if (workerPublicationsClosed_.load(std::memory_order_acquire)) {
        return Status::kWrongState;
    }
    if (!bindPublished_.load(std::memory_order_acquire)) {
        return Status::kEmpty;
    }
    bind = bind_;
    const bool current = matches(bind.identity) &&
                         lifecycle_.load(std::memory_order_acquire) == Lifecycle::kBinding;
    beginNotification();
    bindPublished_.store(false, std::memory_order_release);
    if (current) {
        bindTaken_ = true;
    }
    notifyNetworkBorrowed();
    return current ? Status::kReceived : Status::kStale;
}

Http3ServerConnectionChannel::Status
Http3ServerConnectionChannel::attach(Http3WorkerMailboxScheduler& scheduler,
    const Http3WorkerMailboxScheduler::Registration& registration,
    Connection& connection) noexcept {
    if (!onWorkerOwner()) {
        return Status::kWrongOwner;
    }
    if (workerPublicationsClosed_.load(std::memory_order_acquire) ||
        !registrationMatches(registration, identity_)) {
        return Status::kStale;
    }
    if (!bindTaken_ || lifecycle_.load(std::memory_order_acquire) != Lifecycle::kBinding ||
        attachResultPublished_.load(std::memory_order_acquire)) {
        return Status::kWrongState;
    }
    if (!scheduler.attach(registration.token, connection)) {
        return Status::kUnavailable;
    }
    attachSucceeded_.store(true, std::memory_order_release);
    beginNotification();
    attachResult_ = AttachAck{.identity = identity_};
    attachResultPublished_.store(true, std::memory_order_release);
    notifyNetworkBorrowed();
    return Status::kPublished;
}

Http3ServerConnectionChannel::Status
Http3ServerConnectionChannel::reject(Http3WorkerMailboxScheduler& scheduler,
    const Http3WorkerMailboxScheduler::Registration& registration,
    RejectReason reason) noexcept {
    if (!onWorkerOwner()) {
        return Status::kWrongOwner;
    }
    if (workerPublicationsClosed_.load(std::memory_order_acquire) ||
        !registrationMatches(registration, identity_)) {
        return Status::kStale;
    }
    if (!bindTaken_ || lifecycle_.load(std::memory_order_acquire) != Lifecycle::kBinding ||
        attachResultPublished_.load(std::memory_order_acquire)) {
        return Status::kWrongState;
    }
    if (!scheduler.abandon(registration.token)) {
        return Status::kWrongState;
    }
    beginNotification();
    attachResult_ = AttachRejected{.identity = identity_, .reason = reason};
    attachResultPublished_.store(true, std::memory_order_release);
    notifyNetworkBorrowed();
    return Status::kPublished;
}

Http3ServerConnectionChannel::Status
Http3ServerConnectionChannel::receiveAttachResult(AttachResult& result) noexcept {
    if (!onNetworkOwner()) {
        return Status::kWrongOwner;
    }
    if (networkPublicationsClosed_.load(std::memory_order_acquire)) {
        return Status::kWrongState;
    }
    if (!attachResultPublished_.load(std::memory_order_acquire)) {
        return Status::kEmpty;
    }
    result = attachResult_;
    const auto identity = std::visit([](const auto& item) { return item.identity; }, result);
    const bool attached = std::holds_alternative<AttachAck>(result);
    const bool current = matches(identity) &&
                         lifecycle_.load(std::memory_order_acquire) == Lifecycle::kBinding;
    beginNotification();
    if (current) {
        lifecycle_.store(attached ? Lifecycle::kAttached : Lifecycle::kRejected,
            std::memory_order_release);
    }
    attachResultPublished_.store(false, std::memory_order_release);
    notifyWorkerBorrowed();
    return current ? Status::kReceived : Status::kStale;
}

Http3ServerConnectionChannel::Status
Http3ServerConnectionChannel::publishIntent(Identity identity,
    const TransportIntent& intent) noexcept {
    if (!onWorkerOwner()) {
        return Status::kWrongOwner;
    }
    if (workerPublicationsClosed_.load(std::memory_order_acquire) ||
        networkPublicationsClosed_.load(std::memory_order_acquire) ||
        workerFinalizedPublished_.load(std::memory_order_acquire) ||
        networkFinalizedPublished_.load(std::memory_order_acquire)) {
        return Status::kWrongState;
    }
    if (!matches(identity) || !intentMatchesIdentity(intent, identity)) {
        return Status::kStale;
    }
    const auto lifecycle = lifecycle_.load(std::memory_order_acquire);
    // attach() is worker-affine and only sets this after scheduler.attach()
    // succeeds. Permit both close and reset intents while its AttachAck awaits
    // network consumption; otherwise the next scheduler turn can terminate on
    // a valid intent in this short publication window.
    const bool attachAckPending = lifecycle == Lifecycle::kBinding &&
                                  attachSucceeded_.load(std::memory_order_acquire);
    if ((!isAttached() && !attachAckPending) || closingGeneration_) {
        return Status::kWrongState;
    }

    if (intent.token.kind == Connection::TransportIntentKind::kConnectionClose) {
        beginNotification();
        closingGeneration_ = true;
        closeIntent_ = {.identity = identity, .intent = intent};
        closeIntentPublished_.store(true, std::memory_order_release);
        notifyNetworkBorrowed();
        return Status::kPublished;
    }
    if (intent.token.kind != Connection::TransportIntentKind::kStreamReset) {
        return Status::kWrongState;
    }
    if (hasLastResetSequence_ && intent.token.sequence <= lastResetSequence_) {
        return Status::kStale;
    }
    if (hasOutstandingIntent(identity, intent.token)) {
        return Status::kStale;
    }
    if (!workerToNetworkControl_.hasCapacity() || !hasFreeOutstandingIntent()) {
        return Status::kFull;
    }

    const IntentMessage message{.identity = identity, .intent = intent};
    auto free = std::find_if(outstandingIntents_.begin(), outstandingIntents_.end(),
        [](const OutstandingIntent& entry) { return !entry.occupied; });
    if (free == outstandingIntents_.end()) {
        return Status::kFull;
    }
    beginNotification();
    *free = {.identity = identity, .token = intent.token, .occupied = true};
    hasLastResetSequence_ = true;
    lastResetSequence_ = intent.token.sequence;
    if (!workerToNetworkControl_.tryPublish(message)) {
        std::terminate();
    }
    notifyNetworkBorrowed();
    return Status::kPublished;
}

Http3ServerConnectionChannel::Status
Http3ServerConnectionChannel::receiveIntent(TransportIntent& intent) noexcept {
    if (!onNetworkOwner()) {
        return Status::kWrongOwner;
    }
    if (networkPublicationsClosed_.load(std::memory_order_acquire) ||
        workerPublicationsClosed_.load(std::memory_order_acquire) ||
        workerFinalizedPublished_.load(std::memory_order_acquire) ||
        networkFinalizedPublished_.load(std::memory_order_acquire)) {
        return Status::kWrongState;
    }
    if (closeIntentPublished_.load(std::memory_order_acquire) && !closeIntentReceived_) {
        if (!matches(closeIntent_.identity) ||
            closeIntent_.intent.token.kind != Connection::TransportIntentKind::kConnectionClose) {
            return Status::kStale;
        }
        closeIntentReceived_ = true;
        intent = closeIntent_.intent;
        return Status::kReceived;
    }

    IntentMessage message;
    if (workerToNetworkControl_.tryRead(message)) {
        auto free = std::find_if(pendingIntents_.begin(), pendingIntents_.end(),
            [](const PendingIntent& entry) { return !entry.occupied; });
        if (free == pendingIntents_.end()) {
            return Status::kFull;
        }
        if (!matches(message.identity) || !intentMatchesIdentity(message.intent, message.identity)) {
            beginNotification();
            workerToNetworkControl_.releaseRead();
            notifyWorkerBorrowed();
            return Status::kStale;
        }
        *free = {.identity = message.identity,
            .token = message.intent.token,
            .occupied = true};
        intent = message.intent;
        beginNotification();
        workerToNetworkControl_.releaseRead();
        notifyWorkerBorrowed();
        return Status::kReceived;
    }

    return Status::kEmpty;
}

Http3ServerConnectionChannel::Status
Http3ServerConnectionChannel::acknowledgeIntentAfterHandoff(Identity identity,
    const TransportIntentToken& token, IntentSettlement settlement) noexcept {
    if (!onNetworkOwner()) {
        return Status::kWrongOwner;
    }
    if (networkPublicationsClosed_.load(std::memory_order_acquire) ||
        workerPublicationsClosed_.load(std::memory_order_acquire) ||
        workerFinalizedPublished_.load(std::memory_order_acquire) ||
        networkFinalizedPublished_.load(std::memory_order_acquire)) {
        return Status::kWrongState;
    }
    if (!matches(identity) || token.id.epoch != identity.epoch ||
        token.id.connectionGeneration != identity.connectionGeneration) {
        return Status::kStale;
    }
    if (settlement == IntentSettlement::kTransportRetiredSuperseded &&
        !transportRetired(identity)) {
        return Status::kWrongState;
    }

    if (token.kind == Connection::TransportIntentKind::kConnectionClose) {
        if (!closeIntentPublished_.load(std::memory_order_acquire) || !closeIntentReceived_ ||
            closeIntent_.identity != identity || closeIntent_.intent.token != token ||
            closeIntentAckPublished_.load(std::memory_order_acquire)) {
            return Status::kStale;
        }
        beginNotification();
        closeIntentAck_ = {.identity = identity, .token = token, .settlement = settlement};
        closeIntentAckPublished_.store(true, std::memory_order_release);
        notifyWorkerBorrowed();
        return Status::kPublished;
    }
    if (token.kind != Connection::TransportIntentKind::kStreamReset) {
        return Status::kStale;
    }
    auto pending = std::find_if(pendingIntents_.begin(), pendingIntents_.end(),
        [&](const PendingIntent& entry) {
            return entry.occupied && entry.identity == identity && entry.token == token;
        });
    if (pending == pendingIntents_.end()) {
        return Status::kStale;
    }
    if (!networkToWorkerControl_.hasCapacity()) {
        return Status::kFull;
    }
    beginNotification();
    const TransportIntentAck acknowledgement{.identity = identity,
        .token = token,
        .settlement = settlement};
    if (!networkToWorkerControl_.tryPublish(acknowledgement)) {
        std::terminate();
    }
    *pending = {};
    notifyWorkerBorrowed();
    return Status::kPublished;
}

Http3ServerConnectionChannel::Status
Http3ServerConnectionChannel::receiveIntentAck(
    TransportIntentAck& acknowledgement) noexcept {
    if (!onWorkerOwner()) {
        return Status::kWrongOwner;
    }
    if (workerPublicationsClosed_.load(std::memory_order_acquire) ||
        workerFinalizedPublished_.load(std::memory_order_acquire) ||
        networkFinalizedPublished_.load(std::memory_order_acquire)) {
        return Status::kWrongState;
    }
    if (closeIntentAckPublished_.load(std::memory_order_acquire) && !closeIntentAckTaken_) {
        acknowledgement = closeIntentAck_;
        const bool current = matches(acknowledgement.identity) &&
                             closeIntentPublished_.load(std::memory_order_acquire) &&
                             closeIntent_.identity == acknowledgement.identity &&
                             closeIntent_.intent.token == acknowledgement.token;
        closeIntentAckTaken_ = true;
        if (current) {
            closeIntentAckPublished_.store(false, std::memory_order_release);
            closeIntentPublished_.store(false, std::memory_order_release);
            beginNotification();
            notifyNetworkBorrowed();
        }
        return current ? Status::kReceived : Status::kStale;
    }

    if (!networkToWorkerControl_.tryRead(acknowledgement)) {
        return Status::kEmpty;
    }
    const auto found = std::find_if(outstandingIntents_.begin(), outstandingIntents_.end(),
        [&](const OutstandingIntent& entry) {
            return entry.occupied && entry.identity == acknowledgement.identity &&
                   entry.token == acknowledgement.token;
        });
    const bool current = found != outstandingIntents_.end() &&
                         matches(acknowledgement.identity);
    if (current) {
        *found = {};
    }
    beginNotification();
    networkToWorkerControl_.releaseRead();
    notifyNetworkBorrowed();
    return current ? Status::kReceived : Status::kStale;
}

Http3ServerConnectionChannel::Status
Http3ServerConnectionChannel::publishTransportRetired(Identity identity) noexcept {
    if (!onNetworkOwner()) {
        return Status::kWrongOwner;
    }
    if (networkPublicationsClosed_.load(std::memory_order_acquire) || !matches(identity)) {
        return Status::kStale;
    }
    const auto lifecycle = lifecycle_.load(std::memory_order_acquire);
    if ((lifecycle != Lifecycle::kAttached && lifecycle != Lifecycle::kRejected) ||
        transportRetiredPublished_.load(std::memory_order_acquire)) {
        return Status::kWrongState;
    }
    beginNotification();
    transportRetiredIdentity_ = identity;
    transportRetiredPublished_.store(true, std::memory_order_release);
    notifyWorkerBorrowed();
    return Status::kPublished;
}

Http3ServerConnectionChannel::Status
Http3ServerConnectionChannel::receiveTransportRetired(TransportRetired& retired) noexcept {
    if (!onWorkerOwner()) {
        return Status::kWrongOwner;
    }
    if (workerPublicationsClosed_.load(std::memory_order_acquire)) {
        return Status::kWrongState;
    }
    if (!transportRetiredPublished_.load(std::memory_order_acquire) ||
        transportRetiredTaken_) {
        return Status::kEmpty;
    }
    retired = {.identity = transportRetiredIdentity_};
    if (!matches(retired.identity)) {
        return Status::kStale;
    }
    transportRetiredTaken_ = true;
    return Status::kReceived;
}

Http3ServerConnectionChannel::Status
Http3ServerConnectionChannel::publishAdmissionSealed(Identity identity,
    std::size_t expectedAdmittedRequests, std::uint64_t goawayId) noexcept {
    if (!onNetworkOwner()) {
        return Status::kWrongOwner;
    }
    if (networkPublicationsClosed_.load(std::memory_order_acquire) ||
        !matches(identity) || lifecycle_.load(std::memory_order_acquire) != Lifecycle::kAttached ||
        admissionSealedPublished_.load(std::memory_order_acquire) ||
        transportRetiredPublished_.load(std::memory_order_acquire) ||
        workerFinalizedPublished_.load(std::memory_order_acquire)) {
        return Status::kWrongState;
    }
    beginNotification();
    admissionSealed_ = {.identity = identity,
        .expectedAdmittedRequests = expectedAdmittedRequests,
        .goawayId = goawayId};
    admissionSealedPublished_.store(true, std::memory_order_release);
    notifyWorkerBorrowed();
    return Status::kPublished;
}

Http3ServerConnectionChannel::Status
Http3ServerConnectionChannel::receiveAdmissionSealed(AdmissionSealed& sealed) noexcept {
    if (!onWorkerOwner()) {
        return Status::kWrongOwner;
    }
    if (workerPublicationsClosed_.load(std::memory_order_acquire)) {
        return Status::kWrongState;
    }
    if (!admissionSealedPublished_.load(std::memory_order_acquire) || admissionSealedTaken_) {
        return Status::kEmpty;
    }
    sealed = admissionSealed_;
    if (!matches(sealed.identity)) {
        return Status::kStale;
    }
    admissionSealedTaken_ = true;
    return Status::kReceived;
}

Http3ServerConnectionChannel::Status
Http3ServerConnectionChannel::publishDrainComplete(Identity identity) noexcept {
    if (!onWorkerOwner()) {
        return Status::kWrongOwner;
    }
    if (workerPublicationsClosed_.load(std::memory_order_acquire) ||
        networkPublicationsClosed_.load(std::memory_order_acquire) ||
        !matches(identity) || !admissionSealedPublished_.load(std::memory_order_acquire) ||
        !admissionSealedTaken_ || drainCompletePublished_.load(std::memory_order_acquire) ||
        transportRetiredPublished_.load(std::memory_order_acquire) ||
        workerFinalizedPublished_.load(std::memory_order_acquire)) {
        return Status::kWrongState;
    }
    beginNotification();
    drainComplete_ = {.identity = identity};
    drainCompletePublished_.store(true, std::memory_order_release);
    notifyNetworkBorrowed();
    return Status::kPublished;
}

Http3ServerConnectionChannel::Status
Http3ServerConnectionChannel::receiveDrainComplete(DrainComplete& complete) noexcept {
    if (!onNetworkOwner()) {
        return Status::kWrongOwner;
    }
    if (!drainCompletePublished_.load(std::memory_order_acquire) || drainCompleteTaken_) {
        return Status::kEmpty;
    }
    complete = drainComplete_;
    if (!matches(complete.identity)) {
        return Status::kStale;
    }
    drainCompleteTaken_ = true;
    return Status::kReceived;
}

Http3ServerConnectionChannel::Status
Http3ServerConnectionChannel::closeWorkerPublications(Identity identity) noexcept {
    if (!onWorkerOwner()) {
        return Status::kWrongOwner;
    }
    if (workerPublicationsClosed_.load(std::memory_order_acquire)) {
        return Status::kWrongState;
    }
    const auto lifecycle = lifecycle_.load(std::memory_order_acquire);
    const bool idleRearmed = lifecycle == Lifecycle::kVacant &&
                             hasLastIdentity_.load(std::memory_order_acquire) &&
                             identity_ == identity;
    // Pending intents are server-network-owned and may still be changing. Worker
    // retirement checks its own outstanding tokens; network checks pending
    // tokens only after observing the closed worker publication gate.
    if ((!idleRearmed && !matches(identity)) ||
        (!idleRearmed && lifecycle != Lifecycle::kAttached &&
            lifecycle != Lifecycle::kRejected && lifecycle != Lifecycle::kRevoked) ||
        (!idleRearmed && lifecycle != Lifecycle::kRevoked &&
            (!transportRetired(identity) || !transportRetiredTaken_)) ||
        !workerToNetworkControl_.empty() || !networkToWorkerControl_.empty() ||
        !outstandingIntentsEmpty() ||
        (closeIntentPublished_.load(std::memory_order_acquire) && !closeIntentAckTaken_) ||
        bindPublished_.load(std::memory_order_acquire) ||
        attachResultPublished_.load(std::memory_order_acquire) ||
        revokeAckPublished_.load(std::memory_order_acquire) ||
        (admissionSealedPublished_.load(std::memory_order_acquire) && !admissionSealedTaken_) ||
        (drainCompletePublished_.load(std::memory_order_acquire) && !drainCompleteTaken_) ||
        notificationBorrows_.load(std::memory_order_acquire) != 0) {
        return Status::kWrongState;
    }
    beginNotification();
    workerPublicationsClosed_.store(true, std::memory_order_release);
    notifyNetworkBorrowed();
    return Status::kPublished;
}

Http3ServerConnectionChannel::Status
Http3ServerConnectionChannel::closeNetworkPublications(Identity identity) noexcept {
    if (!onNetworkOwner()) {
        return Status::kWrongOwner;
    }
    if (networkPublicationsClosed_.load(std::memory_order_acquire)) {
        return Status::kWrongState;
    }
    // A new Grant may be rewriting identity_ while the lifecycle is still
    // vacant. Acquire the worker's closed gate before reading that field.
    if (!workerPublicationsClosed_.load(std::memory_order_acquire)) {
        return Status::kWrongState;
    }
    const auto lifecycle = lifecycle_.load(std::memory_order_acquire);
    const bool idleRearmed = lifecycle == Lifecycle::kVacant &&
                             hasLastIdentity_.load(std::memory_order_acquire) &&
                             identity_ == identity;
    const bool revoked = lifecycle == Lifecycle::kRevoked;
    if ((!idleRearmed && !matches(identity)) ||
        (!idleRearmed && !revoked && lifecycle != Lifecycle::kAttached &&
            lifecycle != Lifecycle::kRejected) ||
        (!idleRearmed && !revoked && !transportRetired(identity)) ||
        !workerToNetworkControl_.empty() || !networkToWorkerControl_.empty() ||
        !pendingIntentsEmpty() ||
        (closeIntentPublished_.load(std::memory_order_acquire) && !closeIntentAckTaken_) ||
        bindPublished_.load(std::memory_order_acquire) ||
        attachResultPublished_.load(std::memory_order_acquire) ||
        revokeAckPublished_.load(std::memory_order_acquire) ||
        (admissionSealedPublished_.load(std::memory_order_acquire) && !admissionSealedTaken_) ||
        (drainCompletePublished_.load(std::memory_order_acquire) && !drainCompleteTaken_) ||
        notificationBorrows_.load(std::memory_order_acquire) != 0) {
        return Status::kWrongState;
    }
    beginNotification();
    networkPublicationsClosed_.store(true, std::memory_order_release);
    notifyWorkerBorrowed();
    return Status::kPublished;
}

Http3ServerConnectionChannel::Status
Http3ServerConnectionChannel::publishWorkerFinalized(Identity identity) noexcept {
    if (!onWorkerOwner()) {
        return Status::kWrongOwner;
    }
    if (!workerPublicationsClosed_.load(std::memory_order_acquire) ||
        !networkPublicationsClosed_.load(std::memory_order_acquire) || !matches(identity) ||
        !transportRetired(identity) || workerFinalizedPublished_.load(std::memory_order_acquire) ||
        !workerToNetworkControl_.empty() || !networkToWorkerControl_.empty() ||
        !outstandingIntentsEmpty() || notificationBorrows_.load(std::memory_order_acquire) != 0) {
        return Status::kWrongState;
    }
    beginNotification();
    workerFinalizedIdentity_ = identity;
    workerFinalizedPublished_.store(true, std::memory_order_release);
    notifyNetworkBorrowed();
    return Status::kPublished;
}

Http3ServerConnectionChannel::Status
Http3ServerConnectionChannel::publishNetworkFinalized(Identity identity) noexcept {
    if (!onNetworkOwner()) {
        return Status::kWrongOwner;
    }
    if (!networkPublicationsClosed_.load(std::memory_order_acquire) || !matches(identity) ||
        !transportRetired(identity) ||
        !workerFinalizedPublished_.load(std::memory_order_acquire) ||
        networkFinalizedPublished_.load(std::memory_order_acquire) ||
        !workerToNetworkControl_.empty() || !networkToWorkerControl_.empty() ||
        !pendingIntentsEmpty() || notificationBorrows_.load(std::memory_order_acquire) != 0) {
        return Status::kWrongState;
    }
    beginNotification();
    networkFinalizedIdentity_ = identity;
    networkFinalizedPublished_.store(true, std::memory_order_release);
    notifyWorkerBorrowed();
    return Status::kPublished;
}

Http3ServerConnectionChannel::Status
Http3ServerConnectionChannel::receiveWorkerFinalized(WorkerFinalized& finalized) noexcept {
    if (!onNetworkOwner()) {
        return Status::kWrongOwner;
    }
    if (!workerFinalizedPublished_.load(std::memory_order_acquire) ||
        workerFinalizedTaken_) {
        return Status::kEmpty;
    }
    finalized = {.identity = workerFinalizedIdentity_};
    if (!matches(finalized.identity)) {
        return Status::kStale;
    }
    workerFinalizedTaken_ = true;
    return Status::kReceived;
}

Http3ServerConnectionChannel::Status
Http3ServerConnectionChannel::acknowledgeWorkerFinalized(Identity identity) noexcept {
    if (!onNetworkOwner()) {
        return Status::kWrongOwner;
    }
    if (!workerFinalizedPublished_.load(std::memory_order_acquire) ||
        workerFinalizedIdentity_ != identity || !matches(identity) || !workerFinalizedTaken_ ||
        workerFinalizedAcknowledged_.load(std::memory_order_acquire)) {
        return Status::kStale;
    }
    beginNotification();
    workerFinalizedAcknowledged_.store(true, std::memory_order_release);
    notifyWorkerBorrowed();
    return Status::kPublished;
}

Http3ServerConnectionChannel::Status
Http3ServerConnectionChannel::receiveNetworkFinalized(NetworkFinalized& finalized) noexcept {
    if (!onWorkerOwner()) {
        return Status::kWrongOwner;
    }
    if (!networkFinalizedPublished_.load(std::memory_order_acquire) ||
        networkFinalizedTaken_) {
        return Status::kEmpty;
    }
    finalized = {.identity = networkFinalizedIdentity_};
    if (!matches(finalized.identity)) {
        return Status::kStale;
    }
    networkFinalizedTaken_ = true;
    return Status::kReceived;
}

Http3ServerConnectionChannel::Status
Http3ServerConnectionChannel::acknowledgeNetworkFinalized(Identity identity) noexcept {
    if (!onWorkerOwner()) {
        return Status::kWrongOwner;
    }
    if (!networkFinalizedPublished_.load(std::memory_order_acquire) ||
        networkFinalizedIdentity_ != identity || !matches(identity) || !networkFinalizedTaken_ ||
        networkFinalizedAcknowledged_.load(std::memory_order_acquire)) {
        return Status::kStale;
    }
    beginNotification();
    networkFinalizedAcknowledged_.store(true, std::memory_order_release);
    notifyNetworkBorrowed();
    return Status::kPublished;
}

bool Http3ServerConnectionChannel::readyToRearm() const noexcept {
    if (!onNetworkOwner()) {
        return false;
    }
    const auto lifecycle = lifecycle_.load(std::memory_order_acquire);
    const bool revoked = lifecycle == Lifecycle::kRevoked;
    return !workerStopping_.load(std::memory_order_acquire) &&
           (revoked || ((lifecycle == Lifecycle::kAttached || lifecycle == Lifecycle::kRejected) &&
                           terminalRecordsComplete())) &&
           workerPublicationsClosed_.load(std::memory_order_acquire) &&
           networkPublicationsClosed_.load(std::memory_order_acquire) && allControlEmpty() &&
           !bindPublished_.load(std::memory_order_acquire) &&
           !attachResultPublished_.load(std::memory_order_acquire) &&
           !revokeAckPublished_.load(std::memory_order_acquire) &&
           notificationBorrows_.load(std::memory_order_acquire) == 0;
}

Http3ServerConnectionChannel::Status
Http3ServerConnectionChannel::rearm() noexcept {
    if (!onNetworkOwner()) {
        return Status::kWrongOwner;
    }
    if (!readyToRearm()) {
        return Status::kWrongState;
    }

    closeIntentReceived_ = false;
    closeIntentAck_ = {};
    closeIntentPublished_.store(false, std::memory_order_relaxed);
    closeIntentAckPublished_.store(false, std::memory_order_relaxed);
    transportRetiredIdentity_ = {};
    transportRetiredPublished_.store(false, std::memory_order_relaxed);
    workerFinalizedIdentity_ = {};
    workerFinalizedPublished_.store(false, std::memory_order_relaxed);
    workerFinalizedAcknowledged_.store(false, std::memory_order_relaxed);
    networkFinalizedIdentity_ = {};
    networkFinalizedPublished_.store(false, std::memory_order_relaxed);
    networkFinalizedAcknowledged_.store(false, std::memory_order_relaxed);
    workerFinalizedTaken_ = false;
    admissionSealed_ = {};
    admissionSealedPublished_.store(false, std::memory_order_relaxed);
    admissionSealedTaken_ = false;
    drainComplete_ = {};
    drainCompletePublished_.store(false, std::memory_order_relaxed);
    drainCompleteTaken_ = false;
    workerPublicationsClosed_.store(false, std::memory_order_relaxed);
    networkPublicationsClosed_.store(false, std::memory_order_relaxed);
    lifecycle_.store(Lifecycle::kVacant, std::memory_order_release);
    return Status::kPublished;
}

bool Http3ServerConnectionChannel::readyToDestroy() const noexcept {
    if (!onNetworkOwner()) {
        return false;
    }
    const auto lifecycle = lifecycle_.load(std::memory_order_acquire);
    const bool revoked = lifecycle == Lifecycle::kRevoked;
    const bool finalized =
        (lifecycle == Lifecycle::kAttached || lifecycle == Lifecycle::kRejected) &&
        terminalRecordsComplete();
    const bool hasLastIdentity = hasLastIdentity_.load(std::memory_order_acquire);
    const bool neverUsed = lifecycle == Lifecycle::kVacant && !hasLastIdentity &&
                           allControlEmpty() &&
                           notificationBorrows_.load(std::memory_order_acquire) == 0;
    const bool idleRearmed = lifecycle == Lifecycle::kVacant && hasLastIdentity &&
                             workerPublicationsClosed_.load(std::memory_order_acquire) &&
                             networkPublicationsClosed_.load(std::memory_order_acquire) &&
                             allControlEmpty() &&
                             !bindPublished_.load(std::memory_order_acquire) &&
                             !attachResultPublished_.load(std::memory_order_acquire) &&
                             !revokeAckPublished_.load(std::memory_order_acquire) &&
                             !transportRetiredPublished_.load(std::memory_order_acquire) &&
                             !workerFinalizedPublished_.load(std::memory_order_acquire) &&
                             !networkFinalizedPublished_.load(std::memory_order_acquire) &&
                             !workerFinalizedAcknowledged_.load(std::memory_order_acquire) &&
                             !networkFinalizedAcknowledged_.load(std::memory_order_acquire) &&
                             notificationBorrows_.load(std::memory_order_acquire) == 0;
    return neverUsed || idleRearmed || ((revoked || finalized) && workerPublicationsClosed_.load(std::memory_order_acquire) && networkPublicationsClosed_.load(std::memory_order_acquire) && allControlEmpty() && !bindPublished_.load(std::memory_order_acquire) && !attachResultPublished_.load(std::memory_order_acquire) && !revokeAckPublished_.load(std::memory_order_acquire) && notificationBorrows_.load(std::memory_order_acquire) == 0);
}

std::uint32_t Http3ServerConnectionChannel::notificationBorrows() const noexcept {
    return notificationBorrows_.load(std::memory_order_acquire);
}

bool Http3ServerConnectionChannel::matches(Identity identity) const noexcept {
    const auto lifecycle = lifecycle_.load(std::memory_order_acquire);
    return lifecycle != Lifecycle::kVacant && identity_ == identity;
}

bool Http3ServerConnectionChannel::isAttached() const noexcept {
    return lifecycle_.load(std::memory_order_acquire) == Lifecycle::kAttached;
}

bool Http3ServerConnectionChannel::transportRetired(Identity identity) const noexcept {
    return transportRetiredPublished_.load(std::memory_order_acquire) &&
           transportRetiredIdentity_ == identity;
}

bool Http3ServerConnectionChannel::onNetworkOwner() const noexcept {
    return std::this_thread::get_id() == networkOwner_;
}

bool Http3ServerConnectionChannel::onWorkerOwner() const noexcept {
    return workerOwnerBound_.load(std::memory_order_acquire) &&
           std::this_thread::get_id() == workerOwner_;
}

bool Http3ServerConnectionChannel::allControlEmpty() const noexcept {
    return workerToNetworkControl_.empty() && networkToWorkerControl_.empty() &&
           pendingIntentsEmpty() && outstandingIntentsEmpty() &&
           !closeIntentPublished_.load(std::memory_order_acquire) &&
           !closeIntentAckPublished_.load(std::memory_order_acquire) &&
           (!admissionSealedPublished_.load(std::memory_order_acquire) || admissionSealedTaken_) &&
           (!drainCompletePublished_.load(std::memory_order_acquire) || drainCompleteTaken_);
}

bool Http3ServerConnectionChannel::terminalRecordsComplete() const noexcept {
    return transportRetiredPublished_.load(std::memory_order_acquire) &&
           workerFinalizedPublished_.load(std::memory_order_acquire) &&
           networkFinalizedPublished_.load(std::memory_order_acquire) &&
           workerFinalizedAcknowledged_.load(std::memory_order_acquire) &&
           networkFinalizedAcknowledged_.load(std::memory_order_acquire) &&
           (!admissionSealedPublished_.load(std::memory_order_acquire) || admissionSealedTaken_) &&
           (!drainCompletePublished_.load(std::memory_order_acquire) || drainCompleteTaken_) &&
           transportRetiredIdentity_ == workerFinalizedIdentity_ &&
           transportRetiredIdentity_ == networkFinalizedIdentity_;
}

bool Http3ServerConnectionChannel::hasOutstandingIntent(Identity identity,
    const TransportIntentToken& token) const noexcept {
    return std::any_of(outstandingIntents_.begin(), outstandingIntents_.end(),
        [&](const OutstandingIntent& entry) {
            return entry.occupied && entry.identity == identity && entry.token == token;
        });
}

bool Http3ServerConnectionChannel::hasFreePendingIntent() const noexcept {
    return std::any_of(pendingIntents_.begin(), pendingIntents_.end(),
        [](const PendingIntent& entry) { return !entry.occupied; });
}

bool Http3ServerConnectionChannel::hasFreeOutstandingIntent() const noexcept {
    return std::any_of(outstandingIntents_.begin(), outstandingIntents_.end(),
        [](const OutstandingIntent& entry) { return !entry.occupied; });
}

bool Http3ServerConnectionChannel::pendingIntentsEmpty() const noexcept {
    return std::none_of(pendingIntents_.begin(), pendingIntents_.end(),
        [](const PendingIntent& entry) { return entry.occupied; });
}

bool Http3ServerConnectionChannel::outstandingIntentsEmpty() const noexcept {
    return std::none_of(outstandingIntents_.begin(), outstandingIntents_.end(),
        [](const OutstandingIntent& entry) { return entry.occupied; });
}

Http3ServerConnectionChannel::Identity
Http3ServerConnectionChannel::identityOf(
    const Http3WorkerMailboxScheduler::ConnectionToken& token) noexcept {
    return {.epoch = token.epoch,
        .connectionGeneration = token.connectionGeneration,
        .slot = token.slot,
        .slotGeneration = token.slotGeneration};
}

bool Http3ServerConnectionChannel::registrationMatches(
    const Http3WorkerMailboxScheduler::Registration& registration,
    Identity identity) noexcept {
    return identityOf(registration.token) == identity;
}

bool Http3ServerConnectionChannel::intentMatchesIdentity(
    const TransportIntent& intent, Identity identity) noexcept {
    return intent.token.id.epoch == identity.epoch &&
           intent.token.id.connectionGeneration == identity.connectionGeneration;
}

void Http3ServerConnectionChannel::beginNotification() noexcept {
    if (notificationBorrows_.fetch_add(1, std::memory_order_acq_rel) ==
        std::numeric_limits<std::uint32_t>::max()) {
        std::terminate();
    }
}

void Http3ServerConnectionChannel::endNotification() noexcept {
    const auto previous = notificationBorrows_.fetch_sub(1, std::memory_order_acq_rel);
    if (previous == 0) {
        std::terminate();
    }
    if (previous == 1 &&
        (workerPublicationsClosed_.load(std::memory_order_acquire) ||
            networkPublicationsClosed_.load(std::memory_order_acquire) ||
            transportRetiredPublished_.load(std::memory_order_acquire) ||
            lifecycle_.load(std::memory_order_acquire) == Lifecycle::kRevoked)) {
        // A peer may have observed this borrow and deferred finalization after
        // the first wake. The last borrower must latch the retry itself.
        networkWake_.notify(networkWake_.context);
        workerWake_.notify(workerWake_.context);
    }
}

void Http3ServerConnectionChannel::notifyNetworkBorrowed() noexcept {
    networkWake_.notify(networkWake_.context);
    endNotification();
}

void Http3ServerConnectionChannel::notifyWorkerBorrowed() noexcept {
    workerWake_.notify(workerWake_.context);
    endNotification();
}

}  // namespace ruvia::detail
