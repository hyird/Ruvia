#pragma once

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <vector>

#include <asio/any_io_executor.hpp>

#include "ruvia/core/ConnectionScanner.h"
#include "ruvia/core/Task.h"
#include "ruvia/core/TaskScope.h"
#include "ruvia/core/WorkerSignal.h"
#include "ruvia/core/memory/MemoryPool.h"
#include "ruvia/http/Http3Connection.h"
#include "ruvia/web/detail/http/context/ContextServices.h"
#include "ruvia/web/detail/http3/Http3BufferedRequestDispatch.h"
#include "ruvia/web/detail/http3/Http3SansIoSessionEngine.h"
#include "ruvia/web/detail/http3/Http3ServerBodyBudget.h"
#include "ruvia/web/detail/http3/Http3ServerStreamInput.h"
#include "ruvia/web/detail/http3/Http3StreamMailbox.h"

namespace ruvia::detail {

class RouteTable;
struct HttpServerOptions;
class Http3WorkerMailboxScheduler;

struct Http3ServerDatagramOutput final {
    void* context{};
    void (*send)(void*, std::uint64_t, std::span<const std::byte>){};
};
struct Http3ServerConnectionConfig final {
    std::uint64_t epoch{};
    std::uint64_t connectionGeneration{};
    Http3SansIoSessionLimits session{};
    std::size_t maxTrackedStreams{32};
    ConnectionScanner* connectionScanner{};
    asio::any_io_executor executor{};
    Http3ServerDatagramOutput datagramOutput{};
};

// Worker-affine, transport-independent owner for one HTTP/3 server connection.
// Ordinary request bodies are buffered; explicit stream routes and WebSocket
// CONNECT streams use bounded input. It accepts one routed block/control at a time, never
// drains or stops either shared mailbox, and never waits for capacity.
// All borrowed owners, including ContextServices' worker/token/capability and
// connection-metadata borrows, must outlive this object and its joined tasks.
// Stop sources observed during publication must request stop on this worker:
// StopToken callbacks are synchronous and this owner intentionally has no
// cross-thread queue or mailbox side channel.
// The optional shared body budget must also outlive this owner and every request
// lease it starts; join all children before retiring either owner. Destruction
// also requires transport intents to be handed off, physical retirement to be
// confirmed, or full-connection retirement responsibility to be taken over.
class Http3ServerConnection final {
    struct RequestEntry;
    struct RejectionEntry;
    struct RequestIndexSlot;

public:
    using Dispatch = Http3BufferedRequestDispatch;
    using Input = Http3ServerStreamInput;
    using Session = Http3SansIoSessionEngine;

    enum class TransportIntentKind : std::uint8_t { kStreamReset,
        kOpenPushStream,
        kConnectionClose };

    struct PushStreamOpenResult final {
        enum class Status : std::uint8_t { kOpened,
            kUnavailable,
            kStopped };
        Status status{Status::kUnavailable};
        std::uint64_t streamId{};
    };

    enum class TransportCloseReason : std::uint8_t {
        kNone,
        kLocalStop,
        kRequestRetirementFailure,
        kEntryRetirementFailure,
        kUnexpectedRequestState,
        kRequestConstructionFailure,
        kHandlerFailure,
        kPublishCancellation,
        kPublishFailure,
        kConnectionProtocolError,
        kRequestIndexCapacityExhausted,
        kInputCapacityExhausted,
        kFinalSizeError,
        kInputStopped,
        kSessionNotReady,
        kDispatchStartFailure,
        kTransportIntentCapacityExhausted,
        kTransportIntentSequenceExhausted,
    };

    struct TransportIntentToken final {
        TransportIntentKind kind{TransportIntentKind::kStreamReset};
        Http3StreamMessageId id{};
        std::uint64_t sequence{};

