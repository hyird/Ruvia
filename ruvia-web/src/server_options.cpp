#include "server_options.h"

#include <atomic>
#include <cstddef>
#include <exception>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "failure_report.h"
#include "http_client_ip.h"
#include "http_cors.h"
#include "http_server.h"
#include "ip_address.h"
#include "rate_limit.h"
#include "rate_limiting.h"
#include "startup_config.h"
#include "transport_stream_end.h"

namespace ruvia::detail {
namespace {

void validate_worker_queue_capacity(std::size_t capacity) {
    detail::ensure_positive_size(capacity, "worker queue capacity must be greater than zero");
    if (capacity == std::numeric_limits<std::size_t>::max()) {
        throw std::invalid_argument("worker queue capacity must leave a reserved execution slot");
    }
}

void validate_server_limits(const server_config& config) {
    detail::ensure_positive_size(config.worker_count_, "worker count must be greater than zero");
    if (config.process_signal_handlers_ != process_signal_handler_policy::external_owner && config.process_signal_handlers_ != process_signal_handler_policy::install) {
        throw std::invalid_argument("process signal handler policy is invalid");
    }
    detail::ensure_positive_optional_durations("configured server timeouts must be greater than zero", config.idle_timeout_, config.request_header_timeout_, config.request_body_timeout_, config.write_timeout_);
    detail::ensure_positive_duration(config.connection_scan_interval_, "connection scan interval must be greater than 0");
    validate_worker_queue_capacity(config.worker_queue_capacity_);
    detail::ensure_positive_size(config.max_buffered_body_bytes_, "buffered body limit must be greater than 0");
    detail::ensure_positive_optional_size(config.max_stream_body_bytes_, "configured stream body limit must be greater than zero");
    detail::ensure_positive_size(config.max_websocket_message_bytes_, "websocket message limit must be greater than 0");
    detail::ensure_positive_optional_size(config.max_connections_per_worker_, "configured connection limit must be greater than zero");
    detail::ensure_positive_optional_size(config.max_requests_per_connection_, "configured requests-per-connection limit must be greater than zero");
    detail::ensure_positive_size(config.max_inbound_buffer_bytes_per_worker_, "worker inbound buffer budget must be greater than zero");
    detail::ensure_positive_size(config.max_inbound_buffer_bytes_per_connection_, "connection inbound buffer budget must be greater than zero");
    detail::ensure_positive_duration(config.header_completion_timeout_, "completion deadlines must be greater than zero");
    detail::ensure_positive_optional_durations("completion deadlines must be greater than zero", config.body_completion_timeout_, config.write_completion_timeout_);
    detail::ensure_positive_size(config.http_client_worker_.result_budget_.max_retained_bytes_, "HTTP client retained result byte budget must be greater than zero");
    detail::ensure_positive_size(config.http_client_worker_.result_budget_.max_in_flight_bytes_, "HTTP client in-flight response budget must be greater than zero");
    detail::ensure_positive_size(config.http_client_worker_.max_concurrent_dns_refreshes_, "HTTP client DNS refresh concurrency must be greater than zero");
}

// Only the connection's own transport can end it normally. A handler or
// upstream failure is a real fault even when it carries one of the codes
// is_inbound_connection_end() accepts: an upstream request that timed out or
// was reset says nothing about the inbound peer.
[[nodiscard]] bool is_connection_end(const std::exception_ptr& exception) noexcept {
    try {
        std::rethrow_exception(exception);
    } catch (const inbound_transport_error& error) {
        return is_inbound_connection_end(error.code());
    } catch (...) {
        return false;
    }
}

}  // namespace

void connection_failure_sink::invoke(ip_address remote_address, std::exception_ptr exception) const noexcept {
    if (exception == nullptr || is_connection_end(exception)) {
        return;
    }
    if (counter_ != nullptr) {
        counter_->fetch_add(1, std::memory_order_relaxed);
    }
    if (callback_) {
        callback_(connection_failure_record_access::make(remote_address, std::move(exception)));
        return;
    }
    ruvia::report_unhandled_failure("web connection", exception);
}

void connection_failure_sink::invoke(std::string_view remote_address, std::exception_ptr exception) const noexcept {
    invoke(ip_address::parse(remote_address).value_or(ip_address{}), std::move(exception));
}

void http_server_options::set_server(const server_config& config) {
    validate_server_limits(config);
    if (const auto& compression = config.compression_) {
        ensure_positive_size(compression->min_bytes_, "compression minimum size must be greater than zero");
        if (compression->sync_bytes_ < compression->min_bytes_) {
            throw std::invalid_argument("compression synchronous size must not be smaller than the minimum size");
        }
        if (compression->max_bytes_ < compression->sync_bytes_) {
            throw std::invalid_argument("compression maximum size must not be smaller than the synchronous size");
        }
    }
    std::optional<http_cors_policy> cors;
    if (config.cors_) {
        cors.emplace(*config.cors_);
    }
    server_ = config;
    compression_ = config.compression_;
    cors_ = std::move(cors);
}

void http_server_options::set_rate_limit(const rate_limit_config& config) {
    validate_rate_limit_config(config);
    default_rate_limit_per_worker_ = config.rule_;
    rate_limit_capacity_per_worker_ = config.capacity_per_worker_;
}

void http_server_options::set_trusted_proxies(const trusted_proxy_config& config) {
    http_client_ip_config ip_config;
    ip_config.enabled_ = !config.cidrs_.empty();
    ip_config.trusted_proxies_ = config.cidrs_;
    ip_config.forwarding_headers_ = config.forwarding_headers_;
    ip_config.trust_x_forwarded_proto_ = config.trust_x_forwarded_proto_;
    trusted_proxies_ = http_client_ip_extractor(ip_config);
}

}  // namespace ruvia::detail
