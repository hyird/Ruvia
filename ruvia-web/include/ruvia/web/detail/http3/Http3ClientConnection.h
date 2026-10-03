#pragma once
#include <array>
#include <bitset>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <asio/io_context.hpp>
#include <asio/ip/udp.hpp>

#include "ruvia/core/Task.h"
#include "ruvia/core/TaskScope.h"
#include "ruvia/core/WorkerHandle.h"
#include "ruvia/core/WorkerSignal.h"
#include "ruvia/core/memory/PmrObject.h"
#include "ruvia/http/HttpClient.h"
#include "ruvia/http/HttpDatagram.h"
#include "ruvia/http/HttpHeader.h"
#include "ruvia/http/HttpPriority.h"
#include "ruvia/http/HttpResponse.h"
#include "ruvia/web/HttpClientPushConfig.h"
#include "ruvia/web/detail/client/HttpClientRequestStorage.h"
#include "ruvia/web/detail/client/HttpClientResponseState.h"
#include "ruvia/web/detail/http3/Http3ClientBodyBudget.h"
#include "ruvia/web/detail/http3/Http3ClientReceiveDriver.h"
#include "ruvia/web/detail/http3/Http3ClientRequestDriver.h"
#include "ruvia/web/detail/http3/Http3ClientResponseDelivery.h"
#include "ruvia/web/detail/http3/Http3QuicClientEndpointResolver.h"
#include "ruvia/web/detail/http3/Http3QuicClientSocketSession.h"

namespace ruvia::detail {

struct Http3ClientOriginObserver final {
    void* context{};
    std::size_t connectionSlot{};
    void (*receive)(void*, std::size_t, const HttpOriginAdvertisement&){};
};

class Http3ClientConnection;
// A pool accepts promises into its independent response-memory domain. receive
// returns one retained state reference for the connection; finished balances
// the pool's active-push admission. Neither callback may drive the protocol.
struct Http3ClientPushObserver final {
    void* context{};
    std::size_t connectionSlot{};
    HttpClientPushConfig config{};
    HttpClientResponseState* (*receive)(void*, std::size_t, Http3ClientConnection&, std::uint64_t, const Http3MessageHead&){};
    void (*finished)(void*) noexcept {};
};

// One address-stable connection backend owned by its HTTP client pool. The
// pool's TaskScope owns its sole DNS/QUIC driver Task; request waiters never
// drive sockets. Incremental response delivery binds an address-stable public
// response state while this owner retains the corresponding request node.
// Pool/TLS/worker resources and this object must survive request waiters and
// the driver's join. All methods, including stop and destruction, are worker-
// affine; cross-thread cancellation must post through the worker endpoint.
class Http3ClientConnection final {
public:
    using TimePoint = std::chrono::steady_clock::time_point;
    using RequestId = std::uint64_t;
    enum class Outcome : std::uint8_t {
        kPending,
        kComplete,
        kCancelled,
        kDeadline,
        kConnectFailed,
        kTransportError,
        kProtocolError,
        // Peer reports unprocessed, or local admission stopped before opening a stream.
        kRequestRejected,
        kResponseTooLarge,
        kResultBudgetExceeded,
        kConnectionDraining,
        kQueueFull,
        kInvalidRequest,
    };
    struct Response final {
        explicit Response(std::pmr::memory_resource* resource)
            : headers(resource),
              trailers(resource),
              body(resource) {}
        Outcome outcome{Outcome::kPending};
        std::uint16_t status{};
        // Empty unless the peer's final response head supplied this value.
        std::optional<HttpResponseBodyPlan> responseBodyPlan{};
        std::optional<std::uint64_t> peerResetErrorCode{};
        std::pmr::vector<HttpHeader> headers;
        std::pmr::vector<HttpHeader> trailers;
        std::pmr::string body;
    };
    struct Submission final {
        Outcome outcome{Outcome::kPending};
        RequestId id{};
    };
    struct RejectedRequest final {
        HttpClientRequestStorage request;
        std::optional<TimePoint> deadline{};
    };
    struct LifecycleNotification final {
        void* context{};
        void (*notify)(void*) noexcept {};
    };