        friend bool operator==(const TransportIntentToken& left,
            const TransportIntentToken& right) noexcept {
            return left.kind == right.kind && left.id.epoch == right.id.epoch &&
                   left.id.connectionGeneration == right.id.connectionGeneration &&
                   left.id.streamId == right.id.streamId && left.id.pushId == right.id.pushId && left.sequence == right.sequence;
        }
    };

    struct TransportIntent final {
        TransportIntentToken token{};
        // Exact QUIC RESET_STREAM code. This is not mailbox control.value,
        // which remains reserved for a FIN's cumulative byte count.
        Http3ConnectionErrorCode streamResetErrorCode{
            Http3ConnectionErrorCode::kRequestCancelled};
        TransportCloseReason closeReason{TransportCloseReason::kNone};
        // Suggested HTTP/3 application error code for connection close; absent
        // when the typed reason alone determines transport policy.
        std::optional<Http3ConnectionErrorCode> connectionErrorCode{};
    };

    // The external channel lifecycle owner uses this typed takeover only after
    // it has accepted responsibility for closing the full connection at global
    // stop. This is not a transport-retired confirmation. Both identity fields
    // must match the owner whose pending close is being transferred.
    struct TransportRetirementTakeover final {
        std::uint64_t epoch{};
        std::uint64_t connectionGeneration{};
    };
    // A confirmation means the transport owner has already physically retired
    // this connection. It is distinct from handing off an intent or peer receipt.
    struct TransportRetirementConfirmation final {
        std::uint64_t epoch{};
        std::uint64_t connectionGeneration{};
    };

    enum class EventStatus : std::uint8_t {
        kAccepted,
        kDispatched,
        kRejected,
        kStreamCancelled,
        kInputRejected,
        kProtocolError,
        kConnectionClosed,
        kSessionNotReady,
        kRequestIndexFull,
        kDispatchStartFailed,
        kAdmissionClosed,
        kWrongWorker,
    };

    struct EventResult final {
        EventStatus status{EventStatus::kInputRejected};
        Input::Result input{};
        Session::Rejection rejection{Session::Rejection::kNone};
        // The caller still owns transport policy. True means it must close this
        // connection; no peer RESET or "unprocessed request" evidence is implied.
        bool connectionCloseRequired{};
    };

    enum class RequestStatus : std::uint8_t {
        kUnknown,
        kAdmitting,
        kRunning,
        kReadyToPublish,
        kPublishing,
        kPublished,
        kRejected,
        kCancelled,
        kFailed,
        kProtocolError,
    };

    struct RequestInfo final {
        RequestStatus status{RequestStatus::kUnknown};
        Session::Rejection rejection{Session::Rejection::kNone};
        std::optional<Dispatch::RunStatus> runStatus{};
    };

    enum class PublishStatus : std::uint8_t {
        kNoReadyRequest,
        kAttempted,
        kWrongWorker,
    };

    enum class WorkLane : std::uint8_t { kData,
        kControl,
        kLocal };

    struct WorkLanes final {
        bool data{};
        bool control{};
        bool local{};

        [[nodiscard]] constexpr bool contains(WorkLane lane) const noexcept {
            switch (lane) {
                case WorkLane::kData:
                    return data;
                case WorkLane::kControl:
                    return control;
                case WorkLane::kLocal:
                    return local;
            }
            return false;
        }
    };

    struct WorkState final {
        WorkLanes runnable{};
        WorkLanes blocked{};
        std::size_t runnableCount{};
        std::size_t blockedCount{};
        bool wrongWorker{};
    };

    struct WorkerActivation final {
        WorkState work{};
        std::optional<TransportIntentToken> transportIntent{};
        bool inputCapacityAvailable{false};
    };

    // Borrowed typed activation endpoint. The endpoint is called only on this
    // owner's worker and must only mark/enqueue the supplied snapshot; it must
    // not synchronously publish, destroy, or re-enter the owner.
    struct ActivationRef final {
        using Activate = void (*)(void* context, std::uint64_t epoch,
            std::uint64_t connectionGeneration, std::uint64_t slotGeneration,
            const WorkerActivation& activation) noexcept;

