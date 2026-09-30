#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <optional>
#include <span>
#include <string_view>
#include <system_error>

#include <asio/any_io_executor.hpp>

#include "ruvia/core/ConnectionScanner.h"
#include "ruvia/core/MoveOnlyFunction.h"
#include "ruvia/core/StopToken.h"
#include "ruvia/core/WorkerSignal.h"
#include "ruvia/core/memory/MemoryPool.h"
#include "ruvia/http/HttpResponse.h"
#include "ruvia/http/WebSocketProtocolTypes.h"
#include "ruvia/web/detail/http/context/ContextServices.h"
#include "ruvia/web/detail/http3/Http3BufferedResponseOutput.h"
#include "ruvia/web/detail/http3/Http3SansIoSessionEngine.h"
#include "ruvia/web/detail/http3/Http3StreamMailbox.h"
#include "ruvia/web/detail/server/HttpServerOptions.h"
#include "ruvia/web/detail/server/RequestDeadline.h"
#include "ruvia/web/detail/websocket/WsTransportReadResult.h"

namespace ruvia::detail {

class RouteTable;

struct Http3TunnelCallbacks final {
    void* context{};
    bool (*attachScanner)(void*, std::uint64_t, ConnectionScanner::Entry&) noexcept {};
    void (*outputReady)(void*, std::uint64_t) noexcept {};
    void (*abort)(void*, std::uint64_t) noexcept {};
};

// Worker-affine owner for one buffered HTTP/3 request or WebSocket tunnel. It
// pins the session request while routing, response preparation and bounded
// publication run; it does not own or expose the session stream's private memory.
//
// RFC 9114 §§4.2.2, 7.2.4 and 10.5.1 say SHOULD NOT: the peer setting is
// advisory. Compare the decoded sum(name + value + 32), including :status, not
// QPACK/frame bytes. This dispatch checks before its first mailbox handoff;
// after any HEADERS prefix is handed off it must finish that same field section
// even if a later peer setting is lower. This does not guarantee a final
// transport-level field-section limit.
//
// The connection owner must retire/cancel inbound delivery separately through
// Http3ServerStreamInput. publishStep() never waits for mailbox capacity: the
// connection-level scheduler owns fairness and the mailbox's capacity wait.
// ContextServices is snapshotted by value; its worker/token/capability and
// connection-metadata borrows, and all other constructor references, must
// outlive this owner and its tasks.
class Http3BufferedRequestDispatch final {
public:
    enum class PrepareStatus : std::uint8_t {
        kPrepared,
        kAlreadyPrepared,
        kRequestUnavailable,
        kWrongWorker,
        kCancelled,
        kFailed,
    };

    enum class RunStatus : std::uint8_t {
        kResponseReady,
        kAlreadyRun,
        kWrongWorker,
        kCancelled,
        kFilePayloadUnsupported,
        kTunnelComplete,
        kFailed,
    };

    enum class PublishStatus : std::uint8_t {
        kBytesPublished,
        kControlPublished,
        kFinPublished,
        kBackpressured,
        kComplete,
        kNotReady,
        kWrongWorker,
        kCancelled,
        kPeerLimitRejected,
        kFailed,
    };

    enum class PublishBlockReason : std::uint8_t {
        kNone,
        kData,
        kControl,
    };

    enum class CancellationReason : std::uint8_t {
        kNone,
        kWorkerStop,
        kExplicit,
        kDeadline,
    };

    // DATA/CONTROL identify the required mailbox lane. Local demands are
    // handled by publishStep() without waiting for mailbox capacity.
    enum class PublicationDemand : std::uint8_t {
        kData,
        kControl,
        kLocalComplete,
        kLocalCancelled,
        kLocalMailboxStopped,
        kLocalPeerLimitRejected,
        kLocalFailed,
        kNotReady,
        kWrongWorker,
    };

