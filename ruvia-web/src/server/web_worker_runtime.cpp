#include "server/web_worker_runtime.h"

#include <exception>
#include <memory>
#include <utility>

#include <asio/bind_allocator.hpp>
#include <asio/co_spawn.hpp>
#include <asio/recycling_allocator.hpp>

#include "ruvia/core/async.h"

#include "app/web_worker_dispatch.h"
#include "router/route_table.h"
#include "server/http_server_options_validation.h"

namespace ruvia::detail {

web_worker_runtime::web_worker_runtime(const validated_http_server_configuration& configuration,
    const route_table& routes_value, worker_capability_definitions capabilities)
    : web_worker_runtime(configuration.listeners(), routes_value, capabilities, configuration.options()) {}

web_worker_runtime::web_worker_runtime(std::span<const http_server_listener_definition> listeners,
    const route_table& routes_value, worker_capability_definitions capabilities,
    http_server_options validated_options)
    : runtime_({.queue_capacity_ = validated_options.server_.worker_queue_capacity_,
          .io_policy_ = ruvia::worker_io_policy::single_owner}),
      io_context_(runtime_.io_context()),
      finalize_signal_(runtime_.handle()),
      routes_(routes_value),
      memory_(validated_options.server_.memory_pool_),
      inbound_budget_(*memory_.resource(), validated_options.server_.max_inbound_buffer_bytes_per_worker_),
      codecs_(memory_.resource()),
      background_tasks_(runtime_.handle(), {.resource_ = memory_.resource()}),
      options_(std::move(validated_options)),
      static_roots_(options_, runtime_.handle(), worker_state_, memory_.resource()),
      capabilities_(io_context_, runtime_.handle(), memory_.resource(), capabilities,
          worker_capability_options{
              .default_rate_limit_ = options_.default_rate_limit_per_worker_,
              .route_rate_limits_ = routes_.has_route_rate_limit() ? route_rate_limit_presence::present
                                                                   : route_rate_limit_presence::absent,
              .rate_limit_capacity_ = options_.rate_limit_capacity_per_worker_,
              .max_decoded_body_bytes_ = options_.server_.max_buffered_body_bytes_,
              .http_client_result_budget_ = options_.server_.http_client_result_budget_,
              .blocking_pool_ = options_.blocking_pool_,
              .env_ = options_.env_,
              .trusted_proxies_ =
                  options_.trusted_proxies_.empty() ? nullptr : &options_.trusted_proxies_,
              .precompressed_static_files_ = options_.compression_.has_value(),
          }),
      connections_(io_context_, runtime_.handle(), memory_, routes_, capabilities_, options_, stop_token_, worker_state_, background_tasks_, listeners, runtime_),
      web_worker_dispatch_(std::make_shared<web_worker_dispatch>(
          runtime_, memory_.resource(), capabilities_, background_tasks_, stop_token_)) {
    options_.inbound_budget_ = &inbound_budget_;
    options_.codecs_ = &codecs_;
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
        .failure_ = [this](std::exception_ptr failure) noexcept {
            worker_failures_.fetch_add(1, std::memory_order_relaxed);
            options_.failure_.notify(failure); },
        .shutdown_ = [this]() noexcept {
            connections_.retire_tls();
            worker_state_ = http_server_worker_state::stopped; },
    });
}

web_worker_runtime::~web_worker_runtime() {
    runtime_.request_stop();
#if defined(_WIN32)
    if (windows_accept_ == nullptr) {
        runtime_.finalize();
    }
#else
    runtime_.finalize();
#endif
    runtime_.join();
#if defined(_WIN32)
    if (windows_accept_ != nullptr) {
        windows_accept_->detach(windows_origin_);
    }
#endif
}

void web_worker_runtime::request_drain() noexcept {
    // A refused control means the worker is already stopping, which completes
    // its drain barrier itself.
    (void)runtime_.post_control([this] { drain_on_context(); });
}

bool web_worker_runtime::wait_until_drained(std::chrono::steady_clock::time_point deadline) {
    return connections_.wait_until_drained(deadline);
}

http_server_stats web_worker_runtime::stats() const noexcept {
    auto stats = connections_.stats();
    stats.worker_failures_ = worker_failures_.load(std::memory_order_relaxed);
    return stats;
}

web_worker_handle web_worker_runtime::web_worker() const {
    return web_worker_dispatch::handle(web_worker_dispatch_);
}
#if defined(_WIN32)
void web_worker_runtime::attach_windows_accept(windows_accept_domain& domain, std::size_t origin) {
    connections_.attach_windows_accept(domain, origin);
    windows_accept_ = &domain;
    windows_origin_ = origin;
}

task<void> web_worker_runtime::run_windows_accepts() {
    std::exception_ptr failure;
    try {
        co_await connections_.run_windows_accepts();
    } catch (...) {
        failure = std::current_exception();
        windows_accept_->request_stop();
    }
    windows_accept_joined_ = true;
    if (failure != nullptr) {
        // Nested task allocation can fail before the receiver ever starts.
        // Publish that failure now, not from the scope join after finalization.
        (void)runtime_.fail(std::move(failure));
    }
    if (stop_token_.stop_requested()) {
        runtime_.finalize();
    }
}
#endif

void web_worker_runtime::serve_on_context() {
    if (static_roots_.enabled()) {
        background_tasks_.spawn(static_roots_.refresh());
    }
    connections_.open_admission();
}

void web_worker_runtime::drain_on_context() noexcept {
    if (!http_server_worker_running(worker_state_)) {
        return;
    }
    worker_state_ = http_server_worker_state::draining;
    connections_.drain();
}

void web_worker_runtime::stop_admission_on_context() noexcept {
    worker_state_ = http_server_worker_state::stopped;
    stop_source_.request_stop();
    connections_.stop();
#if defined(_WIN32)
    // A stopped listener IOCP still produces completions for peer origins.
    // The receiver closes/joins only after the entire accept matrix is quiet.
    if (windows_accept_joined_) {
        runtime_.finalize();
    }
#else
    runtime_.finalize();
#endif
}

void web_worker_runtime::finalize_on_context() noexcept {
    capabilities_.close_now();
    finalize_signal_.notify();
}

task<void> web_worker_runtime::run_worker() {
    try {
        // A stop applied before this task first runs has already stopped the
        // worker timers; starting the scanner or capability I/O would fail.
        if (!stop_token_.stop_requested()) {
#if defined(_WIN32)
            windows_accept_joined_ = false;
            try {
                background_tasks_.spawn(run_windows_accepts());
            } catch (...) {
                windows_accept_joined_ = true;
                throw;
            }
#endif
            connections_.prepare();
            co_await capabilities_.start();
            // Ignored once stopping: core has already aborted the barrier.
            runtime_.mark_ready();
        }
    } catch (...) {
        // A stop during startup closes the capabilities whose connect is still
        // pending; that abort is the requested cancellation, not a failure.
        if (!stop_token_.stop_requested()) {
            (void)runtime_.fail(std::current_exception());
        }
    }
    // Retain worker-local state until finalization joins sessions and pending
    // listener operations on this same owner.
    co_await finalize_signal_.wait();
    try {
        co_await background_tasks_.join();
    } catch (...) {
        (void)runtime_.fail(std::current_exception());
    }
    try {
        co_await capabilities_.stop();
    } catch (...) {
        (void)runtime_.fail(std::current_exception());
    }
}

}  // namespace ruvia::detail
