#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <vector>

#include "ruvia/core/WorkerHandle.h"
#include "ruvia/web/detail/http3/Http3ServerConnection.h"

namespace ruvia::detail {

// Stable endpoint for the mailbox consumer's cross-thread capacity callback.
// Its wake target and this object must outlive every callback, including one
// already claimed when stop() clears the mailbox registration. The callback
// never dereferences a scheduler or a connection. Only the worker consumes it.
class Http3WorkerMailboxCapacitySignal final {
public:
    struct WakeRef final {
        void* context{};
        void (*notify)(void*) noexcept = nullptr;

        [[nodiscard]] bool valid() const noexcept {
            return context != nullptr && notify != nullptr;
        }
    };

    explicit Http3WorkerMailboxCapacitySignal(WakeRef wake);
    Http3WorkerMailboxCapacitySignal(const Http3WorkerMailboxCapacitySignal&) = delete;
    Http3WorkerMailboxCapacitySignal& operator=(const Http3WorkerMailboxCapacitySignal&) = delete;

    [[nodiscard]] Http3StreamMailboxCapacityNotifier notifier() noexcept;
    [[nodiscard]] std::uint64_t generation() const noexcept;
    // Clear the coalesced wake before observing the newest generation. A
    // concurrent producer either precedes this load or sends another wake.
    [[nodiscard]] std::uint64_t consume() noexcept;

private:
    static void notifyThunk(void* context) noexcept;
    const WakeRef wake_;
    std::atomic<std::uint64_t> generation_{};
    std::atomic<bool> pending_{};
};

struct Http3WorkerMailboxWakeRef final {
    void* context{};
    void (*notify)(void*) noexcept = nullptr;

    [[nodiscard]] bool valid() const noexcept {
        return context != nullptr && notify != nullptr;
    }
};

// Worker-affine sans-I/O scheduler for already-constructed buffered HTTP/3
// connection owners. It owns only bounded slot/index storage; transport intent
// handoff and the actual worker sleep/wake integration remain with its caller.
class Http3WorkerMailboxScheduler final {
public:
    using Connection = Http3ServerConnection;
    using Mailbox = Http3StreamMailbox;

    static constexpr std::size_t kDefaultControlBurstLimit = 4;

    struct ConnectionToken final {
        std::size_t slot{};
        std::uint64_t epoch{};
        std::uint64_t connectionGeneration{};
        std::uint64_t slotGeneration{};

        friend bool operator==(const ConnectionToken&, const ConnectionToken&) noexcept = default;
    };

    struct Registration final {
        ConnectionToken token{};
        Connection::ActivationRef activation{};
    };

    enum class StepKind : std::uint8_t {
        kIdle,
        kPublication,
        kTransportIntent,
        kReconciled,
        kWrongWorker,
    };

    struct StepResult final {
        StepKind kind{StepKind::kIdle};
        ConnectionToken connection{};
        Connection::PublishAttempt publication{};
        // This obligation is independent of publication.status; the caller
        // must notify the outbound consumer whenever it is true.
        bool notifyPeer{};
        Connection::TransportIntent intent{};
    };

    enum class CapacityArmStatus : std::uint8_t {
        kNoBlockedWork,
        kReady,
        kArmed,
        kWakePending,
        kRecoveryPending,
        kStopped,
        kUnavailable,
        kWrongWorker,
    };

    struct CapacityArmResult final {
        CapacityArmStatus status{CapacityArmStatus::kNoBlockedWork};
        Mailbox::CapacityInterest interest{Mailbox::CapacityInterest::kAny};
    };

    struct Snapshot final {
        std::array<std::size_t, 4> runnable{};  // DATA, CONTROL, LOCAL, intent
        std::array<std::size_t, 3> blocked{};   // DATA, CONTROL, reset intent
        std::size_t attachedConnections{};
        std::size_t freeConnections{};
        std::uint8_t capacityPassLanes{};
        bool localWakePending{};
        bool capacityWakePending{};
        bool capacityWaitArmed{};
        bool wrongWorker{};
    };

