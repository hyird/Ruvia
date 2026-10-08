#include "ruvia/web/detail/server/WebWorkerRuntime.h"

#include <algorithm>
#include <array>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

#include <asio/bind_allocator.hpp>
#include <asio/co_spawn.hpp>
#include <asio/recycling_allocator.hpp>
#include <asio/ssl/context.hpp>
#include <openssl/ssl.h>

#include "ruvia/core/ConnectionScanner.h"
#include "ruvia/core/FailureReport.h"
#include "ruvia/core/memory/ProcessResource.h"
#include "ruvia/http/HttpAscii.h"
#include "ruvia/web/detail/app/WebWorkerDispatch.h"
#include "ruvia/web/detail/http/static/StaticRootIndex.h"
#include "ruvia/web/detail/http3/http3_datagram_channel.h"
#include "ruvia/web/detail/http3/http3_worker.h"
#include "ruvia/web/detail/router/RouteTable.h"
#include "ruvia/web/detail/server/HttpServerOptionsValidation.h"
#include "ruvia/web/detail/server/acceptor.h"
#include "ruvia/web/detail/server/tls/HttpServerTlsIdentity.h"

namespace ruvia::detail {

using TcpEndpoint = asio::ip::tcp::endpoint;

namespace {

int selectAlpnProtocol(SSL*, const unsigned char** out, unsigned char* outLength,
    const unsigned char* in, unsigned int inLength, void*) noexcept {
    // This is the TCP TLS context, so it offers only h2 and http/1.1. HTTP/3
    // negotiates "h3" on this worker's separate QUIC/TLS context and is never
    // advertised through this TCP callback.
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
        .writeTimeout = options.writeTimeout,
        .initial_read_completion_timeout = options.header_completion_timeout,
        .payload_read_completion_timeout = options.body_completion_timeout};
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
          capabilities, true) {}

WebWorkerRuntime::WebWorkerRuntime(const ValidatedHttpServerConfiguration& configuration,
    const RouteTable& routes, WorkerCapabilityDefinitions capabilities)
    : WebWorkerRuntime(configuration, routes, capabilities, false) {}

WebWorkerRuntime::WebWorkerRuntime(const ValidatedHttpServerConfiguration& configuration,
    const RouteTable& routes, WorkerCapabilityDefinitions capabilities,
    bool own_acceptor)
    : WebWorkerRuntime(ValidatedConfigurationTag{}, configuration.listeners(), routes, capabilities,
          configuration.options(), own_acceptor) {}

WebWorkerRuntime::WebWorkerRuntime(ValidatedConfigurationTag,
    std::span<const HttpServerListenerDefinition> listeners, const RouteTable& routes,
    WorkerCapabilityDefinitions capabilities, HttpServerOptions validatedOptions,
    bool own_acceptor)
    : runtime_({.queue_capacity = validatedOptions.worker_queue_capacity,
          .io_policy = ruvia::worker_io_policy::single_owner}),
      ioContext_(runtime_.context().ioContext()),
      workerRuntime_(runtime_.context()),
      serveSignal_(workerRuntime_.handle()),
      finalizeSignal_(workerRuntime_.handle()),
      routes_(routes),
      memory_(validatedOptions.memoryConfig),
      inbound_buffers_(memory_.resource(), validatedOptions.max_inbound_buffer_bytes_per_worker),
      listeners_(memory_.resource()),
      owned_acceptor_(nullptr, PmrObjectDeleter<acceptor>{memory_.resource()}),
      backgroundTasks_(workerRuntime_.handle(), {.resource = memory_.resource()}),
      ownedDocumentRoot_(nullptr, PmrObjectDeleter<StaticRoot>{processResource()}),
      retiredDocumentRoots_(memory_.resource()),
      options_(std::move(validatedOptions)),
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
      http3_(nullptr, PmrObjectDeleter<http3_worker>{memory_.resource()}),
      webWorkerDispatch_(std::make_shared<WebWorkerDispatch>(ioContext_.get_executor(),
          workerRuntime_.handle(), memory_.resource(), capabilities_,
          [this](const std::exception_ptr& failure) { failWorker(failure); })),
      workSetPool_(memory_) {
    options_.inbound_buffer_pool = &inbound_buffers_;
    listeners_.reserve(listeners.size());
    for (const auto& listener : listeners) {
        listeners_.push_back(makePmrObject<HttpServerSessionConfig>(
            memory_.resource(), listener, memory_.resource()));
    }
    for (std::size_t index = 0; index < listeners.size(); ++index) {
        if (listeners[index].http3) {
            http3_ = makePmrObject<http3_worker>(memory_.resource(), workerRuntime_, memory_,
                routes_, capabilities_, connectionScanner_, options_, stopToken_,
                *listeners_[index]->tls(), *listeners[index].http3,
                activeConnectionCount_, connectionsRefused_,
                http3_worker_runtime::failure_notification{this,
                    [](void* object, std::exception_ptr failure) noexcept {
                        static_cast<WebWorkerRuntime*>(object)->failWorker(failure);
                    }});
        }
    }
    if (own_acceptor) {
        const std::array targets{acceptor::worker_target{
            .submission = networkSubmission(),
            .object = this,
            .available = [](void* object) noexcept { return static_cast<WebWorkerRuntime*>(object)->availableForNetworkDispatch(); },
            .accept = [](void* object, NativeAcceptedSocketTicket&& ticket) noexcept { static_cast<WebWorkerRuntime*>(object)->acceptTransferredConnection(std::move(ticket)); },
            .stage_quic = http3_ ? +[](void* object, http3_datagram_channel& channel,
                                        asio::ip::udp::endpoint endpoint, quic_cid_partition partition) { static_cast<WebWorkerRuntime*>(object)->stage_quic(channel, endpoint, partition); }
                                 : nullptr,
        }};
        owned_acceptor_ = makePmrObject<acceptor>(memory_.resource(), listeners, targets, this,
            [](void* object) noexcept { static_cast<WebWorkerRuntime*>(object)->stopAdmission(); });
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
    runtime_.configure({
        .startup = [this] {
            workerState_ = HttpServerWorkerState::kRunning;
            capabilities_.initializeWorkerState();
            asio::co_spawn(ioContext_, ruvia::asAwaitable(runWorker()),
                asio::bind_allocator(asio::recycling_allocator<void>(),
                    [this](std::exception_ptr failure) noexcept {
                        if (failure) {
                            failWorker(failure);
                        }
                    })); },
        .stop_admission = [this] { stopAdmissionOnContext(); },
        .failure = [this](std::exception_ptr failure) noexcept {
            (void)workerCompletion_.markStartupFailed(failure);
            failWorker(failure); },
        .shutdown = [this]() noexcept {
            for (auto& listener : listeners_) {
                listener->tlsContext.reset();
                listener->sniLookup.clear();
                listener->sniContexts.clear();
            }
            capabilities_.shutdownWorkerState();
            workerState_ = HttpServerWorkerState::kStopped;
            (void)workerCompletion_.markStartupFailed(std::make_exception_ptr(
                std::runtime_error("http server worker stopped before startup completed"))); },
    });
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
    // Core has detached escaped endpoints before retiring the execution
    // context. Domain callback state can now be retired without a live producer.
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
    if (prepared_ || runtime_.state() != RuntimeLifecycle::State::kReady) {
        throw std::logic_error("web worker runtime can only be prepared once");
    }
    if (owned_acceptor_) {
        owned_acceptor_->prepare();
    }
    prepared_ = true;
}

void WebWorkerRuntime::launch() {
    if (!prepared_) {
        throw std::logic_error("web worker runtime must be prepared before launch");
    }
    try {
        if (owned_acceptor_) {
            owned_acceptor_->launch();
            owned_acceptor_->wait_until_ready();
            owned_acceptor_->rethrow_failure();
        }
        runtime_.start();
    } catch (...) {
        const auto failure = std::current_exception();
        if (runtime_.failure() != failure) {
            throw;  // A rejected lifecycle call is not a worker failure.
        }
        (void)workerCompletion_.markStartupFailed(failure);
        (void)workerCompletion_.recordWorkerFailure(failure);
        // Core still owes owner-affine draining. App performs the external
        // producer barrier before requesting phase-two finalization.
        if (!runtime_.started() && http3_ != nullptr) {
            http3_->abandon_before_launch();
        }
        throw;
    }
}

void WebWorkerRuntime::waitUntilReady() {
    if (runtime_.state() == RuntimeLifecycle::State::kReady) {
        throw std::logic_error("web worker runtime has not been launched");
    }
    workerCompletion_.waitForStartup();
}

void WebWorkerRuntime::requestServe() {
    if (runtime_.state() != RuntimeLifecycle::State::kRunning) {
        return;
    }
    (void)runtime_.post_control([this] {
        if (!httpServerWorkerRunning(workerState_) || serveRequested_ ||
            stopToken_.stopRequested()) {
            return;
        }
        serveRequested_ = true;
        serveSignal_.notify();
    });
}

bool WebWorkerRuntime::waitUntilServing() {
    const bool ready = workerCompletion_.waitForServing();
    if (!ready || !owned_acceptor_) {
        return ready;
    }
    owned_acceptor_->request_serve();
    if (!owned_acceptor_->wait_until_serving()) {
        owned_acceptor_->rethrow_failure();
        return false;
    }
    return true;
}

void WebWorkerRuntime::stop() noexcept {
    if (runtime_.state() == RuntimeLifecycle::State::kStopped) {
        return;
    }
    stopAdmission();
    if (!owned_acceptor_) {
        finalizeAfterNetworkQuiesced();
    }
}

void WebWorkerRuntime::stopAdmission() noexcept {
    if (owned_acceptor_) {
        owned_acceptor_->stop();
    }
    runtime_.request_stop();
    if (!runtime_.started() && http3_ != nullptr) {
        http3_->abandon_before_launch();
    }
}

void WebWorkerRuntime::finalizeAfterNetworkQuiesced() noexcept {
    runtime_.finalize([this] { stopOnContext(); });
}

void WebWorkerRuntime::join() {
    if (workerRuntime_.handle().isCurrent()) {
        throw std::logic_error("web worker cannot join itself");
    }
    if (owned_acceptor_) {
        owned_acceptor_->join();
        finalizeAfterNetworkQuiesced();
    }
    runtime_.join();
    const auto failure = workerCompletion_.workerFailure();
    if (failure != nullptr) {
        std::rethrow_exception(failure);
    }
    runtime_.rethrow_failure();
    if (owned_acceptor_) {
        owned_acceptor_->rethrow_failure();
    }
}

TcpEndpoint WebWorkerRuntime::localEndpoint(std::size_t listenerIndex) const {
    if (!owned_acceptor_) {
        throw std::out_of_range("listener acceptor is not configured on this worker");
    }
    return owned_acceptor_->local_endpoint(listenerIndex);
}

bool WebWorkerRuntime::availableForNetworkDispatch() const noexcept {
    if (runtime_.state() != RuntimeLifecycle::State::kRunning ||
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

void WebWorkerRuntime::stage_quic(http3_datagram_channel& channel,
    asio::ip::udp::endpoint endpoint, ruvia::quic_cid_partition partition) {
    if (!http3_ || runtime_.started()) {
        throw std::logic_error("worker QUIC channel cannot be staged in this state");
    }
    http3_->stage(channel, std::move(endpoint), partition);
    if (runtime_.state() != RuntimeLifecycle::State::kReady) {
        http3_->abandon_before_launch();
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

    serveSignal_.notify();
    stop_http3();
    connectionScanner_.stop();
    connectionScanner_.closeAll();
}

void WebWorkerRuntime::stopOnContext() noexcept {
    if (!httpServerWorkerRunning(workerState_)) {
        capabilities_.closeNow();
        finalizeSignal_.notify();
        return;
    }

    stopAdmissionOnContext();
    capabilities_.closeNow();
    workerState_ = HttpServerWorkerState::kStopped;
    finalizeSignal_.notify();
}

void WebWorkerRuntime::failWorker(const std::exception_ptr& failure) noexcept {
    if (!workerCompletion_.recordWorkerFailure(failure)) {
        return;
    }
    // Counted after the dedupe above, so a worker failing once counts once.
    workerFailures_.fetch_add(1, std::memory_order_relaxed);
    runtime_.request_stop();
    if (owned_acceptor_) {
        owned_acceptor_->stop();
    }
    options_.workerFailure.notify(failure);
}

void WebWorkerRuntime::stop_http3() noexcept {
    if (http3_) {
        http3_->stop();
    }
}

Task<void> WebWorkerRuntime::runWorker() {
    if (!httpServerWorkerRunning(workerState_)) {
        stop_http3();
        (void)workerCompletion_.markStartupFailed(
            std::make_exception_ptr(std::runtime_error("web worker startup cancelled")));
        workerCompletion_.markServingAborted();
        co_return;
    }
    try {
        for (auto& listener : listeners_) {
            configureTlsContext(*listener);
        }
        connectionScanner_.start();
        co_await capabilities_.connect();
        if (http3_ != nullptr && !stopToken_.stopRequested()) {
            http3_->start();
        }
        if (stopToken_.stopRequested()) {
            stop_http3();
            (void)workerCompletion_.markStartupFailed(
                std::make_exception_ptr(std::runtime_error("web worker startup cancelled")));
            workerCompletion_.markServingAborted();
        } else {
            (void)workerCompletion_.markStartupReady();
        }

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
            networkServing_.store(true, std::memory_order_release);
            (void)workerCompletion_.markServing();
            // Retain worker-local state until the lifecycle caller has joined
            // ingress and sends the finalization control.
            if (!stopToken_.stopRequested()) {
                co_await serveSignal_.wait();
            }
        }

    } catch (...) {
        const auto failure = std::current_exception();
        (void)workerCompletion_.markStartupFailed(failure);
        stop_http3();
        failWorker(failure);
    }
    co_await finalizeSignal_.wait();
    try {
        co_await backgroundTasks_.join();
    } catch (...) {
        const auto failure = std::current_exception();
        (void)workerCompletion_.markStartupFailed(failure);
        failWorker(failure);
    }
    if (http3_) {
        try {
            co_await http3_->join();
        } catch (...) {
            failWorker(std::current_exception());
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
