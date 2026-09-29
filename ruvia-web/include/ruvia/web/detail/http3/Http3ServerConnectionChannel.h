#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <thread>
#include <variant>

#include "ruvia/web/detail/http3/Http3WorkerMailboxScheduler.h"

namespace ruvia::detail {

// Fixed-capacity per-connection admission/control ledger. DATA remains in the
// stream mailbox: this type never copies request/response bytes.
class Http3ServerConnectionChannel final {
public:
    using Connection = Http3ServerConnection;
    using TransportIntent = Connection::TransportIntent;
    using TransportIntentToken = Connection::TransportIntentToken;

    struct Identity final {
        std::uint64_t epoch{};
        std::uint64_t connectionGeneration{};
        std::size_t slot{};
        std::uint64_t slotGeneration{};

        friend bool operator==(const Identity&, const Identity&) noexcept = default;
    };

    struct Notification final {
        // notify() synchronously latches a reliable wake obligation. It must not
        // re-enter the channel or retain a channel pointer. The target context
        // outlives both owners and every already-latched callback.
        void* context{};
        void (*notify)(void*) noexcept = nullptr;
    };

    enum class Status : std::uint8_t {
        kPublished,
        kReceived,
        kEmpty,
        kFull,
        kStale,
        kWrongState,
        kWrongOwner,
        kUnavailable,
    };

    static constexpr std::size_t kControlCapacity = 2;
    static constexpr std::size_t kIntentTrackingCapacity = 2 * kControlCapacity;

    struct GrantPublication final {
        Status status{Status::kUnavailable};
        Identity identity{};
        Http3WorkerMailboxScheduler::Registration registration{};
    };

    struct ConnectionMetadataView final {
        std::string_view remoteAddress{};
        std::string_view clientCertificateSubject{};
        std::uint16_t remotePort{};
    };

    struct Bind final {
        Identity identity{};
        ConnectionMetadataView metadata{};
    };

    enum class RejectReason : std::uint8_t {
        kCapacity,
        kConstructionFailed,
        kStopping,
    };

    struct AttachAck final {
        Identity identity{};
    };

    struct AttachRejected final {
        Identity identity{};
        RejectReason reason{RejectReason::kCapacity};
    };

    using AttachResult = std::variant<AttachAck, AttachRejected>;

    struct RevokeAck final {
        Identity identity{};
    };

    enum class IntentSettlement : std::uint8_t {
        kExecutedHandoff,
        kTransportRetiredSuperseded,
    };

    struct TransportIntentAck final {
        Identity identity{};
        TransportIntentToken token{};
        IntentSettlement settlement{IntentSettlement::kExecutedHandoff};
    };

    struct TransportRetired final {
        Identity identity{};
    };

    // Sticky request-admission seal sent from network to worker. expectedAdmittedRequests
    // is generation-local and excludes rejected post-GOAWAY streams.
    struct AdmissionSealed final {
        Identity identity{};
        std::size_t expectedAdmittedRequests{};
        std::uint64_t goawayId{};
    };

    // Published only after every request admitted before the seal has completed.
    struct DrainComplete final {
        Identity identity{};
    };

    struct WorkerFinalized final {
        Identity identity{};
    };

    struct NetworkFinalized final {
        Identity identity{};
    };

    // Construct on the server network owner. Grant/commit/revoke and all network
    // controls are serialized there; worker controls are worker-affine. No
    // WorkerHandle::post fallback is used.
    explicit Http3ServerConnectionChannel(
        Notification networkWake, Notification workerWake);
    ~Http3ServerConnectionChannel();

    Http3ServerConnectionChannel(const Http3ServerConnectionChannel&) = delete;
    Http3ServerConnectionChannel& operator=(const Http3ServerConnectionChannel&) = delete;
    Http3ServerConnectionChannel(Http3ServerConnectionChannel&&) = delete;
    Http3ServerConnectionChannel& operator=(Http3ServerConnectionChannel&&) = delete;