        void* context{};
        Activate activate{};
        std::uint64_t slotGeneration{};

        [[nodiscard]] bool valid() const noexcept {
            return context != nullptr && activate != nullptr && slotGeneration != 0;
        }
    };

    struct PublishAttempt final {
        PublishStatus status{PublishStatus::kNoReadyRequest};
        std::uint64_t streamId{};
        std::optional<ruvia::http3_critical_stream_output::stream_kind> criticalKind{};
        // When status is kAttempted this is the exact result returned by the
        // selected dispatch, including notifyPeer and the mailbox block lane.
        Dispatch::PublishResult publication{};
    };

    Http3ServerConnection(const RouteTable& routes, WorkerMemory& worker,
        ContextServices services, const HttpServerOptions& options,
        Http3StreamMailbox& outbound, ActivationRef activation,
        Http3ServerConnectionConfig config = {});
    // Shares a worker-owned buffered-body budget with other connections. The
    // budget must outlive this connection and every request lease it starts.
    Http3ServerConnection(const RouteTable& routes, WorkerMemory& worker,
        ContextServices services, const HttpServerOptions& options,
        Http3StreamMailbox& outbound, ActivationRef activation,
        Http3ServerBodyBudget& bodyBudget,
        Http3ServerConnectionConfig config = {});
    ~Http3ServerConnection();
    Http3ServerConnection(const Http3ServerConnection&) = delete;
    Http3ServerConnection& operator=(const Http3ServerConnection&) = delete;
    Http3ServerConnection(Http3ServerConnection&&) = delete;
    Http3ServerConnection& operator=(Http3ServerConnection&&) = delete;

    // The caller routes exactly one borrowed block/control here and releases a
    // block after return. A validated request FIN automatically starts one
    // buffered dispatch when the session is ready.
    [[nodiscard]] EventResult acceptData(const Http3StreamMailbox::BorrowedBlock& block) &;
    [[nodiscard]] bool resumeQpackInput() noexcept;
    [[nodiscard]] bool canAcceptInput(std::uint64_t streamId, std::size_t wireBytes) const noexcept;
    EventResult acceptData(const Http3StreamMailbox::BorrowedBlock&) && = delete;
    [[nodiscard]] EventResult acceptControl(const Http3StreamControl& control) &;
    EventResult acceptControl(const Http3StreamControl&) && = delete;

    // A worker-affine snapshot of the active intrusive queues. DATA/CONTROL
    // blocked entries are disjoint; local work never waits for mailbox capacity.
    [[nodiscard]] WorkState workState() const noexcept;

    // Makes at most one nonblocking publishStep attempt from an eligible
    // runnable lane. Backpressure parks the request on the exact mailbox lane;
    // it is not retried until reactivateBlocked() is called for that lane.
    [[nodiscard]] PublishAttempt publishOne(WorkLanes eligibleLanes) & noexcept;
    PublishAttempt publishOne(WorkLanes) && = delete;

    // Explicit capacity notification: reactivates only requests parked on the
    // named DATA/CONTROL lanes. The caller is already handling the capacity
    // event; no independent transport or mailbox operation is performed.
    [[nodiscard]] std::size_t reactivateBlocked(WorkLanes lanes) & noexcept;
    std::size_t reactivateBlocked(WorkLanes) && = delete;

    // Synchronous and idempotent. Returns true only on the transition that
    // requires the external owner to close the transport. It logically stops
    // input, records a persistent typed close intent, cancels active handlers,
    // and wakes the scheduler; it does not touch the transport or either shared
    // mailbox.
    void receiveDatagram(std::span<const std::byte> bytes) noexcept;
    [[nodiscard]] bool requestStop() & noexcept;
    bool requestStop() && = delete;

    // Stop/admission must be closed before joining. Even an empty TaskScope is
    // actually joined; the returned task is lazy, so discarding it has no effect.
    [[nodiscard]] Task<void> join() &;
    Task<void> join() && = delete;

