#include "ruvia/edge/EdgeServer.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <ctime>
#include <future>
#include <limits>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include <asio/as_tuple.hpp>
#include <asio/bind_cancellation_slot.hpp>
#include <asio/buffer.hpp>
#include <asio/cancellation_signal.hpp>
#include <asio/cancellation_type.hpp>
#include <asio/co_spawn.hpp>
#include <asio/experimental/awaitable_operators.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/post.hpp>
#include <asio/ssl.hpp>
#include <asio/steady_timer.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/write.hpp>

#include <openssl/ssl.h>  // negotiated ALPN read-back

#include "ruvia/core/memory/MemoryPool.h"
#include "ruvia/edge/detail/EdgeByteRange.h"
#include "ruvia/edge/detail/EdgeCache.h"
#include "ruvia/edge/detail/EdgeConfig.h"
#include "ruvia/edge/detail/EdgeDiskTier.h"
#include "ruvia/edge/detail/EdgeFreshness.h"
#include "ruvia/edge/detail/EdgeHeaderRules.h"
#include "ruvia/edge/detail/EdgeHttp1ResponseWriter.h"
#include "ruvia/edge/detail/EdgeHttp1Wire.h"
#include "ruvia/edge/detail/EdgeHttp2ResponseWriter.h"
#include "ruvia/edge/detail/EdgeResponseWriter.h"
#include "ruvia/edge/detail/EdgeTlsContext.h"
#include "ruvia/edge/detail/OriginFetcher.h"
#include "ruvia/http/HttpCache.h"
#include "ruvia/http/HttpHeader.h"
#include "ruvia/http/HttpKnownMethod.h"
#include "ruvia/http/HttpRequest.h"
#include "ruvia/http/HttpStatus.h"
#include "ruvia/http/ProtocolByteLimit.h"
#include "ruvia/http/detail/HttpRequestInternal.h"
#include "ruvia/http/detail/http1/Http1ChunkedBodyDecoder.h"
#include "ruvia/http/detail/http1/Http1ServerRequestParser.h"
#include "ruvia/http/detail/http2/Http2Connection.h"
#include "ruvia/http/detail/http2/Http2Event.h"
#include "ruvia/http/detail/http2/Http2RequestBuilder.h"

namespace ruvia::edge {

namespace {

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

[[nodiscard]] asio::ip::tcp::endpoint toAsioEndpoint(const EdgeEndpoint& endpoint) {
    return {asio::ip::make_address(endpoint.address), endpoint.port};
}

// Upper bound on a whole buffered client request (head plus any forwarded body).
constexpr std::size_t kMaxRequestBytes = 1u * 1024u * 1024u;

// How long a persistent client connection may sit idle awaiting its next request.
constexpr std::chrono::seconds kKeepAliveIdleTimeout{60};

}  // namespace

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

    [[nodiscard]] static std::string cacheVariantPrefix(
        std::string_view method,
        std::string_view frontHost,
        std::string_view target);

    [[nodiscard]] TlsContextPtr loadTlsContext() const noexcept;
    void storeTlsContext(TlsContextPtr context) noexcept;
    void dispatchControl(std::function<void()> operation);
    void spawnTracked(asio::awaitable<void> operation);
    void requestStopOnWorker() noexcept;

    asio::awaitable<void> acceptLoop();
    asio::awaitable<void> handleTlsSession(
        asio::ip::tcp::socket socket,
        TlsContextPtr context);
    template <typename Stream>
    asio::awaitable<void> handleSession(Stream stream);
    asio::awaitable<bool> serveRequest(
        const EdgeRequest& request,
        ResponseWriter& writer);
    template <typename Stream>
    asio::awaitable<bool> handleFramedRequest(
        Stream& stream,
        const detail::Http1ServerRequestParseState& parsed,
        std::string_view wireBody,
        std::string_view clientAddress);
    template <typename Stream>
    asio::awaitable<void> handleHttp2Session(Stream stream, std::string clientAddress);

    void wakeInFlight(const std::string& key);
    void recordRequest(const AccessLogEntry& entry) noexcept;

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
    EdgeDiskTier disk_;
    OriginFetcher fetcher_;
    std::size_t maxCacheableBytes_{8u * 1024u * 1024u};
    std::unordered_map<std::string, InFlightFetch> inFlight_;
    std::function<void(const AccessLogEntry&)> accessLog_;
    std::thread worker_;
};

EdgeServer::Impl::Impl(EdgeEndpoint endpoint, EdgeServerOptions options)
    : acceptor_(ioContext_, toAsioEndpoint(endpoint)),
      shutdownSignal_(ioContext_),
      activeOperations_(memory_.resource()),
      config_(memory_.resource()),
      cache_(options.cache, memory_.resource()),
      disk_(options.cacheDirectory, options.maxDiskCacheBytes),
      fetcher_(options.fetch),
      maxCacheableBytes_(options.maxCacheableBytes),
      accessLog_(std::move(options.accessLog)) {
    const auto bound = acceptor_.local_endpoint();
    localEndpoint_ = EdgeEndpoint{bound.address().to_string(), bound.port()};
    shutdownSignal_.expires_at((std::chrono::steady_clock::time_point::max)());
    if (options.tls) {
        tlsEnabled_ = true;
        storeTlsContext(buildServerTlsContext(*options.tls));
    }
}

EdgeServer::Impl::~Impl() {
    {
        const std::lock_guard lock(lifecycleMutex_);
        if (workerThreadId_ == std::this_thread::get_id()) {
            std::terminate();
        }
    }
    stop();
}

void EdgeServer::Impl::start() {
    std::unique_lock lock(lifecycleMutex_);
    if (lifecycle_ != Lifecycle::kReady) {
        throw std::logic_error("EdgeServer::start() may be called only once");
    }

    std::promise<std::thread::id> identityPromise;
    auto identity = identityPromise.get_future();
    std::promise<void> runGatePromise;
    auto runGate = runGatePromise.get_future();

    worker_ = std::thread(
        [this, identityPromise = std::move(identityPromise),
         runGate = std::move(runGate)]() mutable {
            identityPromise.set_value(std::this_thread::get_id());
            runGate.wait();
            ioContext_.run();
            const std::lock_guard finishedLock(lifecycleMutex_);
            workerThreadId_ = {};
            if (lifecycle_ != Lifecycle::kReady) {
                lifecycle_ = Lifecycle::kStopped;
            }
            lifecycleChanged_.notify_all();
        });
    workerThreadId_ = identity.get();

    try {
        spawnTracked(acceptLoop());
        lifecycle_ = Lifecycle::kRunning;
        runGatePromise.set_value();
    } catch (...) {
        ioContext_.stop();
        runGatePromise.set_value();
        std::thread failed = std::move(worker_);
        lock.unlock();
        failed.join();
        ioContext_.restart();
        throw;
    }
}

void EdgeServer::Impl::requestStopOnWorker() noexcept {
    shutdownRequestedOnWorker_ = true;
    asio::error_code ignore;
    acceptor_.close(ignore);
    shutdownSignal_.cancel(ignore);
    // Moving the registry is allocation-free. Completion callbacks erase from
    // the now-empty live registry, so even inline cancellation cannot invalidate
    // this traversal; each callback itself retains its signal until completion.
    auto operations = std::move(activeOperations_);
    for (const auto& operation : operations) {
        operation->emit(asio::cancellation_type::terminal);
    }
    // Do not call io_context::stop(): it abandons ready completions and keeps
    // their coroutine frames until io_context destruction, after several Impl
    // members captured by those frames have already been destroyed. Closing and
    // cancelling all roots lets run() return only after structured teardown.
}

