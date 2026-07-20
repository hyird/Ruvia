#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
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
#include <asio/thread_pool.hpp>

#include "ruvia/edge/DiskCache.h"
#include "ruvia/edge/EdgeCache.h"
#include "ruvia/edge/EdgeConfig.h"
#include "ruvia/edge/OriginFetcher.h"
#include "ruvia/http/HttpHeader.h"
#include "ruvia/http/HttpKnownMethod.h"

namespace ruvia {
class Http1ParsedRequest;
namespace detail {
class Http2Connection;
class Http2StreamState;
}  // namespace detail
}  // namespace ruvia

namespace ruvia::edge {

// A logical client request, independent of the wire protocol (HTTP/1 or HTTP/2).
// All views are borrowed and must outlive the serve call.
struct EdgeRequest final {
    std::string_view method;
    HttpKnownMethod knownMethod{HttpKnownMethod::kUnknown};
    std::string_view target;
    std::string_view host;
    std::span<const HttpHeaderView> headers;
    std::optional<std::string_view> body;  // already-decoded request body, if any
    std::string_view clientAddress;
    bool keepAlive{true};
};

// Wire adapter the serve core drives to emit a response, implemented once per
// protocol. A response is either one buffered respond(), or respondHead() then
// respondChunk()* then respondEnd(). Each call returns false if the client is
// gone. The serve core supplies fully-curated headers plus the X-Cache label,
// Age and keep-alive intent; the adapter adds protocol framing.
class ResponseWriter {
public:
    ResponseWriter() = default;
    virtual ~ResponseWriter() = default;
    ResponseWriter(const ResponseWriter&) = delete;
    ResponseWriter& operator=(const ResponseWriter&) = delete;

    virtual asio::awaitable<bool> respond(
        std::uint16_t status,
        const std::vector<std::pair<std::string, std::string>>& headers,
        std::string_view body,
        std::string_view cacheResult,
        std::optional<std::uint64_t> age,
        bool omitBody,
        bool keepAlive) = 0;

    virtual asio::awaitable<bool> respondHead(
        std::uint16_t status,
        const std::vector<std::pair<std::string, std::string>>& headers,
        std::string_view cacheResult,
        bool hasBody,
        std::optional<std::size_t> contentLength,
        bool keepAlive) = 0;

    virtual asio::awaitable<bool> respondChunk(std::string_view chunk) = 0;
    virtual asio::awaitable<bool> respondEnd() = 0;

    [[nodiscard]] virtual std::size_t bytesWritten() const = 0;
};

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

struct EdgeServerOptions final {
    EdgeCache::Limits cache{};
    OriginFetcher::Limits fetch{};
    // Largest response body the edge will accumulate to cache. A larger response
    // still streams to the client; it just is not stored.
    std::size_t maxCacheableBytes{8u * 1024u * 1024u};
    // When set, the data listener terminates TLS with this certificate/key.
    std::optional<EdgeTlsConfig> tls{};
    // When set, responses are also cached on disk under this directory as a
    // persistent second tier behind the memory cache: it survives restarts and
    // holds far more than RAM. All disk I/O runs on a dedicated background thread,
    // never on the event loop. Absent by default (memory-only, zero overhead).
    std::optional<std::filesystem::path> cacheDirectory{};
    std::size_t maxDiskCacheBytes{256u * 1024u * 1024u};
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
// This is a library: the control plane is exposed only as these member functions,
// so an embedding application wires up its own management surface (an HTTP admin
// API, a config-file reload, health checks, load balancing) on top of them.
//
// The data listener speaks HTTP/1.1 and, when TLS is terminated (EdgeServerOptions
// ::tls), negotiates HTTP/2 over ALPN -- an h2 client is served by the same cache
// core, one stream at a time. Origins are reached over HTTP or HTTPS per their
// config. Multiplexed h2 concurrency and multi-worker scaling are future work.
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

    // --- Control plane (thread-safe; callable while running) ---

    // Map (or replace) a front-facing Host to an origin. Returns true if this
    // created a new mapping, false if it replaced an existing one.
    bool addOrigin(std::string frontHost, OriginSettings settings);

    // Remove a Host mapping. Returns true if a mapping was removed.
    bool removeOrigin(std::string_view frontHost);