    // Worker-owner only. Grant identity is based on the actual scheduler slot
    // reservation. Keep the returned Registration until attach/reject/revoke.
    // Generations must increase lexicographically across rearm; exhausted or
    // wrapped identities are rejected, never reused.
    [[nodiscard]] GrantPublication reserveAndPublishGrant(
        Http3WorkerMailboxScheduler& scheduler, std::uint64_t epoch,
        std::uint64_t connectionGeneration) noexcept;
    // Permanently seals this channel against later Grants. The worker publishes
    // this before stopping; network rearm and worker Grant publication then have
    // a single acquire/release ordering point.
    [[nodiscard]] Status stopGrantPublication() noexcept;

    // Side-effect-free observation on the server network owner. A Grant remains
    // pending until commitAccepted() or revokeGrant(). Call commitAccepted() only after
    // transport.acceptConnections(1) returns exactly one real connection id.
    // An empty accept leaves this Grant untouched. If commit then fails, the
    // server network owner still owns and must physically retire the accepted id/SSL.
    [[nodiscard]] Status peekGrant(Identity& identity) const noexcept;
    [[nodiscard]] Status commitAccepted(Identity identity) noexcept;
    [[nodiscard]] Status commitAccepted(
        Identity identity, ConnectionMetadataView metadata) noexcept;
    [[nodiscard]] Status revokeGrant() noexcept;
    [[nodiscard]] Status receiveRevoke(Identity& identity) noexcept;
    [[nodiscard]] Status acknowledgeRevoke(Http3WorkerMailboxScheduler& scheduler,
        const Http3WorkerMailboxScheduler::Registration& registration) noexcept;
    [[nodiscard]] Status receiveRevokeAck(RevokeAck& acknowledgement) noexcept;
    [[nodiscard]] Status receiveBind(Bind& bind) noexcept;

    // attach() publishes success only after the real scheduler.attach(token,
    // connection) succeeds. Construction failures use reject(), which abandons
    // the reservation before publishing a distinct AttachRejected record.
    [[nodiscard]] Status attach(Http3WorkerMailboxScheduler& scheduler,
        const Http3WorkerMailboxScheduler::Registration& registration,
        Connection& connection) noexcept;
    [[nodiscard]] Status reject(Http3WorkerMailboxScheduler& scheduler,
        const Http3WorkerMailboxScheduler::Registration& registration,
        RejectReason reason) noexcept;
    [[nodiscard]] Status receiveAttachResult(AttachResult& result) noexcept;

    // Stream-reset intents retain the complete TransportIntent value in a fixed
    // CONTROL lane. On kFull, keep the scheduler StepResult/intent and do not
    // acknowledge it; retry that exact value after the dequeue wake. Connection-
    // close intents use a separate sticky record, independent of lane capacity.
    [[nodiscard]] Status publishIntent(Identity identity,
        const TransportIntent& intent) noexcept;
    [[nodiscard]] Status receiveIntent(TransportIntent& intent) noexcept;
    // Call after either an executed handoff or physical-retirement supersession
    // of this exact received token. Superseded is valid only after this generation
    // has published TransportRetired. A full ACK lane retains the exact token and
    // settlement so the caller can retry without changing its claim.
    [[nodiscard]] Status acknowledgeIntentAfterHandoff(Identity identity,
        const TransportIntentToken& token,
        IntentSettlement settlement = IntentSettlement::kExecutedHandoff) noexcept;
    [[nodiscard]] Status receiveIntentAck(TransportIntentAck& acknowledgement) noexcept;