    // Construction is startup-only and preallocates all connection slots.
    // capacitySignal is optional, but without it a mailbox capacity wait
    // reports kUnavailable and must never be slept on. The borrowed signal
    // outlives this scheduler; its notifier can outlive this scheduler too.
    Http3WorkerMailboxScheduler(const WorkerHandle& worker, std::size_t maxConnections,
        std::pmr::memory_resource* resource = std::pmr::get_default_resource(),
        Http3WorkerMailboxCapacitySignal* capacitySignal = nullptr,
        std::size_t controlBurstLimit = kDefaultControlBurstLimit,
        Http3WorkerMailboxWakeRef workerWake = {});
    ~Http3WorkerMailboxScheduler();
    Http3WorkerMailboxScheduler(const Http3WorkerMailboxScheduler&) = delete;
    Http3WorkerMailboxScheduler& operator=(const Http3WorkerMailboxScheduler&) = delete;
    Http3WorkerMailboxScheduler(Http3WorkerMailboxScheduler&&) = delete;
    Http3WorkerMailboxScheduler& operator=(Http3WorkerMailboxScheduler&&) = delete;

    // O(1) reservation from a startup-preallocated free-slot stack. The
    // returned activation context remains address-stable until retire().
    [[nodiscard]] std::optional<Registration> reserve(
        std::uint64_t epoch, std::uint64_t connectionGeneration) noexcept;
    [[nodiscard]] bool attach(ConnectionToken token, Connection& connection) noexcept;
    // Roll back a reservation if owner construction failed before attach.
    [[nodiscard]] bool abandon(ConnectionToken token) noexcept;

    // Begin retirement after requestStop(): this fences old activations and
    // removes publication work but preserves any pending close/reset plan.
    // The slot is reusable only after owner.join(), intent handoff/retirement,
    // callback detachment, and retire().
    [[nodiscard]] bool beginRetirement(ConnectionToken token) noexcept;
    [[nodiscard]] bool retire(ConnectionToken token) noexcept;

    // Performs at most one owner.publishOne() or returns one by-value transport
    // intent plan. No intent is acknowledged here. A returned reset/open/close must
    // be explicitly acknowledged only after reliable handoff by the caller.
    [[nodiscard]] StepResult step() noexcept;
    [[nodiscard]] bool acknowledgeIntent(ConnectionToken connection,
        const Connection::TransportIntentToken& intent,
        std::optional<Connection::PushStreamOpenResult> opened = {}) noexcept;
    // Use when a reset or push-open plan could not be handed off because the
    // shared CONTROL lane is full. The intent remains owned by the connection.
    [[nodiscard]] bool parkIntentForControlCapacity(ConnectionToken connection,
        const Connection::TransportIntentToken& intent) noexcept;

    // Bind the one outbound mailbox before reserving connections. The borrowed
    // mailbox must outlive every capacity arm and its notifier callbacks.
    [[nodiscard]] bool bindMailbox(Mailbox& mailbox) noexcept;
    // Retry channel reset intents after a worker notification. This is a
    // bounded pass independent of response-mailbox capacity.
    void notifyTransportCapacity() noexcept;
    // Global single-producer capacity wait on that same mailbox. Call only
    // when blocked DATA or CONTROL publication exists. Ready/early
    // notifications are latched into a finite recovery pass.
    [[nodiscard]] CapacityArmResult armCapacityWait() noexcept;

    // A coalesced local wake duty for the upper worker loop. It is deliberately
    // separate from the cross-thread capacity callback and does not claim that
    // a production async sleep is integrated.
    [[nodiscard]] bool takeLocalWakeObligation() noexcept;
    [[nodiscard]] Snapshot snapshot() const noexcept;

private:
    enum class SlotState : std::uint8_t { kFree,
        kReserved,
        kActive,
        kRetiring,
        kExhausted };
    enum class Lane : std::uint8_t { kData,
        kControl,
        kLocal,
        kIntent };
    enum class QueueId : std::uint8_t { kDataRunnable,
        kControlRunnable,
        kLocalRunnable,
        kIntentRunnable,
        kDataBlocked,
        kControlBlocked,
        kIntentBlocked,
        kCount };

