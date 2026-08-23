#include "ruvia/web/detail/server/WebWorkerRuntime.h"
#include "ruvia/core/detail/util/FailureReport.h"
#include "ruvia/core/detail/worker/WorkerDispatcher.h"
#include "ruvia/web/detail/app/WebWorkerDispatch.h"

#include "ruvia/web/detail/server/tls/HttpServerTlsVerify.h"

#include <asio/bind_allocator.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/post.hpp>
#include <asio/recycling_allocator.hpp>
#include <asio/ssl/context.hpp>
#include <asio/ssl/error.hpp>
#include <asio/system_error.hpp>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <memory>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

#if !defined(_WIN32)
#include <sys/socket.h>
#endif

#include "ruvia/core/detail/io/ConnectionScanner.h"
#include "ruvia/core/Timer.h"
#include "ruvia/core/memory/ProcessResource.h"
#include "ruvia/web/detail/http/static/StaticRootIndex.h"
#include "ruvia/web/detail/server/HttpServerOptionsValidation.h"
#include "ruvia/web/detail/router/RouteTable.h"
#include "ruvia/http/detail/field/HeaderTokenUtils.h"
#include "ruvia/core/detail/io/AsioAwait.h"
#include "ruvia/http/detail/util/AsciiCase.h"

namespace ruvia::detail {

using TcpEndpoint = asio::ip::tcp::endpoint;

namespace {

int selectAlpnProtocol(SSL*, const unsigned char** out, unsigned char* outLength, const unsigned char* in, unsigned int inLength, void*) noexcept {
    // Only h2 and http/1.1 are offered. HTTP/3 / QUIC is explicitly not supported (no "h3"
    // token, no UDP/QUIC listener); a peer offering only h3 falls back to http/1.1 or fails ALPN.
    static constexpr unsigned char protocols[] = {2, 'h', '2', 8, 'h', 't', 't', 'p', '/', '1', '.', '1'};
    if (SSL_select_next_proto(const_cast<unsigned char**>(out), outLength, protocols, static_cast<unsigned int>(sizeof(protocols)), in, inLength) == OPENSSL_NPN_NEGOTIATED) {
        return SSL_TLSEXT_ERR_OK;
    }
    return SSL_TLSEXT_ERR_NOACK;
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

int copyPrivateKeyPassword(char* buffer, int bufferSize, int, void* userData) noexcept {
    if (buffer == nullptr || bufferSize <= 0 || userData == nullptr) {
        return 0;
    }

    const auto& password = *static_cast<const std::pmr::string*>(userData);
    const auto capacity = static_cast<std::size_t>(bufferSize);
    if (password.size() >= capacity) {
        return 0;
    }

    std::memcpy(buffer, password.data(), password.size());
    buffer[password.size()] = '\0';
    return static_cast<int>(password.size());
}

[[nodiscard]] asio::error_code translateOpenSslError(unsigned long error) {
#if (OPENSSL_VERSION_NUMBER >= 0x30000000L)
    if (ERR_SYSTEM_ERROR(error)) {
        return asio::error_code(static_cast<int>(ERR_GET_REASON(error)), asio::error::get_system_category());
    }
#endif
    return asio::error_code(static_cast<int>(error), asio::error::get_ssl_category());
}

[[noreturn]] void throwTlsContextFileError(const char* operation) {
    throw asio::system_error(translateOpenSslError(::ERR_get_error()), operation);
}

void useCertificateChainFile(asio::ssl::context& context, const std::pmr::string& filename) {
    ::ERR_clear_error();
    if (::SSL_CTX_use_certificate_chain_file(context.native_handle(), filename.c_str()) != 1) {
        throwTlsContextFileError("use_certificate_chain_file");
    }
}

void usePrivateKeyFile(asio::ssl::context& context, const std::pmr::string& filename) {
    ::ERR_clear_error();
    if (::SSL_CTX_use_PrivateKey_file(context.native_handle(), filename.c_str(), SSL_FILETYPE_PEM) != 1) {
        throwTlsContextFileError("use_private_key_file");
    }
}

void loadVerifyFile(asio::ssl::context& context, const std::pmr::string& filename) {
    ::ERR_clear_error();
    if (::SSL_CTX_load_verify_locations(context.native_handle(), filename.c_str(), nullptr) != 1) {
        throwTlsContextFileError("load_verify_file");
    }
}

[[nodiscard]] ConnectionScannerOptions makeConnectionScannerOptions(const HttpServerOptions& options) noexcept {
    return ConnectionScannerOptions{.scanInterval = options.scanInterval, .idleTimeout = options.idleTimeout, .initialReadTimeout = options.requestHeaderTimeout, .payloadReadTimeout = options.requestBodyTimeout, .writeTimeout = options.writeTimeout};
}

}  // namespace

WebWorkerRuntime::WebWorkerRuntime(TcpEndpoint endpoint, const RouteTable& routes, WorkerCapabilityDefinitions capabilities, HttpServerOptions options)
    : WebWorkerRuntime(HttpServerListenerDefinition(std::move(endpoint)), routes, capabilities, std::move(options)) {}

WebWorkerRuntime::WebWorkerRuntime(HttpServerListenerDefinition listener, const RouteTable& routes, WorkerCapabilityDefinitions capabilities, HttpServerOptions options)
    : WebWorkerRuntime(std::span<const HttpServerListenerDefinition>(&listener, 1), routes, capabilities, std::move(options)) {}

WebWorkerRuntime::WebWorkerRuntime(std::span<const HttpServerListenerDefinition> listeners, const RouteTable& routes, WorkerCapabilityDefinitions capabilities, HttpServerOptions options)
    : WebWorkerRuntime(ValidatedOptionsTag{}, listeners, routes, capabilities, validatedHttpServerOptions(std::move(options))) {}

WebWorkerRuntime::WebWorkerRuntime(ValidatedOptionsTag, std::span<const HttpServerListenerDefinition> listeners, const RouteTable& routes, WorkerCapabilityDefinitions capabilities, HttpServerOptions validatedOptions)
    // One worker thread runs all I/O on this context; cross-thread access is
    // limited to stop()'s asio::post, which UNSAFE_IO keeps locked. Only the
    // reactor's per-descriptor I/O locking is elided.
    : ioContext_(ASIO_CONCURRENCY_HINT_UNSAFE_IO),
      serveGate_(ioContext_, std::chrono::steady_clock::time_point::max()),
      workerRuntime_(ioContext_, validatedOptions.workerMailboxCapacity),
      routes_(routes),
      memory_(validatedOptions.memoryConfig),
      listeners_(memory_.resource()),
      backgroundTasks_(workerRuntime_.handle(), {.resource = memory_.resource()}),
      ownedDocumentRoot_(nullptr, PmrObjectDeleter<StaticRoot>{processResource()}),
      retiredDocumentRoots_(memory_.resource()),
      options_(std::move(validatedOptions)),
      connectionScanner_(workerRuntime_.handle(), makeConnectionScannerOptions(options_)),
      capabilities_(
          ioContext_,
          workerRuntime_.handle(),
          memory_.resource(),
          capabilities,
          WorkerCapabilityOptions{
              .maxHttpClientOrigins = options_.httpClientOriginCacheCapacityPerWorker,
              .defaultRateLimit = options_.defaultRateLimitPerWorker,
              .routeRateLimits = routes_.hasRouteRateLimit() ? RouteRateLimitPresence::kPresent : RouteRateLimitPresence::kAbsent,
              .rateLimitCapacity = options_.rateLimitCapacityPerWorker,
              .maxDecodedBodyBytes = options_.maxBufferedBodyBytes,
              .blockingPool = options_.blockingPool,
              .env = options_.env,
              .trustedProxies = options_.trustedProxies.empty() ? nullptr : &options_.trustedProxies,
              .precompressedStaticFiles = options_.compression.has_value(),
          },
          connectionScanner_),
      webWorkerDispatch_(std::make_shared<WebWorkerDispatch>(ioContext_.get_executor(), workerRuntime_.handle(), memory_.resource(), capabilities_, [this](std::exception_ptr failure) { failWorker(std::move(failure)); })),
      workSetPool_(memory_) {
    if (listeners.empty()) {
        throw std::invalid_argument("HTTP server worker requires at least one listener");
    }
    listeners_.reserve(listeners.size());
    for (const auto& listener : listeners) {
        validateHttpServerListener(listener);
        listeners_.push_back(makePmrObject<HttpServerListener>(memory_.resource(), ioContext_, listener, memory_.resource()));
    }
    if (options_.documentRoot.refreshOptions() != nullptr) {
        const auto* configuredRoot = options_.documentRoot.root();
        if (configuredRoot == nullptr) std::terminate();
        auto rootOptions = StaticRootAccess::options(*configuredRoot);
        ownedDocumentRoot_ = makePmrObject<StaticRoot>(processResource(), configuredRoot->path(), std::move(rootOptions));
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
        reportUnhandledFailure("web server worker", std::current_exception());
    }
    // Retire the execution context first. Failure shutdown already releases
    // abandoned mailbox tasks on the worker; detach defensively releases any
    // task left by a context that stopped outside the managed run loop. Each
    // dropped WebWorker task reconciles its outstanding_ reservation before
    // retire() checks it. Public handles may outlive this server, so detach also
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
    for (const auto& listener : listeners_) {
        configureAcceptor(*listener);
        configureTlsContext(*listener);
    }
    prepared_ = true;
}

void WebWorkerRuntime::launch() {
    if (!prepared_) {
        throw std::logic_error("web worker runtime must be prepared before launch");
    }
    if (!lifecycle_.start()) {
        throw std::logic_error("web worker runtime cannot be launched twice");
    }
    workerState_ = HttpServerWorkerState::kRunning;

    try {
        workerThread_ = std::thread([this] { runIoContext(); });
    } catch (...) {
        (void)lifecycle_.requestStop();
        if (workerThread_.joinable()) {
            workerThread_.join();
        } else {
            stopOnContext();
        }
        lifecycle_.completeStop();
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
        if (!httpServerWorkerRunning(workerState_) || serveRequested_) {
            return;
        }
        serveRequested_ = true;
        std::error_code ignored;
        serveGate_.cancel(ignored);
    });
}

bool WebWorkerRuntime::waitUntilServing() {
    return workerCompletion_.waitForServing();
}

void WebWorkerRuntime::stop() {
    if (!lifecycle_.requestStop()) {
        return;
    }

    webWorkerDispatch_->close();
    workerRuntime_.close();
    asio::post(ioContext_, [this] { stopOnContext(); });
}

void WebWorkerRuntime::join() {
    if (workerRuntime_.handle().isCurrent()) {
        throw std::logic_error("cannot join a Web worker runtime from its worker");
    }
    if (workerThread_.joinable()) {
        workerThread_.join();
    }
    lifecycle_.completeStop();
    const auto failure = workerCompletion_.workerFailure();
    if (failure != nullptr) {
        std::rethrow_exception(failure);
    }
}

TcpEndpoint WebWorkerRuntime::localEndpoint(std::size_t listenerIndex) const {
    if (listenerIndex >= listeners_.size()) {
        throw std::out_of_range("listener index is not configured on this worker");
    }
    return listeners_[listenerIndex]->endpoint;
}

HttpServerStats WebWorkerRuntime::stats() const noexcept {
    HttpServerStats stats;
    stats.activeConnections = activeConnectionCount_.load(std::memory_order_relaxed);
    stats.connectionsRefused = connectionsRefused_.load(std::memory_order_relaxed);
    stats.connectionFailures = connectionFailures_.load(std::memory_order_relaxed);
    stats.acceptFailures = acceptFailures_.load(std::memory_order_relaxed);
    stats.workerFailures = workerFailures_.load(std::memory_order_relaxed);
    stats.documentRootRefreshFailures = documentRootRefreshFailures_.load(std::memory_order_relaxed);
    return stats;
}

WebWorkerHandle WebWorkerRuntime::webWorker() const {
    return webWorkerDispatch_->handle();
}
void WebWorkerRuntime::configureAcceptor(HttpServerListener& listener) {
    std::error_code ec;

    listener.acceptor.open(listener.endpoint.protocol(), ec);
    if (ec) {
        throw std::runtime_error("failed to open acceptor: " + ec.message());
    }

    listener.acceptor.set_option(asio::socket_base::reuse_address(true), ec);
    if (ec) {
        throw std::runtime_error("failed to enable SO_REUSEADDR: " + ec.message());
    }

#if defined(SO_REUSEPORT) && !defined(_WIN32)
    int enabled = 1;
    if (::setsockopt(listener.acceptor.native_handle(), SOL_SOCKET, SO_REUSEPORT, &enabled, sizeof(enabled)) != 0) {
        throw std::system_error(errno, std::generic_category(), "failed to enable SO_REUSEPORT");
    }
#elif !defined(_WIN32)
    throw std::runtime_error("SO_REUSEPORT is required but not available on this platform/toolchain");
#endif

    listener.acceptor.bind(listener.endpoint, ec);
    if (ec) {
        throw std::runtime_error("failed to bind acceptor: " + ec.message());
    }

    listener.acceptor.listen(asio::socket_base::max_listen_connections, ec);
    if (ec) {
        throw std::runtime_error("failed to listen: " + ec.message());
    }

    listener.endpoint = listener.acceptor.local_endpoint(ec);
    if (ec) {
        throw std::runtime_error("failed to read local endpoint: " + ec.message());
    }
}

void WebWorkerRuntime::configureTlsContext(HttpServerListener& listener) {
    listener.sniContexts.clear();
    listener.sniLookup.clear();
    const auto* tls = listener.tls();
    if (tls == nullptr) {
        listener.tlsContext.reset();
        return;
    }
    if (tls->identity.certificateChainFile.empty() || tls->identity.privateKeyFile.empty()) {
        throw std::invalid_argument("TLS requires certificate chain and private key files");
    }

    const auto configure = [tls](asio::ssl::context& context, const std::pmr::string& certificateChainFile, const std::pmr::string& privateKeyFile, const std::pmr::string& privateKeyPassword) {
        context.set_options(asio::ssl::context::default_workarounds | asio::ssl::context::no_sslv2 | asio::ssl::context::no_sslv3 | asio::ssl::context::no_tlsv1 | asio::ssl::context::no_tlsv1_1 | asio::ssl::context::single_dh_use);
        SSL_CTX_set_options(context.native_handle(), SSL_OP_NO_COMPRESSION);
        SSL_CTX_set_alpn_select_cb(context.native_handle(), selectAlpnProtocol, nullptr);
        if (!privateKeyPassword.empty()) {
            SSL_CTX_set_default_passwd_cb(context.native_handle(), copyPrivateKeyPassword);
            SSL_CTX_set_default_passwd_cb_userdata(context.native_handle(), const_cast<std::pmr::string*>(&privateKeyPassword));
        }
        useCertificateChainFile(context, certificateChainFile);
        usePrivateKeyFile(context, privateKeyFile);
        if (tls->clientCertificates.has_value()) {
            loadVerifyFile(context, tls->clientCertificates->verifyFile);
            context.set_verify_mode(httpServerTlsVerifyMode(tls->clientCertificates->requirement));
        }
    };

    // Per-host SNI certificates first, so the lookup can point at stable storage.
    listener.sniContexts.reserve(tls->sniIdentities.size());
    for (const auto& sni : tls->sniIdentities) {
        auto& context = listener.sniContexts.emplace_back(asio::ssl::context::tls_server);
        configure(context, sni.identity.certificateChainFile, sni.identity.privateKeyFile, sni.identity.privateKeyPassword);
    }
    listener.sniLookup.reserve(tls->sniIdentities.size());
    for (std::size_t i = 0; i < tls->sniIdentities.size(); ++i) {
        listener.sniLookup.emplace_back(tls->sniIdentities[i].host, &listener.sniContexts[i]);
    }

    listener.tlsContext.emplace(asio::ssl::context::tls_server);
    auto& context = *listener.tlsContext;
    configure(context, tls->identity.certificateChainFile, tls->identity.privateKeyFile, tls->identity.privateKeyPassword);
    if (!listener.sniLookup.empty()) {
        SSL_CTX_set_tlsext_servername_callback(context.native_handle(), &selectSniContext);
        SSL_CTX_set_tlsext_servername_arg(context.native_handle(), &listener.sniLookup);
    }
}

void WebWorkerRuntime::stopOnContext() noexcept {
    if (!httpServerWorkerRunning(workerState_)) {
        return;
    }

    workerState_ = HttpServerWorkerState::kStopped;
    stopSource_.requestStop();
    webWorkerDispatch_->close();
    workerRuntime_.close();
    std::error_code ignored;
    for (const auto& listener : listeners_) {
        listener->acceptor.cancel(ignored);
        ignored.clear();
        listener->acceptor.close(ignored);
        ignored.clear();
    }
    serveGate_.cancel(ignored);
    ignored.clear();
    connectionScanner_.stop();
    connectionScanner_.closeAll();
    capabilities_.closeNow();
    workerRuntime_.stopTimers();
}

void WebWorkerRuntime::failWorker(std::exception_ptr failure) noexcept {
    if (!workerCompletion_.recordWorkerFailure(failure)) {
        return;
    }
    // Counted after the dedupe above, so a worker failing once counts once.
    workerFailures_.fetch_add(1, std::memory_order_relaxed);
    (void)lifecycle_.requestStop();
    options_.workerFailure.notify(failure);
    stopOnContext();
}

void WebWorkerRuntime::runIoContext() noexcept {
    bool workerFailed = false;
    try {
        workerRuntime_.run(
            [this] {
                capabilities_.initializeWorkerState();
                asio::co_spawn(ioContext_, taskAsAwaitable(runWorker()), asio::bind_allocator(asio::recycling_allocator<void>(), asio::detached));
            },
            [this, &workerFailed](std::exception_ptr failure) noexcept {
                workerFailed = true;
                (void)workerCompletion_.markStartupFailed(failure);
                failWorker(std::move(failure));
                lifecycle_.completeStop();
                workerState_ = HttpServerWorkerState::kStopped;
            },
            [this]() noexcept { capabilities_.shutdownWorkerState(); });
    } catch (...) {
        const auto failure = std::current_exception();
        (void)workerCompletion_.markStartupFailed(failure);
        failWorker(failure);
        lifecycle_.completeStop();
        workerState_ = HttpServerWorkerState::kStopped;
        return;
    }
    if (workerFailed) {
        return;
    }

    lifecycle_.completeStop();
    workerState_ = HttpServerWorkerState::kStopped;
    (void)workerCompletion_.markStartupFailed(std::make_exception_ptr(std::runtime_error("http server worker stopped before startup completed")));
}

Task<void> WebWorkerRuntime::runWorker() {
    if (!httpServerWorkerRunning(workerState_)) {
        co_return;
    }
    bool backgroundJoinStarted = false;
    try {
        connectionScanner_.start();
        co_await capabilities_.connect();
        (void)workerCompletion_.markStartupReady();

        const auto serveCompletion = co_await asyncAsio([this](auto handler) mutable {
            serveGate_.async_wait(std::move(handler));
        });
        const auto serveError = serveCompletion.errorCode();
        if (serveError && serveError != asio::error::operation_aborted) {
            throw std::system_error(serveError, "web worker serve gate failed");
        }
        if (!serveRequested_ || !httpServerWorkerRunning(workerState_)) {
            workerCompletion_.markServingAborted();
            co_await capabilities_.join();
            co_return;
        }

        if (options_.documentRoot.refreshOptions() != nullptr) {
            backgroundTasks_.spawn(staticRootRefreshLoop());
        }
        for (const auto& listener : listeners_) {
            backgroundTasks_.spawn(superviseListener(*listener));
        }
        (void)workerCompletion_.markServing();
        backgroundJoinStarted = true;
        co_await backgroundTasks_.join();
    } catch (...) {
        const auto failure = std::current_exception();
        (void)workerCompletion_.markStartupFailed(failure);
        failWorker(failure);
    }
    if (!backgroundJoinStarted && backgroundTasks_.size() != 0) {
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
    if (refreshOptions == nullptr) std::terminate();
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
        if (co_await sleepFor(workerRuntime_.handle(), interval) == TimerSleepResult::kStopRequested) {
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
        StaticRootOptions rootOptions;
        try {
            // Both operations copy PMR-backed configuration. They are outside
            // tryRunBlocking because the source snapshot is worker-owned, but a
            // transient allocation failure here is still a refresh failure,
            // not a reason to terminate the listener and discard its last
            // complete index.
            rootPath = currentRoot->path();
            rootOptions = StaticRootAccess::options(*currentRoot);
        } catch (...) {
            documentRootRefreshFailures_.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        DocumentRootPtr candidate(nullptr, PmrObjectDeleter<StaticRoot>{processResource()});
        try {
            auto rebuilt = co_await ruvia::tryRunBlocking(*options_.blockingPool, workerRuntime_.handle(), [rootPath = std::move(rootPath), rootOptions = std::move(rootOptions)]() mutable {
                return makePmrObject<StaticRoot>(processResource(), rootPath, std::move(rootOptions));
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
        if (StaticRootAccess::fingerprint(*candidate) == StaticRootAccess::fingerprint(*currentRoot) && StaticRootAccess::sameSnapshot(*candidate, *currentRoot)) {
            continue;
        }

        // A binding is a request-scoped lease. If no request can still hold
        // the current index, replacing it may destroy the old root directly;
        // otherwise retain the old immutable snapshot until every in-flight
        // dispatch releases its move-only binding. Publishing a raw pointer
        // without this retirement step leaves a suspended coroutine with a
        // dangling StaticRoot after the next poll.
        try {
            if (ownedDocumentRoot_ != nullptr && StaticRootAccess::hasActiveBindings(*ownedDocumentRoot_)) {
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