void EdgeServer::Impl::stop() {
    std::thread worker;
    {
        std::unique_lock lock(lifecycleMutex_);
        for (;;) {
            const bool onWorker =
                workerThreadId_ == std::this_thread::get_id();

            if (lifecycle_ == Lifecycle::kReady) {
                asio::error_code ignore;
                acceptor_.close(ignore);
                lifecycle_ = Lifecycle::kStopped;
                lifecycleChanged_.notify_all();
                break;
            }
            if (lifecycle_ == Lifecycle::kRunning) {
                lifecycle_ = Lifecycle::kStopping;
                if (onWorker) {
                    lock.unlock();
                    requestStopOnWorker();
                    return;
                }
                lifecycleChanged_.wait(
                    lock, [this] { return pendingControls_ == 0; });
                try {
                    asio::post(ioContext_, [this] { requestStopOnWorker(); });
                } catch (...) {
                    // No stop handler was published. Restore the only state in
                    // which another caller can retry, and wake stop/control
                    // waiters so none remains parked behind a phantom owner.
                    lifecycle_ = Lifecycle::kRunning;
                    lifecycleChanged_.notify_all();
                    throw;
                }
                break;
            }
            if (lifecycle_ == Lifecycle::kStopping) {
                if (onWorker) {
                    return;
                }
                lifecycleChanged_.wait(lock, [this] {
                    return lifecycle_ != Lifecycle::kStopping;
                });
                // A failed publication rolls back to Running. Compete to issue
                // the stop request again; a successful one reaches Stopped.
                continue;
            }
            break;  // kStopped
        }

        if (worker_.joinable()) {
            worker = std::move(worker_);
        }
    }
    if (worker.joinable()) {
        worker.join();
    }
    disk_.stop();
}

void EdgeServer::Impl::join() {
    std::thread worker;
    {
        std::unique_lock lock(lifecycleMutex_);
        if (workerThreadId_ == std::this_thread::get_id()) {
            throw std::logic_error("EdgeServer::join() cannot join its worker thread");
        }
        if (lifecycle_ == Lifecycle::kReady) {
            // Joining before start is a no-op. In particular, do not permanently
            // stop the optional disk executor that a later start() will use.
            return;
        }
        lifecycleChanged_.wait(
            lock, [this] { return lifecycle_ == Lifecycle::kStopped; });
        if (worker_.joinable()) {
            worker = std::move(worker_);
        }
    }
    if (worker.joinable()) {
        worker.join();
    }
    disk_.stop();
}

EdgeEndpoint EdgeServer::Impl::localEndpoint() const {
    return localEndpoint_;
}

EdgeServer::Impl::TlsContextPtr EdgeServer::Impl::loadTlsContext() const noexcept {
    return tlsContext_;
}

void EdgeServer::Impl::storeTlsContext(TlsContextPtr context) noexcept {
    tlsContext_ = std::move(context);
}

void EdgeServer::Impl::spawnTracked(asio::awaitable<void> operation) {
    auto cancellation = std::make_shared<asio::cancellation_signal>();
    activeOperations_.push_back(cancellation);
    try {
        asio::co_spawn(
            ioContext_,
            std::move(operation),
            asio::bind_cancellation_slot(
                cancellation->slot(),
                [this, cancellation](std::exception_ptr) noexcept {
                    std::erase(activeOperations_, cancellation);
                }));
        if (shutdownRequestedOnWorker_) {
            cancellation->emit(asio::cancellation_type::terminal);
        }
    } catch (...) {
        std::erase(activeOperations_, cancellation);
        throw;
    }
}

void EdgeServer::Impl::dispatchControl(std::function<void()> operation) {
    std::unique_lock lock(lifecycleMutex_);
    for (;;) {
        if (workerThreadId_ == std::this_thread::get_id()) {
            lock.unlock();
            operation();
            return;
        }

        if (lifecycle_ == Lifecycle::kReady ||
            lifecycle_ == Lifecycle::kStopped) {
            // There is no owner thread in these states, so the lifecycle mutex
            // is the temporary owner and serializes embedding threads.
            operation();
            return;
        }

        if (lifecycle_ == Lifecycle::kStopping) {
            lifecycleChanged_.wait(lock, [this] {
                return lifecycle_ != Lifecycle::kStopping;
            });
            // Stop publication may have rolled back. Re-evaluate ownership and
            // either post to the live worker or run after it has stopped.
            continue;
        }
        break;  // kRunning
    }

    ++pendingControls_;
    try {
        asio::post(ioContext_, [this, operation = std::move(operation)]() mutable {
            operation();
            const std::lock_guard completedLock(lifecycleMutex_);
            --pendingControls_;
            lifecycleChanged_.notify_all();
        });
    } catch (...) {
        --pendingControls_;
        lifecycleChanged_.notify_all();
        throw;
    }
}

bool EdgeServer::Impl::addOrigin(std::string frontHost, OriginSettings settings) {
    auto task = std::make_shared<std::packaged_task<bool()>>(
        [this, frontHost = std::move(frontHost), settings = std::move(settings)]() mutable {
            return config_.addOrigin(std::move(frontHost), std::move(settings));
        });
    auto result = task->get_future();
    dispatchControl([task = std::move(task)] { (*task)(); });
    return result.get();
}

bool EdgeServer::Impl::removeOrigin(std::string_view frontHost) {
    auto task = std::make_shared<std::packaged_task<bool()>>(
        [this, frontHost = std::string(frontHost)] {
            return config_.removeOrigin(frontHost);
        });
    auto result = task->get_future();
    dispatchControl([task = std::move(task)] { (*task)(); });
    return result.get();
}

bool EdgeServer::Impl::setTlsCertificate(const EdgeTlsConfig& tls) {
    if (!tlsEnabled_) {
        return false;  // TLS was not enabled at startup; the listener is plaintext
    }
    try {
        auto context = buildServerTlsContext(tls);
        auto task = std::make_shared<std::packaged_task<void()>>(
            [this, context = std::move(context)]() mutable {
                storeTlsContext(std::move(context));
            });
        auto result = task->get_future();
        dispatchControl([task = std::move(task)] { (*task)(); });
        result.get();
    } catch (...) {
        return false;  // invalid PEM
    }
    return true;
}

bool EdgeServer::Impl::purge(std::string_view frontHost, std::string_view target) {
    // Remove every cached variant of the URL, not just one encoding.
    const std::string prefix = cacheVariantPrefix("GET", frontHost, target);
    auto task = std::make_shared<std::packaged_task<bool()>>(
        [this, prefix] { return cache_.purgePrefix(prefix) > 0; });
    auto memoryResult = task->get_future();
    dispatchControl([task = std::move(task)] { (*task)(); });
    const bool removed = memoryResult.get();
    if (disk_.enabled()) {
        const auto diskResult = disk_.purgePrefixSync(prefix);
        return diskResult.complete && (diskResult.removed > 0 || removed);
    }
    return removed;
}

bool EdgeServer::Impl::clearCache() {
    auto task = std::make_shared<std::packaged_task<void()>>(
        [this] { cache_.clear(); });
    auto memoryResult = task->get_future();
    dispatchControl([task = std::move(task)] { (*task)(); });
    memoryResult.get();
    if (disk_.enabled()) {
        return disk_.clearSync();
    }
    return true;
}

void EdgeServer::Impl::recordRequest(const AccessLogEntry& entry) noexcept {
    if (accessLog_) {
        try {
            accessLog_(entry);
        } catch (...) {
            // Observability is not part of response correctness. In particular,
            // RequestRecord invokes this from its destructor, whose implicit
            // noexcept contract must never turn a user callback into process
            // termination.
        }
    }
}

void EdgeServer::Impl::wakeInFlight(const std::string& key) {
    const auto it = inFlight_.find(key);
    if (it == inFlight_.end()) {
        return;
    }
    for (auto* waiter : it->second.waiters) {
        waiter->cancel();
    }
    inFlight_.erase(it);
}

std::string EdgeServer::Impl::cacheVariantPrefix(
    std::string_view method,
    std::string_view frontHost,
    std::string_view target) {
    std::string key;
    key.reserve(method.size() + frontHost.size() + target.size() + 3);
    key.append(method);
    key.push_back('\n');
    for (const char c : frontHost) {
        key.push_back(toLowerAscii(c));
    }
    key.push_back('\n');
    key.append(target);
    // This terminal delimiter makes the prefix identify exactly one URI. A
    // prefix for `/a` must never invalidate `/ab`.
    key.push_back('\n');
    return key;
}

asio::awaitable<void> EdgeServer::Impl::acceptLoop() {
    for (;;) {
        auto [ec, socket] =
            co_await acceptor_.async_accept(asio::as_tuple(asio::use_awaitable));
        if (ec) {
            if (ec == asio::error::operation_aborted) {
                break;
            }
            continue;
        }
        auto tlsContext = loadTlsContext();
        if (tlsContext != nullptr) {
            spawnTracked(
                handleTlsSession(std::move(socket), std::move(tlsContext)));
        } else {
            spawnTracked(handleSession(std::move(socket)));
        }
    }
}

