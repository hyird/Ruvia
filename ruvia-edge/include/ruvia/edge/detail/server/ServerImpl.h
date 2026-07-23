#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include <asio/awaitable.hpp>
#include <asio/cancellation_signal.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/ssl.hpp>
#include <asio/steady_timer.hpp>

#include "ruvia/core/memory/MemoryPool.h"
#include "ruvia/edge/EdgeServer.h"
#include "ruvia/edge/detail/cache/CacheKey.h"
#include "ruvia/edge/detail/cache/EdgeCache.h"
#include "ruvia/edge/detail/server/EdgeConfig.h"
#include "ruvia/edge/detail/cache/DiskTier.h"
#include "ruvia/edge/detail/ResponseWriter.h"
#include "ruvia/edge/detail/OriginFetcher.h"
#include "ruvia/http/HttpHeader.h"
#include "ruvia/http/HttpKnownMethod.h"
#include "ruvia/http/detail/http1/Http1ServerRequestParser.h"

namespace ruvia::edge {

// One buffered client request handed to the serve core, protocol-independent.
// Views are valid only for the duration of the serve call.
struct EdgeRequest final {
    std::string_view method;
    HttpKnownMethod knownMethod{HttpKnownMethod::kUnknown};
    std::string_view target;
    std::string_view host;
    std::span<const HttpHeaderView> headers;
    std::optional<std::string_view> body;
    std::string_view clientAddress;
    bool keepAlive{true};
};

// The edge node's runtime: the worker thread and its io_context, the listener,
// the cache tiers and the origin fetcher. Declared here rather than inside its
// single translation unit because its members are defined across three, one per
// concern:
//
//   server/Server.cpp   the public facade, construction, the lifecycle state
//                       machine and the control plane serialized onto the worker
//   server/Session.cpp  the accept loop, the TLS handshake and ALPN split, and
//                       the HTTP/1 and HTTP/2 session drivers
//   server/Serve.cpp    the protocol-independent serve core: cache lookup,
//                       revalidation, request coalescing and background refresh
class EdgeServer::Impl final {
public:
    Impl(EdgeEndpoint endpoint, EdgeServerOptions options);
    ~Impl();

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    void start();
    void stop();
    void join();
    [[nodiscard]] EdgeEndpoint localEndpoint() const;
    [[nodiscard]] EdgeStats stats() const;

    bool addOrigin(std::string frontHost, OriginSettings settings);
    bool removeOrigin(std::string_view frontHost);
    bool setTlsCertificate(const EdgeTlsConfig& tls);
    bool purge(std::string_view frontHost, std::string_view target);
    [[nodiscard]] bool clearCache();

private:
    using TlsContextPtr = std::shared_ptr<asio::ssl::context>;

    enum class Lifecycle : std::uint8_t {
        kReady,
        kRunning,
        kStopping,
        kStopped,
    };

    // Holds one accepted connection's slot in the node's connection budget.
    // Moved into the session coroutine's frame, so the slot is released exactly
    // when that frame is destroyed -- however the session ended, including an
    // unwind. Worker-affine: every slot change happens on the Edge worker.
    class ConnectionLease final {
    public:
        explicit ConnectionLease(std::atomic<std::size_t>& count) noexcept
            : count_(&count) {
            count_->fetch_add(1, std::memory_order_relaxed);
        }

        ConnectionLease(const ConnectionLease&) = delete;
        ConnectionLease& operator=(const ConnectionLease&) = delete;
        ConnectionLease& operator=(ConnectionLease&&) = delete;

        ConnectionLease(ConnectionLease&& other) noexcept
            : count_(std::exchange(other.count_, nullptr)) {}

        ~ConnectionLease() {
            if (count_ != nullptr) {
                count_->fetch_sub(1, std::memory_order_relaxed);
            }
        }

    private:
        std::atomic<std::size_t>* count_;
    };

    [[nodiscard]] TlsContextPtr loadTlsContext() const noexcept;
    void storeTlsContext(TlsContextPtr context) noexcept;
    void dispatchControl(std::function<void()> operation);
    // Spawns a detached coroutine that captures Impl. `kind` names it in the
    // failure report its completion makes when the coroutine ends by throwing.
    void spawnTracked(asio::awaitable<void> operation, EdgeTaskKind kind);
    void requestStopOnWorker() noexcept;

