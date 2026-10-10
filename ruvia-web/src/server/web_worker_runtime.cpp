#include "server/web_worker_runtime.h"

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

#include "ruvia/core/connection_scanner.h"
#include "ruvia/core/failure_report.h"
#include "ruvia/core/memory/process_resource.h"
#include "ruvia/http/http_ascii.h"

#include "app/web_worker_dispatch.h"
#include "http/static_root_index.h"
#include "http3/http3_datagram_channel.h"
#include "http3/http3_worker.h"
#include "router/route_table.h"
#include "server/acceptor.h"
#include "server/http_server_options_validation.h"
#include "server/http_server_tls_identity.h"

namespace ruvia::detail {

using tcp_endpoint_type = asio::ip::tcp::endpoint;

web_worker_runtime::web_worker_runtime(tcp_endpoint_type endpoint, const route_table& routes_value,
    worker_capability_definitions capabilities, http_server_options options)
    : web_worker_runtime(http_server_listener_definition(std::move(endpoint)), routes_value, capabilities,
          std::move(options)) {}

web_worker_runtime::web_worker_runtime(http_server_listener_definition listener_value, const route_table& routes_value,
    worker_capability_definitions capabilities, http_server_options options)
    : web_worker_runtime(std::span<const http_server_listener_definition>(&listener_value, 1), routes_value,
          capabilities, std::move(options)) {}

web_worker_runtime::web_worker_runtime(std::span<const http_server_listener_definition> listeners,
    const route_table& routes_value, worker_capability_definitions capabilities, http_server_options options)
    : web_worker_runtime(validate_http_server_configuration(listeners, std::move(options)), routes_value,
          capabilities, true) {}

web_worker_runtime::web_worker_runtime(const validated_http_server_configuration& configuration,
    const route_table& routes_value, worker_capability_definitions capabilities)
    : web_worker_runtime(configuration, routes_value, capabilities, false) {}

web_worker_runtime::web_worker_runtime(const validated_http_server_configuration& configuration,
    const route_table& routes_value, worker_capability_definitions capabilities,
    bool own_acceptor)
    : web_worker_runtime(validated_configuration_tag_type{}, configuration.listeners(), routes_value, capabilities,
          configuration.options(), own_acceptor) {}

web_worker_runtime::web_worker_runtime(validated_configuration_tag_type,
    std::span<const http_server_listener_definition> listeners, const route_table& routes_value,
    worker_capability_definitions capabilities, http_server_options validated_options,
    bool own_acceptor)
    : runtime_({.queue_capacity_ = validated_options.worker_queue_capacity_,
          .io_policy_ = ruvia::worker_io_policy::single_owner}),
      io_context_(runtime_.context().io_context()),
      worker_runtime_(runtime_.context()),
      serve_signal_(worker_runtime_.handle()),
      finalize_signal_(worker_runtime_.handle()),
      routes_(routes_value),
      memory_(validated_options.memory_config_),
      inbound_buffers_(memory_.resource(), validated_options.max_inbound_buffer_bytes_per_worker_),
      owned_acceptor_(nullptr, pmr_object_deleter<acceptor>{memory_.resource()}),
      background_tasks_(worker_runtime_.handle(), {.resource_ = memory_.resource()}),
      options_(std::move(validated_options)),
      static_roots_(options_, worker_runtime_.handle(), worker_state_, memory_.resource()),
      capabilities_(io_context_, worker_runtime_.handle(), memory_.resource(), capabilities,
          worker_capability_options{
              .default_rate_limit_ = options_.default_rate_limit_per_worker_,
              .route_rate_limits_ = routes_.has_route_rate_limit() ? route_rate_limit_presence::present
                                                                   : route_rate_limit_presence::absent,
              .rate_limit_capacity_ = options_.rate_limit_capacity_per_worker_,
              .max_decoded_body_bytes_ = options_.max_buffered_body_bytes_,
              .http_client_result_budget_ = options_.http_client_result_budget_,
              .blocking_pool_ = options_.blocking_pool_,
              .env_ = options_.env_,
              .trusted_proxies_ =
                  options_.trusted_proxies_.empty() ? nullptr : &options_.trusted_proxies_,
              .precompressed_static_files_ = options_.compression_.has_value(),
          }),
      connections_(io_context_, worker_runtime_.handle(), memory_, routes_, capabilities_, options_, stop_token_, worker_state_, background_tasks_, listeners),
      http3_(nullptr, pmr_object_deleter<http3_worker>{memory_.resource()}),
      web_worker_dispatch_(std::make_shared<web_worker_dispatch>(io_context_.get_executor(),
          worker_runtime_.handle(), memory_.resource(), capabilities_,
          [this](const std::exception_ptr& failure) { fail_worker(failure); })) {
    options_.inbound_buffer_pool_ = &inbound_buffers_;
    for (std::size_t index = 0; index < listeners.size(); ++index) {
        if (listeners[index].http3_) {
            http3_ = make_pmr_object<http3_worker>(memory_.resource(), worker_runtime_, memory_,
                routes_, capabilities_, connections_.scanner(), options_, stop_token_,
                *connections_.listener(index).tls(), *listeners[index].http3_,
                connections_.active_counter(), connections_.refused_counter(),
                http3_worker_runtime::failure_notification{this,
                    [](void* object, std::exception_ptr failure) noexcept {
                        static_cast<web_worker_runtime*>(object)->fail_worker(failure);
                    }});
        }
    }
    if (own_acceptor) {
        const std::array targets{acceptor::worker_target{
            .submission_ = network_submission(),
            .object_ = this,
            .available_ = [](void* object) noexcept { return static_cast<web_worker_runtime*>(object)->available_for_network_dispatch(); },
            .accept_ = [](void* object, native_accepted_socket_ticket&& ticket) noexcept { static_cast<web_worker_runtime*>(object)->accept_transferred_connection(std::move(ticket)); },
            .stage_quic_ = http3_ ? +[](void* object, http3_datagram_channel& channel,
                                         asio::ip::udp::endpoint endpoint, quic_cid_partition partition) { static_cast<web_worker_runtime*>(object)->stage_quic(channel, endpoint, partition); }
                                  : nullptr,
        }};
        owned_acceptor_ = make_pmr_object<acceptor>(memory_.resource(), listeners, targets, this,
            [](void* object) noexcept { static_cast<web_worker_runtime*>(object)->stop_admission(); });
    }
    runtime_.configure({
        .startup_ = [this] {
            worker_state_ = http_server_worker_state::running;
            capabilities_.initialize_worker_state();
            asio::co_spawn(io_context_, ruvia::as_awaitable(run_worker()),
                asio::bind_allocator(asio::recycling_allocator<void>(),
                    [this](std::exception_ptr failure) noexcept {
                        if (failure) {
                            fail_worker(failure);
                        }
                    })); },
        .stop_admission_ = [this] { stop_admission_on_context(); },
        .failure_ = [this](std::exception_ptr failure) noexcept {
            (void)worker_completion_.mark_startup_failed(failure);
            fail_worker(failure); },
        .shutdown_ = [this]() noexcept {
            connections_.retire_tls();
            capabilities_.shutdown_worker_state();
            worker_state_ = http_server_worker_state::stopped;
            (void)worker_completion_.mark_startup_failed(std::make_exception_ptr(
                std::runtime_error("http server worker stopped before startup completed"))); },
    });
}

web_worker_runtime::~web_worker_runtime() {
    stop();
    try {
        join();
    } catch (...) {
        // join() rethrows this worker's failure, and a destructor cannot pass
        // it on. A server destroyed without an explicit join -- or one whose
        // failure raced the application's own shutdown -- would otherwise take the
        // reason with it.
        ruvia::report_unhandled_failure("web server worker", std::current_exception());
    }
    // Core has detached escaped endpoints before retiring the execution
    // context. Domain callback state can now be retired without a live producer.
    web_worker_dispatch_->retire();
}

void web_worker_runtime::start() {
    prepare();
    launch();
    wait_until_ready();
    request_serve();
    if (!wait_until_serving()) {
        throw std::runtime_error("web worker stopped before it began serving");
    }
}

void web_worker_runtime::prepare() {
    if (prepared_ || runtime_.state() != runtime_lifecycle::state_type::ready) {
        throw std::logic_error("web worker runtime can only be prepared once");
    }
    if (owned_acceptor_) {
        owned_acceptor_->prepare();
    }
    prepared_ = true;
}

void web_worker_runtime::launch() {
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
        (void)worker_completion_.mark_startup_failed(failure);
        (void)worker_completion_.record_failure(failure);
        // Core still owes owner-affine draining. application performs the external
        // producer barrier before requesting phase-two finalization.
        if (!runtime_.started() && http3_ != nullptr) {
            http3_->abandon_before_launch();
        }
        throw;
    }
}