asio::awaitable<void> EdgeServer::Impl::handleTlsSession(
    asio::ip::tcp::socket socket,
    TlsContextPtr context) {
    // The accept loop pins the context for this session's lifetime; a runtime
    // rotation only affects connections accepted afterward.
    asio::ssl::stream<asio::ip::tcp::socket> stream(std::move(socket), *context);
    auto [ec] = co_await stream.async_handshake(
        asio::ssl::stream_base::server, asio::as_tuple(asio::use_awaitable));
    if (ec) {
        asio::error_code ignore;
        stream.lowest_layer().close(ignore);
        co_return;
    }

    // Dispatch to HTTP/2 when ALPN negotiated it, otherwise HTTP/1.1.
    const unsigned char* protocol = nullptr;
    unsigned int protocolLength = 0;
    SSL_get0_alpn_selected(stream.native_handle(), &protocol, &protocolLength);
    if (protocolLength == 2 && protocol != nullptr && std::memcmp(protocol, "h2", 2) == 0) {
        std::string clientAddress;
        asio::error_code addressError;
        const auto remote = stream.lowest_layer().remote_endpoint(addressError);
        if (!addressError) {
            clientAddress = remote.address().to_string();
        }
        co_await handleHttp2Session(std::move(stream), std::move(clientAddress));
        co_return;
    }
    co_await handleSession(std::move(stream));
}

template <typename Stream>
asio::awaitable<void> EdgeServer::Impl::handleSession(Stream stream) {
    using namespace asio::experimental::awaitable_operators;

    std::string clientAddress;
    {
        asio::error_code ec;
        const auto remote = stream.lowest_layer().remote_endpoint(ec);
        if (!ec) {
            clientAddress = remote.address().to_string();
        }
    }

    const auto tuple = asio::as_tuple(asio::use_awaitable);
    const auto writeStatus = [&stream, tuple](std::string wire) -> asio::awaitable<void> {
        co_await asio::async_write(stream, asio::buffer(wire.data(), wire.size()), tuple);
    };

    std::string inbound;
    std::array<char, 8192> buffer;
    const detail::Http1ServerRequestParser parser;
    asio::steady_timer idleTimer(ioContext_);

    // Serve requests on this connection until one closes it, the client goes
    // away, or the connection sits idle past the keep-alive timeout.
    bool keepGoing = true;
    while (keepGoing) {
        std::size_t consumed = 0;
        bool framed = false;
        for (;;) {
            auto parseState = parser.parseMessage(inbound);
            if (const auto* failure = parseState.failure()) {
                const auto protocolError = failure->protocolError();
                co_await writeStatus(buildStatusWire(
                    protocolError.status().value(),
                    parseState.connectionPlan.protocolVersion()));
                keepGoing = false;
                break;
            }
            const auto* needBody = parseState.needRequestBody();
            const auto* ready = parseState.messageReady();
            const bool exceedsEdgeRequestLimit =
                (needBody != nullptr &&
                 needBody->requiredTotalBytes().has_value() &&
                 *needBody->requiredTotalBytes() > kMaxRequestBytes) ||
                (ready != nullptr &&
                 ready->messageBytes() > kMaxRequestBytes);
            if (exceedsEdgeRequestLimit) {
                // Apply the edge product's tighter buffered-request policy to
                // parser metadata before waiting for or dispatching the body.
                // Checking only inbound.size() after messageReady lets the read
                // that completes an oversized request jump over the limit.
                co_await writeStatus(buildStatusWire(
                    413, parseState.connectionPlan.protocolVersion()));
                keepGoing = false;
                break;
            }
            if (ready != nullptr) {
                consumed = ready->messageBytes();
                const auto wireBody = std::string_view(inbound).substr(
                    ready->headerBytes(),
                    ready->messageBytes() - ready->headerBytes());
                keepGoing = co_await handleFramedRequest(
                    stream, parseState, wireBody, clientAddress);
                framed = true;
                break;
            }
            if (inbound.size() > kMaxRequestBytes) {
                co_await writeStatus(buildStatusWire(
                    413, parseState.connectionPlan.protocolVersion()));
                keepGoing = false;
                break;
            }
            idleTimer.expires_after(kKeepAliveIdleTimeout);
            auto raced = co_await (
                stream.async_read_some(asio::buffer(buffer), tuple) ||
                idleTimer.async_wait(tuple) ||
                shutdownSignal_.async_wait(tuple));
            if (raced.index() == 1) {
                keepGoing = false;  // idle too long
                break;
            }
            if (raced.index() == 2) {
                keepGoing = false;  // direct server shutdown
                break;
            }
            auto& [ec, n] = std::get<0>(raced);
            if (n > 0) {
                inbound.append(buffer.data(), n);
            }
            if (ec) {
                keepGoing = false;  // client closed or read error
                break;
            }
        }
        if (framed) {
            inbound.erase(0, consumed);  // keep any pipelined bytes for the next request
        }
    }

    asio::error_code ignore;
    stream.lowest_layer().shutdown(asio::ip::tcp::socket::shutdown_both, ignore);
}

template <typename Stream>
asio::awaitable<bool> EdgeServer::Impl::handleFramedRequest(
    Stream& stream,
    const detail::Http1ServerRequestParseState& parsed,
    std::string_view wireBody,
    std::string_view clientAddress) {
    const auto& request = parsed.request;
    Http1ResponseWriter<Stream> writer(stream, parsed.connectionPlan);

    EdgeRequest edgeRequest;
    edgeRequest.method = request.method();
    edgeRequest.knownMethod = request.knownMethod();
    edgeRequest.target = request.target();
    edgeRequest.host = request.header("host").value_or("");
    edgeRequest.headers = request.headers();
    edgeRequest.clientAddress = clientAddress;
    edgeRequest.keepAlive =
        parsed.connectionPlan.disposition() ==
        detail::Http1ConnectionDisposition::kReuse;

    // Read and decode the request body for methods that carry one, so the serve
    // core can forward it. `decodedBody` backs edgeRequest.body across the serve.
    std::string decodedBody;
    if (edgeRequest.knownMethod != HttpKnownMethod::kGet &&
        edgeRequest.knownMethod != HttpKnownMethod::kHead) {
        const auto& bodyPlan = parsed.bodyPlan;
        if (bodyPlan.knownLength() != nullptr) {
            edgeRequest.body = wireBody;
        } else if (bodyPlan.chunked() != nullptr) {
            ruvia::detail::Http1ChunkedBodyDecoder decoder(
                ProtocolByteLimit::limited(kMaxRequestBytes));
            std::string chunkBuffer(wireBody);
            bool decodeOk = true;
            for (;;) {
                const auto decoded = decoder.decode(chunkBuffer);
                if (decoded.failure() != nullptr) {
                    decodeOk = false;
                    break;
                }
                if (const auto* chunk = decoded.bodyChunk()) {
                    decodedBody.append(chunk->bytes());
                    chunkBuffer.erase(0, decoded.consumedBytes());
                    continue;
                }
                if (decoded.complete() != nullptr) {
                    break;
                }
                decodeOk = false;  // need-more is impossible: message is complete
                break;
            }
            if (!decodeOk) {
                const std::vector<std::pair<std::string, std::string>> noHeaders;
                co_await writer.respond(400, noHeaders, {}, "ERROR", std::nullopt, false, false);
                co_return false;
            }
            edgeRequest.body = decodedBody;
        }
    }

    const bool continueServing = co_await serveRequest(edgeRequest, writer);
    co_return continueServing && writer.connectionReusable();
}