    struct Slot;
    struct Link final {
        Slot* previous{};
        Slot* next{};
        std::uint64_t blockedGeneration{};
        bool linked{};
    };
    struct Slot final {
        Http3WorkerMailboxScheduler* scheduler{};
        std::size_t index{};
        std::uint64_t epoch{};
        std::uint64_t connectionGeneration{};
        std::uint64_t slotGeneration{};
        Connection* owner{};
        SlotState state{SlotState::kFree};
        std::optional<Connection::TransportIntentToken> offeredIntent{};
        std::optional<Connection::TransportIntentToken> parkedIntent{};
        std::array<Link, static_cast<std::size_t>(QueueId::kCount)> links{};
    };
    struct SlotQueue final {
        Slot* head{};
        Slot* tail{};
        std::size_t size{};
    };

    [[nodiscard]] static constexpr std::size_t queueIndex(QueueId id) noexcept {
        return static_cast<std::size_t>(id);
    }
    [[nodiscard]] bool onWorker() const noexcept;
    [[nodiscard]] Slot* validate(ConnectionToken token) noexcept;
    [[nodiscard]] const Slot* validate(ConnectionToken token) const noexcept;
    [[nodiscard]] static ConnectionToken tokenFor(const Slot& slot) noexcept;
    [[nodiscard]] bool sameIntent(
        const std::optional<Connection::TransportIntentToken>& current,
        const std::optional<Connection::TransportIntentToken>& previous) const noexcept;
    void setLinked(Slot& slot, QueueId id, bool linked) noexcept;
    void pushBack(Slot& slot, QueueId id) noexcept;
    void remove(Slot& slot, QueueId id) noexcept;
    [[nodiscard]] Slot* popFront(QueueId id) noexcept;
    void syncSlot(Slot& slot, const Connection::WorkerActivation& activation) noexcept;
    void syncOwner(Slot& slot) noexcept;
    void receiveActivation(Slot& slot, std::uint64_t epoch,
        std::uint64_t connectionGeneration, std::uint64_t slotGeneration,
        const Connection::WorkerActivation& activation) noexcept;
    static void activationThunk(void* context, std::uint64_t epoch,
        std::uint64_t connectionGeneration, std::uint64_t slotGeneration,
        const Connection::WorkerActivation& activation) noexcept;
    void consumeCapacityWake() noexcept;
    void startCapacityPass(std::uint8_t lanes) noexcept;
    void normalizeCapacityPass() noexcept;
    [[nodiscard]] bool hasRecoverable(QueueId id) noexcept;
    [[nodiscard]] bool hasLaneWork(Lane lane) noexcept;
    [[nodiscard]] std::optional<Lane> selectLane() noexcept;
    [[nodiscard]] StepResult stepPublication(Slot& slot, Lane lane) noexcept;
    [[nodiscard]] StepResult stepIntent(Slot& slot) noexcept;
    [[nodiscard]] Slot* selectRecoveryControl(bool& isIntent) noexcept;
    void clearSlot(Slot& slot) noexcept;

    const WorkerHandle& worker_;
    Mailbox* mailbox_{};
    const std::size_t maxConnections_;
    Http3WorkerMailboxCapacitySignal* const capacitySignal_;
    const std::size_t controlBurstLimit_;
    const Http3WorkerMailboxWakeRef workerWake_;
    std::pmr::vector<Slot> slots_;
    std::pmr::vector<std::size_t> freeSlots_;
    std::array<SlotQueue, static_cast<std::size_t>(QueueId::kCount)> queues_{};
    std::uint64_t consumedCapacityGeneration_{};
    std::size_t attachedConnections_{};
    std::size_t blockedGeneration_{};
    std::size_t capacityPassCutoff_{};
    std::uint8_t capacityPassLanes_{};
    std::uint8_t armedCapacityLanes_{};
    Lane nextLane_{Lane::kData};
    std::size_t controlBurst_{};
    bool preferIntentRecovery_{};
    bool capacityWaitArmed_{};
    bool localWakePending_{};
};

}  // namespace ruvia::detail
