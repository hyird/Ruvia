#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <thread>
#include <vector>

#include <asio/io_context.hpp>
#include <asio/steady_timer.hpp>

#include "ruvia/core/WorkerRuntimeContext.h"
#include "ruvia/core/memory/MemoryPool.h"
#include "ruvia/core/memory/PmrObject.h"
#include "ruvia/http/Http3ServerRequestAdmission.h"
#include "ruvia/http/Http3StreamFrames.h"
#include "ruvia/http/Http3VarInt.h"
#include "ruvia/web/detail/http3/Http3CriticalStreamDriver.h"
#include "ruvia/web/detail/http3/Http3DatagramEndpoint.h"
#include "ruvia/web/detail/http3/Http3QuicServerTransport.h"
#include "ruvia/web/detail/http3/Http3QuicTlsContext.h"
#include "ruvia/web/detail/http3/Http3QuicWireOwner.h"
#include "ruvia/web/detail/http3/Http3ServerConnectionChannel.h"
#include "ruvia/web/detail/http3/Http3ServerStreamOutput.h"
#include "ruvia/web/detail/http3/Http3StreamMailbox.h"

namespace ruvia::detail {

class Http3WorkerServer;
struct HttpServerListenerDefinition;

// Runs on the server network thread and owns long-lived HTTP/3 UDP/QUIC wire
// I/O. SSL*, BIO, UDP, timers, critical streams, and response output stay here.
class Http3NetworkRuntime final {
public:
    struct WorkerTarget final {
        Http3WorkerServer* server{};
        std::size_t maxConnections{};
        std::uint32_t mailboxCapacity{};
        std::size_t maxRequestsPerConnection{};
        std::optional<std::chrono::milliseconds> idleTimeout{};
        std::optional<std::chrono::milliseconds> requestHeaderTimeout{};
        std::optional<std::chrono::milliseconds> requestBodyTimeout{};
        std::optional<std::chrono::milliseconds> writeTimeout{};
    };

    struct FailureNotification final {
        void* context{};
        void (*notify)(void*, std::exception_ptr) noexcept {};
    };

    Http3NetworkRuntime(ruvia::WorkerRuntimeContext& networkRuntime,
        const HttpServerListenerDefinition& listener,
        std::span<const WorkerTarget> workers, FailureNotification failure);
    ~Http3NetworkRuntime();

    Http3NetworkRuntime(const Http3NetworkRuntime&) = delete;
    Http3NetworkRuntime& operator=(const Http3NetworkRuntime&) = delete;

    // Called on the server network owner before worker loops start or either runtime serves.
    void stageWorkerLinks();
    // All following methods are server-network-owner only except wake(), which is the
    // bounded cross-thread notification endpoint used by worker publications.
    void start();
    void wake() noexcept;
    void stop() noexcept;
    [[nodiscard]] bool drained() const noexcept;
    [[nodiscard]] asio::ip::udp::endpoint localEndpoint() const;

private:
    enum class TunnelEstablishedResult : std::uint8_t {
        kAccepted,
        kIgnoredTerminal,
        kProtocolFailure,
    };

    struct Stream final {
        explicit Stream(std::pmr::memory_resource* resource)
            : frameTracker(nullptr, PmrObjectDeleter<Http3StreamFrames>{resource}) {}

        enum class ReceivePhase : std::uint8_t { kHeaders,
            kBody };

        Http3QuicServerTransport::StreamId id{};
        std::uint64_t receivedBytes{};
        std::optional<Http3StreamControl> pendingControl;
        std::unique_ptr<Http3StreamFrames, PmrObjectDeleter<Http3StreamFrames>> frameTracker;
        std::chrono::steady_clock::time_point lastInputActivity{};
        std::optional<std::uint64_t> tunnelEstablishedBarrier{};
        ReceivePhase receivePhase{ReceivePhase::kHeaders};
        bool accepted{};
        bool requestStream{};
        bool inputTerminal{};
        bool inputFin{};
        bool inputReset{};
        bool writeTimeoutNotified{};
        bool tunnelEstablished{};

