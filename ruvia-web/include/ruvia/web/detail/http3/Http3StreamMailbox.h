#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <span>
#include <variant>
#include <vector>

#include "ruvia/http/Http3Connection.h"
#include "ruvia/http/http3_critical_stream_output.h"

namespace ruvia::detail {

struct Http3StreamMessageId final {
    std::uint64_t epoch{};
    std::uint64_t connectionGeneration{};
    std::uint64_t streamId{};
    // A response on a real server-initiated UNI stream binds its Push ID.
    // Ordinary request streams leave this empty; critical streams use their
    // separate mailbox destination and cannot masquerade as pushed responses.
    std::optional<std::uint64_t> pushId{};
};

struct Http3CriticalStreamMessageId final {
    std::uint64_t epoch{};
    std::uint64_t connectionGeneration{};
    ruvia::http3_critical_stream_output::stream_kind kind{ruvia::http3_critical_stream_output::stream_kind::qpack_encoder};
};
using Http3MailboxDestination = std::variant<Http3StreamMessageId, Http3CriticalStreamMessageId>;

struct Http3StreamControl final {
    enum class Kind : std::uint8_t { kConnectionClosed,
        kStreamReset,
        kWritable,
        kStreamFin,
        kTunnelEstablished };
    Kind kind{Kind::kConnectionClosed};
    Http3StreamMessageId id{};
    // kStreamFin: final cumulative byte count for this stream. Since control
    // and data lanes are independent, a consumer may observe FIN before older
    // DATA; it must defer FIN until all preceding stream bytes are consumed.
    // kTunnelEstablished: cumulative response bytes that must be accepted by
    // QUIC before the network owner disables the request-body timeout.
    // For a server-network-published peer kStreamReset, value is the cumulative number
    // of this stream's DATA bytes successfully published before the RESET. It
    // is only a mailbox cross-lane barrier, never QUIC RESET_STREAM Final Size.
    // Locally generated RESET intents do not use this value. The peer error code
    // has a separate typed field; never encode it in value.
    std::uint64_t value{};
    // Used only for kStreamReset. Defaults to H3_REQUEST_CANCELLED; this field
    // carries the actual peer error code for server-network-published peer RESETs.
    Http3ConnectionErrorCode streamResetErrorCode{
        Http3ConnectionErrorCode::kRequestCancelled};
};

// Startup-bound notification target, borrowed through consumer/publisher quiescence.
// May run on the consumer or stop() caller thread. Only enqueue a producer wakeup;
// do not re-enter/destroy the mailbox or access producer-private state here.
// The wakeup obligation must not be dropped: use a coalesced, lifetime-safe
// dispatcher notification, not an unchecked best-effort post.
struct Http3StreamMailboxCapacityNotifier final {
    void* context{};
    void (*notify)(void*) noexcept {};
};

class Http3StreamMailbox final {
public:
    // Large enough to amortize mailbox handoff while staying close to a few
    // QUIC packets; startup allocates one block per configured credit in both
    // directions, so this is also the per-credit byte-memory bound.
    static constexpr std::size_t kMaxBlockBytes = 4 * 1024;

    enum class SendResult : std::uint8_t { kSent,
        kSentNotifyPeer,
        kFull,
        kNoBlock,
        kTooLarge,
        kStopped,
        kReservationActive };
    enum class ReservationResult : std::uint8_t { kReserved,
        kFull,
        kNoBlock,
        kStopped,
        kReservationActive };
    enum class CommitResult : std::uint8_t { kSent,
        kSentNotifyPeer,
        kZeroBytes,
        kTooLarge,
        kInactive };
    enum class ControlResult : std::uint8_t { kSent,
        kSentNotifyPeer,
        kFull,
        kStopped };

    enum class CapacityInterest : std::uint8_t { kData,
        kControl,
        kAny };
    enum class CapacityWaitResult : std::uint8_t { kReady,
        kArmed,
        kStopped,
        kUnavailable,
        kInvalidInterest };

    // A producer-affine DATA slot and block reservation. Moving transfers the
    // reservation but not its producer-thread affinity. It must be committed or
    // aborted, and destroyed, on the mailbox's producer thread. Its lifetime
    // keeps one publisher admitted, so stop() cannot report quiescence until it
    // is returned; stop() does not revoke an already admitted reservation.
    // writableBytes() is valid only while active and must not be retained or
    // accessed after commit/abort. Invalid commit sizes abort and return credit.
    class DataReservation final {
    public:
        DataReservation() = default;
        ~DataReservation();
        DataReservation(const DataReservation&) = delete;
        DataReservation& operator=(const DataReservation&) = delete;
        DataReservation(DataReservation&& other) noexcept;
        DataReservation& operator=(DataReservation&& other) noexcept;