    // Replace the certificate/key used to terminate client TLS, taking effect for
    // new connections. Returns false if TLS was not enabled at startup or the PEM
    // is invalid. Thread-safe; callable while running.
    bool setTlsCertificate(const EdgeTlsConfig& tls);

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
    // Serve one logical request, emitting the response through `writer`. Returns
    // true to keep the connection open (keep-alive), false to close it. This is
    // the protocol-agnostic core shared by the HTTP/1 and HTTP/2 wire adapters.
    asio::awaitable<bool> serveRequest(const EdgeRequest& request, ResponseWriter& writer);

    // Handle one framed HTTP/1 request: build the logical request and drive the
    // serve core with an HTTP/1 response writer over `stream`.
    template <typename Stream>
    asio::awaitable<bool> handleFramedRequest(
        Stream& stream,
        const Http1ParsedRequest& parsed,
        std::string_view clientAddress,
        bool keepAlive);
    // Drive an HTTP/2 connection over `stream`: each completed request runs in its
    // own handler coroutine (true multiplexing), sharing the connection and a
    // single writer coroutine; streamed responses are written incrementally.
    template <typename Stream>
    asio::awaitable<void> handleHttp2Session(Stream stream, std::string clientAddress);


    // Wake every request waiting on an in-flight fetch for `key` and drop the
    // entry (called when the leader's fetch finishes, however it ended).
    void wakeInFlight(const std::string& key);

    // Session accounting for graceful shutdown: a session decrements the live
    // count when it ends, and once a drain is in progress and the last session is
    // gone the event loop is stopped.
    void onSessionFinished();
    void maybeCompleteDrain();

    // Emit the access-log entry for a completed request, if a callback is set.
    void recordRequest(const AccessLogEntry& entry);

    // Everything a background stale-while-revalidate refresh needs, owned so it
    // outlives the request that started it.
    struct RefreshJob final {
        std::string key;
        std::string host;
        std::uint16_t port{0};
        bool https{false};
        std::string target;
        std::string acceptEncoding;  // the variant's normalized Accept-Encoding
        std::shared_ptr<const CachedResponse> stored;
    };

    // Refresh one cache entry off the request path: conditionally re-fetch it and
    // update the cache, registered in the in-flight map so it also serves any
    // waiting foreground misses.
    asio::awaitable<void> backgroundRefresh(RefreshJob job);

    // Disk-tier helpers (no-ops when no cache directory is configured). The
    // lookup runs the blocking read on diskPool_ and resumes on the event loop;
    // the store/purge helpers post fire-and-forget work to diskPool_ so the
    // request path never blocks on disk I/O.
    asio::awaitable<std::shared_ptr<const CachedResponse>> diskLookup(std::string key);
    void diskStore(std::string key, std::shared_ptr<const CachedResponse> entry);
    void diskPurge(std::string key);
    void diskPurgePrefix(std::string prefix);

    asio::io_context ioContext_;
    asio::ip::tcp::acceptor acceptor_;
    // Graceful-shutdown coordination (touched only on the event-loop thread).
    // drainSignal_ (armed to never expire) is cancelled to wake idle keep-alive
    // and HTTP/2 reads so they close; drainDeadline_ bounds the wait.
    asio::steady_timer drainSignal_;
    asio::steady_timer drainDeadline_;
    bool draining_{false};
    int liveSessions_{0};
    // Server TLS context, swappable at runtime (null when TLS is disabled). A new
    // connection loads the current one; in-flight sessions keep their own.
    std::shared_ptr<asio::ssl::context> tlsContext_;
    // One origin fetch in progress for a cache key, with the requests waiting on
    // it (request coalescing / single-flight). The waiter timers are cancelled to
    // wake the followers when the leader's fetch completes.
    struct InFlightFetch final {
        std::vector<asio::steady_timer*> waiters;
    };

    EdgeConfig config_;
    EdgeCache cache_;
    // Optional persistent disk tier and the single background thread all its I/O
    // runs on. Declared after cache_ and before worker_ so, on teardown, the pool
    // (and its in-flight disk tasks) is joined before disk_ is destroyed.
    std::optional<DiskCache> disk_;
    std::optional<asio::thread_pool> diskPool_;
    OriginFetcher fetcher_;
    std::size_t maxCacheableBytes_{8u * 1024u * 1024u};
    std::unordered_map<std::string, InFlightFetch> inFlight_;
    std::function<void(const AccessLogEntry&)> accessLog_;
    std::jthread worker_;
};

}  // namespace ruvia::edge
