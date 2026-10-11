#include "server/web_worker_runtime.h"

#include <exception>
#include <memory>
#include <stdexcept>
#include <utility>

#include <asio/bind_allocator.hpp>
#include <asio/co_spawn.hpp>
#include <asio/recycling_allocator.hpp>

#include "ruvia/core/asio_task.h"

#include "app/web_worker_dispatch.h"
#include "http3/http3_datagram_channel.h"
#include "http3/http3_worker.h"
#include "router/route_table.h"
#include "server/http_server_options_validation.h"

namespace ruvia::detail {

web_worker_runtime::web_worker_runtime(const validated_http_server_configuration& configuration,
    const route_table& routes_value, worker_capability_definitions capabilities)
    : web_worker_runtime(configuration.listeners(), routes_value, capabilities, configuration.options()) {}

web_worker_runtime::web_worker_runtime(std::span<const http_server_listener_definition> listeners,
    const route_table& routes_value, worker_capability_definitions capabilities,
    http_server_options validated_options)
    : runtime_({.queue_capacity_ = validated_options.worker_queue_capacity_,
          .io_policy_ = ruvia::worker_io_policy::single_owner}),
      io_context_(runtime_.context().io_context()),
      worker_runtime_(runtime_.context()),
      finalize_signal_(worker_runtime_.handle()),
      routes_(routes_value),
      memory_(validated_options.memory_config_),
      inbound_buffers_(memory_.resource(), validated_options.max_inbound_buffer_bytes_per_worker_),
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
      web_worker_dispatch_(std::make_shared<web_worker_dispatch>(
          runtime_, memory_.resource(), capabilities_, background_tasks_, stop_token_)) {
    options_.inbound_buffer_pool_ = &inbound_buffers_;
    for (std::size_t index = 0; index < listeners.size(); ++index) {
        if (listeners[index].http3_) {
            http3_ = make_pmr_object<http3_worker>(memory_.resource(), worker_runtime_, memory_,
                routes_, capabilities_, connections_.scanner(), options_, stop_token_,
                *connections_.listener(index).tls(), *listeners[index].http3_,
                connections_.active_counter(), connections_.refused_counter(),
                http3_worker::failure_notification{this,
                    [](void* object, std::exception_ptr failure) noexcept {
                        (void)static_cast<web_worker_runtime*>(object)->runtime_.fail(failure);
                    }});
        }
    }
    runtime_.configure({
        .startup_ = [this] {
            worker_state_ = http_server_worker_state::running;
            asio::co_spawn(io_context_, ruvia::as_awaitable(run_worker()),
                asio::bind_allocator(asio::recycling_allocator<void>(),
                    [this](std::exception_ptr failure) noexcept {
                        if (failure) {
                            (void)runtime_.fail(failure);
                        }
                    })); },
        .serve_ = [this] { serve_on_context(); },
        .stop_admission_ = [this] { stop_admission_on_context(); },
        .finalize_ = [this] { finalize_on_context(); },
        // Releases a datagram channel the acceptor staged into a worker that
        // never launched, so ingress retirement does not wait for its owner.
        .abandon_ = [this] {
            if (http3_ != nullptr) {
                http3_->abandon_before_launch();
            } },
        .failure_ = [this](std::exception_ptr failure) noexcept {
            worker_failures_.fetch_add(1, std::memory_order_relaxed);
            options_.failure_.notify(failure); },
        .shutdown_ = [this]() noexcept {
            connections_.retire_tls();
            worker_state_ = http_server_worker_state::stopped; },
    });
}

web_worker_runtime::~web_worker_runtime() {
    // Stop admission, then finalize: a worker destroyed outside the
    // application's worker_group has no external producers left.
    runtime_.finalize();
    runtime_.join();
}

bool web_worker_runtime::available_for_network_dispatch() const noexcept {
    return connections_.available();
}

void web_worker_runtime::accept_transferred_connection(native_accepted_socket_ticket&& ticket) noexcept {
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
    return web_worker_dispatch::handle(web_worker_dispatch_);
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

void web_worker_runtime::serve_on_context() {
    if (options_.document_root_.refresh_options() != nullptr) {
        background_tasks_.spawn(static_roots_.refresh());
    }
    connections_.open_admission();
}

void web_worker_runtime::stop_admission_on_context() noexcept {
    connections_.close_admission();
    stop_source_.request_stop();
    stop_http3();
    connections_.stop();
}

void web_worker_runtime::finalize_on_context() noexcept {
    capabilities_.close_now();
    worker_state_ = http_server_worker_state::stopped;
    finalize_signal_.notify();
}

void web_worker_runtime::stop_http3() noexcept {
    if (http3_) {
        http3_->stop();
    }
}

task<void> web_worker_runtime::run_worker() {
    try {
        // A stop applied before this task first runs has already stopped the
        // worker timers; starting the scanner or capability I/O would fail.
        if (!stop_token_.stop_requested()) {
            connections_.prepare();
            co_await capabilities_.start();
            if (http3_ != nullptr && !stop_token_.stop_requested()) {
                http3_->start();
            }
            // Ignored once stopping: core has already aborted the barrier.
            runtime_.mark_ready();
        }
    } catch (...) {
        stop_http3();
        (void)runtime_.fail(std::current_exception());
    }
    // Retain worker-local state until the lifecycle caller has joined ingress
    // and finalized this worker.
    co_await finalize_signal_.wait();
    try {
        co_await background_tasks_.join();
    } catch (...) {
        (void)runtime_.fail(std::current_exception());
    }
    if (http3_) {
        try {
            co_await http3_->join();
        } catch (...) {
            (void)runtime_.fail(std::current_exception());
        }
    }
    try {
        co_await capabilities_.stop();
    } catch (...) {
        (void)runtime_.fail(std::current_exception());
    }
}

}  // namespace ruvia::detail
