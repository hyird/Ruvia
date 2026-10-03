#pragma once

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ruvia/web/detail/http3/Http3SansIoSessionEngine.h"
#include "ruvia/web/detail/http3/Http3StreamMailbox.h"

namespace ruvia {
class WorkerMemory;
}

namespace ruvia::detail {

// Worker-affine receive adapter for one QUIC connection. The mailbox owner
// routes each block/control here individually; this class never drains shared
// queues. DATA byte counts include HTTP/3 frame bytes, not just request body.
// Terminal stream entries stay as tombstones until this connection is retired.
// Streaming consumers are paced by the connection owner before acceptData();
// it retains bounded mailbox blocks while the worker body backlog is full.
class Http3ServerStreamInput final {
    enum class StreamPhase : std::uint8_t { kOpen,
        kFinished,
        kResetPending,
        kReset,
        kCancelled,
        kFailed,
        kConnectionClosed };

    struct StreamState final {
        explicit StreamState(std::pmr::memory_resource* resource)
            : pendingBytes(resource) {}
        std::pmr::string pendingBytes;
        bool qpackBlocked{};
        bool pendingFin{};
        std::uint64_t wireBytes{};
        std::optional<std::uint64_t> finalSize;
        std::optional<std::uint64_t> resetPublishedBytes;
        Http3ConnectionErrorCode resetErrorCode{
            Http3ConnectionErrorCode::kRequestCancelled};
        StreamPhase phase{StreamPhase::kOpen};
        bool requestStream{};
        bool requestActive{};
        bool receivedEarlyData{};
    };

    struct StreamSlot final {
        explicit StreamSlot(std::pmr::memory_resource* resource)
            : state(resource) {}
        std::uint64_t streamId{};
        StreamState state;
        bool occupied{};
    };

public:
    enum class Status : std::uint8_t {
        kFed,
        kDeferredFin,
        kDeferredQpack,
        kDeferredReset,
        kFinished,
        kReset,
        kConnectionClosed,
        kLocalCancelled,
        kIgnoredControl,
        kForeignEpoch,
        kStaleConnection,
        kClosedStream,
        kDuplicateFin,
        kFinalSizeError,
        kCapacityExhausted,
        kInvalidInput,
        kStopped,
        kProtocolError,
    };

    // kFinished means ordered receive FIN was accepted, not route admission.
    // The owner still inspects session rejection/readiness before dispatch.
    struct Result final {
        Status status{Status::kFed};
        Http3ConnectionResult protocol{};
    };

    // maxTrackedStreams bounds active entries plus retained tombstones. The
    // worker-PMR lookup table is allocated at construction; worker must outlive
    // this input and the associated session. epoch/generation identify this
    // one connection. Bind before the first session feed; thereafter route all
    // receive, cancellation and stop operations through this adapter. Session
    // queries, leases and completed-response release remain direct operations.
    Http3ServerStreamInput(Http3SansIoSessionEngine& session, WorkerMemory& worker,
        std::uint64_t epoch, std::uint64_t connectionGeneration,
        std::size_t maxTrackedStreams);
    ~Http3ServerStreamInput() = default;
    Http3ServerStreamInput(const Http3ServerStreamInput&) = delete;
    Http3ServerStreamInput& operator=(const Http3ServerStreamInput&) = delete;
    Http3ServerStreamInput(Http3ServerStreamInput&&) = delete;
    Http3ServerStreamInput& operator=(Http3ServerStreamInput&&) = delete;

    struct ResumedInput final {
        std::uint64_t streamId{};
        Result result{};
    };
    [[nodiscard]] bool canAcceptInput(std::uint64_t streamId) const noexcept;
    [[nodiscard]] bool receivedEarlyData(std::uint64_t streamId) const noexcept;
    [[nodiscard]] std::optional<ResumedInput> resumeQpack() noexcept;

    // Call once for each routed mailbox block. A mismatched identity is
    // reported without touching input or session state. The borrowed block is
    // not retained and can be released as soon as this call returns. Final-size
    // inconsistency or tracking-capacity exhaustion stops this input/session;
    // the transport owner must close that connection, not retry the message.
    [[nodiscard]] Result acceptData(const Http3StreamMailbox::BorrowedBlock& block) noexcept;
    // kStreamFin.value is the final cumulative wire-byte count. It may precede
    // queued DATA. Peer RESET.value is the server network's cumulative successfully
    // published DATA-byte count, not QUIC Final Size; RESET is deferred until
    // that barrier is consumed. Local cancellation/connection-close controls
    // never synthesize a peer RESET. Callers may route controls before DATA.
    [[nodiscard]] Result acceptControl(const Http3StreamControl& control) noexcept;

    // After transport termination, route local cancellation through this input
    // (not directly through the session) so queued DATA cannot recreate state.
    // May precede HEADERS; only request stream IDs are accepted. No peer RESET
    // is fabricated, and a dispatch lease continues to pin its storage.
    [[nodiscard]] Result cancelRequest(std::uint64_t streamId) noexcept;

    // Stop delivery and retire the session. Idempotent; all subsequent matching
    // input is rejected, and existing per-stream tombstones are retained.
    void stop() noexcept;

    [[nodiscard]] std::size_t trackedStreamCount() const noexcept;
    [[nodiscard]] std::size_t observedRequestStreamCount() const noexcept;
    [[nodiscard]] std::size_t activeRequestStreamCount() const noexcept;
    [[nodiscard]] bool stopped() const noexcept;

private:
    [[nodiscard]] Status identityStatus(const Http3StreamMessageId& id) const noexcept;
    [[nodiscard]] Result acceptFin(const Http3StreamControl& control) noexcept;
    [[nodiscard]] Result applyPeerReset(std::uint64_t streamId, StreamState& state) noexcept;
    [[nodiscard]] Result finalSizeFailure() noexcept;
    [[nodiscard]] Result feedSession(std::uint64_t streamId, std::string_view bytes,
        bool fin, StreamState& state) noexcept;
    [[nodiscard]] static std::size_t tableCapacity(std::size_t maxTrackedStreams);
    [[nodiscard]] StreamState* findOrCreate(std::uint64_t streamId,
        Status& failure) noexcept;
    void closeForConnectionError() noexcept;
    void finishRequestStream(StreamState& state) noexcept;
    void clearQpack(StreamState& state) noexcept;

    Http3SansIoSessionEngine& session_;
    const std::uint64_t epoch_;
    const std::uint64_t connectionGeneration_;
    const std::size_t maxTrackedStreams_;
    std::pmr::vector<StreamSlot> streams_;
    std::size_t trackedStreamCount_{};
    std::size_t observedRequestStreamCount_{};
    std::size_t activeRequestStreamCount_{};
    std::size_t blockedQpackCount_{};
    std::size_t nextQpackResume_{};
    bool stopped_{false};
};

}  // namespace ruvia::detail
