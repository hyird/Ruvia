#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <thread>
#include <vector>

#include "ruvia/http/Http3Connection.h"
#include "ruvia/http/quic_connection.h"
#include "ruvia/web/detail/http3/http3_stream_buffer.h"

namespace ruvia {
class WorkerMemory;
}

namespace ruvia::detail {

struct Http3ServerStreamOutputConfig final {
    // Bounds active stream slots plus retained tombstones; entries are not recycled.
    std::size_t maxTrackedStreams{32};
    // Bound retained borrowed_block nodes; size to the local buffer's block capacity.
    std::size_t maxQueuedBlocks{32};
    // Bounds service operations per scheduler turn; each scan visits at most one table.
    std::size_t maxDriveWorkItems{16};
    // Per-response-stream write inactivity timeout; nullopt disables it.
    std::optional<std::chrono::milliseconds> writeTimeout{};
};

// Worker-affine response egress for the wire half that already routed
// buffer messages for one QUIC connection. It never drains the shared buffer.
// Accepted blocks remain borrowed until all bytes are accepted by SSL or the
// corresponding stream/connection SSL owner has been retired. The transport and
// worker-owned memory resource must outlive this object; destruction with live streams or
// borrowed blocks is a contract violation, so stop() explicitly before teardown.
class Http3ServerStreamOutput final {
public:
    using StreamId = std::uint64_t;
    using TransportError = ruvia::quic_operation_status;
    using StreamWrite = ruvia::quic_stream_write_result;
    struct StreamTermination final {
        TransportError send{TransportError::would_block};
        TransportError close{TransportError::would_block};
    };

    enum class Status : std::uint8_t {
        kAccepted,
        kBackpressured,
        kWritable,
        kFinDeferred,
        kDuplicateFin,
        kFinished,
        kReset,
        kCancelled,
        kConnectionClosed,
        kForeignEpoch,
        kStaleConnection,
        kInvalidInput,
        kInvalidStreamId,
        kClosedStream,
        kCapacityExhausted,
        kFinalSizeError,
        kTransportError,
        kStopped,
        kUnsafeToRelease,
        kIdle,
        kProgress,
    };

    // kFinished records accepted local FIN plus wrapper retirement, not peer delivery/ACK.
    enum class StreamState : std::uint8_t {
        kOpen,
        kFinPending,
        kStopping,
        kFinished,
        kReset,
        kCancelled,
        kFailed,
        kConnectionClosed,
    };

    struct Result final {
        Status status{Status::kAccepted};
        TransportError transportError{TransportError::accepted};
        TransportError writeStatus{TransportError::accepted};
        StreamTermination termination{};
    };

    struct DriveResult final {
        Status status{Status::kIdle};
        // Budgeted service operations, including retryable SSL write attempts.
        std::size_t operations{};
        std::size_t scannedSlots{};
        std::size_t writeCalls{};
        std::size_t acceptedBytes{};
        std::size_t wouldBlockWrites{};
        std::size_t finishedStreams{};
        std::size_t timedOutStreams{};
        StreamId lastStreamId{};
        StreamId errorStreamId{};
        TransportError writeStatus{TransportError::accepted};
        TransportError transportError{TransportError::accepted};
        StreamTermination termination{};
        // True for accepted bytes, consumed blocks, or completed/retired stream work.
        bool madeProgress{};
        // The current bounded scan has more slots, or a completed scan with
        // progress/activity needs another pass. An inactive WANT-only scan parks.
        bool needsReschedule{};
    };

    struct StreamInfo final {
        StreamId streamId{};
        StreamState state{StreamState::kOpen};
        std::uint64_t receivedWireBytes{};
        std::uint64_t acceptedWireBytes{};
        std::uint64_t queuedWireBytes{};
        std::optional<std::uint64_t> finalWireBytes{};
        std::size_t queuedBlocks{};
        TransportError lastWriteStatus{TransportError::accepted};
        TransportError finishError{TransportError::accepted};
        // SSL_stream_conclude accepted local FIN; receive completion is tracked
        // by the transport until readStream observes peer EOF or RESET.
        bool sendFinAccepted{};
        bool timedOut{};
        StreamTermination termination{};
        std::optional<std::uint64_t> pushId{};
    };

    Http3ServerStreamOutput(ruvia::quic_connection& connection,
        std::pmr::memory_resource* resource, std::uint64_t epoch,
        std::uint64_t connectionGeneration, Http3ServerStreamOutputConfig config = {});
    Http3ServerStreamOutput(ruvia::quic_connection& connection, WorkerMemory& worker,
        std::uint64_t epoch, std::uint64_t connectionGeneration,
        Http3ServerStreamOutputConfig config = {});
    ~Http3ServerStreamOutput();
    Http3ServerStreamOutput(const Http3ServerStreamOutput&) = delete;
    Http3ServerStreamOutput& operator=(const Http3ServerStreamOutput&) = delete;
    Http3ServerStreamOutput(Http3ServerStreamOutput&&) = delete;
    Http3ServerStreamOutput& operator=(Http3ServerStreamOutput&&) = delete;

    // On kAccepted ownership moves into the bounded FIFO. On kBackpressured,
    // identity mismatch, or other non-acceptance the caller still owns block and
    // must retain or explicitly release it. IDs outside ordinary client bidi
    // and explicitly bound push streams fail the connection closed. Mismatched
    // identities are detected before any transport operation.
    [[nodiscard]] Result acceptData(http3_stream_buffer::borrowed_block& block);

