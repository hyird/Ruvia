#pragma once

#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>

#include "ruvia/edge/EdgeTypes.h"

namespace ruvia::edge {

// One completed request. Views are valid only for the callback invocation.
struct AccessLogEntry final {
    std::string_view clientAddress;
    std::string_view method;
    std::string_view host;
    std::string_view target;
    std::uint16_t status{0};
    std::string_view cacheResult;
    std::size_t bytesToClient{0};
};

// Which detached task an exception escaped from. The node keeps serving in
// every case; the failure is reported so it cannot pass unobserved.
enum class EdgeTaskKind : std::uint8_t {
    kAcceptLoop,         // spawning an accepted connection failed
    kSession,            // a client connection's coroutine threw
    kBackgroundRefresh,  // a stale-while-revalidate refresh threw
    kWorker,             // an exception escaped the worker's io_context::run()
    kAccessLog,          // the accessLog callback itself threw
    kDiskCache,          // a queued disk-tier write threw on the disk thread
    kControl,            // a control operation failed; its caller also got false
};

// Number of EdgeTaskKind values, for per-kind counter arrays.
inline constexpr std::size_t kEdgeTaskKindCount = static_cast<std::size_t>(EdgeTaskKind::kControl) + 1;

// A task failure. `exception` is never null and may be rethrown to inspect it.
struct EdgeTaskFailure final {
    EdgeTaskKind kind{EdgeTaskKind::kSession};
    std::exception_ptr exception;
};

// A point-in-time view of the node, for health checks and metrics. Counters are
// cumulative since construction and never reset. Reading them is safe from any
// thread, but the values are sampled independently rather than as one atomic
// snapshot, so treat them as a set of gauges and not as a consistent tuple.
struct EdgeStats final {
    // Client connections held right now, against EdgeServerOptions::maxConnections.
    std::size_t activeConnections{0};
    // Connections closed on accept because that budget was full. A rising
    // count means the node is shedding load rather than queueing it.
    std::size_t connectionsRefused{0};
    // Origin requests answered without dialing because the breaker was open.
    std::size_t originCircuitRejections{0};
    // Cumulative task failures, matching the taskFailure callback's kinds. A
    // node with no failure callback can still be monitored through these.
    std::size_t acceptFailures{0};
    std::size_t sessionFailures{0};
    std::size_t backgroundRefreshFailures{0};
    std::size_t workerFailures{0};
    std::size_t accessLogFailures{0};
    std::size_t diskCacheFailures{0};
    std::size_t controlFailures{0};
};

struct EdgeServerOptions final {
    EdgeCacheLimits cache{};
    OriginFetchLimits fetch{};
    std::size_t maxCacheableBytes{8u * 1024u * 1024u};
    // Concurrent client connections the node will hold. Beyond it, further
    // connections are accepted and immediately closed, which keeps the backlog
    // draining instead of letting the listener queue grow without bound.
    // Absence means the only limit is the process file-descriptor budget --
    // reaching that turns every accept into an error, so prefer a real value on
    // any public listener.
    std::optional<std::size_t> maxConnections{};
    std::optional<EdgeTlsConfig> tls{};
    // Enables the persistent second tier. One live EdgeServer exclusively owns
    // a cache directory; construction rejects concurrent reuse so its LRU and
    // byte accounting cannot diverge from the committed files.
    std::optional<std::filesystem::path> cacheDirectory{};
    std::size_t maxDiskCacheBytes{256u * 1024u * 1024u};
    // Runs on the Edge worker. It must not block and must not destroy the server.
    // An exception is contained and does not change the response or stop the
    // worker; it is reported to taskFailure as kAccessLog.
    std::function<void(const AccessLogEntry&)> accessLog{};
    // Every exception that escapes a detached Edge task is reported here rather
    // than discarded. Runs on the Edge worker under the same rules as accessLog,
    // except kDiskCache (the disk thread) and kControl (the thread that called
    // the control operation); invocations are serialized, so the callback needs
    // no lock of its own. Without a callback the node writes one line per
    // failure to stderr: a task failure is never silent. A throwing callback
    // falls back to that same line.
    std::function<void(const EdgeTaskFailure&)> taskFailure{};
};

// A caching reverse proxy with one owned event-loop thread. The public surface
// contains only runtime-independent values; Asio, TLS objects, protocol writers,
// caches and origin sockets are implementation details behind Impl.
//
// start() may be called once. stop() requests immediate shutdown and joins when
// called off-worker; destruction also stops and joins. Control-plane operations
// are synchronous and safe from any thread: while running they are serialized
// onto the Edge worker, preserving single-owner hot-path state.
class EdgeServer final {
public:
    explicit EdgeServer(EdgeEndpoint endpoint, EdgeServerOptions options = {});
    ~EdgeServer();

    EdgeServer(const EdgeServer&) = delete;
    EdgeServer& operator=(const EdgeServer&) = delete;

    void start();
    void stop();
    void join();

    [[nodiscard]] EdgeEndpoint localEndpoint() const;

    // Safe from any thread, at any point in the lifecycle.
    [[nodiscard]] EdgeStats stats() const;

    [[nodiscard]] bool addOrigin(std::string frontHost, OriginSettings settings);
    [[nodiscard]] bool removeOrigin(std::string_view frontHost);
    [[nodiscard]] bool setTlsCertificate(const EdgeTlsConfig& tls);
    [[nodiscard]] bool purge(std::string_view frontHost, std::string_view target);
    // Clears memory and disk tiers synchronously. Returns false if any
    // committed disk entry could not be removed and therefore remains usable.
    [[nodiscard]] bool clearCache();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ruvia::edge