    // receiveBodyBudget may borrow a pool-owned worker-affine owner shared by
    // successive connections. It must outlive this connection and its receive
    // leases; null selects the connection-local budget, which is detached with
    // the response before this connection can be destroyed.
    Http3ClientConnection(asio::io_context& io, const WorkerHandle& worker,
        TaskScope& poolTasks, http3_quic_client_tls_context& tls, HttpOriginView origin,
        std::chrono::milliseconds connectTimeout,
        std::pmr::memory_resource* resource, std::size_t maxRequests = 32,
        std::size_t maxResponseBytes = 16 * 1024 * 1024,
        std::chrono::milliseconds idleTimeout = std::chrono::seconds(30),
        Http3ClientBodyBudget* receiveBodyBudget = nullptr,
        std::optional<std::chrono::milliseconds> writeTimeout = std::chrono::seconds(30));
    Http3ClientConnection(asio::io_context& io, const WorkerHandle& worker,
        TaskScope& poolTasks, http3_quic_client_tls_context& tls, HttpOriginView origin,
        std::chrono::milliseconds connectTimeout, std::pmr::memory_resource* resource,
        std::size_t maxRequests, std::size_t maxResponseBytes,
        std::chrono::milliseconds idleTimeout, Http3ClientBodyBudget* receiveBodyBudget,
        std::optional<std::chrono::milliseconds> writeTimeout,
        LifecycleNotification lifecycleNotification, Http3QpackConfig qpack = {},
        Http3ClientOriginObserver originObserver = {}, Http3ClientPushObserver pushObserver = {},
        ruvia::quic_version initial_version = ruvia::quic_version::v1,
        bool enable_early_data = false);
    ~Http3ClientConnection();
    Http3ClientConnection(const Http3ClientConnection&) = delete;
    Http3ClientConnection& operator=(const Http3ClientConnection&) = delete;
    Http3ClientConnection(Http3ClientConnection&&) = delete;
    Http3ClientConnection& operator=(Http3ClientConnection&&) = delete;

    // Request storage becomes owned before this returns. An accepted request
    // remains address stable, including a pending SSL_write WANT range, until
    // completion or cancellation and explicit release(id).
    [[nodiscard]] Submission submit(HttpClientRequestStorage request,
        std::optional<TimePoint> deadline = {});
    // Internal response-state delivery reuses the same request node and sole
    // connection driver. The state is borrowed until its consumer releases it
    // and all request waiters have left; its owner must keep this connection
    // alive for that entire interval.
    [[nodiscard]] Submission submit(HttpClientRequestStorage request,
        HttpClientResponseState& response, std::optional<TimePoint> deadline = {});
    // Uses the original absolute deadline rather than granting a new timeout.
    [[nodiscard]] Submission submit(RejectedRequest request);
    void start();
    void startIfNeeded();
    void cancel(RequestId id) noexcept;
    [[nodiscard]] bool reprioritize(RequestId id, HttpPriority priority);
    [[nodiscard]] Task<void> wait(RequestId id);
    [[nodiscard]] const Response* result(RequestId id) const noexcept;
    [[nodiscard]] bool release(RequestId id) noexcept;
    // Detaches a terminal, retired incremental response without modifying its
    // response data. A nonempty body lease may survive only through a budget
    // shared by successive connections on this worker. The caller must retain
    // the response state, its PMR/result-budget owners, and any external budget;
    // this does not transfer ownership or permit cross-worker/pool teardown.
    // Unprocessed requests remain available to their retry/handoff path.
    [[nodiscard]] bool releaseResponseRequest(RequestId id) noexcept;
    void abandonResponse(RequestId id) noexcept;
    void consumerReleased(RequestId id) noexcept;
    [[nodiscard]] HttpDatagramSessionConfig datagramConfig(RequestId id) const;
    [[nodiscard]] bool sendDatagram(RequestId id, std::span<const std::byte> wire);
    // Consumes a peer-rejected or locally unstarted terminal request after all waiters leave.
    // For a bound public response, detaches its empty response state without publishing the
    // rejection so the pool can retry once; any observed response data makes handoff unsafe.
    // Stream retirement already completed before terminal publication. Returned storage keeps
    // its worker allocator, whose owner must outlive it, and preserves even an expired deadline.
    [[nodiscard]] std::optional<RejectedRequest> takeRejectedRequest(RequestId id);
    [[nodiscard]] ruvia::quic_path_migration start_path_migration(
        const asio::ip::udp::endpoint& local_endpoint);
    [[nodiscard]] std::optional<ruvia::quic_path_migration> path_migration(
        std::uint64_t id) const noexcept;
    [[nodiscard]] std::optional<ruvia::quic_path_migration> active_path_migration() const noexcept;
    [[nodiscard]] std::uint64_t quic_generation() const noexcept {
        return quic_generation_;
    }
    [[nodiscard]] ruvia::quic_operation_status cancel_path_migration(std::uint64_t id);
    void requestStop() noexcept;
    [[nodiscard]] bool running() const noexcept {
        return running_;
    }
    [[nodiscard]] bool accepting() const noexcept {
        return !stopping_ && !draining_ && !terminal_ &&
               requests_.size() < maxRequests_;
    }
    [[nodiscard]] bool terminal() const noexcept {
        return terminal_;
    }
    [[nodiscard]] std::size_t retainedRequests() const noexcept {
        return requests_.size();
    }
    [[nodiscard]] std::size_t retainedResultBodyBytes() const noexcept {
        return retainedResultBodyBytes_;
    }
    [[nodiscard]] std::size_t active_response_streams() const noexcept {
        return responseEngine_.liveStreamCount();
    }

private:
    friend struct http3_client_connection_test_access;