    struct PublishResult final {
        PublishStatus status{PublishStatus::kNotReady};
        std::size_t bytesPublished{};
        // Honor even on failure: already-published output cannot be recalled.
        bool notifyPeer{false};
        // Non-None only for backpressure; identifies the capacity lane, not cursor state.
        PublishBlockReason blockReason{PublishBlockReason::kNone};
    };

    Http3BufferedRequestDispatch(Http3SansIoSessionEngine& session, const RouteTable& routes,
        WorkerMemory& worker, ContextServices services, const HttpServerOptions& options,
        Http3StreamMailbox& outbound, Http3StreamMessageId messageId,
        ConnectionScanner::Entry& scannerEntry, asio::any_io_executor executor,
        Http3TunnelCallbacks tunnelCallbacks);
    ~Http3BufferedRequestDispatch();
    Http3BufferedRequestDispatch(const Http3BufferedRequestDispatch&) = delete;
    Http3BufferedRequestDispatch& operator=(const Http3BufferedRequestDispatch&) = delete;
    Http3BufferedRequestDispatch(Http3BufferedRequestDispatch&&) = delete;
    Http3BufferedRequestDispatch& operator=(Http3BufferedRequestDispatch&&) = delete;

    // Both tasks are lazy. The first actual start acquires the request lease;
    // constructing and discarding either cold task changes no session state.
    [[nodiscard]] Task<PrepareStatus> prepare() &;
    Task<PrepareStatus> prepare() && = delete;
    [[nodiscard]] Task<RunStatus> runHandler() &;
    Task<RunStatus> runHandler() && = delete;

    // Pure, worker-affine query. A foreign worker gets kWrongWorker before any
    // dispatch/session/mailbox state is inspected. Does not publish, acknowledge,
    // drain mailbox returns, allocate/free, invoke handlers, change state, or
    // release resources.
    [[nodiscard]] PublicationDemand publicationDemand() const noexcept;

    // Copies at most one cursor segment (clipped to one mailbox block) per
    // call. Only successful trySend/trySendControl operations are acknowledged.
    // Backpressure identifies the capacity lane without arming a wait here.
    // notifyPeer transfers the exact wakeup obligation, independently of status.
    // Local terminal demands are committed and cleaned up here, never by the query.
    [[nodiscard]] PublishResult publishStep() & noexcept;
    PublishResult publishStep() && = delete;

    // During publication only, subscribe the supplied allocation-free callback
    // to an explicitly armed request deadline. No registration is made for the
    // default no-deadline case. The callback is synchronous on the stop source's
    // requesting thread; connection owners require that source to be worker-affine.
    [[nodiscard]] bool registerPublicationDeadlineCallback(
        MoveOnlyFunction<void()> callback) &;
    bool registerPublicationDeadlineCallback(MoveOnlyFunction<void()>) && = delete;

    // The reason is latched before requestDeadline_ is destroyed, so owners can
    // distinguish deadline cancellation from worker stop and explicit cancel.
    [[nodiscard]] CancellationReason cancellationReason() const noexcept;

    // Worker-affine. Marks output terminal before requesting stop. The caller
    // must separately retire inbound delivery and await/join a running handler.
    void cancel() & noexcept;
    void cancel() && = delete;

    [[nodiscard]] Task<std::error_code> publishTunnelHandshake(
        std::span<const char> headersFrame);
    void notifyTunnelInput() noexcept;
    [[nodiscard]] Task<WsTransportReadResult> readTunnel(std::pmr::string& buffer);
    [[nodiscard]] Task<std::error_code> writeTunnel(std::string_view bytes,
        WebSocketTransportDisposition disposition);
    [[nodiscard]] Task<bool> waitTunnelReceiveEnd();
    void abortTunnel() noexcept;
    [[nodiscard]] asio::any_io_executor executor() const noexcept {
        return executor_;
    }

