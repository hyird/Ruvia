#include "ruvia/web/detail/server/WebWorkerRuntime.h"

#include <algorithm>
#include <cerrno>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

#if !defined(_WIN32)
#include <sys/socket.h>
#endif

#include <asio/bind_allocator.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/post.hpp>
#include <asio/recycling_allocator.hpp>
#include <asio/ssl/context.hpp>
#include <openssl/ssl.h>

#include "ruvia/core/ConnectionScanner.h"
#include "ruvia/core/FailureReport.h"
#include "ruvia/core/memory/ProcessResource.h"
#include "ruvia/http/HttpAscii.h"
#include "ruvia/web/detail/app/WebWorkerDispatch.h"
#include "ruvia/web/detail/http/static/StaticRootIndex.h"
#include "ruvia/web/detail/router/RouteTable.h"
#include "ruvia/web/detail/server/HttpServerOptionsValidation.h"
#include "ruvia/web/detail/server/tls/HttpServerTlsIdentity.h"

namespace ruvia::detail {

using TcpEndpoint = asio::ip::tcp::endpoint;

namespace {

int selectAlpnProtocol(SSL*, const unsigned char** out, unsigned char* outLength,
    const unsigned char* in, unsigned int inLength, void*) noexcept {
    // This is the TCP TLS context, so it offers only h2 and http/1.1. HTTP/3
    // negotiates "h3" on the separately owned UDP/QUIC server network context and is
    // never advertised through this callback.
    static constexpr unsigned char protocols[] = {
        2, 'h', '2', 8, 'h', 't', 't', 'p', '/', '1', '.', '1'};
    if (SSL_select_next_proto(const_cast<unsigned char**>(out), outLength, protocols,
            static_cast<unsigned int>(sizeof(protocols)), in, inLength) == OPENSSL_NPN_NEGOTIATED) {
        return SSL_TLSEXT_ERR_OK;
    }
    return SSL_TLSEXT_ERR_NOACK;
}

[[nodiscard]] StaticRootPrecompressionOptions documentRootPrecompressionOptions(
    const HttpServerOptions& options) noexcept {
    const auto* configured = options.documentRoot.precompressionOptions();
    if (configured == nullptr || !options.compression.has_value()) {
        return {};
    }
    return *configured;
}

// RFC 6066 SNI: switch the connection to the per-host SSL_CTX when the client's
// server name matches a configured certificate; otherwise keep the default.
int selectSniContext(SSL* ssl, int*, void* arg) noexcept {
    if (ssl == nullptr || arg == nullptr) {
        return SSL_TLSEXT_ERR_OK;
    }
    const char* name = SSL_get_servername(ssl, TLSEXT_NAMETYPE_host_name);
    if (name == nullptr) {
        return SSL_TLSEXT_ERR_OK;
    }
    const auto& lookup = *static_cast<const SniContextLookup*>(arg);
    for (const auto& [host, context] : lookup) {
        if (httpAsciiEqualsIgnoreCase(host, name)) {
            SSL_set_SSL_CTX(ssl, context->native_handle());
            break;
        }
    }
    return SSL_TLSEXT_ERR_OK;
}

[[nodiscard]] ruvia::ConnectionScannerOptions makeConnectionScannerOptions(
    const HttpServerOptions& options) noexcept {
    return ruvia::ConnectionScannerOptions{.scanInterval = options.scanInterval,
        .idleTimeout = options.idleTimeout,
        .initialReadTimeout = options.requestHeaderTimeout,
        .payloadReadTimeout = options.requestBodyTimeout,
        .writeTimeout = options.writeTimeout};
}

}  // namespace

WebWorkerRuntime::WebWorkerRuntime(TcpEndpoint endpoint, const RouteTable& routes,
    WorkerCapabilityDefinitions capabilities, HttpServerOptions options)
    : WebWorkerRuntime(HttpServerListenerDefinition(std::move(endpoint)), routes, capabilities,
          std::move(options)) {}

WebWorkerRuntime::WebWorkerRuntime(HttpServerListenerDefinition listener, const RouteTable& routes,
    WorkerCapabilityDefinitions capabilities, HttpServerOptions options)
    : WebWorkerRuntime(std::span<const HttpServerListenerDefinition>(&listener, 1), routes,
          capabilities, std::move(options)) {}

WebWorkerRuntime::WebWorkerRuntime(std::span<const HttpServerListenerDefinition> listeners,
    const RouteTable& routes, WorkerCapabilityDefinitions capabilities, HttpServerOptions options)
    : WebWorkerRuntime(validateHttpServerConfiguration(listeners, std::move(options)), routes,
          capabilities, ConnectionOwnershipMode::kOwnListeners) {}

WebWorkerRuntime::WebWorkerRuntime(const ValidatedHttpServerConfiguration& configuration,
    const RouteTable& routes, WorkerCapabilityDefinitions capabilities)
    : WebWorkerRuntime(configuration, routes, capabilities,
          ConnectionOwnershipMode::kTransferredSessions) {}

WebWorkerRuntime::WebWorkerRuntime(const ValidatedHttpServerConfiguration& configuration,
    const RouteTable& routes, WorkerCapabilityDefinitions capabilities,
    ConnectionOwnershipMode connectionOwnershipMode)
    : WebWorkerRuntime(ValidatedConfigurationTag{}, configuration.listeners(), routes, capabilities,
          configuration.options(), connectionOwnershipMode) {}

WebWorkerRuntime::WebWorkerRuntime(ValidatedConfigurationTag,
    std::span<const HttpServerListenerDefinition> listeners, const RouteTable& routes,
    WorkerCapabilityDefinitions capabilities, HttpServerOptions validatedOptions,
    ConnectionOwnershipMode connectionOwnershipMode)
    // One worker thread runs all I/O on this context; cross-thread access is
    // limited to stop()'s asio::post, which UNSAFE_IO keeps locked. Only the
    // reactor's per-descriptor I/O locking is elided.
    : ioContext_(ASIO_CONCURRENCY_HINT_UNSAFE_IO),
      workerRuntime_(ioContext_, validatedOptions.workerMailboxCapacity),
      finalizeGuard_(asio::make_work_guard(ioContext_)),
      serveSignal_(workerRuntime_.handle()),
      finalizeSignal_(workerRuntime_.handle()),
      routes_(routes),
      memory_(validatedOptions.memoryConfig),
      listeners_(memory_.resource()),
      acceptors_(memory_.resource()),
      backgroundTasks_(workerRuntime_.handle(), {.resource = memory_.resource()}),
      ownedDocumentRoot_(nullptr, PmrObjectDeleter<StaticRoot>{processResource()}),
      retiredDocumentRoots_(memory_.resource()),
      options_(std::move(validatedOptions)),
      connectionOwnershipMode_(connectionOwnershipMode),
      connectionScanner_(workerRuntime_.handle(), makeConnectionScannerOptions(options_)),
      capabilities_(ioContext_, workerRuntime_.handle(), memory_.resource(), capabilities,
          WorkerCapabilityOptions{
              .defaultRateLimit = options_.defaultRateLimitPerWorker,
              .routeRateLimits = routes_.hasRouteRateLimit() ? RouteRateLimitPresence::kPresent
                                                             : RouteRateLimitPresence::kAbsent,
              .rateLimitCapacity = options_.rateLimitCapacityPerWorker,
              .maxDecodedBodyBytes = options_.maxBufferedBodyBytes,
              .httpClientResultBudget = options_.httpClientResultBudget,
              .blockingPool = options_.blockingPool,
              .env = options_.env,
              .trustedProxies =
                  options_.trustedProxies.empty() ? nullptr : &options_.trustedProxies,
              .precompressedStaticFiles = options_.compression.has_value(),
          }),
      http3Server_(nullptr, PmrObjectDeleter<Http3WorkerServer>{memory_.resource()}),
      webWorkerDispatch_(std::make_shared<WebWorkerDispatch>(ioContext_.get_executor(),
          workerRuntime_.handle(), memory_.resource(), capabilities_,
          [this](const std::exception_ptr& failure) { failWorker(failure); })),
      workSetPool_(memory_) {
    const auto http3Listener = std::ranges::find_if(listeners,
        [](const HttpServerListenerDefinition& listener) { return listener.http3.has_value(); });
    if (http3Listener != listeners.end()) {
        if (!options_.maxConnections.has_value() ||
            options_.workerMailboxCapacity > std::numeric_limits<std::uint32_t>::max()) {
            throw std::invalid_argument("HTTP/3 worker limits are not representable");
        }
        http3Server_ = makePmrObject<Http3WorkerServer>(memory_.resource(), workerRuntime_,
            workerRuntime_.handle(), memory_, routes_, capabilities_, options_, stopToken_,
            *options_.maxConnections,
            static_cast<std::uint32_t>(options_.workerMailboxCapacity), activeConnectionCount_,
            connectionsRefused_);
    }

    listeners_.reserve(listeners.size());
    if (connectionOwnershipMode_ == ConnectionOwnershipMode::kOwnListeners) {
        acceptors_.reserve(listeners.size());
    }
    for (const auto& listener : listeners) {
        listeners_.push_back(makePmrObject<HttpServerSessionConfig>(
            memory_.resource(), listener, memory_.resource()));
        if (connectionOwnershipMode_ == ConnectionOwnershipMode::kOwnListeners) {
            acceptors_.push_back(makePmrObject<HttpServerAcceptor>(
                memory_.resource(), ioContext_, listener));
        }
    }
    if (options_.documentRoot.refreshOptions() != nullptr) {
        const auto* configuredRoot = options_.documentRoot.root();
        if (configuredRoot == nullptr) {
            std::terminate();
        }
        ownedDocumentRoot_ = StaticRootAccess::clone(processResource(), *configuredRoot);
        const auto precompression = documentRootPrecompressionOptions(options_);
        if (precompression.enabled()) {
            StaticRootAccess::installPrecompressedVariants(
                *ownedDocumentRoot_, configuredRoot, precompression);
        }
        options_.documentRoot.publish(*ownedDocumentRoot_);
    }
    // Claim the failure sink's counter. Every reporting site shares this one
    // options_ instance, so the count cannot drift from what the callback saw.
    options_.connectionFailure.counter = &connectionFailures_;
}

WebWorkerRuntime::~WebWorkerRuntime() {
    stop();
    try {
        join();
    } catch (...) {
        // join() rethrows this worker's failure, and a destructor cannot pass
        // it on. A server destroyed without an explicit join -- or one whose
        // failure raced the App's own shutdown -- would otherwise take the
        // reason with it.
        ruvia::reportUnhandledFailure("web server worker", std::current_exception());
    }
    // Retire the execution context first. Failure shutdown already releases
    // abandoned mailbox tasks on the worker; detach defensively releases any
    // task left by a context that stopped outside the managed run loop. A post
    // producer can still be inside core's factory at this point; retire waits
    // only for started callbacks, while that producer later abandons its own
    // reservation. Public handles may outlive this server, so detach also
    // leaves them a terminal endpoint before Asio objects are destroyed.
    workerRuntime_.detach();
    webWorkerDispatch_->retire();
}

void WebWorkerRuntime::start() {
    prepare();
    launch();
    waitUntilReady();
    requestServe();
    if (!waitUntilServing()) {
        throw std::runtime_error("web worker stopped before it began serving");
    }
}

void WebWorkerRuntime::prepare() {
    if (prepared_ || lifecycle_.state() != RuntimeLifecycle::State::kReady) {
        throw std::logic_error("web worker runtime can only be prepared once");
    }
    for (std::size_t i = 0; i < listeners_.size(); ++i) {
        if (connectionOwnershipMode_ == ConnectionOwnershipMode::kOwnListeners) {
            configureAcceptor(*acceptors_[i]);
        }
        configureTlsContext(*listeners_[i]);
    }
    prepared_ = true;
}

void WebWorkerRuntime::launch() {
    if (!prepared_) {
        throw std::logic_error("web worker runtime must be prepared before launch");
    }
    std::lock_guard threadStateLock(threadStateMutex_);
    if (!lifecycle_.start()) {
        throw std::logic_error("web worker runtime cannot be launched twice");
    }
    workerState_ = HttpServerWorkerState::kRunning;

    try {
        workerThread_ = std::thread([this] { runIoContext(); });
        threadLaunched_ = true;
    } catch (...) {
        const auto failure = std::current_exception();
        struct UnstartedRuntimeCleanup final {
            WebWorkerRuntime& runtime;

            ~UnstartedRuntimeCleanup() {
                for (const auto& acceptor : runtime.acceptors_) {
                    asio::error_code ignored;
                    acceptor->acceptor.close(ignored);
                }
                runtime.webWorkerDispatch_->close();
                runtime.webWorkerDispatch_->retire();
                runtime.workerRuntime_.close();
                runtime.workerRuntime_.detach();
                runtime.finalizeGuard_.reset();
                (void)runtime.lifecycle_.requestStop();
                runtime.lifecycle_.completeStop();
            }
        } cleanup{*this};
        (void)workerCompletion_.markStartupFailed(failure);
        (void)workerCompletion_.recordWorkerFailure(failure);
        throw;
    }
}

void WebWorkerRuntime::waitUntilReady() {
    if (lifecycle_.state() == RuntimeLifecycle::State::kReady) {
        throw std::logic_error("web worker runtime has not been launched");
    }
    workerCompletion_.waitForStartup();
}

void WebWorkerRuntime::requestServe() {
    if (lifecycle_.state() != RuntimeLifecycle::State::kRunning) {
        return;
    }
    asio::post(ioContext_, [this] {
        if (!httpServerWorkerRunning(workerState_) || serveRequested_ ||
            stopToken_.stopRequested()) {
            return;
        }
        serveRequested_ = true;
        serveSignal_.notify();
    });
}

bool WebWorkerRuntime::waitUntilServing() {
    return workerCompletion_.waitForServing();
}

void WebWorkerRuntime::stop() noexcept {
    if (lifecycle_.state() == RuntimeLifecycle::State::kStopped) {
        return;
    }
    stopAdmission();
    finalizeAfterNetworkQuiesced();
}

void WebWorkerRuntime::stopAdmission() noexcept {
    (void)lifecycle_.requestStop();
    workerRuntime_.close();

    bool threadLaunched = false;
    {
        std::lock_guard lock(threadStateMutex_);
        threadLaunched = threadLaunched_;
    }
    if (!threadLaunched) {
        if (http3Server_ != nullptr) {
            http3Server_->abandonBeforeLaunch();
        }
        return;
    }
    // This internal control remains available after dispatcher admission closes.
    // Even calls made on the owner thread are deferred so StopToken callbacks do
    // not run inline on App::stop()'s caller stack.
    workerRuntime_.deferOrTerminate([this] { stopAdmissionOnContext(); });
}

void WebWorkerRuntime::finalizeAfterNetworkQuiesced() noexcept {
    {
        std::lock_guard lock(threadStateMutex_);
        if (!threadLaunched_) {
            // No worker owner exists yet; startup/prepare resources can be retired
            // by the external lifecycle owner.
            finalizeGuard_.reset();
            return;
        }
    }
    // Dispatcher defer remains reliable after close(): it bypasses bounded
    // admission while still targeting the attached owner context.
    workerRuntime_.deferOrTerminate([this] { stopOnContext(); });
}

void WebWorkerRuntime::join() {
    if (workerRuntime_.handle().isCurrent()) {
        throw std::logic_error("cannot join a Web worker runtime from its worker");
    }
    std::thread joiningThread;
    {
        std::lock_guard lock(threadStateMutex_);
        if (workerThread_.joinable()) {
            joiningThread = std::move(workerThread_);
        }
    }
    if (joiningThread.joinable()) {
        joiningThread.join();
    }
    lifecycle_.completeStop();
    const auto failure = workerCompletion_.workerFailure();
    if (failure != nullptr) {
        std::rethrow_exception(failure);
    }
}

TcpEndpoint WebWorkerRuntime::localEndpoint(std::size_t listenerIndex) const {
    if (listenerIndex >= acceptors_.size()) {
        throw std::out_of_range("listener acceptor is not configured on this worker");
    }
    return acceptors_[listenerIndex]->endpoint;
}

bool WebWorkerRuntime::availableForNetworkDispatch() const noexcept {
    if (lifecycle_.state() != RuntimeLifecycle::State::kRunning ||
        !networkServing_.load(std::memory_order_acquire)) {
        return false;
    }
    return !options_.maxConnections.has_value() ||
           activeConnectionCount_.load(std::memory_order_relaxed) < *options_.maxConnections;
}

HttpServerStats WebWorkerRuntime::stats() const noexcept {
    HttpServerStats stats;
    stats.activeConnections = activeConnectionCount_.load(std::memory_order_relaxed);
    stats.connectionsRefused = connectionsRefused_.load(std::memory_order_relaxed);
    stats.connectionFailures = connectionFailures_.load(std::memory_order_relaxed);
    stats.acceptFailures = acceptFailures_.load(std::memory_order_relaxed);
    stats.workerFailures = workerFailures_.load(std::memory_order_relaxed);
    stats.documentRootRefreshFailures =
        documentRootRefreshFailures_.load(std::memory_order_relaxed);
    return stats;
}

WebWorkerHandle WebWorkerRuntime::webWorker() const {
    return webWorkerDispatch_->handle();
}
void WebWorkerRuntime::configureAcceptor(HttpServerAcceptor& listener) {
    std::error_code ec;

    (void)listener.acceptor.open(listener.endpoint.protocol(), ec);
    if (ec) {
        throw std::runtime_error("failed to open acceptor: " + ec.message());
    }

    (void)listener.acceptor.set_option(asio::socket_base::reuse_address(true), ec);
    if (ec) {
        throw std::runtime_error("failed to enable SO_REUSEADDR: " + ec.message());
    }

#if defined(SO_REUSEPORT) && !defined(_WIN32)
    int enabled = 1;
    if (::setsockopt(listener.acceptor.native_handle(), SOL_SOCKET, SO_REUSEPORT, &enabled,
            sizeof(enabled)) != 0) {
        throw std::system_error(errno, std::generic_category(), "failed to enable SO_REUSEPORT");
    }
#elif !defined(_WIN32)
    throw std::runtime_error(
        "SO_REUSEPORT is required but not available on this platform/toolchain");
#endif

    (void)listener.acceptor.bind(listener.endpoint, ec);
    if (ec) {
        throw std::runtime_error("failed to bind acceptor: " + ec.message());
    }

    (void)listener.acceptor.listen(asio::socket_base::max_listen_connections, ec);
    if (ec) {
        throw std::runtime_error("failed to listen: " + ec.message());
    }

    listener.endpoint = listener.acceptor.local_endpoint(ec);
    if (ec) {
        throw std::runtime_error("failed to read local endpoint: " + ec.message());
    }
}

void WebWorkerRuntime::configureTlsContext(HttpServerSessionConfig& listener) {
    listener.sniContexts.clear();
    listener.sniLookup.clear();
    const auto* tls = listener.tls();
    if (tls == nullptr) {
        listener.tlsContext.reset();
        return;
    }
    const auto configure = [tls](asio::ssl::context& context,
                               const HttpServerListenerDefinition::TlsIdentity& identity) {
        context.set_options(asio::ssl::context::default_workarounds | asio::ssl::context::no_sslv2 |
                            asio::ssl::context::no_sslv3 | asio::ssl::context::no_tlsv1 |
                            asio::ssl::context::no_tlsv1_1 | asio::ssl::context::single_dh_use);
        SSL_CTX_set_options(context.native_handle(), SSL_OP_NO_COMPRESSION);
        SSL_CTX_set_alpn_select_cb(context.native_handle(), selectAlpnProtocol, nullptr);
        configureHttpServerTlsIdentity(
            context.native_handle(), identity, tls->clientCertificates);
    };

    // Per-host SNI certificates first, so the lookup can point at stable storage.
    listener.sniContexts.reserve(tls->sniIdentities.size());
    for (const auto& sni : tls->sniIdentities) {
        auto& context = listener.sniContexts.emplace_back(asio::ssl::context::tls_server);
        configure(context, sni.identity);
    }
    listener.sniLookup.reserve(tls->sniIdentities.size());
    for (std::size_t i = 0; i < tls->sniIdentities.size(); ++i) {
        listener.sniLookup.emplace_back(tls->sniIdentities[i].host, &listener.sniContexts[i]);
    }

    listener.tlsContext.emplace(asio::ssl::context::tls_server);
    auto& context = *listener.tlsContext;
    configure(context, tls->identity);
    if (!listener.sniLookup.empty()) {
        SSL_CTX_set_tlsext_servername_callback(context.native_handle(), &selectSniContext);
        SSL_CTX_set_tlsext_servername_arg(context.native_handle(), &listener.sniLookup);
    }
}

void WebWorkerRuntime::stopAdmissionOnContext() noexcept {
    networkServing_.store(false, std::memory_order_release);
    stopSource_.requestStop();
    webWorkerDispatch_->close();
    workerRuntime_.close();

    std::error_code ignored;
    for (const auto& listener : acceptors_) {
        (void)listener->acceptor.cancel(ignored);
        ignored.clear();
        (void)listener->acceptor.close(ignored);
        ignored.clear();
    }
    serveSignal_.notify();
    if (http3Server_ != nullptr) {
        http3Server_->requestStop();
    }
    connectionScanner_.stop();
    connectionScanner_.closeAll();
    workerRuntime_.stopTimers();
}

void WebWorkerRuntime::stopOnContext() noexcept {
    if (!httpServerWorkerRunning(workerState_)) {
        capabilities_.closeNow();
        if (connectionOwnershipMode_ == ConnectionOwnershipMode::kTransferredSessions) {
            finalizeSignal_.notify();
        }
        finalizeGuard_.reset();
        return;
    }

    stopAdmissionOnContext();
    capabilities_.closeNow();
    workerState_ = HttpServerWorkerState::kStopped;
    if (connectionOwnershipMode_ == ConnectionOwnershipMode::kTransferredSessions) {
        finalizeSignal_.notify();
    }
    finalizeGuard_.reset();
}

void WebWorkerRuntime::failWorker(const std::exception_ptr& failure) noexcept {
    if (!workerCompletion_.recordWorkerFailure(failure)) {
        return;
    }
    // Counted after the dedupe above, so a worker failing once counts once.
    workerFailures_.fetch_add(1, std::memory_order_relaxed);
    (void)lifecycle_.requestStop();
    workerRuntime_.close();
    stopAdmissionOnContext();
    // A standalone worker has no external network producer to quiesce. App
    // workers defer owner-thread teardown until the App has joined network.
    if (connectionOwnershipMode_ == ConnectionOwnershipMode::kOwnListeners) {
        stopOnContext();
    }
    options_.workerFailure.notify(failure);
}

void WebWorkerRuntime::runIoContext() noexcept {
    bool workerFailed = false;
    try {
        workerRuntime_.run(
            [this] {
                capabilities_.initializeWorkerState();
                asio::co_spawn(ioContext_, ruvia::asAwaitable(runWorker()),
                    asio::bind_allocator(asio::recycling_allocator<void>(), asio::detached));
            },
            [this, &workerFailed](const std::exception_ptr& failure) noexcept {
                workerFailed = true;
                (void)workerCompletion_.markStartupFailed(failure);
                failWorker(failure);
                if (connectionOwnershipMode_ == ConnectionOwnershipMode::kOwnListeners) {
                    lifecycle_.completeStop();
                    workerState_ = HttpServerWorkerState::kStopped;
                }
            },
            [this]() noexcept { capabilities_.shutdownWorkerState(); });
    } catch (...) {
        const auto failure = std::current_exception();
        (void)workerCompletion_.markStartupFailed(failure);
        failWorker(failure);
        if (connectionOwnershipMode_ == ConnectionOwnershipMode::kOwnListeners) {
            lifecycle_.completeStop();
            workerState_ = HttpServerWorkerState::kStopped;
        }
        return;
    }
    if (workerFailed) {
        return;
    }

    lifecycle_.completeStop();
    workerState_ = HttpServerWorkerState::kStopped;
    (void)workerCompletion_.markStartupFailed(std::make_exception_ptr(
        std::runtime_error("http server worker stopped before startup completed")));
}

Task<void> WebWorkerRuntime::runWorker() {
    if (!httpServerWorkerRunning(workerState_)) {
        co_return;
    }
    bool backgroundJoinStarted = false;
    try {
        connectionScanner_.start();
        co_await capabilities_.connect();
        if (http3Server_ != nullptr && !stopToken_.stopRequested()) {
            // Spawn first: run() is lazy but TaskScope starts it synchronously on
            // this worker, so it can wait for install() without allocating after
            // the scheduler becomes live.
            backgroundTasks_.spawn(http3Server_->run());
            if (!http3Server_->install()) {
                throw std::runtime_error("failed to install HTTP/3 worker bridge");
            }
        }
        (void)workerCompletion_.markStartupReady();

        while (!serveRequested_ && httpServerWorkerRunning(workerState_) &&
               !stopToken_.stopRequested()) {
            co_await serveSignal_.wait();
        }
        if (!serveRequested_ || !httpServerWorkerRunning(workerState_) ||
            stopToken_.stopRequested()) {
            workerCompletion_.markServingAborted();
        } else {
            if (options_.documentRoot.refreshOptions() != nullptr) {
                backgroundTasks_.spawn(staticRootRefreshLoop());
            }
            if (connectionOwnershipMode_ == ConnectionOwnershipMode::kOwnListeners) {
                for (std::size_t i = 0; i < listeners_.size(); ++i) {
                    backgroundTasks_.spawn(superviseListener(i, *acceptors_[i]));
                }
            }
            networkServing_.store(true, std::memory_order_release);
            (void)workerCompletion_.markServing();
            if (connectionOwnershipMode_ == ConnectionOwnershipMode::kTransferredSessions) {
                // App workers remain alive until network has joined and the run
                // thread sends the reliable finalization control.
                if (!stopToken_.stopRequested()) {
                    co_await serveSignal_.wait();
                }
            } else {
                backgroundJoinStarted = true;
                co_await backgroundTasks_.join();
            }
        }

    } catch (...) {
        const auto failure = std::current_exception();
        (void)workerCompletion_.markStartupFailed(failure);
        failWorker(failure);
    }
    if (connectionOwnershipMode_ == ConnectionOwnershipMode::kTransferredSessions) {
        // Preserve the owner coroutine and worker-local capabilities until the
        // network-quiesced barrier. Admission cancellation above may finish work,
        // but must not join/retire its owner before phase two.
        co_await finalizeSignal_.wait();
    }
    // A completed child leaves an open scope even when size() is already zero.
    // Join exactly once regardless of whether its last completion beat stop.
    if (!backgroundJoinStarted) {
        try {
            co_await backgroundTasks_.join();
        } catch (...) {
            const auto failure = std::current_exception();
            (void)workerCompletion_.markStartupFailed(failure);
            failWorker(failure);
        }
    }
    try {
        co_await capabilities_.join();
    } catch (...) {
        const auto failure = std::current_exception();
        (void)workerCompletion_.markStartupFailed(failure);
        failWorker(failure);
    }
}

Task<void> WebWorkerRuntime::staticRootRefreshLoop() {
    const auto* refreshOptions = options_.documentRoot.refreshOptions();
    if (refreshOptions == nullptr) {
        std::terminate();
    }
    const auto interval = refreshOptions->refreshInterval;
    const auto reclaimRetiredRoots = [this]() noexcept {
        std::erase_if(retiredDocumentRoots_, [](const DocumentRootPtr& root) {
            return root == nullptr || !StaticRootAccess::hasActiveBindings(*root);
        });
    };
    for (;;) {
        if (!httpServerWorkerRunning(workerState_)) {
            co_return;
        }
        if (co_await sleepFor(workerRuntime_.handle(), interval) ==
            TimerSleepResult::kStopRequested) {
            co_return;
        }
        if (!httpServerWorkerRunning(workerState_)) {
            co_return;
        }

        // Lease counts belong to the snapshot they protect. Reclaim old
        // generations independently; a long request on an older generation
        // must not pin every newer generation published by polling.
        reclaimRetiredRoots();

        const auto* currentRoot = options_.documentRoot.root();
        if (currentRoot == nullptr) {
            // The validated document-root configuration owns this invariant. Keep
            // the loop defensive anyway: a broken runtime binding must not
            // turn a background task into a null dereference on the worker.
            documentRootRefreshFailures_.fetch_add(1, std::memory_order_relaxed);
            co_return;
        }

        std::filesystem::path rootPath;
        std::optional<StaticRootConfigStorage> rootConfig;
        try {
            // Both operations copy PMR-backed configuration. They are outside
            // tryRunBlocking because the source snapshot is worker-owned, but a
            // transient allocation failure here is still a refresh failure,
            // not a reason to terminate the listener and discard its last
            // complete index.
            rootPath = currentRoot->path();
            rootConfig.emplace(StaticRootAccess::copyConfig(*currentRoot, processResource()));
        } catch (...) {
            documentRootRefreshFailures_.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        DocumentRootPtr candidate(nullptr, PmrObjectDeleter<StaticRoot>{processResource()});
        try {
            auto rebuilt = co_await ruvia::tryRunBlocking(*options_.blockingPool,
                workerRuntime_.handle(),
                [rootPath = std::move(rootPath), rootConfig = std::move(*rootConfig)]() mutable {
                    return StaticRootAccess::make(
                        processResource(), rootPath, std::move(rootConfig));
                });
            if (!rebuilt.completed()) {
                if (rebuilt.failed()) {
                    // A refresh is transactional: keep serving the last complete
                    // index, but expose repeated filesystem/permission failures to
                    // metrics instead of silently turning them into stale content.
                    documentRootRefreshFailures_.fetch_add(1, std::memory_order_relaxed);
                }
                if (!httpServerWorkerRunning(workerState_)) {
                    co_return;
                }
                continue;
            }

            candidate = std::move(rebuilt).value();
        } catch (...) {
            // The offload wrapper reports queue/pool shutdown as a status, but
            // creating its one-shot channel or transporting a result can still
            // fail with an allocation/runtime exception. Refresh is best effort:
            // retain the last complete snapshot and keep the listener alive.
            documentRootRefreshFailures_.fetch_add(1, std::memory_order_relaxed);
            if (!httpServerWorkerRunning(workerState_)) {
                co_return;
            }
            continue;
        }

        if (!httpServerWorkerRunning(workerState_)) {
            co_return;
        }
        if (StaticRootAccess::fingerprint(*candidate) ==
                StaticRootAccess::fingerprint(*currentRoot) &&
            StaticRootAccess::sameSnapshot(*candidate, *currentRoot)) {
            continue;
        }

        const auto precompression = documentRootPrecompressionOptions(options_);
        if (precompression.enabled()) {
            try {
                auto prepared =
                    co_await ruvia::tryRunBlocking(*options_.blockingPool, workerRuntime_.handle(),
                        [candidate = std::move(candidate), precompression]() mutable {
                            StaticRootAccess::installPrecompressedVariants(
                                *candidate, nullptr, precompression);
                            return std::move(candidate);
                        });
                if (!prepared.completed()) {
                    if (prepared.failed()) {
                        documentRootRefreshFailures_.fetch_add(1, std::memory_order_relaxed);
                    }
                    if (!httpServerWorkerRunning(workerState_)) {
                        co_return;
                    }
                    continue;
                }
                candidate = std::move(prepared).value();
            } catch (...) {
                documentRootRefreshFailures_.fetch_add(1, std::memory_order_relaxed);
                if (!httpServerWorkerRunning(workerState_)) {
                    co_return;
                }
                continue;
            }
            if (!httpServerWorkerRunning(workerState_)) {
                co_return;
            }
        }

        // A binding is a request-scoped lease. If no request can still hold
        // the current index, replacing it may destroy the old root directly;
        // otherwise retain the old immutable snapshot until every in-flight
        // dispatch releases its move-only binding. Publishing a raw pointer
        // without this retirement step leaves a suspended coroutine with a
        // dangling StaticRoot after the next poll.
        try {
            if (ownedDocumentRoot_ != nullptr &&
                StaticRootAccess::hasActiveBindings(*ownedDocumentRoot_)) {
                // A vector growth is the only fallible part of publication.
                // The old pointer remains owned by this server if growth is
                // rejected, so the candidate can be discarded and the next
                // poll can retry without exposing a half-published root.
                retiredDocumentRoots_.push_back(std::move(ownedDocumentRoot_));
            }
        } catch (...) {
            documentRootRefreshFailures_.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        ownedDocumentRoot_ = std::move(candidate);
        options_.documentRoot.publish(*ownedDocumentRoot_);
    }
}

}  // namespace ruvia::detail