    struct Request final {
        Request(RequestId value, Http3ClientRequestWrite&& write, const WorkerHandle& worker,
            std::pmr::memory_resource* resource, std::optional<TimePoint> absoluteDeadline,
            bool replay_safe)
            : id(value),
              writer(std::move(write)),
              signal(worker),
              response(resource),
              deadline(absoluteDeadline),
              replay_safe(replay_safe) {}
        Request(RequestId value, Http3ClientRequestWrite&& write, const WorkerHandle& worker,
            std::pmr::memory_resource* resource, std::optional<TimePoint> absoluteDeadline,
            bool replay_safe, HttpClientResponseState& responseState,
            Http3ClientBodyBudget& bodyBudget)
            : Request(value, std::move(write), worker, resource, absoluteDeadline, replay_safe) {
            responseState_ = &responseState;
            delivery.emplace(responseState, &bodyBudget);
        }
        RequestId id;
        Http3ClientRequestDriver writer;
        WorkerSignal signal;
        Response response;
        std::optional<TimePoint> deadline;
        std::optional<TimePoint> writeDeadline;
        std::optional<TimePoint> continueDeadline;
        std::optional<HttpPriority> pending_priority_update;
        std::size_t waiters{};
        HttpClientResponseState* responseState_{};
        std::optional<Http3ClientResponseDelivery> delivery{};
        bool consumerReleased{};
        bool responseParserRegistered{};
        bool streamRetired{};
        bool cancelRequested{};
        bool replay_safe{};
        bool early_data_eligible{};
    };
    struct Push final {
        RequestId id{};
        std::uint64_t pushId{};
        std::optional<std::uint64_t> streamId{};
        std::optional<TimePoint> deadline{};
        HttpClientResponseState* state{};
        std::optional<Http3ClientResponseDelivery> delivery{};
        bool cancelRequested{};
        bool peerCancelled{};
    };
    struct PeerPushStream final {
        std::uint64_t pushId{};
        std::uint64_t streamId{};
        std::optional<TimePoint> deadline{};
    };
    using PushList = std::pmr::list<Push>;
    static constexpr std::size_t kMaxRememberedPushes = 128;
    using RequestList = std::pmr::list<Request>;
    using SessionOwner = std::unique_ptr<Http3QuicClientSocketSession,
        PmrObjectDeleter<Http3QuicClientSocketSession>>;