        [[nodiscard]] TunnelEstablishedResult acceptTunnelEstablished(
            const Http3StreamControl& control,
            Http3ServerConnectionChannel::Identity identity,
            std::uint64_t acceptedWireBytes) noexcept {
            if (control.kind != Http3StreamControl::Kind::kTunnelEstablished ||
                control.id.epoch != identity.epoch ||
                control.id.connectionGeneration != identity.connectionGeneration ||
                control.id.streamId != id || !requestStream || control.value == 0 ||
                control.value > kHttp3VarIntMax) {
                return TunnelEstablishedResult::kProtocolFailure;
            }
            if (inputReset || (inputTerminal && !inputFin)) {
                return TunnelEstablishedResult::kIgnoredTerminal;
            }
            if (receivePhase != ReceivePhase::kBody || tunnelEstablished ||
                tunnelEstablishedBarrier) {
                return TunnelEstablishedResult::kProtocolFailure;
            }
            tunnelEstablishedBarrier = control.value;
            (void)confirmTunnelEstablished(acceptedWireBytes);
            return TunnelEstablishedResult::kAccepted;
        }

        [[nodiscard]] bool confirmTunnelEstablished(
            std::uint64_t acceptedWireBytes) noexcept {
            if (!tunnelEstablishedBarrier ||
                acceptedWireBytes < *tunnelEstablishedBarrier) {
                return false;
            }
            tunnelEstablishedBarrier.reset();
            tunnelEstablished = true;
            return true;
        }

        [[nodiscard]] bool bodyTimeoutApplies() const noexcept {
            return requestStream && receivePhase == ReceivePhase::kBody &&
                   !inputFin && !inputReset && !inputTerminal && !tunnelEstablished;
        }
    };

    struct Connection final {
        explicit Connection(std::pmr::memory_resource* resource)
            : streams(resource),
              output(nullptr, PmrObjectDeleter<Http3ServerStreamOutput>{resource}),
              critical(nullptr, PmrObjectDeleter<Http3CriticalStreamDriver>{resource}) {}

        Http3ServerConnectionChannel::Identity identity{};
        bool hasLastIdentity{};
        Http3QuicServerTransport::ConnectionId transportId{};
        std::pmr::vector<Stream> streams;
        std::size_t nextInputStreamIndex{};
        std::size_t nextTunnelHandshakeStreamIndex{};
        std::size_t tunnelHandshakeScanRemaining{};
        std::size_t pendingTunnelHandshakes{};
        bool tunnelHandshakeScanDirty{};
        std::unique_ptr<Http3ServerStreamOutput,
            PmrObjectDeleter<Http3ServerStreamOutput>>
            output;
        std::unique_ptr<Http3CriticalStreamDriver,
            PmrObjectDeleter<Http3CriticalStreamDriver>>
            critical;
        struct PendingIntentSettlement final {
            Http3ServerConnection::TransportIntentToken token{};
            Http3ServerConnectionChannel::IntentSettlement settlement{
                Http3ServerConnectionChannel::IntentSettlement::kExecutedHandoff};
        };
        std::optional<PendingIntentSettlement> pendingIntentAck;
        std::optional<Http3ServerRequestAdmissionPlanner> admissionPlanner;
        std::optional<std::chrono::steady_clock::time_point> drainDeadline;
        std::size_t admittedRequestCount{};
        std::size_t peerUnidirectionalStreamCount{};
        std::size_t rejectedRequestCount{};
        bool grantReceived{};
        bool accepted{};
        bool bindPublished{};
        bool attachResolved{};
        bool attached{};
        bool admissionSealedPublished{};
        bool goawayQueued{};
        bool goawayBytesAccepted{};
        bool drainCompleteReceived{};
        bool drainReadyForClose{};
        bool gracefulCloseStarted{};
        bool gracefulCloseAbandoned{};
        std::optional<Http3ConnectionErrorCode> closeErrorCode;
        // closeStarted denotes the rapid, no-flush forced path only.
        bool closeStarted{};
        bool transportRetiredPublished{};
        bool workerFinalized{};
        bool workerFinalizedAcknowledged{};
        bool networkPublicationsClosed{};
        bool networkFinalizedPublished{};
        bool revokeRequested{};
        bool revokeAcknowledged{};
    };

    struct WorkerLink final {
        WorkerLink(std::pmr::memory_resource* resource, Http3NetworkRuntime& network,
            WorkerTarget configured);