    // Bind only a server UNI stream actually opened by this wire half.
    // The binding is immutable and must precede handler response publication.
    [[nodiscard]] Result registerPushStream(StreamId streamId, std::uint64_t pushId);
    [[nodiscard]] Result acceptCriticalData(http3_stream_buffer::borrowed_block& block, StreamId streamId);
    [[nodiscard]] Result acceptControl(const http3_stream_control& control);
    [[nodiscard]] Result cancelStream(StreamId streamId,
        std::uint64_t errorCode = static_cast<std::uint64_t>(Http3ConnectionErrorCode::kRequestCancelled));

    // Performs at most maxDriveWorkItems service operations and scans at most
    // one table per call. A write item is one SSL attempt; WANT counts as an
    // operation but not progress, so kIdle may accompany nonzero operations.
    // kProgress reflects real progress only. A FIN item submits the send FIN
    // and attempts direction-complete retirement. Persistent scan state requests
    // another turn while slots remain; productive or activity-requested scans
    // get one more pass, while a full no-progress scan parks. WANT retains the
    // exact borrowed address, size and contents while later streams get a turn.
    [[nodiscard]] DriveResult drive();

    // Reopens a parked scan or requests a fresh pass after owner-thread UDP,
    // timer, QUIC, or stream-read activity. New accepted data, FIN, and stream
    // retirement do so automatically.
    void notifyTransportActivity();

    // Marks all live streams terminal before resetting/closing their SSL wrappers.
    // If an individual wrapper cannot be proven retired, closes the whole bound
    // connection before returning its borrowed blocks. Repeated calls are safe;
    // a failed close may be retried. kUnsafeToRelease forbids destruction; queued
    // blocks remain borrowed until a later stop succeeds.
    [[nodiscard]] Result stop();

    [[nodiscard]] std::optional<StreamInfo> streamInfo(StreamId streamId) const;
    // trackedStreamCount includes retained terminal tombstones. liveStreamCount
    // counts response writes begun but not yet locally FINished; pendingStreamCount
    // counts live streams with queued data or a locally-ready FIN.
    [[nodiscard]] std::size_t trackedStreamCount() const;
    [[nodiscard]] std::size_t liveStreamCount() const;
    [[nodiscard]] std::size_t pendingStreamCount() const;
    [[nodiscard]] std::size_t queuedBlockCount() const;
    [[nodiscard]] bool stopped() const;
    [[nodiscard]] bool connectionRetired() const;

private:
    [[nodiscard]] Result acceptAddressedData(http3_stream_buffer::borrowed_block& block, StreamId streamId, std::uint64_t epoch, std::uint64_t generation);
    static constexpr std::uint32_t kNoNode = UINT32_MAX;

    enum class IdentityStatus : std::uint8_t { kMatch,
        kForeignEpoch,
        kStaleConnection };

    struct StreamSlot final {
        StreamInfo info{};
        std::optional<std::chrono::steady_clock::time_point> lastWriteActivity{};
        std::uint32_t head{kNoNode};
        std::uint32_t tail{kNoNode};
        bool occupied{};
    };

    struct BlockNode final {
        http3_stream_buffer::borrowed_block block{};
        std::uint32_t next{kNoNode};
        std::size_t offset{};
    };

    void requireOwnerThread() const;
    [[nodiscard]] IdentityStatus identityStatus(const http3_stream_id& id) const noexcept;
    [[nodiscard]] bool validResponseStreamId(StreamId streamId) const noexcept;
    [[nodiscard]] StreamSlot* findStream(StreamId streamId) noexcept;
    [[nodiscard]] const StreamSlot* findStream(StreamId streamId) const noexcept;
    [[nodiscard]] StreamSlot* findOrCreateStream(StreamId streamId);
    [[nodiscard]] bool isTerminal(StreamState state) const noexcept;
    [[nodiscard]] bool connectionIdentityGone();
    [[nodiscard]] bool closeConnectionAndRelease();
    [[nodiscard]] bool retireStream(StreamSlot& slot, StreamState terminalState,
        std::uint64_t errorCode);
    void releaseNode(std::uint32_t index) noexcept;
    void releaseStreamQueue(StreamSlot& slot) noexcept;
    void releaseAllQueues() noexcept;
    void markConnectionClosed() noexcept;
    [[nodiscard]] Result failConnection(Status status,
        TransportError error = TransportError::closing);
    [[nodiscard]] Result handleWriteFailure(StreamSlot& slot,
        TransportError writeStatus);
    [[nodiscard]] bool finishStream(StreamSlot& slot, TransportError& error);
    [[nodiscard]] bool writeTimedOut(const StreamSlot& slot,
        std::chrono::steady_clock::time_point now) const noexcept;
    void requestScan() noexcept;
    [[nodiscard]] static std::size_t tableCapacity(std::size_t maxTrackedStreams);

    ruvia::quic_connection& connection_;
    const std::uint64_t epoch_;
    const std::uint64_t connectionGeneration_;
    const std::size_t maxTrackedStreams_;
    const std::size_t maxQueuedBlocks_;
    const std::size_t maxDriveWorkItems_;
    const std::optional<std::chrono::milliseconds> writeTimeout_;
    const std::thread::id ownerThread_;
    std::pmr::vector<StreamSlot> streams_;
    std::pmr::vector<BlockNode> nodes_;
    std::uint32_t freeNode_{kNoNode};
    std::size_t trackedStreamCount_{};
    std::size_t queuedBlockCount_{};
    std::size_t roundRobinSlot_{};
    std::size_t roundRemainingSlots_{};
    bool roundMadeProgress_{};
    bool scanAgain_{};
    bool stopped_{};
    bool connectionRetired_{};
    bool retirementFailed_{};
    bool stopComplete_{};
};

}  // namespace ruvia::detail