void web_worker_runtime::wait_until_ready() {
    if (runtime_.state() == runtime_lifecycle::state_type::ready) {
        throw std::logic_error("web worker runtime has not been launched");
    }
    worker_completion_.wait_for_startup();
}

void web_worker_runtime::request_serve() {
    if (runtime_.state() != runtime_lifecycle::state_type::running) {
        return;
    }
    (void)runtime_.post_control([this] {
        if (!http_server_worker_running(worker_state_) || serve_requested_ ||
            stop_token_.stop_requested()) {
            return;
        }
        serve_requested_ = true;
        serve_signal_.notify();
    });
}

bool web_worker_runtime::wait_until_serving() {
    const bool ready = worker_completion_.wait_for_serving();
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

void web_worker_runtime::stop() noexcept {
    if (runtime_.state() == runtime_lifecycle::state_type::stopped) {
        return;
    }
    stop_admission();
    if (!owned_acceptor_) {
        finalize_after_network_quiesced();
    }
}

void web_worker_runtime::stop_admission() noexcept {
    if (owned_acceptor_) {
        owned_acceptor_->stop();
    }
    runtime_.request_stop();
    if (!runtime_.started() && http3_ != nullptr) {
        http3_->abandon_before_launch();
    }
}

void web_worker_runtime::finalize_after_network_quiesced() noexcept {
    runtime_.finalize([this] { stop_on_context(); });
}

void web_worker_runtime::join() {
    if (worker_runtime_.handle().is_current()) {
        throw std::logic_error("web worker cannot join itself");
    }
    if (owned_acceptor_) {
        owned_acceptor_->join();
        finalize_after_network_quiesced();
    }
    runtime_.join();
    const auto failure = worker_completion_.failure();
    if (failure != nullptr) {
        std::rethrow_exception(failure);
    }
    runtime_.rethrow_failure();
    if (owned_acceptor_) {
        owned_acceptor_->rethrow_failure();
    }
}

tcp_endpoint_type web_worker_runtime::local_endpoint(std::size_t listener_index) const {
    if (!owned_acceptor_) {
        throw std::out_of_range("listener acceptor is not configured on this worker");
    }
    return owned_acceptor_->local_endpoint(listener_index);
}

bool web_worker_runtime::available_for_network_dispatch() const noexcept {
    return runtime_.state() == runtime_lifecycle::state_type::running && connections_.available();
}

void web_worker_runtime::accept_transferred_connection(native_accepted_socket_ticket&& ticket) noexcept {
    if (runtime_.state() != runtime_lifecycle::state_type::running) {
        return;
    }
    connections_.accept(std::move(ticket));
}

http_server_stats web_worker_runtime::stats() const noexcept {
    auto stats = connections_.stats();
    stats.worker_failures_ = worker_failures_.load(std::memory_order_relaxed);
    stats.document_root_refresh_failures_ =
        static_roots_.failures();
    return stats;
}

web_worker_handle web_worker_runtime::web_worker() const {
    return web_worker_dispatch_->handle();
}

void web_worker_runtime::stage_quic(http3_datagram_channel& channel,
    asio::ip::udp::endpoint endpoint, ruvia::quic_cid_partition partition) {
    if (!http3_ || runtime_.started()) {
        throw std::logic_error("worker QUIC channel cannot be staged in this state");
    }
    http3_->stage(channel, std::move(endpoint), partition);
    if (runtime_.state() != runtime_lifecycle::state_type::ready) {
        http3_->abandon_before_launch();
    }
}

void web_worker_runtime::stop_admission_on_context() noexcept {
    connections_.close_admission();
    stop_source_.request_stop();
    web_worker_dispatch_->close();
    worker_runtime_.close();

    serve_signal_.notify();
    stop_http3();
    connections_.stop();
}

void web_worker_runtime::stop_on_context() noexcept {
    if (!http_server_worker_running(worker_state_)) {
        capabilities_.close_now();
        finalize_signal_.notify();
        return;
    }

    stop_admission_on_context();
    capabilities_.close_now();
    worker_state_ = http_server_worker_state::stopped;
    finalize_signal_.notify();
}

void web_worker_runtime::fail_worker(const std::exception_ptr& failure) noexcept {
    if (!worker_completion_.record_failure(failure)) {
        return;
    }
    // Counted after the dedupe above, so a worker failing once counts once.
    worker_failures_.fetch_add(1, std::memory_order_relaxed);
    runtime_.request_stop();
    if (owned_acceptor_) {
        owned_acceptor_->stop();
    }
    options_.failure_.notify(failure);
}

void web_worker_runtime::stop_http3() noexcept {
    if (http3_) {
        http3_->stop();
    }
}

task<void> web_worker_runtime::run_worker() {
    if (!http_server_worker_running(worker_state_)) {
        stop_http3();
        worker_completion_.mark_startup_aborted();
        worker_completion_.mark_serving_aborted();
        co_return;
    }
    try {
        // A stop applied before this task first runs has already stopped the
        // worker timers; starting the scanner or capability I/O would fail.
        if (!stop_token_.stop_requested()) {
            connections_.prepare();
            co_await capabilities_.connect();
        }
        if (http3_ != nullptr && !stop_token_.stop_requested()) {
            http3_->start();
        }
        if (stop_token_.stop_requested()) {
            stop_http3();
            worker_completion_.mark_startup_aborted();
            worker_completion_.mark_serving_aborted();
        } else {
            (void)worker_completion_.mark_startup_ready();
        }

        while (!serve_requested_ && http_server_worker_running(worker_state_) &&
               !stop_token_.stop_requested()) {
            co_await serve_signal_.wait();
        }
        if (!serve_requested_ || !http_server_worker_running(worker_state_) ||
            stop_token_.stop_requested()) {
            worker_completion_.mark_serving_aborted();
        } else {
            if (options_.document_root_.refresh_options() != nullptr) {
                background_tasks_.spawn(static_roots_.refresh());
            }
            connections_.open_admission();
            (void)worker_completion_.mark_serving();
            // Retain worker-local state until the lifecycle caller has joined
            // ingress and sends the finalization control.
            if (!stop_token_.stop_requested()) {
                co_await serve_signal_.wait();
            }
        }

    } catch (...) {
        const auto failure = std::current_exception();
        (void)worker_completion_.mark_startup_failed(failure);
        stop_http3();
        fail_worker(failure);
    }
    co_await finalize_signal_.wait();
    try {
        co_await background_tasks_.join();
    } catch (...) {
        const auto failure = std::current_exception();
        (void)worker_completion_.mark_startup_failed(failure);
        fail_worker(failure);
    }
    if (http3_) {
        try {
            co_await http3_->join();
        } catch (...) {
            fail_worker(std::current_exception());
        }
    }
    try {
        co_await capabilities_.join();
    } catch (...) {
        const auto failure = std::current_exception();
        (void)worker_completion_.mark_startup_failed(failure);
        fail_worker(failure);
    }
}

}  // namespace ruvia::detail