asio::awaitable<bool> EdgeServer::Impl::serveRequest(
    const EdgeRequest& request, ResponseWriter& writer) {
    // Per-request accounting: defaults to an error result; success paths set the
    // label/status below, and the byte count comes from the writer.
    std::string_view resultLabel = "ERROR";
    std::uint16_t recordedStatus = 0;
    const Headers noHeaders;

    const bool isGet = request.knownMethod == HttpKnownMethod::kGet;
    const bool isHead = request.knownMethod == HttpKnownMethod::kHead;
    const bool requestHasAuthorization =
        findRequestHeader(request.headers, "authorization").has_value();
    const bool requestHasCondition =
        findRequestHeader(request.headers, "if-match").has_value() ||
        findRequestHeader(request.headers, "if-none-match").has_value() ||
        findRequestHeader(request.headers, "if-modified-since").has_value() ||
        findRequestHeader(request.headers, "if-unmodified-since").has_value() ||
        findRequestHeader(request.headers, "if-range").has_value();
    CacheControlFieldParser requestCacheControlParser;
    bool hasRequestCacheControl = false;
    for (const auto& field : request.headers) {
        if (iequals(field.name(), "cache-control")) {
            hasRequestCacheControl = true;
            requestCacheControlParser.update(field.value());
        }
    }
    const CacheControl requestCacheControl = requestCacheControlParser.finish();
    bool legacyPragmaNoCache = false;
    if (!hasRequestCacheControl) {
        for (const auto& field : request.headers) {
            if (iequals(field.name(), "pragma") &&
                detail::httpHasToken(field.value(), "no-cache")) {
                legacyPragmaNoCache = true;
                break;
            }
        }
    }
    // The edge currently chooses not to calculate request-specific freshness
    // constraints. Forwarding is conservative and preserves the client's
    // preference; max-stale merely widens what the client accepts and needs no
    // forced validation.
    const bool requestForcesValidation =
        requestCacheControl.noCache || requestCacheControl.maxAge.has_value() ||
        requestCacheControl.minFresh.has_value() || legacyPragmaNoCache;
    const std::string_view frontHost = hostWithoutPort(request.host);
    const std::string_view target = request.target;
    const bool keepAlive = request.keepAlive;

    struct RequestRecord final {
        Impl* self;
        const EdgeRequest* request;
        const ResponseWriter* writer;
        const std::string_view* result;
        const std::uint16_t* status;
        ~RequestRecord() noexcept {
            self->recordRequest(AccessLogEntry{
                request->clientAddress, request->method, request->host, request->target,
                *status, *result, writer->bytesWritten()});
        }
    };
    const RequestRecord record{this, &request, &writer, &resultLabel, &recordedStatus};
    (void)record;

        // 3. Resolve one stable owner-thread lease. It remains valid if a later
        // control operation replaces or removes the mapping while this request
        // is suspended in origin I/O.
        auto origin = config_.findOrigin(frontHost);
        if (!origin) {
            co_await writer.respond(502, noHeaders, {}, "ERROR", std::nullopt, false, false);
            co_return false;
        }

        // Unsafe methods always write through. This MVP also conservatively
        // forwards conditional and authenticated retrievals instead of trying
        // to evaluate client validators or authenticated reuse locally; caching
        // is optional, while changing either request's semantics is forbidden.
        const bool cacheBypassMethod = !isGet && !isHead;
        const bool unsafeMethod = !isHttpMethodSafe(request.method);
        const bool cannotUseStoredResponse =
            cacheBypassMethod || requestHasCondition || requestHasAuthorization ||
            requestForcesValidation;
        if (requestCacheControl.onlyIfCached && cannotUseStoredResponse) {
            recordedStatus = 504;
            resultLabel = "MISS";
            co_return co_await writer.respond(
                       504, noHeaders, {}, "MISS", std::nullopt, false, keepAlive) &&
                keepAlive;
        }
        if (cannotUseStoredResponse ||
            (requestCacheControl.noStore && !requestCacheControl.onlyIfCached)) {
            std::pmr::vector<HttpHeaderView> passHeaders(memory_.resource());
            for (const auto& field : request.headers) {
                std::pmr::string lower(memory_.resource());
                lower.reserve(field.name().size());
                for (const char c : field.name()) {
                    lower.push_back(toLowerAscii(c));
                }
                if (isConnectionOrFramingField(lower) ||
                    connectionNominates(request.headers, field.name()) ||
                    lower == "host" ||
                    lower == "via" || lower == "forwarded" ||
                    lower.starts_with("x-forwarded-")) {
                    continue;
                }
                passHeaders.push_back(field);
            }
            if (!request.clientAddress.empty()) {
                passHeaders.emplace_back(
                    std::string_view("X-Forwarded-For"),
                    std::string_view(request.clientAddress));
            }
            if (!request.host.empty()) {
                passHeaders.emplace_back(
                    std::string_view("X-Forwarded-Host"), request.host);
            }
            passHeaders.emplace_back(
                std::string_view("X-Forwarded-Proto"),
                tlsEnabled_ ? std::string_view("https") : std::string_view("http"));
            passHeaders.emplace_back(
                std::string_view("Via"), std::string_view("1.1 ruvia-edge"));

            OriginRequest passRequest;
            passRequest.method = request.method;
            passRequest.target = target;
            passRequest.headers = passHeaders;
            passRequest.body = request.body;

            // Stream the origin response straight through to the client (never
            // cached); the writer re-frames an unknown length as chunked.
            std::uint16_t passStatus = 0;
            bool passHeadSent = false;
            bool passAborted = false;
            ResponseSink passSink;
            passSink.onHead = [&](const OriginResponseHead& head) -> asio::awaitable<bool> {
                passStatus = head.status;
                const Headers responseHeaders =
                    endToEndResponseHeaders(head.headers);
                if (!co_await writer.respondHead(head.status, responseHeaders, "BYPASS",
                                                 head.hasBody, head.contentLength, keepAlive)) {
                    passAborted = true;
                    co_return false;
                }
                passHeadSent = true;
                co_return true;
            };
            passSink.onBody = [&](std::string_view chunk) -> asio::awaitable<bool> {
                if (!co_await writer.respondChunk(chunk)) {
                    passAborted = true;
                    co_return false;
                }
                co_return true;
            };

            auto passStream = co_await fetcher_.fetch(
                ioContext_.get_executor(), origin->upstreamHost, origin->upstreamPort,
                origin->https, passRequest, passSink);
            if (passAborted) {
                co_return false;
            }
            if (passStream.outcome != OriginFetchOutcome::kOk) {
                if (passHeadSent) {
                    co_return false;  // partial response already sent
                }
                const std::uint16_t gatewayStatus =
                    passStream.outcome == OriginFetchOutcome::kTimeout ? 504 : 502;
                recordedStatus = gatewayStatus;
                co_await writer.respond(
                    gatewayStatus, noHeaders, {}, "ERROR", std::nullopt, false, false);
                co_return false;
            }
            if (!co_await writer.respondEnd()) {
                co_return false;
            }
            // A successful unsafe method invalidates every cached variant of this
            // URI (RFC 9111 section 4.4).
            if (unsafeMethod && passStatus < 400) {
                cache_.purgePrefix(cacheVariantPrefix("GET", frontHost, target));
                disk_.purgePrefix(cacheVariantPrefix("GET", frontHost, target));
            }
            resultLabel = "BYPASS";
            recordedStatus = passStatus;
            co_return keepAlive;
        }

        std::time_t now = std::time(nullptr);
        // Preserve the complete Accept-Encoding field value. Dropping weights,
        // repeated lines, or the absent-vs-empty distinction can make a shared
        // cache serve a representation selected for a different request.
        const std::string variantPrefix =
            cacheVariantPrefix("GET", frontHost, target);
        const auto acceptEncoding = combinedRequestFieldValue(
            request.headers, "accept-encoding");
        std::string key = variantPrefix;
        // The primary cache key includes the complete request authority. The
        // mapping host deliberately ignores a port for routing, but two target
        // URIs with different ports are not the same cache key. ASCII case is
        // canonicalized because URI hosts are case-insensitive.
        for (const char byte : request.host) {
            key.push_back(toLowerAscii(byte));
        }
        key.push_back('\n');
        key.push_back(acceptEncoding ? '1' : '0');
        if (acceptEncoding) {
            key.append(*acceptEncoding);
        }

        // Serve a cached entry, honoring a single client byte-range (206, or 416
        // when unsatisfiable) served from the full cached body.
        const auto serveHit = [&](const CachedResponse& entry) -> asio::awaitable<bool> {
            resultLabel = "HIT";
            const auto age = cachedResponseAge(entry, now);
            if (!isHead) {
                if (const auto rangeHeader = findRequestHeader(request.headers, "range")) {
                    const auto range = parseSingleByteRange(*rangeHeader, entry.body.size());
                    if (range.unsatisfiable) {
                        recordedStatus = 416;
                        Headers headers;
                        headers.emplace_back(
                            "Content-Range", "bytes */" + std::to_string(entry.body.size()));
                        co_return co_await writer.respond(
                                   416, headers, {}, "HIT", std::nullopt, false, keepAlive) &&
                            keepAlive;
                    }
                    if (range.satisfiable) {
                        recordedStatus = 206;
                        Headers headers = entry.headers;
                        headers.emplace_back(
                            "Content-Range",
                            "bytes " + std::to_string(range.start) + "-" +
                                std::to_string(range.end) + "/" +
                                std::to_string(entry.body.size()));
                        const std::string_view slice = std::string_view(entry.body).substr(
                            range.start, range.end - range.start + 1);
                        co_return co_await writer.respond(
                                   206, headers, slice, "HIT", age, false, keepAlive) &&
                            keepAlive;
                    }
                }
            }
            recordedStatus = entry.status;
            co_return co_await writer.respond(
                       entry.status, entry.headers, entry.body, "HIT", age, isHead, keepAlive) &&
                keepAlive;
        };

        // 4. Serve a fresh cache hit without touching the origin.
        auto hit = cache_.lookup(key, now);
        if (hit.status == CacheLookupStatus::kMiss && disk_.enabled()) {
            // Memory miss: consult the persistent disk tier and, on a hit,
            // promote the entry into the hot memory cache.
            if (auto diskEntry = co_await disk_.lookup(key)) {
                cache_.store(key, std::move(*diskEntry));
                hit = cache_.lookup(key, now);
            }
        }
        if (hit.status == CacheLookupStatus::kFresh) {
            co_return co_await serveHit(*hit.entry);
        }
        // A stale entry may still be revalidated with the origin below.
        CacheEntryLease staleEntry =
            hit.status == CacheLookupStatus::kStale ? hit.entry : CacheEntryLease{};

        // stale-while-revalidate: a stale entry still inside its stale-while-
        // revalidate window is served immediately while a single background job
        // refreshes it, so the client never waits on the origin.
        if (isGet && staleEntry && staleEntry->staleWhileRevalidate > 0 &&
            now <= staleEntry->expiresAt +
                       static_cast<std::time_t>(staleEntry->staleWhileRevalidate)) {
            if (inFlight_.find(key) == inFlight_.end()) {
                inFlight_.try_emplace(key);  // one refresh per key
                spawnTracked(backgroundRefresh(RefreshJob{
                    key,
                    std::string(origin->upstreamHost),
                    origin->upstreamPort,
                    origin->https,
                    std::string(target),
                    acceptEncoding,
                    staleEntry}));
            }
            const auto age = cachedResponseAge(*staleEntry, now);
            resultLabel = "STALE";
            recordedStatus = staleEntry->status;
            co_return co_await writer.respond(
                staleEntry->status, staleEntry->headers, staleEntry->body, "STALE", age,
                isHead, keepAlive) && keepAlive;
        }

        // only-if-cached forbids contacting the origin. A fresh hit or an
        // explicitly reusable stale hit has already returned above; everything
        // left is a cache miss for this request's constraints.
        if (requestCacheControl.onlyIfCached) {
            recordedStatus = 504;
            resultLabel = "MISS";
            co_return co_await writer.respond(
                       504, noHeaders, {}, "MISS", std::nullopt, false, keepAlive) &&
                keepAlive;
        }

        // Request coalescing (GET only): if a fetch for this key is already in
        // flight, wait for it and re-check the cache instead of sending the origin
        // a duplicate request. Whoever finds no in-flight fetch becomes the leader
        // and registers one; leaderGuard wakes the followers when it finishes.
        bool becameLeader = false;
        if (isGet) {
            for (;;) {
                if (inFlight_.find(key) == inFlight_.end()) {
                    inFlight_.try_emplace(key);
                    becameLeader = true;
                    break;
                }
                // A detached HTTP/2 handler whose client has gone is cancelled on
                // session teardown; stop coalescing rather than wait for a leader
                // that can no longer serve this dead connection. No-op for HTTP/1,
                // whose handler carries no cancellation slot.
                if ((co_await asio::this_coro::cancellation_state).cancelled() !=
                    asio::cancellation_type::none) {
                    co_return false;
                }
                asio::steady_timer waitTimer(ioContext_);
                waitTimer.expires_at((std::chrono::steady_clock::time_point::max)());
                inFlight_[key].waiters.push_back(&waitTimer);
                co_await waitTimer.async_wait(asio::as_tuple(asio::use_awaitable));
                // Drop our waiter before it can dangle. The leader's wakeInFlight()
                // erases the whole entry when it finishes, so remove ours only if
                // the entry is still present -- the teardown-cancel path, where
                // wakeInFlight() has not run for this key.
                if (const auto entry = inFlight_.find(key); entry != inFlight_.end()) {
                    std::erase(entry->second.waiters, &waitTimer);
                }
                if ((co_await asio::this_coro::cancellation_state).cancelled() !=
                    asio::cancellation_type::none) {
                    co_return false;
                }
                now = std::time(nullptr);
                auto woken = cache_.lookup(key, now);
                if (woken.status == CacheLookupStatus::kFresh) {
                    co_return co_await serveHit(*woken.entry);
                }
                staleEntry =
                    woken.status == CacheLookupStatus::kStale
                    ? woken.entry
                    : CacheEntryLease{};
            }
        }
        struct LeaderGuard final {
            Impl* self;
            const std::string* key;
            bool active;
            ~LeaderGuard() {
                if (active) {
                    self->wakeInFlight(*key);
                }
            }
        } leaderGuard{this, &key, becameLeader};

        // 5. Miss (or stale): fetch from the origin. Forward the client's request
        // headers minus hop-by-hop fields, Host (regenerated for the upstream),
        // Range plus client conditionals, and client-supplied forwarding headers
        // (dropped so a client cannot spoof them). Accept-Encoding is forwarded so
        // the origin may compress; the cache key includes every field line and
        // weight so variants are stored separately.
        std::pmr::vector<HttpHeaderView> forwardHeaders(memory_.resource());
        for (const auto& field : request.headers) {
            std::pmr::string lower(memory_.resource());
            lower.reserve(field.name().size());
            for (const char c : field.name()) {
                lower.push_back(toLowerAscii(c));
            }
            if (isConnectionOrFramingField(lower) ||
                connectionNominates(request.headers, field.name()) ||
                lower == "host" ||
                lower == "range" || lower == "if-none-match" ||
                lower == "if-modified-since" || lower == "if-match" ||
                lower == "if-unmodified-since" || lower == "if-range" ||
                lower == "via" || lower == "forwarded" ||
                lower.starts_with("x-forwarded-")) {
                continue;
            }
            forwardHeaders.push_back(field);
        }
        if (!request.clientAddress.empty()) {
            forwardHeaders.emplace_back(
                std::string_view("X-Forwarded-For"),
                std::string_view(request.clientAddress));
        }
        if (!request.host.empty()) {
            forwardHeaders.emplace_back(
                std::string_view("X-Forwarded-Host"), request.host);
        }
        forwardHeaders.emplace_back(
            std::string_view("X-Forwarded-Proto"),
            tlsEnabled_ ? std::string_view("https") : std::string_view("http"));
        forwardHeaders.emplace_back(
            std::string_view("Via"), std::string_view("1.1 ruvia-edge"));

        // Revalidate a stale entry with a conditional request when it carries a
        // validator, so an unchanged resource comes back as a bodyless 304.
        if (staleEntry) {
            if (const auto etag = findHeaderValue(staleEntry->headers, "etag")) {
                forwardHeaders.emplace_back(std::string_view("If-None-Match"), *etag);
            } else if (const auto lastModified =
                           findHeaderValue(staleEntry->headers, "last-modified")) {
                forwardHeaders.emplace_back(
                    std::string_view("If-Modified-Since"), *lastModified);
            }
        }

        OriginRequest originRequest;
        originRequest.method = request.method;  // GET or HEAD
        originRequest.target = target;
        originRequest.headers = forwardHeaders;

        // stale-if-error: a stale copy within its stale-if-error window is served
        // when the origin cannot be reached (or answers 5xx), instead of an error.
        const auto serveStaleOnError = [&]() -> bool {
            return staleEntry && staleEntry->staleIfError > 0 &&
                now <= staleEntry->expiresAt +
                           static_cast<std::time_t>(staleEntry->staleIfError);
        };
        const auto writeStale = [&]() -> asio::awaitable<bool> {
            const auto age = cachedResponseAge(*staleEntry, now);
            co_return co_await writer.respond(
                staleEntry->status, staleEntry->headers, staleEntry->body, "STALE", age,
                isHead, keepAlive);
        };

        // Streaming sink: writes the client head then each body chunk as the
        // origin responds, and tees a cacheable body into cacheBuffer. A 304
        // (revalidation) or a stale-if-error-covered 5xx declines streaming so the
        // stored body is served after the fetch instead.
        std::uint16_t respStatus = 0;
        Headers respHeaders;
        bool headSent = false;
        bool clientAborted = false;
        bool caching = false;
        std::string cacheBuffer;
        FreshnessDecision cacheDecision;
        const std::time_t originRequestTime = std::time(nullptr);

        ResponseSink sink;
        sink.onHead = [&](const OriginResponseHead& head) -> asio::awaitable<bool> {
            now = std::time(nullptr);
            respStatus = head.status;
            respHeaders = endToEndResponseHeaders(head.headers);
            if (staleEntry && head.status == 304) {
                co_return false;  // revalidation: serve the stored body below
            }
            if (head.status >= 500 && serveStaleOnError()) {
                co_return false;  // stale-if-error: serve the stored body below
            }
            if (!co_await writer.respondHead(head.status, respHeaders, "MISS",
                                             head.hasBody, head.contentLength, keepAlive)) {
                clientAborted = true;
                co_return false;
            }
            headSent = true;
            if (!isHead) {
                cacheDecision =
                    evaluateFreshness(buildFreshnessInput(
                        head.status,
                        respHeaders,
                        now,
                        originRequestTime,
                        requestHasAuthorization));
                caching = cacheDecision.cacheable && cacheableUnderVary(respHeaders);
            }
            co_return true;
        };
        sink.onBody = [&](std::string_view chunk) -> asio::awaitable<bool> {
            if (caching) {
                if (cacheBuffer.size() + chunk.size() > maxCacheableBytes_) {
                    caching = false;  // too big to cache; keep streaming
                    cacheBuffer.clear();
                    cacheBuffer.shrink_to_fit();
                } else {
                    cacheBuffer.append(chunk);
                }
            }
            if (!co_await writer.respondChunk(chunk)) {
                clientAborted = true;
                co_return false;
            }
            co_return true;
        };

        auto fetchResult = co_await fetcher_.fetch(
            ioContext_.get_executor(), origin->upstreamHost, origin->upstreamPort,
            origin->https, originRequest, sink);

        if (clientAborted) {
            co_return false;  // the client went away mid-response
        }
        if (fetchResult.outcome != OriginFetchOutcome::kOk) {
            now = std::time(nullptr);
            if (headSent) {
                co_return false;  // partial response already sent; close
            }
            if (serveStaleOnError()) {
                resultLabel = "STALE";
                recordedStatus = staleEntry->status;
                co_return co_await writeStale() && keepAlive;
            }
            const std::uint16_t gatewayStatus =
                fetchResult.outcome == OriginFetchOutcome::kTimeout ? 504 : 502;
            recordedStatus = gatewayStatus;
            co_await writer.respond(
                gatewayStatus, noHeaders, {}, "ERROR", std::nullopt, false, false);
            co_return false;
        }

        // The sink declined to stream (304 revalidation, or a stale-if-error 5xx):
        // serve the stored body instead.
        if (!headSent && staleEntry) {
            if (respStatus == 304) {
                Headers merged = mergeStoredHeaders(staleEntry->headers, respHeaders);
                const auto decision =
                    evaluateFreshness(buildFreshnessInput(
                        staleEntry->status,
                        merged,
                        now,
                        originRequestTime,
                        requestHasAuthorization));
                CachedResponse refreshed;
                refreshed.status = staleEntry->status;
                refreshed.body = staleEntry->body;
                refreshed.headers = std::move(merged);
                refreshed.storedAt = now;
                refreshed.initialAge = decision.initialAge;
                refreshed.expiresAt = decision.cacheable ? decision.expiresAt : now;
                refreshed.staleWhileRevalidate = decision.staleWhileRevalidate;
                refreshed.staleIfError = decision.staleIfError;
                const bool storable = decision.cacheable && cacheableUnderVary(refreshed.headers);
                if (storable) {
                    disk_.store(key, refreshed);
                    cache_.store(key, CachedResponse(refreshed));
                } else {
                    cache_.purge(key);  // no longer has usable freshness
                    disk_.purge(key);
                }
                resultLabel = "REVALIDATED";
                recordedStatus = refreshed.status;
                co_return co_await writer.respond(
                    refreshed.status, refreshed.headers, refreshed.body, "REVALIDATED",
                    refreshed.initialAge, isHead, keepAlive) && keepAlive;
            }
            resultLabel = "STALE";  // 5xx covered by stale-if-error
            recordedStatus = staleEntry->status;
            co_return co_await writeStale() && keepAlive;
        }

        // A full response streamed successfully: finish the framing and commit the
        // cache if the whole body was accumulated within the size cap.
        if (!co_await writer.respondEnd()) {
            co_return false;
        }
        if (caching) {
            CachedResponse entry;
            entry.status = respStatus;
            entry.headers = std::move(respHeaders);
            entry.body = std::move(cacheBuffer);
            entry.storedAt = now;
            entry.initialAge = cacheDecision.initialAge;
            entry.expiresAt = cacheDecision.expiresAt;
            entry.staleWhileRevalidate = cacheDecision.staleWhileRevalidate;
            entry.staleIfError = cacheDecision.staleIfError;
            disk_.store(key, entry);
            cache_.store(key, std::move(entry));
        } else if (staleEntry && respStatus < 500) {
            // A successful/full replacement that is no longer storable (for
            // example no-store/private, unsupported Vary, or an oversized new
            // representation) supersedes the stale entry. Keeping it would let
            // a later stale-if-error path resurrect data the origin withdrew.
            cache_.purge(key);
            disk_.purge(key);
        }
        resultLabel = "MISS";
        recordedStatus = respStatus;
        co_return keepAlive;
}

