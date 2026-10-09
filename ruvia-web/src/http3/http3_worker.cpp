#include "http3/http3_worker.h"

#include <stdexcept>
#include <utility>

#include "http3/http3_capacity.h"

namespace ruvia::detail {

http3_worker::http3_worker(worker_runtime_context& runtime, worker_memory& memory,
    const route_table& routes_value, worker_capabilities& capabilities,
    connection_scanner& scanner, const http_server_options& options,
    const stop_token& stop_token_value, const http_server_listener_definition::tls_type& tls,
    http3_listen_config config, std::atomic<std::size_t>& active_connections,
    std::atomic<std::size_t>& refused_connections,
    http3_worker_runtime::failure_notification failure)
    : runtime_(runtime),
      memory_(memory),
      options_(options),
      stop_token_(stop_token_value),
      tls_(tls),
      config_(config),
      failure_(failure),
      server_(runtime.handle(), memory, routes_value, capabilities, scanner,
          runtime.io_context().get_executor(), options, stop_token_value,
          options.max_connections_.value(), normalize_http3_capacity(config, 1).stream_slots_,
          active_connections, refused_connections),
      transport_(nullptr, pmr_object_deleter<http3_worker_runtime>{memory.resource()}),
      tasks_(runtime.handle(), {.resource_ = memory.resource()}) {}

http3_worker::~http3_worker() {
    if (transport_ || staged_channel_.load(std::memory_order_acquire)) {
        std::terminate();
    }
}

void http3_worker::stage(http3_datagram_channel& channel,
    asio::ip::udp::endpoint endpoint, quic_cid_partition partition) {
    if (staged_channel_.load(std::memory_order_acquire) != nullptr) {
        throw std::logic_error("HTTP/3 worker already has a staged channel");
    }
    channel.stage_worker(runtime_);
    endpoint_ = std::move(endpoint);
    partition_ = partition;
    staged_channel_.store(&channel, std::memory_order_release);
    // The second check closes the stop-before-publication race.
    if (abandoned_.load(std::memory_order_acquire)) {
        abandon_before_launch();
    }
}

void http3_worker::abandon_before_launch() noexcept {
    abandoned_.store(true, std::memory_order_release);
    if (auto* channel = staged_channel_.exchange(nullptr, std::memory_order_acq_rel)) {
        channel->abandon_worker();
    }
}

void http3_worker::start() {
    if (!runtime_.handle().is_current() || started_) {
        throw std::logic_error("HTTP/3 component must start once on its owner");
    }
    started_ = true;
    struct cold_channel_owner final {
        http3_datagram_channel* channel_;
        ~cold_channel_owner() {
            if (channel_) {
                channel_->abandon_worker();
            }
        }
    } cold{staged_channel_.exchange(nullptr, std::memory_order_acq_rel)};
    try {
        if (!cold.channel_) {
            throw std::logic_error("HTTP/3 worker has no staged acceptor channel");
        }
        transport_ = make_pmr_object<http3_worker_runtime>(memory_.resource(), runtime_,
            endpoint_, tls_, config_, http3_worker_runtime::worker_target{.server_ = &server_, .max_connections_ = options_.max_connections_.value(), .buffer_capacity_ = normalize_http3_capacity(config_, 1).stream_slots_, .max_requests_per_connection_ = options_.max_requests_per_connection_.value(), .idle_timeout_ = options_.idle_timeout_, .request_header_timeout_ = options_.request_header_timeout_, .request_body_timeout_ = options_.request_body_timeout_, .write_timeout_ = options_.write_timeout_},
            *cold.channel_, partition_, failure_);
        cold.channel_ = nullptr;
        transport_->stage();
        if (!server_.install()) {
            throw std::runtime_error("failed to install HTTP/3 worker bridge");
        }
        tasks_.spawn(server_.run());
        if (!stop_token_.stop_requested()) {
            transport_->start();
        } else {
            transport_->stop();
        }
        tasks_.spawn(transport_->run_datagrams());
    } catch (...) {
        stop();
        throw;
    }
}

void http3_worker::stop() noexcept {
    if (!runtime_.handle().is_current()) {
        std::terminate();
    }
    if (server_.run_started()) {
        server_.request_stop();
    } else {
        server_.abandon_before_launch();
    }
    if (transport_) {
        if (transport_->runner_started()) {
            transport_->stop();
        } else {
            transport_->abandon_before_launch();
        }
    } else {
        abandon_before_launch();
    }
}

task<void> http3_worker::join() {
    std::exception_ptr failure;
    try {
        co_await tasks_.join();
    } catch (...) {
        failure = std::current_exception();
        stop();
    }
    if (transport_) {
        co_await transport_->join();
        transport_.reset();
    }
    if (failure) {
        std::rethrow_exception(failure);
    }
}

}  // namespace ruvia::detail