    asio::awaitable<void> acceptLoop();
    asio::awaitable<void> handleTlsSession(
        asio::ip::tcp::socket socket,
        TlsContextPtr context,
        ConnectionLease lease);
    template <typename Stream>
    asio::awaitable<void> handleSession(Stream stream, ConnectionLease lease);
    // What one served request reports to the access log. serveRequest starts it
    // at ERROR, and whichever path terminates the request names itself.
    struct RequestOutcome final {
        std::string_view label{"ERROR"};
        std::uint16_t status{0};
    };

    asio::awaitable<bool> serveRequest(
        const EdgeRequest& request,
        ResponseWriter& writer);
    // The uncacheable path: unsafe methods, conditional or authenticated
    // retrievals, and no-store. Nothing here consults or fills the cache.
    asio::awaitable<bool> servePassThrough(
        const EdgeRequest& request,
        ResponseWriter& writer,
        const OriginLease& origin,
        RequestOutcome& outcome);
    template <typename Stream>
    asio::awaitable<bool> handleFramedRequest(
        Stream& stream,
        const ruvia::detail::Http1ServerRequestParseState& parsed,
        std::string_view wireBody,
        std::string_view clientAddress);
    template <typename Stream>
    asio::awaitable<void> handleHttp2Session(Stream stream, std::string clientAddress);

    void wakeInFlight(const std::string& key);
    void recordRequest(const AccessLogEntry& entry) noexcept;
    // The one place a caught exception may end: it reaches the application's
    // taskFailure callback, or stderr when there is none. Never discards.
    // Callable from the disk thread as well as the worker, so it serializes.
    void reportFailure(EdgeTaskKind kind, std::exception_ptr exception) noexcept;
    // Whether an exception is asio unwinding a cancelled coroutine, which is
    // how a task stops on shutdown rather than a failure to report.
    [[nodiscard]] static bool isCancellationUnwind(
        std::exception_ptr exception) noexcept;

    struct RefreshJob final {
        std::string key;
        std::string host;
        std::uint16_t port{0};
        bool https{false};
        std::string target;
        std::optional<std::string> acceptEncoding;
        CacheEntryLease stored;
    };

    asio::awaitable<void> backgroundRefresh(RefreshJob job);

    mutable std::mutex lifecycleMutex_;
    std::condition_variable lifecycleChanged_;
    Lifecycle lifecycle_{Lifecycle::kReady};
    std::thread::id workerThreadId_{};
    std::size_t pendingControls_{0};

    WorkerMemory memory_;
    asio::io_context ioContext_;
    asio::ip::tcp::acceptor acceptor_;
    EdgeEndpoint localEndpoint_;
    bool tlsEnabled_{false};
    asio::steady_timer shutdownSignal_;
    bool shutdownRequestedOnWorker_{false};
    // Every detached coroutine that captures Impl is registered here. Shutdown
    // cancels them and lets io_context drain their completion handlers before
    // the worker exits, so no frame can outlive the members it references.
    std::pmr::vector<std::shared_ptr<asio::cancellation_signal>> activeOperations_;
    TlsContextPtr tlsContext_;

    struct InFlightFetch final {
        std::vector<asio::steady_timer*> waiters;
    };

    EdgeConfig config_;
    EdgeCache cache_;
    DiskTier disk_;
    OriginFetcher fetcher_;
    std::size_t maxCacheableBytes_{8u * 1024u * 1024u};
    // Declared here to match the constructor's initializer order.
    std::optional<std::size_t> maxConnections_;
    // Atomic because stats() reads them from the caller's thread while the
    // worker is updating them. Relaxed throughout: these are counters, and no
    // other state is published through them.
    std::atomic<std::size_t> activeConnections_{0};
    std::atomic<std::size_t> connectionsRefused_{0};
    // One counter per EdgeTaskKind, indexed by its value.
    std::array<std::atomic<std::size_t>, kEdgeTaskKindCount> failureCounts_{};
    std::unordered_map<std::string, InFlightFetch> inFlight_;
    std::function<void(const AccessLogEntry&)> accessLog_;
    std::function<void(const EdgeTaskFailure&)> taskFailure_;
    mutable std::mutex failureMutex_;  // the disk thread reports too
    std::thread worker_;
};

}  // namespace ruvia::edge