    // The server network owner records physical retirement only after its
    // actually released the connection. The sticky fact is also a one-shot
    // worker signal; attached/rejected generations must consume it before closing
    // worker publications. WorkerFinalized requires the worker's scheduler/
    // connection retirement and callback detachment to be complete;
    // NetworkFinalized requires its transport/map/callback retirement to be
    // complete. These are separate sticky controls, not ordinary lane messages.
    [[nodiscard]] Status publishTransportRetired(Identity identity) noexcept;
    [[nodiscard]] Status receiveTransportRetired(TransportRetired& retired) noexcept;
    [[nodiscard]] Status publishAdmissionSealed(Identity identity,
        std::size_t expectedAdmittedRequests, std::uint64_t goawayId) noexcept;
    [[nodiscard]] Status receiveAdmissionSealed(AdmissionSealed& sealed) noexcept;
    [[nodiscard]] Status publishDrainComplete(Identity identity) noexcept;
    [[nodiscard]] Status receiveDrainComplete(DrainComplete& complete) noexcept;
    // Caller precondition: each generation's producer/callback sources are
    // stopped and joined before its owner closes this gate. The channel cannot
    // observe external owner joins or deferred wake-target lifetime. Finalized
    // records become publishable only after both gates are closed.
    [[nodiscard]] Status closeWorkerPublications(Identity identity) noexcept;
    [[nodiscard]] Status closeNetworkPublications(Identity identity) noexcept;
    [[nodiscard]] Status publishWorkerFinalized(Identity identity) noexcept;
    [[nodiscard]] Status publishNetworkFinalized(Identity identity) noexcept;
    [[nodiscard]] Status receiveWorkerFinalized(WorkerFinalized& finalized) noexcept;
    [[nodiscard]] Status acknowledgeWorkerFinalized(Identity identity) noexcept;
    [[nodiscard]] Status receiveNetworkFinalized(NetworkFinalized& finalized) noexcept;
    [[nodiscard]] Status acknowledgeNetworkFinalized(Identity identity) noexcept;

    // Caller precondition: both connection owners have stopped and joined their
    // related callbacks. The channel checks owner-published gates, sticky
    // confirmations, empty lanes/pending records, and synchronous notification
    // borrows, but cannot verify external joins/deferred wake-target lifetime.
    // rearm leaves the worker-owned identity untouched until the next reservation.
    [[nodiscard]] bool readyToRearm() const noexcept;
    [[nodiscard]] Status rearm() noexcept;
    // Query/destroy only after both owner threads and generation callbacks join.
    [[nodiscard]] bool readyToDestroy() const noexcept;
    [[nodiscard]] std::uint32_t notificationBorrows() const noexcept;

private:
    enum class Lifecycle : std::uint8_t {
        kVacant,
        kGrantAvailable,
        kRevokeRequested,
        kBinding,
        kAttached,
        kRejected,
        kRevoked,
    };

    template <typename T, std::size_t Capacity>
    class SpscLane final {
    public:
        [[nodiscard]] bool hasCapacity() const noexcept {
            return !slots_[write_].published.load(std::memory_order_acquire);
        }

        [[nodiscard]] bool tryPublish(const T& value) noexcept {
            auto& slot = slots_[write_];
            if (slot.published.load(std::memory_order_acquire)) {
                return false;
            }
            slot.value = value;
            slot.published.store(true, std::memory_order_release);
            write_ = (write_ + 1) % Capacity;
            return true;
        }

        [[nodiscard]] bool tryRead(T& value) const noexcept {
            const auto& slot = slots_[read_];
            if (!slot.published.load(std::memory_order_acquire)) {
                return false;
            }
            value = slot.value;
            return true;
        }

        void releaseRead() noexcept {
            auto& slot = slots_[read_];
            slot.published.store(false, std::memory_order_release);
            read_ = (read_ + 1) % Capacity;
        }

        [[nodiscard]] bool empty() const noexcept {
            for (const auto& slot : slots_) {
                if (slot.published.load(std::memory_order_acquire)) {
                    return false;
                }
            }
            return true;
        }

    private:
        struct Slot final {
            T value{};
            std::atomic<bool> published{};
        };

        std::array<Slot, Capacity> slots_{};
        std::size_t write_{};  // producer-owned
        std::size_t read_{};   // consumer-owned
    };

    struct IntentMessage final {
        Identity identity{};
        TransportIntent intent{};
    };

    struct PendingIntent final {
        Identity identity{};
        TransportIntentToken token{};
        bool occupied{};
    };

    struct OutstandingIntent final {
        Identity identity{};
        TransportIntentToken token{};
        bool occupied{};
    };