asio::awaitable<void> EdgeServer::Impl::backgroundRefresh(RefreshJob job) {
    // Wake any foreground waiters and drop the in-flight entry however this ends.
    struct Guard final {
        Impl* self;
        const std::string* key;
        ~Guard() { self->wakeInFlight(*key); }
    } guard{this, &job.key};

    // A conditional GET for the same variant (validator + the variant's encoding).
    std::pmr::vector<HttpHeaderView> headers(memory_.resource());
    if (const auto etag = findHeaderValue(job.stored->headers, "etag")) {
        headers.emplace_back(std::string_view("If-None-Match"), *etag);
    } else if (const auto lastModified =
                   findHeaderValue(job.stored->headers, "last-modified")) {
        headers.emplace_back(std::string_view("If-Modified-Since"), *lastModified);
    }
    if (job.acceptEncoding) {
        headers.emplace_back(
            std::string_view("Accept-Encoding"),
            std::string_view(*job.acceptEncoding));
    }
    headers.emplace_back(std::string_view("Via"), std::string_view("1.1 ruvia-edge"));

    OriginRequest request;
    request.method = "GET";
    request.target = job.target;
    request.headers = headers;

    // Background sink: accumulate a cacheable body; it never writes to a client.
    std::uint16_t status = 0;
    Headers respHeaders;
    std::string body;
    bool caching = false;
    FreshnessDecision decision;
    std::time_t now = std::time(nullptr);
    const std::time_t originRequestTime = now;

    ResponseSink sink;
    sink.onHead = [&](const OriginResponseHead& head) -> asio::awaitable<bool> {
        now = std::time(nullptr);
        status = head.status;
        respHeaders = endToEndResponseHeaders(head.headers);
        if (head.status == 304) {
            co_return false;  // not modified: refresh freshness below
        }
        decision = evaluateFreshness(buildFreshnessInput(
            head.status, respHeaders, now, originRequestTime, false));
        caching = decision.cacheable && cacheableUnderVary(respHeaders);
        co_return caching;  // only download a body we intend to cache
    };
    sink.onBody = [&](std::string_view chunk) -> asio::awaitable<bool> {
        if (body.size() + chunk.size() > maxCacheableBytes_) {
            caching = false;
            co_return false;  // too big to cache: abandon the refresh
        }
        body.append(chunk);
        co_return true;
    };

    auto result = co_await fetcher_.fetch(
        ioContext_.get_executor(), job.host, job.port, job.https, request, sink);
    if (result.outcome != OriginFetchOutcome::kOk) {
        co_return;  // origin unreachable: leave the stale entry in place
    }

    if (status == 304) {
        Headers merged = mergeStoredHeaders(job.stored->headers, respHeaders);
        const auto refreshed =
            evaluateFreshness(buildFreshnessInput(
                job.stored->status, merged, now, originRequestTime, false));
        if (refreshed.cacheable && cacheableUnderVary(merged)) {
            CachedResponse entry;
            entry.status = job.stored->status;
            entry.body = job.stored->body;
            entry.storedAt = now;
            entry.initialAge = refreshed.initialAge;
            entry.expiresAt = refreshed.expiresAt;
            entry.staleWhileRevalidate = refreshed.staleWhileRevalidate;
            entry.staleIfError = refreshed.staleIfError;
            entry.headers = std::move(merged);
            disk_.store(job.key, entry);
            cache_.store(job.key, std::move(entry));
        }
        co_return;
    }

    if (caching) {
        CachedResponse entry;
        entry.status = status;
        entry.headers = std::move(respHeaders);
        entry.body = std::move(body);
        entry.storedAt = now;
        entry.initialAge = decision.initialAge;
        entry.expiresAt = decision.expiresAt;
        entry.staleWhileRevalidate = decision.staleWhileRevalidate;
        entry.staleIfError = decision.staleIfError;
        disk_.store(job.key, entry);
        cache_.store(job.key, std::move(entry));
    } else if (status < 500) {
        // A background 2xx/3xx/4xx full response replaced the old
        // representation but cannot itself be stored. Drop the stale copy;
        // 5xx validation failures intentionally leave it available to policy.
        cache_.purge(job.key);
        disk_.purge(job.key);
    }
}