        WorkerTarget target;
        Http3StreamMailbox requestMailbox;
        std::pmr::vector<std::unique_ptr<Http3ServerConnectionChannel,
            PmrObjectDeleter<Http3ServerConnectionChannel>>>
            channels;
        std::pmr::vector<Http3ServerConnectionChannel*> channelViews;
        std::pmr::vector<Connection> connections;
        std::optional<Http3StreamMailbox::BorrowedBlock> pendingResponse;
        std::optional<Http3StreamControl> pendingResponseControl;
        bool responseMailboxDrained{};
    };

    static void networkWake(void* context) noexcept;
    static void workerWake(void* context) noexcept;
    [[nodiscard]] bool protocolPump() noexcept;
    void requireOwnerThread() const noexcept;
    void requestStopOnOwner() noexcept;
    void reportFailure(std::exception_ptr failure) noexcept;
    void scheduleMonitor() noexcept;
    void monitor(const asio::error_code& error) noexcept;
    [[nodiscard]] bool pumpWorker(WorkerLink& worker) noexcept;
    [[nodiscard]] bool pumpChannels(WorkerLink& worker) noexcept;
    [[nodiscard]] bool admit(WorkerLink& worker, std::size_t index) noexcept;
    [[nodiscard]] bool retireUnbound(WorkerLink& worker, std::size_t index) noexcept;
    [[nodiscard]] bool requestRevoke(WorkerLink& worker, std::size_t index) noexcept;
    [[nodiscard]] bool attach(WorkerLink& worker, std::size_t index) noexcept;
    [[nodiscard]] bool pumpInput(WorkerLink& worker, std::size_t index) noexcept;
    [[nodiscard]] bool pumpResponses(WorkerLink& worker) noexcept;
    [[nodiscard]] static TunnelEstablishedResult acceptTunnelEstablished(Connection& connection,
        const Http3StreamControl& control, std::uint64_t acceptedWireBytes) noexcept;
    [[nodiscard]] static bool confirmTunnelEstablished(Connection& connection,
        Http3QuicServerTransport::StreamId streamId,
        std::uint64_t acceptedWireBytes) noexcept;
    static void notePeerFin(Connection& connection,
        Http3QuicServerTransport::StreamId streamId) noexcept;
    static void noteInputReset(Connection& connection,
        Http3QuicServerTransport::StreamId streamId) noexcept;
    static void completeInputTerminal(Connection& connection,
        Http3QuicServerTransport::StreamId streamId) noexcept;
    void terminateRequestStream(Connection& connection,
        Http3QuicServerTransport::StreamId streamId, std::uint64_t errorCode) noexcept;
    void stopRequestInput(Connection& connection,
        Http3QuicServerTransport::StreamId streamId) noexcept;
    [[nodiscard]] bool pumpOutput(WorkerLink& worker, std::size_t index) noexcept;
    [[nodiscard]] bool announceGoaway(WorkerLink& worker, std::size_t index) noexcept;
    [[nodiscard]] bool sealAdmission(WorkerLink& worker, std::size_t index) noexcept;
    [[nodiscard]] bool rejectRequestStream(WorkerLink& worker, std::size_t index,
        Http3QuicServerTransport::StreamId streamId) noexcept;
    [[nodiscard]] bool retire(WorkerLink& worker, std::size_t index) noexcept;
    [[nodiscard]] Connection* findConnection(
        WorkerLink& worker, Http3StreamMessageId id) noexcept;
    void closeConnection(Connection& connection,
        Http3ConnectionErrorCode reason) noexcept;
    void notifyWorker(WorkerLink& worker) noexcept;

    ruvia::WorkerRuntimeContext& networkRuntime_;
    asio::io_context& ioContext_;
    const std::thread::id ownerThread_;
    WorkerMemory memory_;
    asio::ip::address bindAddress_;
    Http3QuicTlsContext tls_;
    Http3QuicWireOwner wire_;
    std::pmr::vector<std::unique_ptr<WorkerLink, PmrObjectDeleter<WorkerLink>>> workers_;
    asio::steady_timer monitorTimer_;
    FailureNotification failureNotification_{};
    std::exception_ptr failure_;
    std::atomic<bool> wakeScheduled_{};
    std::size_t nextWorker_{};
    std::size_t pumpBudget_{512};
    std::chrono::milliseconds drainTimeout_{};
    bool staged_{};
    bool running_{};
    bool stopping_{};
    bool monitorScheduled_{};
    bool failureReported_{};
    bool transportActivityForPump_{};
    friend struct Http3NetworkRuntimeTestAccess;
};

}  // namespace ruvia::detail