        [[nodiscard]] explicit operator bool() const noexcept {
            return owner_ != nullptr;
        }
        [[nodiscard]] std::span<std::byte> writableBytes() const noexcept;
        [[nodiscard]] CommitResult commit(std::size_t size) noexcept;
        void abort() noexcept;

    private:
        friend class Http3StreamMailbox;
        DataReservation(Http3StreamMailbox* owner, std::uint32_t index,
            Http3StreamMessageId id) noexcept;
        Http3StreamMailbox* owner_{};
        std::uint32_t index_{};
        Http3StreamMessageId id_{};
    };

    class BorrowedBlock final {
    public:
        BorrowedBlock() = default;
        ~BorrowedBlock();
        BorrowedBlock(const BorrowedBlock&) = delete;
        BorrowedBlock& operator=(const BorrowedBlock&) = delete;
        BorrowedBlock(BorrowedBlock&& other) noexcept;
        BorrowedBlock& operator=(BorrowedBlock&& other) noexcept;

        [[nodiscard]] explicit operator bool() const noexcept {
            return owner_ != nullptr;
        }
        [[nodiscard]] std::span<const std::byte> bytes() const noexcept;
        [[nodiscard]] const Http3StreamMessageId& id() const noexcept {
            return std::get<Http3StreamMessageId>(id_);
        }
        [[nodiscard]] const Http3CriticalStreamMessageId* critical() const noexcept {
            return std::get_if<Http3CriticalStreamMessageId>(&id_);
        }
        void release() noexcept;

    private:
        friend class Http3StreamMailbox;
        BorrowedBlock(Http3StreamMailbox* owner, std::uint32_t index, std::size_t size,
            Http3MailboxDestination id) noexcept;
        Http3StreamMailbox* owner_{};
        std::uint32_t index_{};
        std::size_t size_{};
        Http3MailboxDestination id_{};
    };

    // Capacities are fixed and preallocated at construction. The resource must outlive the
    // mailbox; destroy the mailbox on its producer/resource-owner thread after stop and
    // publishersQuiescent(). Outstanding BorrowedBlock or DataReservation instances at
    // destruction terminate.
    // Consumer-side release publishes a block index and may enqueue a capacity
    // wakeup; it never deallocates storage. Stop explicitly, drain queued wakeups
    // and join any sleeping producer before destroying the mailbox/notifier target.
    Http3StreamMailbox(std::uint32_t blockCount, std::uint32_t dataSlots,
        std::uint32_t controlSlots,
        std::pmr::memory_resource* resource = std::pmr::get_default_resource(),
        Http3StreamMailboxCapacityNotifier capacityNotifier = {});
    ~Http3StreamMailbox();
    Http3StreamMailbox(const Http3StreamMailbox&) = delete;
    Http3StreamMailbox& operator=(const Http3StreamMailbox&) = delete;
    Http3StreamMailbox(Http3StreamMailbox&&) = delete;
    Http3StreamMailbox& operator=(Http3StreamMailbox&&) = delete;

    // Single producer only. A failed send consumes neither the input bytes nor a queue slot.
    // While this producer owns a DataReservation, DATA trySend is rejected with
    // kReservationActive; the CONTROL lane remains independently usable.
    [[nodiscard]] SendResult trySendCritical(Http3CriticalStreamMessageId id, std::span<const std::byte> bytes) noexcept;
    [[nodiscard]] SendResult trySend(Http3StreamMessageId id,
        std::span<const std::byte> bytes) noexcept;
    // Reserves the current DATA slot and one free block before exposing writable
    // storage. On failure, reservation remains empty. The producer must drain
    // returned blocks before reserving if it needs those credits.
    [[nodiscard]] ReservationResult reserveData(Http3StreamMessageId id,
        DataReservation& reservation) noexcept;
    [[nodiscard]] ControlResult trySendControl(const Http3StreamControl& event) noexcept;

    // Single consumer only. Data and control lanes are independently drained;
    // kStreamFin must be applied only at its final byte count, not at dequeue.
    // After draining both lanes call finishDrain(); if true, drain again.
    [[nodiscard]] bool tryReceive(BorrowedBlock& block) noexcept;
    [[nodiscard]] bool tryReceiveControl(Http3StreamControl& event) noexcept;

