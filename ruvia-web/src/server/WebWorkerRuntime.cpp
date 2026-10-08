#include "server/WebWorkerRuntime.h"

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

#include "app/WebWorkerDispatch.h"
#include "http/StaticRootIndex.h"
#include "http3/http3_datagram_channel.h"
#include "http3/http3_worker.h"
#include "router/RouteTable.h"
#include "server/HttpServerOptionsValidation.h"
#include "server/HttpServerTlsIdentity.h"
#include "server/acceptor.h"

namespace ruvia::detail {

using TcpEndpoint = asio::ip::tcp::endpoint;

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
      owned_acceptor_(nullptr, PmrObjectDeleter<acceptor>{memory_.resource()}),
      backgroundTasks_(workerRuntime_.handle(), {.resource = memory_.resource()}),
      options_(std::move(validatedOptions)),
      static_roots_(options_, workerRuntime_.handle(), workerState_, memory_.resource()),
      capabilities_(ioContext_, workerRuntime_.handle(), memory_.resource(), capabilities,
          WorkerCapabilityOptions{
              .defaultRateLimit = options_.defaultRateLimitPerWorker,
              .routeRateLimits = routes_.hasRouteRateLimit() ? RouteRateLimitPresence::kPresent
                                                             : RouteRateLimitPresence::kAbsent,
              .rateLimitCapacity = options_.rateLimitCapacityPerWorker,
              .maxDecodedBodyBytes = options_.max_buffered_body_bytes,
              .http_client_result_budget = options_.http_client_result_budget,
              .blockingPool = options_.blockingPool,
              .env = options_.env,
              .trustedProxies =
                  options_.trustedProxies.empty() ? nullptr : &options_.trustedProxies,
              .precompressedStaticFiles = options_.compression.has_value(),
          }),
      connections_(ioContext_, workerRuntime_.handle(), memory_, routes_, capabilities_, options_, stopToken_, workerState_, backgroundTasks_, listeners),
      http3_(nullptr, PmrObjectDeleter<http3_worker>{memory_.resource()}),
      webWorkerDispatch_(std::make_shared<WebWorkerDispatch>(ioContext_.get_executor(),
          workerRuntime_.handle(), memory_.resource(), capabilities_,
          [this](const std::exception_ptr& failure) { failWorker(failure); })) {
    options_.inbound_buffer_pool = &inbound_buffers_;
    for (std::size_t index = 0; index < listeners.size(); ++index) {
        if (listeners[index].http3) {
            http3_ = makePmrObject<http3_worker>(memory_.resource(), workerRuntime_, memory_,
                routes_, capabilities_, connections_.scanner(), options_, stopToken_,
                *connections_.listener(index).tls(), *listeners[index].http3,
                connections_.active_counter(), connections_.refused_counter(),
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
            (void)workerCompletion_.mark_startup_failed(failure);
            failWorker(failure); },
        .shutdown = [this]() noexcept {
            connections_.retire_tls();
            capabilities_.shutdownWorkerState();
            workerState_ = HttpServerWorkerState::kStopped;
            (void)workerCompletion_.mark_startup_failed(std::make_exception_ptr(
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
        (void)workerCompletion_.mark_startup_failed(failure);
        (void)workerCompletion_.record_failure(failure);
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
    workerCompletion_.wait_for_startup();
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
    const bool ready = workerCompletion_.wait_for_serving();
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
    const auto failure = workerCompletion_.failure();
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
    return runtime_.state() == RuntimeLifecycle::State::kRunning && connections_.available();
}

void WebWorkerRuntime::acceptTransferredConnection(NativeAcceptedSocketTicket&& ticket) noexcept {
    if (runtime_.state() != RuntimeLifecycle::State::kRunning) {
        return;
    }
    connections_.accept(std::move(ticket));
}

HttpServerStats WebWorkerRuntime::stats() const noexcept {
    auto stats = connections_.stats();
    stats.workerFailures = workerFailures_.load(std::memory_order_relaxed);
    stats.documentRootRefreshFailures =
        static_roots_.failures();
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

void WebWorkerRuntime::stopAdmissionOnContext() noexcept {
    connections_.close_admission();
    stopSource_.requestStop();
    webWorkerDispatch_->close();
    workerRuntime_.close();

    serveSignal_.notify();
    stop_http3();
    connections_.stop();
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
    if (!workerCompletion_.record_failure(failure)) {
        return;
    }
    // Counted after the dedupe above, so a worker failing once counts once.
    workerFailures_.fetch_add(1, std::memory_order_relaxed);
    runtime_.request_stop();
    if (owned_acceptor_) {
        owned_acceptor_->stop();
    }
    options_.failure.notify(failure);
}

void WebWorkerRuntime::stop_http3() noexcept {
    if (http3_) {
        http3_->stop();
    }
}

Task<void> WebWorkerRuntime::runWorker() {
    if (!httpServerWorkerRunning(workerState_)) {
        stop_http3();
        (void)workerCompletion_.mark_startup_failed(
            std::make_exception_ptr(std::runtime_error("web worker startup cancelled")));
        workerCompletion_.mark_serving_aborted();
        co_return;
    }
    try {
        connections_.prepare();
        co_await capabilities_.connect();
        if (http3_ != nullptr && !stopToken_.stopRequested()) {
            http3_->start();
        }
        if (stopToken_.stopRequested()) {
            stop_http3();
            (void)workerCompletion_.mark_startup_failed(
                std::make_exception_ptr(std::runtime_error("web worker startup cancelled")));
            workerCompletion_.mark_serving_aborted();
        } else {
            (void)workerCompletion_.mark_startup_ready();
        }

        while (!serveRequested_ && httpServerWorkerRunning(workerState_) &&
               !stopToken_.stopRequested()) {
            co_await serveSignal_.wait();
        }
        if (!serveRequested_ || !httpServerWorkerRunning(workerState_) ||
            stopToken_.stopRequested()) {
            workerCompletion_.mark_serving_aborted();
        } else {
            if (options_.documentRoot.refreshOptions() != nullptr) {
                backgroundTasks_.spawn(static_roots_.refresh());
            }
            connections_.open_admission();
            (void)workerCompletion_.mark_serving();
            // Retain worker-local state until the lifecycle caller has joined
            // ingress and sends the finalization control.
            if (!stopToken_.stopRequested()) {
                co_await serveSignal_.wait();
            }
        }

    } catch (...) {
        const auto failure = std::current_exception();
        (void)workerCompletion_.mark_startup_failed(failure);
        stop_http3();
        failWorker(failure);
    }
    co_await finalizeSignal_.wait();
    try {
        co_await backgroundTasks_.join();
    } catch (...) {
        const auto failure = std::current_exception();
        (void)workerCompletion_.mark_startup_failed(failure);
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
        (void)workerCompletion_.mark_startup_failed(failure);
        failWorker(failure);
    }
}

}  // namespace ruvia::detail