    [[nodiscard]] bool handlerActive() const noexcept;
    [[nodiscard]] bool responseReady() const noexcept;
    // True once all response bytes and the final FIN control are enqueued in
    // outbound_; this is handoff completion, not transport write or peer receipt.
    [[nodiscard]] bool complete() const noexcept;
    // Cumulative HTTP/3 wire bytes successfully handed to outbound_.
    [[nodiscard]] std::uint64_t publishedWireBytes() const noexcept;
    [[nodiscard]] std::exception_ptr failure() const noexcept;

private:
    enum class State : std::uint8_t {
        kCold,
        kPreparing,
        kPrepared,
        kRunning,
        kOutputReady,
        kPublishing,
        kComplete,
        kCancelled,
        kPeerLimitRejected,
        kFailed,
    };

    [[nodiscard]] Task<RunStatus> runHandlerInner();
    [[nodiscard]] Task<RunStatus> runWebSocketHandler();
    [[nodiscard]] bool onWorker() const noexcept;
    void notifyTunnelOutput() noexcept;
    [[nodiscard]] PublishResult publishTunnelStep(PublicationDemand demand) noexcept;
    [[nodiscard]] bool cancellationRequested() const noexcept;
    void latchCancellationReason() const noexcept;
    [[nodiscard]] bool exceedsPeerFieldSectionLimit() const noexcept;
    void fail(std::exception_ptr failure = {}) noexcept;
    [[nodiscard]] bool releaseDispatchStorage() noexcept;
    static void peerTransportFinTimeoutTick(void* target, std::int64_t nowMs) noexcept;
    void armPeerTransportFinTimeout() noexcept;
    void disarmPeerTransportFinTimeout() noexcept;

    Http3SansIoSessionEngine& session_;
    const RouteTable& routes_;
    WorkerMemory& worker_;
    const ContextServices services_;
    const HttpServerOptions& options_;
    Http3StreamMailbox& outbound_;
    const Http3StreamMessageId messageId_;
    ConnectionScanner::Entry& scannerEntry_;
    ConnectionScanner::PeriodicCheckRegistration peerTransportFinCheck_;
    asio::any_io_executor executor_;
    const Http3TunnelCallbacks tunnelCallbacks_;
    WorkerSignal tunnelInputAvailable_;
    WorkerSignal tunnelOutputAvailable_;

    // Declaration order makes destruction output/cursor -> response -> request
    // arena -> lease. The lease is returned only after every dependent object dies.
    std::optional<Http3SansIoSessionEngine::RequestLease> lease_;
    std::optional<RequestMemory> requestMemory_;
    std::optional<HttpResponse> response_;
    std::optional<Http3BufferedResponseOutput> output_;
    std::pmr::string tunnelHandshakeFrame_;
    std::size_t tunnelHandshakeOffset_{};
    std::pmr::string tunnelDataFrame_;
    std::uint64_t tunnelPublishedWireBytes_{};
    bool tunnelMode_{};
    bool tunnelDataPending_{};
    bool tunnelEstablishedPending_{};
    bool tunnelEstablishedPublished_{};
    bool tunnelFinPending_{};
    bool tunnelOutputEnded_{};
    std::chrono::milliseconds peerTransportFinTimeout_{};
    std::int64_t peerTransportFinDeadlineMs_{};
    bool peerFinTimeoutArmed_{};
    bool tunnelAborted_{};

    StopSource requestStopSource_;
    StopToken combinedWorkerAndRequestStop_;
    StopRegistration tunnelStopRegistration_;
    StopRegistration publicationDeadlineRegistration_;
    std::optional<RequestDeadline> requestDeadline_;
    std::optional<ContextServices> requestServices_;

    std::exception_ptr failure_;
    std::uint64_t publishedWireBytes_{};
    State state_{State::kCold};
    mutable CancellationReason cancellationReason_{CancellationReason::kNone};
    bool handlerActive_{false};
    bool cancellationRequested_{false};
    bool deadlineArmed_{};
    bool publicationDeadlineCallbackRegistered_{};
};

}  // namespace ruvia::detail