    [[nodiscard]] RequestInfo requestInfo(std::uint64_t streamId) const noexcept;
    [[nodiscard]] std::size_t activeRequestCount() const noexcept;
    // An unfinished rejected request retains one bounded, worker-owned receive record.
    [[nodiscard]] std::size_t activeRejectionCount() const noexcept;
    [[nodiscard]] std::size_t activeTaskCount() const noexcept;
    [[nodiscard]] std::size_t readyRequestCount() const noexcept;
    [[nodiscard]] std::size_t trackedRequestCount() const noexcept;
    [[nodiscard]] std::size_t activeSessionStreamCount() const noexcept;
    // A worker-side predicate for the AdmissionSealed generation handshake.
    // It excludes persistent peer-unidirectional streams, and waits for every
    // admitted request stream, active handler, rejection and output publication
    // to become terminal. Terminal request frames may finish their deferred local
    // cleanup during transport retirement; the owner still joins them before reuse.
    [[nodiscard]] bool drainReady(std::size_t expectedAdmittedRequests) const noexcept;
    [[nodiscard]] bool stopped() const noexcept;
    // Sticky close intent. Callers must not rely only on the event result that
    // first reported the failure, since their wakeup/result may be coalesced.
    [[nodiscard]] bool transportCloseRequired() const noexcept;

    // Worker-affine intent access. Intents are returned by value, with
    // connection close taking priority over the intrusive per-stream reset
    // chain. Ack means the transport owner has
    // reliably accepted responsibility for the intent; it does not mean a peer
    // observed the operation or that the transport has retired. A close ACK
    // settles only its own token; every reset remains independently owed. Ack
    // never sends a notification; the caller owns the mailbox notify obligation
    // from a successful reset-control send. join() alone is not transport
    // retirement or responsibility transfer.
    [[nodiscard]] std::optional<TransportIntent> peekTransportIntent() const noexcept;
    [[nodiscard]] bool ackTransportIntent(const TransportIntentToken& token,
        std::optional<PushStreamOpenResult> opened = {}) & noexcept;
    bool ackTransportIntent(const TransportIntentToken&) && = delete;
    [[nodiscard]] bool takeOverTransportRetirement(
        TransportRetirementTakeover takeover) & noexcept;
    bool takeOverTransportRetirement(TransportRetirementTakeover) && = delete;
    [[nodiscard]] bool confirmTransportRetired(
        TransportRetirementConfirmation confirmation) & noexcept;
    bool confirmTransportRetired(TransportRetirementConfirmation) && = delete;
    [[nodiscard]] std::size_t pendingTransportIntentCount() const noexcept;
    [[nodiscard]] bool transportRetired() const noexcept;

private:
    friend class Http3WorkerMailboxScheduler;
    friend struct Http3ServerConnectionResetIntentTestAccess;
    friend struct http3_worker_server_test_access;

    static constexpr std::size_t kNoIntentSlot = static_cast<std::size_t>(-1);
    static constexpr std::uint64_t kReservedCloseIntentSequence =
        static_cast<std::uint64_t>(-1);

    enum class ResetIntentOrigin : std::uint8_t { kLocalCancellation,
        kStreamProtocolError };

    enum class QueueKind : std::uint8_t {
        kNone,
        kDataRunnable,
        kControlRunnable,
        kLocalRunnable,
        kDataBlocked,
        kControlBlocked,
    };

    struct IntrusiveQueue final {
        RequestIndexSlot* head{};
        RequestIndexSlot* tail{};
    };

    struct PendingInterimResponse final {
        std::pmr::vector<char> frame;
        std::size_t offset{};
    };