    static void onPushEvent(void* context, const Http3ConnectionEvent& event);
    [[nodiscard]] PushList::iterator findPush(RequestId id) noexcept;
    [[nodiscard]] PushList::iterator findPushByStream(std::uint64_t streamId) noexcept;
    void finishPush(PushList::iterator push, Outcome outcome);
    void settlePush(std::uint64_t pushId) noexcept;
    [[nodiscard]] bool sweepPushes();
    [[nodiscard]] bool flushPushControl();
    void requireOwnerThread() const;
    static void onReceiveBodyBudgetReleased(void* context) noexcept;
    static void onResponseEvent(void* context, const Http3ConnectionEvent& event);
    void wakeReceiveDriver() noexcept;
    [[nodiscard]] RequestList::iterator find(RequestId id) noexcept;
    [[nodiscard]] RequestList::const_iterator find(RequestId id) const noexcept;
    [[nodiscard]] Submission submitImpl(HttpClientRequestStorage request,
        std::optional<TimePoint> deadline, HttpClientResponseState* response);
    [[nodiscard]] Task<void> drive();
    [[nodiscard]] Task<bool> driveEndpoint(const asio::ip::udp::endpoint& peer, TimePoint connectDeadline);
    void cache_path_migration() noexcept;
    [[nodiscard]] bool sweep(bool requestsMayStart, bool early_data_only = false);
    [[nodiscard]] bool receivePeerStreams();
    [[nodiscard]] bool replay_rejected_early_streams();
    [[nodiscard]] bool receiveRequests();
    [[nodiscard]] bool receiveDatagrams();
    [[nodiscard]] bool driveRequestWriters(bool early_data_only = false);
    [[nodiscard]] bool driveCriticalOutput();
    void finishRequest(Request& request, Outcome outcome);
    void maybeReleaseResponseRequest(Request& request) noexcept;
    void reapReleasedResponseRequests() noexcept;
    void finishAll(Outcome outcome) noexcept;
    [[nodiscard]] bool retireRequest(Request& request, bool graceful = false) noexcept;
    [[nodiscard]] std::optional<TimePoint> nextDeadline(
        std::optional<TimePoint> connectDeadline) const noexcept;

    std::thread::id ownerThread_;
    asio::io_context& io_;
    const WorkerHandle& worker_;
    TaskScope& poolTasks_;
    http3_quic_client_tls_context& tls_;
    std::pmr::memory_resource* resource_;
    std::pmr::string host_;
    std::pmr::string authority_;
    std::uint16_t port_;
    ruvia::quic_version initial_version_;
    bool enable_early_data_{};
    TimePoint::duration connectTimeout_;
    TimePoint::duration idleTimeout_;
    std::optional<TimePoint::duration> writeTimeout_;
    std::size_t maxRequests_;
    std::size_t maxResponseBytes_;
    Http3QuicClientEndpointResolver resolver_;
    // The connection-local budget remains for sans-I/O storage and terminal
    // response bodies. Receive-state leases may instead borrow the pool owner.
    Http3ClientBodyBudget bodyBudget_;
    Http3ClientBodyBudget* receiveBodyBudget_{};
    LifecycleNotification lifecycleNotification_{};
    Http3ClientOriginObserver originObserver_{};
    Http3ClientPushObserver pushObserver_{};
    Http3ClientSansIoSessionEngine responseEngine_;
    Http3ClientReceiveDriver receiver_;
    std::array<std::pmr::string, 3> criticalOutput_;
    std::array<std::size_t, 3> criticalOutputOffset_{};
    std::array<std::optional<TimePoint>, 3> criticalWriteDeadlines_{};
    SessionOwner session_;
    Http3ClientBodyBudget::WakeRegistration receiveBodyBudgetWake_;
    RequestList requests_;
    PushList pushes_;
    std::pmr::vector<PeerPushStream> peerPushStreams_;
    std::bitset<kMaxRememberedPushes> seenPushes_{};
    std::bitset<kMaxRememberedPushes> settledPushes_{};
    std::bitset<kMaxRememberedPushes> cancelledPushes_{};
    std::uint64_t authorizedPushId_{};
    std::size_t pendingPushCredits_{};
    std::pmr::vector<std::uint64_t> peerStreams_;
    RequestId nextRequestId_{};
    std::size_t retainedResultBodyBytes_{};
    bool running_{};
    std::uint64_t quic_generation_{};
    std::uint64_t next_path_migration_id_{1};
    std::uint64_t quic_connection_migration_id_{};
    std::uint64_t quic_path_migration_generation_{};
    std::optional<ruvia::quic_path_migration> quic_path_migration_{};
    bool starting_{};
    bool stopping_{};
    bool draining_{};
    bool terminal_{};
    Outcome terminalFailure_{Outcome::kTransportError};
};

}  // namespace ruvia::detail