    // Producer-only, one capacity wait at a time. Arm after a failed send;
    // DATA-containing interests return kUnavailable while a DATA reservation
    // is held (CONTROL-only waits remain independent). kAny is ready when either
    // DATA or CONTROL has capacity. kArmed transfers
    // the wakeup obligation to consumer/stop. Notification can precede return,
    // so use a latched wakeup and recheck on every wakeup. kReady has already
    // collected returned blocks when DATA is of interest. kUnavailable means
    // no notifier was configured or a held reservation prevents the requested
    // DATA wait; never sleep in that case.
    [[nodiscard]] CapacityWaitResult armCapacityWait(CapacityInterest interest) noexcept;

    // Producer-only: returned indices become reusable only after this call.
    // Returns zero while a DATA reservation is held so abort restores the exact
    // free-block stack position and order.
    [[nodiscard]] std::uint32_t drainReturns() noexcept;
    // Closes admission without waiting. False means an admitted publisher is still in flight.
    [[nodiscard]] bool stop() noexcept;
    [[nodiscard]] bool stopped() const noexcept;
    [[nodiscard]] bool publishersQuiescent() const noexcept;

    // Consumer calls after draining both lanes; true means it must drain again immediately.
    // Producers receive kSentNotifyPeer exactly when they acquire the wakeup obligation.
    [[nodiscard]] bool finishDrain() noexcept;

    [[nodiscard]] std::uint32_t blockCapacity() const noexcept;

private:
    [[nodiscard]] SendResult sendAddress(Http3MailboxDestination id, std::span<const std::byte> bytes) noexcept;
    struct Block final {
        std::array<std::byte, kMaxBlockBytes> bytes{};
    };
    struct DataSlot final {
        std::uint32_t block{};
        std::uint32_t size{};
        Http3MailboxDestination id{};
    };

    [[nodiscard]] bool beginPublish() noexcept;
    void endPublish() noexcept;
    [[nodiscard]] CommitResult commitDataReservation(std::uint32_t index,
        Http3StreamMessageId id, std::size_t size) noexcept;
    void abortDataReservation(std::uint32_t index) noexcept;
    void returnBlock(std::uint32_t index) noexcept;
    [[nodiscard]] bool claimNotification() noexcept;
    [[nodiscard]] bool hasPending() const noexcept;
    void clearCapacityWait(std::uint64_t registration) noexcept;
    void notifyCapacity(std::uint8_t mask) noexcept;
    static constexpr std::uint8_t kDataCapacity = 1;
    static constexpr std::uint8_t kControlCapacity = 2;
    static constexpr std::uint64_t kCapacityInterestMask =
        kDataCapacity | kControlCapacity;
    static constexpr std::uint64_t kCapacityWaitGenerationStep = 4;

    const std::uint32_t blockCount_;
    const std::uint32_t dataCapacity_;
    const std::uint32_t controlCapacity_;
    std::pmr::memory_resource* resource_;
    const Http3StreamMailboxCapacityNotifier capacityNotifier_;
    // Low bits hold the interest set; upper bits identify the registration so
    // a notifier cannot clear a newer waiter after losing a CAS race.
    std::atomic<std::uint64_t> capacityWait_{};
    std::pmr::vector<Block> blocks_;
    std::pmr::vector<DataSlot> dataSlots_;
    std::pmr::vector<Http3StreamControl> controlSlots_;
    std::pmr::vector<std::uint32_t> freeBlocks_;
    std::pmr::vector<std::uint32_t> returns_;
    std::uint32_t freeCount_{};     // producer-owned
    std::uint64_t dataWrite_{};     // producer-owned
    bool dataReservationActive_{};  // producer-owned; pins slot/block and beginPublish()
    std::atomic<std::uint64_t> dataRead_{};
    std::atomic<std::uint64_t> dataPublished_{};
    std::uint64_t dataReadLocal_{};  // consumer-owned
    std::uint64_t controlWrite_{};   // producer-owned
    std::atomic<std::uint64_t> controlRead_{};
    std::atomic<std::uint64_t> controlPublished_{};
    std::uint64_t controlReadLocal_{};  // consumer-owned
    std::uint64_t returnWrite_{};       // consumer-owned
    std::uint64_t returnReadLocal_{};   // producer-owned
    std::atomic<std::uint64_t> returnRead_{};
    std::atomic<std::uint64_t> returnPublished_{};
    // High bit closes publication; low bits count sends admitted before stop.
    std::atomic<std::uint64_t> publishState_{};
    std::atomic<std::uint32_t> outstandingBorrows_{};
    std::atomic<bool> notification_{};
};

}  // namespace ruvia::detail