    struct RequestIndexSlot final {
        std::uint64_t streamId{};
        std::optional<std::uint64_t> pushId{};
        bool pushCancelled{};
        std::optional<std::pmr::vector<char>> pushPrefix{};
        RequestEntry* entry{};
        std::optional<PendingInterimResponse> interimResponse{};
        std::uint64_t responsePreludeBytes{};
        bool requestStarted{};
        RejectionEntry* rejectionEntry{};
        RequestIndexSlot* queuePrevious{};
        RequestIndexSlot* queueNext{};
        QueueKind queue{QueueKind::kNone};
        RequestStatus status{RequestStatus::kUnknown};
        Session::Rejection rejection{Session::Rejection::kNone};
        std::optional<Dispatch::RunStatus> runStatus{};
        std::size_t resetIntentPrevious{kNoIntentSlot};
        std::size_t resetIntentNext{kNoIntentSlot};
        std::uint64_t resetIntentSequence{};
        Http3ConnectionErrorCode resetIntentErrorCode{
            Http3ConnectionErrorCode::kRequestCancelled};
        ResetIntentOrigin resetIntentOrigin{ResetIntentOrigin::kLocalCancellation};
        bool resetIntentPending{};
        bool occupied{};
    };

    struct EntryRetirement;
    struct RequestRetirement;
    struct PendingPush;

    [[nodiscard]] Task<bool> pushRequest(std::uint64_t parentStreamId, HttpPushRequestView request);
    void processPushCancellations() noexcept;
    void observePushCancellation(std::uint64_t pushId) noexcept;
    void retireRequestInput(std::uint64_t streamId) noexcept;

    [[nodiscard]] Task<void> runRequest(std::uint64_t streamId);
    [[nodiscard]] EventResult handleInputResult(std::uint64_t streamId,
        Input::Result result, bool fromControl);
    [[nodiscard]] EventResult admitFinishedRequest(std::uint64_t streamId,
        Input::Result result);
    [[nodiscard]] EventResult startRejection(std::uint64_t streamId,
        Session::Rejection rejection, Input::Result result, bool receiveFinished);
    [[nodiscard]] EventResult retireRejectedReceive(std::uint64_t streamId,
        Input::Result result, bool reset);
    [[nodiscard]] PublishAttempt publishRejection(RequestIndexSlot& slot,
        RejectionEntry& entry) noexcept;
    void cancelRejectionOutput(RejectionEntry& entry, RequestStatus status) noexcept;
    void finishRejection(RejectionEntry& entry) noexcept;
    [[nodiscard]] RequestIndexSlot* findOrCreateRequestSlot(
        std::uint64_t streamId, bool& created) noexcept;
    [[nodiscard]] RequestIndexSlot* findRequestSlot(std::uint64_t streamId) noexcept;
    [[nodiscard]] const RequestIndexSlot* findRequestSlot(std::uint64_t streamId) const noexcept;
    void enqueueRunnable(RequestIndexSlot& slot, QueueKind kind, bool notify) noexcept;
    [[nodiscard]] bool queueContinueResponse(std::uint64_t streamId);
    [[nodiscard]] PublishAttempt publishInterimResponse(RequestIndexSlot& slot) noexcept;
    void enqueueForDemand(RequestIndexSlot& slot, bool notify) noexcept;
    void enqueueBlocked(RequestIndexSlot& slot, Dispatch::PublishBlockReason reason) noexcept;
    void removeQueued(RequestIndexSlot& slot) noexcept;
    [[nodiscard]] RequestIndexSlot* selectRunnable(WorkLanes eligibleLanes) const noexcept;
    [[nodiscard]] bool reactivateBlockedOne(WorkLane lane) & noexcept;
    [[nodiscard]] IntrusiveQueue& queueFor(QueueKind kind) noexcept;
    [[nodiscard]] const IntrusiveQueue& queueFor(QueueKind kind) const noexcept;
    void publicationStopped(RequestEntry& entry) noexcept;
    void cancelDeadlineEntry(RequestEntry& entry) noexcept;
    void cancelEntry(RequestEntry& entry, RequestStatus status) noexcept;
    void stopEntries(bool inputAlreadyStopped) noexcept;
    void notifyActivation() noexcept;
    [[nodiscard]] WorkerActivation activationSnapshot() const noexcept;
    [[nodiscard]] bool detachActivationAfterJoin() noexcept;
    [[nodiscard]] bool enqueueResetIntent(RequestIndexSlot& slot,
        Http3ConnectionErrorCode errorCode = Http3ConnectionErrorCode::kRequestCancelled,
        ResetIntentOrigin origin = ResetIntentOrigin::kLocalCancellation) noexcept;
    void unlinkResetIntent(RequestIndexSlot& slot) noexcept;
    void requireConnectionClose(TransportCloseReason reason,
        std::optional<Http3ConnectionErrorCode> errorCode = {}) noexcept;
    void finishEntry(RequestEntry& entry) noexcept;
    void initialize();
    Http3ServerDatagramOutput datagramOutput_{};
    [[nodiscard]] bool onWorker() const noexcept;
    [[nodiscard]] bool attachTunnelScanner(std::uint64_t streamId,
        ConnectionScanner::Entry& entry) noexcept;
    void tunnelOutputReady(std::uint64_t streamId) noexcept;
    void abortTunnel(std::uint64_t streamId) noexcept;
    static bool attachTunnelScannerThunk(void* context, std::uint64_t streamId,
        ConnectionScanner::Entry& entry) noexcept;
    static void tunnelOutputReadyThunk(void* context, std::uint64_t streamId) noexcept;
    static void requestInputConsumedThunk(void* context) noexcept;
    static void abortTunnelThunk(void* context, std::uint64_t streamId) noexcept;
    [[nodiscard]] static std::size_t indexCapacity(std::size_t maxTrackedStreams);

