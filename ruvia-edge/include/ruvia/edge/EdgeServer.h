#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include <asio/awaitable.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/ssl.hpp>
#include <asio/steady_timer.hpp>

#include "ruvia/edge/EdgeCache.h"
#include "ruvia/edge/EdgeConfig.h"
#include "ruvia/edge/OriginFetcher.h"

namespace ruvia {
class Http1ParsedRequest;
}

namespace ruvia::edge {

// PEM-encoded certificate chain and private key for terminating client TLS.
struct EdgeTlsConfig final {
    std::string certificateChainPem;
    std::string privateKeyPem;
};

// One completed request, handed to the access-log callback. All string views are
// borrowed and valid only for the duration of the callback; copy to retain.
// cacheResult mirrors the X-Cache label (HIT/MISS/REVALIDATED/STALE/BYPASS) or
// ERROR for an edge-generated error or an aborted response.
struct AccessLogEntry final {
    std::string_view clientAddress;
    std::string_view method;
    std::string_view host;
    std::string_view target;
    std::uint16_t status{0};
    std::string_view cacheResult;
    std::size_t bytesToClient{0};
};

// Aggregate counters since start, reported by the /stats management endpoint.
struct EdgeMetrics final {
    std::uint64_t requests{0};
    std::uint64_t hits{0};
    std::uint64_t misses{0};
    std::uint64_t revalidated{0};
    std::uint64_t stale{0};
    std::uint64_t bypass{0};
    std::uint64_t errors{0};
    std::uint64_t bytesToClient{0};
};

struct EdgeServerOptions final {
    EdgeCache::Limits cache{};
    OriginFetcher::Limits fetch{};
    // Largest response body the edge will accumulate to cache. A larger response
    // still streams to the client; it just is not stored.
    std::size_t maxCacheableBytes{8u * 1024u * 1024u};
    // When set, the data listener terminates TLS with this certificate/key.
    std::optional<EdgeTlsConfig> tls{};
    // When set, a separate management listener is bound to this endpoint exposing
    // the runtime control API over HTTP (see the class comment). It is
    // unauthenticated, so bind it to a trusted interface (e.g. loopback) only.
    std::optional<asio::ip::tcp::endpoint> adminEndpoint{};
    // Invoked (on the event-loop thread) once per completed request. Optional.
    std::function<void(const AccessLogEntry&)> accessLog{};
};

// A caching reverse-proxy edge node running its own single-thread event loop.
// It faces clients over plaintext HTTP/1.1, resolves each request's Host against
// a dynamically mutable origin table, serves fresh responses from an in-memory
// LRU cache, and fetches from the mapped origin on a miss (caching the result
// when the response's Cache-Control allows a shared cache to store it).
//
// The origin table and the cache are the control plane: addOrigin/removeOrigin
// and purge/clearCache are safe to call at runtime from any thread while the
// server is running -- the origin table is published copy-on-write and the cache
// is mutex-guarded, so a request in flight never observes a half-applied change.
//
// An optional management listener (EdgeServerOptions::adminEndpoint) exposes the
// same control plane over HTTP for operators and scripts:
//   PUT    /origins/<host>?upstream=<host>&port=<n>   map/replace an origin
//   DELETE /origins/<host>                            remove an origin
//   POST   /purge?host=<host>&target=<path>           drop one cached entry
//   DELETE /cache                                     drop every cached entry
//   GET    /stats                                     cache entry count and bytes
// It is unauthenticated and must be bound to a trusted interface only.
//
// The data listener speaks HTTP/1.1 and can terminate TLS (EdgeServerOptions::
// tls); origins are reached over HTTP or HTTPS per their config. HTTP/2 and
// multi-worker scaling are future work.
class EdgeServer final {
public:
    EdgeServer(const asio::ip::tcp::endpoint& endpoint, EdgeServerOptions options = {});
    ~EdgeServer();

    EdgeServer(const EdgeServer&) = delete;
    EdgeServer& operator=(const EdgeServer&) = delete;

    // Launch the worker thread that runs the event loop. Idempotent-unsafe: call
    // once. localEndpoint() is valid before start() because the acceptor binds in
    // the constructor.
    void start();

    // Stop accepting and unwind the event loop, then join the worker thread.
    void stop();
    void join();

    [[nodiscard]] asio::ip::tcp::endpoint localEndpoint() const;
    // The bound management endpoint, if an adminEndpoint was configured.
    [[nodiscard]] std::optional<asio::ip::tcp::endpoint> localAdminEndpoint() const;

    // --- Control plane (thread-safe; callable while running) ---

    // Map (or replace) a front-facing Host to an origin. Returns true if this
    // created a new mapping, false if it replaced an existing one.
    bool addOrigin(std::string frontHost, OriginSettings settings);

    // Remove a Host mapping. Returns true if a mapping was removed.
    bool removeOrigin(std::string_view frontHost);

    // Drop the cached GET response for one Host+target. Returns true if an entry
    // was removed.
    bool purge(std::string_view frontHost, std::string_view target);

    // Drop every cached response.
    void clearCache();

private:
    [[nodiscard]] static std::string cacheKey(
        std::string_view method,
        std::string_view frontHost,
        std::string_view target);

    asio::awaitable<void> acceptLoop();
    // Terminate TLS on an accepted socket, then run the session over it.
    asio::awaitable<void> handleTlsSession(asio::ip::tcp::socket socket);
    // Run the keep-alive session over any stream (plain TCP or TLS).
    template <typename Stream>
    asio::awaitable<void> handleSession(Stream stream);
    // Handle one framed request. Returns true to keep the connection open for a
    // next request (keep-alive), false to close it.
    template <typename Stream>
    asio::awaitable<bool> handleFramedRequest(
        Stream& stream,
        const Http1ParsedRequest& parsed,
        std::string_view clientAddress,
        bool keepAlive);
    asio::awaitable<void> adminAcceptLoop();
    asio::awaitable<void> handleAdminSession(asio::ip::tcp::socket socket);

    // Wake every request waiting on an in-flight fetch for `key` and drop the
    // entry (called when the leader's fetch finishes, however it ended).
    void wakeInFlight(const std::string& key);

    // Count a completed request and emit its access-log entry.
    void recordRequest(const AccessLogEntry& entry);

    asio::io_context ioContext_;
    asio::ip::tcp::acceptor acceptor_;
    std::optional<asio::ssl::context> tlsContext_;
    std::optional<asio::ip::tcp::acceptor> adminAcceptor_;
    // One origin fetch in progress for a cache key, with the requests waiting on
    // it (request coalescing / single-flight). The waiter timers are cancelled to
    // wake the followers when the leader's fetch completes.
    struct InFlightFetch final {
        std::vector<asio::steady_timer*> waiters;
    };

    EdgeConfig config_;
    EdgeCache cache_;
    OriginFetcher fetcher_;
    std::size_t maxCacheableBytes_{8u * 1024u * 1024u};
    std::unordered_map<std::string, InFlightFetch> inFlight_;
    std::function<void(const AccessLogEntry&)> accessLog_;
    EdgeMetrics metrics_;
    std::jthread worker_;
};

}  // namespace ruvia::edge