    [[nodiscard]] bool matches(Identity identity) const noexcept;
    [[nodiscard]] bool isAttached() const noexcept;
    [[nodiscard]] bool transportRetired(Identity identity) const noexcept;
    [[nodiscard]] bool onNetworkOwner() const noexcept;
    [[nodiscard]] bool onWorkerOwner() const noexcept;
    [[nodiscard]] bool allControlEmpty() const noexcept;
    [[nodiscard]] bool terminalRecordsComplete() const noexcept;
    [[nodiscard]] bool hasOutstandingIntent(Identity identity,
        const TransportIntentToken& token) const noexcept;
    [[nodiscard]] bool hasFreePendingIntent() const noexcept;
    [[nodiscard]] bool hasFreeOutstandingIntent() const noexcept;
    [[nodiscard]] bool pendingIntentsEmpty() const noexcept;
    [[nodiscard]] bool outstandingIntentsEmpty() const noexcept;
    [[nodiscard]] static Identity identityOf(
        const Http3WorkerMailboxScheduler::ConnectionToken& token) noexcept;
    [[nodiscard]] static bool registrationMatches(
        const Http3WorkerMailboxScheduler::Registration& registration,
        Identity identity) noexcept;
    [[nodiscard]] static bool intentMatchesIdentity(
        const TransportIntent& intent, Identity identity) noexcept;
    void beginNotification() noexcept;
    void endNotification() noexcept;
    void notifyNetworkBorrowed() noexcept;
    void notifyWorkerBorrowed() noexcept;

    const Notification networkWake_;
    const Notification workerWake_;
    const std::thread::id networkOwner_;
    std::thread::id workerOwner_{};
    std::atomic<bool> workerOwnerBound_{};
    std::atomic<bool> workerStopping_{};
    std::atomic<bool> attachSucceeded_{};
    std::atomic<Lifecycle> lifecycle_{Lifecycle::kVacant};
    Identity identity_{};  // worker-owned; published before lifecycle release
    std::atomic<bool> hasLastIdentity_{};
    std::uint64_t lastEpoch_{};
    std::uint64_t lastConnectionGeneration_{};

    Bind bind_{};
    std::atomic<bool> bindPublished_{};
    bool bindTaken_{};
    bool revokeTaken_{};
    RevokeAck revokeAck_{};
    std::atomic<bool> revokeAckPublished_{};
    AttachResult attachResult_{AttachAck{}};
    std::atomic<bool> attachResultPublished_{};

    SpscLane<IntentMessage, kControlCapacity> workerToNetworkControl_{};
    SpscLane<TransportIntentAck, kControlCapacity> networkToWorkerControl_{};
    std::array<PendingIntent, kIntentTrackingCapacity> pendingIntents_{};
    std::array<OutstandingIntent, kIntentTrackingCapacity> outstandingIntents_{};
    std::atomic<bool> closeIntentPublished_{};
    bool closeIntentReceived_{};   // server-network-owned
    bool closingGeneration_{};     // worker-owned until both gates close
    bool hasLastResetSequence_{};  // worker-owned
    std::uint64_t lastResetSequence_{};
    IntentMessage closeIntent_{};
    std::atomic<bool> closeIntentAckPublished_{};
    bool closeIntentAckTaken_{};  // worker-owned
    TransportIntentAck closeIntentAck_{};

    std::atomic<bool> workerPublicationsClosed_{};
    std::atomic<bool> networkPublicationsClosed_{};
    Identity transportRetiredIdentity_{};
    std::atomic<bool> transportRetiredPublished_{};
    AdmissionSealed admissionSealed_{};  // server-network-owned; release-published
    std::atomic<bool> admissionSealedPublished_{};
    bool admissionSealedTaken_{};    // worker-owned
    DrainComplete drainComplete_{};  // worker-owned; release-published
    std::atomic<bool> drainCompletePublished_{};
    bool drainCompleteTaken_{};  // server-network-owned
    Identity workerFinalizedIdentity_{};
    std::atomic<bool> workerFinalizedPublished_{};
    Identity networkFinalizedIdentity_{};
    std::atomic<bool> networkFinalizedPublished_{};
    std::atomic<bool> workerFinalizedAcknowledged_{};
    std::atomic<bool> networkFinalizedAcknowledged_{};
    bool transportRetiredTaken_{};  // worker-owned
    bool workerFinalizedTaken_{};   // server-network-owned
    bool networkFinalizedTaken_{};  // worker-owned
    std::atomic<std::uint32_t> notificationBorrows_{};
};

}  // namespace ruvia::detail