    const ContextServices services_;
    WorkerMemory& worker_;
    const RouteTable& routes_;
    const HttpServerOptions& options_;
    Http3StreamMailbox& outbound_;
    ActivationRef activation_;
    const std::uint64_t epoch_;
    const std::uint64_t connectionGeneration_;
    const std::size_t maxTrackedStreams_;
    ConnectionScanner* connectionScanner_{};
    asio::any_io_executor executor_;
    Session session_;
    Input input_;
    TaskScope tasks_;
    std::pmr::vector<RequestIndexSlot> requestIndex_;
    PendingPush* pendingPushHead_{};
    PendingPush* pendingPushTail_{};
    PendingPush* pushIntentHead_{};
    PendingPush* pushIntentTail_{};
    std::size_t pendingPushCount_{};
    std::size_t pendingPushIntentCount_{};
    std::uint64_t nextPushId_{};
    IntrusiveQueue dataRunnable_{};
    IntrusiveQueue controlRunnable_{};
    IntrusiveQueue localRunnable_{};
    IntrusiveQueue dataBlocked_{};
    IntrusiveQueue controlBlocked_{};
    std::size_t resetIntentHead_{kNoIntentSlot};
    std::size_t resetIntentTail_{kNoIntentSlot};
    std::size_t pendingResetIntentCount_{};
    std::size_t trackedRequestCount_{};
    std::size_t activeRequestCount_{};
    std::size_t activeRejectionCount_{};
    std::size_t readyRequestCount_{};
    std::size_t blockedRequestCount_{};
    WorkLane nextPublishLane_{WorkLane::kData};
    bool criticalOutputBlocked_{};
    bool preferCriticalOutput_{true};
    std::size_t nextCriticalOutput_{};
    bool admissionClosed_{};
    bool stopRequested_{};
    bool transportCloseRequired_{};
    std::uint64_t nextTransportIntentSequence_{1};
    TransportCloseReason closeIntentReason_{TransportCloseReason::kNone};
    std::optional<Http3ConnectionErrorCode> closeIntentErrorCode_{};
    bool closeIntentPending_{};
    bool closeIntentHandedOff_{};
    bool transportRetirementTakenOver_{};
    bool transportRetired_{};
    bool everSpawned_{};
    bool joinStarted_{};
    bool joinCompleted_{};
};

}  // namespace ruvia::detail