template <typename Stream>
asio::awaitable<void> EdgeServer::Impl::handleHttp2Session(Stream stream, std::string clientAddress) {
    using namespace asio::experimental::awaitable_operators;
    const auto tuple = asio::as_tuple(asio::use_awaitable);
    const auto executor = co_await asio::this_coro::executor;

    std::pmr::unsynchronized_pool_resource resource;
    detail::Http2Connection connection(&resource, detail::Http2Role::kServer);
    connection.beginConnection();

    asio::steady_timer writeWake(executor);
    // Woken when a handler finishes, so the draining reader can notice the last
    // in-flight stream completed even while it is blocked awaiting client frames.
    asio::steady_timer handlersIdle(executor);
    std::pmr::unordered_map<std::uint32_t, asio::steady_timer*> drainWaiters(
        memory_.resource());
    bool shuttingDown = false;
    int activeHandlers = 0;
    // Per-stream cancellation signals for the detached response handlers. On
    // teardown beginShutdown() emits terminal cancellation on each, so a handler
    // parked somewhere it does not otherwise observe the shutdown -- the request-
    // coalescing wait, or an in-flight origin fetch -- is released and unwinds.
    // Node-based storage: cancellation_signal is not movable, and the slot a
    // spawned handler binds must stay valid until that handler completes.
    std::pmr::unordered_map<std::uint32_t, asio::cancellation_signal> handlerCancels(
        memory_.resource());
    Http2SessionShared shared{connection, writeWake, drainWaiters, shuttingDown};

    // Wake the writer and, once tearing down, release every parked handler so it
    // can observe the shutdown and unwind rather than await a window forever.
    const auto beginShutdown = [&]() {
        shuttingDown = true;
        for (auto& [id, timer] : drainWaiters) {
            timer->cancel();
        }
        // Release every still-running handler wherever it is parked (coalescing
        // wait, origin fetch, disk lookup) so none resumes into the locals below
        // after this frame returns. Cancellation posts the abort, so a handler's
        // completion callback -- which erases from handlerCancels -- runs later,
        // not re-entrantly during this loop.
        for (auto& [id, signal] : handlerCancels) {
            signal.emit(asio::cancellation_type::terminal);
        }
        writeWake.cancel();
    };

    // One stream's response handler: build the logical request from the pinned
    // stream and drive the serve core with an HTTP/2 writer. Named-local so its
    // closure outlives the coroutines co_spawn()ed from it.
    auto serveStream =
        [this, &shared, resource = &resource, clientAddress = std::string_view(clientAddress)](
            std::uint32_t streamId, detail::Http2StreamState& streamState,
            std::pmr::string requestBody) -> asio::awaitable<void> {
        HttpRequest httpRequest = detail::HttpRequestAccess::make();
        const auto buildResult =
            detail::Http2RequestBuilder::build(streamState, httpRequest, resource, requestBody);
        if (buildResult.built() == nullptr) {
            (void)shared.connection.submitReset(streamId, detail::Http2ErrorCode::kProtocolError);
            shared.writeWake.cancel();
            co_return;
        }

        EdgeRequest edgeRequest;
        edgeRequest.method = httpRequest.method();
        edgeRequest.knownMethod = httpRequest.knownMethod();
        edgeRequest.target = httpRequest.target();
        edgeRequest.host = streamState.requestAuthority();
        edgeRequest.headers = httpRequest.headers();
        edgeRequest.clientAddress = clientAddress;
        edgeRequest.keepAlive = true;
        if (edgeRequest.knownMethod != HttpKnownMethod::kGet &&
            edgeRequest.knownMethod != HttpKnownMethod::kHead && !requestBody.empty()) {
            edgeRequest.body = std::string_view(requestBody);
        }

        Http2ResponseWriter writer(shared, streamId, edgeRequest.knownMethod, resource);
        (void)co_await serveRequest(edgeRequest, writer);
        // If the serve core returned without completing the response (client gone
        // mid-stream), reset the stream so it does not dangle.
        if (!writer.ended()) {
            auto* s = shared.connection.stream(streamId);
            if (s != nullptr && !s->isAborted()) {
                (void)shared.connection.submitReset(streamId,
                                                    detail::Http2ErrorCode::kInternalError);
            }
        }
        shared.writeWake.cancel();
    };

    // Writer coroutine: the sole owner of async_write. It drains the connection's
    // pending output, then parks on writeWake until more is produced. It exits once
    // the session is shutting down and no handler is still running, or on a fatal
    // connection error, or if a write fails.
    auto writer = [&]() -> asio::awaitable<void> {
        for (;;) {
            while (connection.wantsWrite()) {
                std::pmr::string out(&resource);
                connection.takeOutput(out);
                auto [ec, n] = co_await asio::async_write(
                    stream, asio::buffer(out.data(), out.size()), tuple);
                (void)n;
                if (ec) {
                    beginShutdown();
                    co_return;
                }
            }
            if (connection.connectionError().has_value() ||
                (shuttingDown && activeHandlers == 0)) {
                co_return;
            }
            writeWake.expires_at((std::chrono::steady_clock::time_point::max)());
            co_await writeWake.async_wait(tuple);
        }
    };

    // Reader loop: read, feed, dispatch each completed request to its own handler
    // coroutine so a slow origin on one stream never blocks the others.
    std::array<char, 16384> readBuffer;
    std::pmr::unordered_map<std::uint32_t, std::pmr::string> requestBodies(
        memory_.resource());

    auto reader = [&]() -> asio::awaitable<void> {
        for (;;) {
            std::size_t readSize = 0;
            asio::error_code readError;
            auto raced = co_await (
                stream.async_read_some(asio::buffer(readBuffer), tuple) ||
                shutdownSignal_.async_wait(tuple));
            if (raced.index() == 1) {
                break;
            }
            std::tie(readError, readSize) = std::get<0>(raced);
            if (readError) {
                break;
            }
            (void)connection.feed(std::string_view(readBuffer.data(), readSize));
            writeWake.cancel();  // feed may have queued control frames

            // DATA events borrow the current input span and retain receive-window
            // debt. Copy the whole event batch first, then return each stream's
            // accumulated credit exactly once; acknowledging inside the loop
            // could cover later events whose borrowed bytes are not copied yet.
            std::array<std::uint32_t,
                       detail::Http2LocalSettings::kMaxConcurrentStreams>
                copiedBodyStreams{};
            std::size_t copiedBodyStreamCount = 0;
            const auto markBodyCopied = [&](std::uint32_t streamId) {
                const auto copied = std::span(copiedBodyStreams)
                                        .first(copiedBodyStreamCount);
                if (std::ranges::find(copied, streamId) != copied.end()) {
                    return true;
                }
                if (copiedBodyStreamCount == copiedBodyStreams.size()) {
                    return false;
                }
                copiedBodyStreams[copiedBodyStreamCount++] = streamId;
                return true;
            };
            const auto unmarkBodyCopied = [&](std::uint32_t streamId) {
                auto copied = std::span(copiedBodyStreams)
                                  .first(copiedBodyStreamCount);
                const auto found = std::ranges::find(copied, streamId);
                if (found == copied.end()) {
                    return;
                }
                --copiedBodyStreamCount;
                *found = copiedBodyStreams[copiedBodyStreamCount];
            };
            const auto resetBodyStream = [&](std::uint32_t streamId,
                                             detail::Http2ErrorCode error) {
                unmarkBodyCopied(streamId);
                requestBodies.erase(streamId);
                (void)connection.submitReset(streamId, error);
                writeWake.cancel();
            };

            for (;;) {
                const auto event = connection.nextEvent();
                if (!event.has_value()) {
                    break;
                }
                if (const auto* head = event->messageHead()) {
                    const auto streamId = head->streamId();
                    const auto* streamState = connection.stream(streamId);
                    const auto* knownLength = streamState == nullptr
                        ? nullptr
                        : streamState->remoteContent().allowedKnownLength();
                    if (knownLength != nullptr &&
                        knownLength->declaredLength() > kMaxRequestBytes) {
                        resetBodyStream(
                            streamId, detail::Http2ErrorCode::kCancel);
                    } else {
                        requestBodies.try_emplace(streamId);
                    }
                } else if (const auto* chunk = event->messageBodyChunk()) {
                    const auto streamId = chunk->streamId();
                    const auto body = requestBodies.find(streamId);
                    if (body == requestBodies.end()) {
                        resetBodyStream(
                            streamId,
                            detail::Http2ErrorCode::kInternalError);
                        continue;
                    }
                    if (chunk->bytes().size() >
                        kMaxRequestBytes - body->second.size()) {
                        resetBodyStream(
                            streamId, detail::Http2ErrorCode::kCancel);
                        continue;
                    }
                    body->second.append(chunk->bytes());
                    if (!markBodyCopied(streamId)) {
                        resetBodyStream(
                            streamId,
                            detail::Http2ErrorCode::kInternalError);
                    }
                } else if (const auto* end = event->messageEnd()) {
                    const auto streamId = end->streamId();
                    auto* streamState = connection.stream(streamId);
                    if (streamState == nullptr) {
                        continue;
                    }
                    std::pmr::string body(memory_.resource());
                    if (auto it = requestBodies.find(streamId); it != requestBodies.end()) {
                        body = std::move(it->second);
                        requestBodies.erase(it);
                    }
                    // Register cancellation before acquiring the stream lease.
                    // If allocation or co_spawn() fails synchronously, roll back
                    // every piece of bookkeeping: otherwise the draining loop
                    // below would wait forever for a handler that never started.
                    auto [cancelIt, inserted] = handlerCancels.try_emplace(streamId);
                    if (!inserted) {
                        throw std::logic_error(
                            "duplicate HTTP/2 handler for one stream");
                    }
                    bool handlerRegistered = false;
                    try {
                        // Pin so the stream's request/response storage outlives
                        // the detached handler; unpin on its completion.
                        connection.pinStream(streamId);
                        ++activeHandlers;
                        handlerRegistered = true;
                        asio::co_spawn(
                            executor, serveStream(streamId, *streamState, std::move(body)),
                            asio::bind_cancellation_slot(
                                cancelIt->second.slot(),
                                [&, streamId](std::exception_ptr failure) {
                                    if (failure != nullptr && !shuttingDown) {
                                        auto* failedStream = connection.stream(streamId);
                                        if (failedStream != nullptr &&
                                            !failedStream->isAborted()) {
                                            // A handler that unwinds without a
                                            // terminal response still owes the
                                            // peer a stream terminal state.
                                            (void)connection.submitReset(
                                                streamId,
                                                detail::Http2ErrorCode::kInternalError);
                                        }
                                    }
                                    connection.unpinStream(streamId);
                                    drainWaiters.erase(streamId);
                                    handlerCancels.erase(streamId);
                                    --activeHandlers;
                                    writeWake.cancel();  // let the writer re-check its exit
                                    handlersIdle.cancel();  // wake a draining reader
                                }));
                    } catch (...) {
                        if (handlerRegistered) {
                            --activeHandlers;
                            connection.unpinStream(streamId);
                        }
                        handlerCancels.erase(cancelIt);
                        throw;
                    }
                } else if (const auto* closed = event->streamClosed()) {
                    // The peer reset/closed the stream: wake its parked handler so
                    // it sees the abort and unwinds.
                    unmarkBodyCopied(closed->streamId());
                    if (const auto it = drainWaiters.find(closed->streamId());
                        it != drainWaiters.end()) {
                        it->second->cancel();
                    }
                    requestBodies.erase(closed->streamId());
                }
            }

            for (std::size_t index = 0;
                 index < copiedBodyStreamCount;
                 ++index) {
                connection.releaseReceivedData(copiedBodyStreams[index]);
            }
            if (copiedBodyStreamCount != 0) {
                writeWake.cancel();
            }

            // Resume any handler whose flow-control window just reopened.
            for (const std::uint32_t id : connection.takeDrainedDataStreams()) {
                if (const auto it = drainWaiters.find(id); it != drainWaiters.end()) {
                    it->second->cancel();
                }
            }
            writeWake.cancel();  // handlers may have produced output

            if (connection.connectionError().has_value()) {
                break;
            }
        }
        beginShutdown();
    };

    try {
        co_await (reader() && writer());
    } catch (...) {
        // A synchronous throw (e.g. bad_alloc from the writer's output buffer or
        // the reader's body accumulation) can escape the group while a detached
        // handler is still awaiting its origin fetch. Signal teardown so every
        // handler unwinds, then fall through to join them before the locals they
        // captured by reference are destroyed.
        beginShutdown();
    }

    // The reader and writer have both finished. A connection error (or the throw
    // above) can end them while a per-stream handler is still awaiting its origin
    // fetch or coalescing on another stream; those handlers captured this frame's
    // locals by reference. beginShutdown() has run, so shuttingDown is set and
    // each remaining handler's current await was cancelled -- it unwinds without
    // re-parking. Wait for the last one before destroying the locals.
    while (activeHandlers > 0) {
        handlersIdle.expires_at((std::chrono::steady_clock::time_point::max)());
        co_await handlersIdle.async_wait(tuple);
    }

    asio::error_code ignore;
    stream.lowest_layer().shutdown(asio::ip::tcp::socket::shutdown_both, ignore);
}

EdgeServer::EdgeServer(EdgeEndpoint endpoint, EdgeServerOptions options)
    : impl_(std::make_unique<Impl>(std::move(endpoint), std::move(options))) {}

EdgeServer::~EdgeServer() = default;

void EdgeServer::start() {
    impl_->start();
}

void EdgeServer::stop() {
    impl_->stop();
}

void EdgeServer::join() {
    impl_->join();
}

EdgeEndpoint EdgeServer::localEndpoint() const {
    return impl_->localEndpoint();
}

bool EdgeServer::addOrigin(std::string frontHost, OriginSettings settings) {
    return impl_->addOrigin(std::move(frontHost), std::move(settings));
}

bool EdgeServer::removeOrigin(std::string_view frontHost) {
    return impl_->removeOrigin(frontHost);
}

bool EdgeServer::setTlsCertificate(const EdgeTlsConfig& tls) {
    return impl_->setTlsCertificate(tls);
}

bool EdgeServer::purge(std::string_view frontHost, std::string_view target) {
    return impl_->purge(frontHost, target);
}

bool EdgeServer::clearCache() {
    return impl_->clearCache();
}

}  // namespace ruvia::edge
