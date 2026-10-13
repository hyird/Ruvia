#include "ruvia/core/memory/process_resource.h"

#include "server/http_server_options_validation.h"
#include "util/config_validation.h"

namespace ruvia::detail {

validated_http_server_configuration::validated_http_server_configuration(
    std::span<const http_server_listener_definition> listeners, http_server_options&& options)
    : listeners_(process_resource()),
      options_(std::move(options)) {
    listeners_.reserve(listeners.size());
    for (const auto& listener : listeners) {
        auto& validated = listeners_.emplace_back(listener.clone(listeners_.get_allocator().resource()));
        // One ticket key set per TLS listener for this configuration, so TLS
        // resumption does not depend on which worker accepts the connection.
        // Credentials were loaded once by listen(); every worker installs them.
        if (auto* tls = std::get_if<http_server_listener_definition::tls_type>(&validated.transport_)) {
            tls->session_ticket_keys_ = tls_session_ticket_keys::generate();
        }
    }
}

namespace {

// Interval and precompression thresholds are validated when the document root
// is configured; only the blocking-pool dependency is known at run time.
void validate_document_root_runtime_config(const http_server_options& options) {
    if (options.document_root_.publisher() != nullptr && options.blocking_pool_ == nullptr) {
        throw std::invalid_argument(
            "document root refresh cannot run while the blocking pool is disabled");
    }
}

void validate_worker_queue_capacity(std::size_t capacity) {
    detail::ensure_positive_size(capacity, "worker queue capacity must be greater than zero");
    if (capacity == std::numeric_limits<std::size_t>::max()) {
        throw std::invalid_argument("worker queue capacity must leave a reserved execution slot");
    }
}

}  // namespace

void validate_server_limits(const server_config& config) {
    detail::ensure_positive_size(config.worker_count_, "worker count must be greater than zero");
    if (config.process_signal_handlers_ != process_signal_handler_policy::external_owner &&
        config.process_signal_handlers_ != process_signal_handler_policy::install) {
        throw std::invalid_argument("process signal handler policy is invalid");
    }
    if (config.drain_timeout_.count() < 0) {
        throw std::invalid_argument("drain timeout must not be negative");
    }
    detail::ensure_positive_optional_durations("configured server timeouts must be greater than zero",
        config.idle_timeout_, config.request_header_timeout_, config.request_body_timeout_,
        config.write_timeout_);
    detail::ensure_positive_duration(
        config.connection_scan_interval_, "connection scan interval must be greater than 0");
    validate_worker_queue_capacity(config.worker_queue_capacity_);
    ruvia::validate_memory_pool_config(config.memory_pool_);
    detail::ensure_positive_size(config.max_buffered_body_bytes_, "buffered body limit must be greater than 0");
    detail::ensure_positive_optional_size(
        config.max_stream_body_bytes_, "configured stream body limit must be greater than zero");
    detail::ensure_positive_size(
        config.max_websocket_message_bytes_, "websocket message limit must be greater than 0");
    detail::ensure_positive_optional_size(
        config.max_connections_per_worker_, "configured connection limit must be greater than zero");
    detail::ensure_positive_optional_size(config.max_requests_per_connection_,
        "configured requests-per-connection limit must be greater than zero");
    detail::ensure_positive_size(config.max_inbound_buffer_bytes_per_worker_,
        "worker inbound buffer budget must be greater than zero");
    detail::ensure_positive_size(config.max_inbound_buffer_bytes_per_connection_,
        "connection inbound buffer budget must be greater than zero");
    detail::ensure_positive_optional_durations("completion deadlines must be greater than zero",
        config.header_completion_timeout_, config.body_completion_timeout_);
    detail::ensure_positive_size(config.http_client_result_budget_.max_retained_bytes_,
        "HTTP client retained result byte budget must be greater than zero");
    detail::ensure_positive_size(config.http_client_result_budget_.max_in_flight_bytes_,
        "HTTP client in-flight response budget must be greater than zero");
}

namespace {

// server_ was validated by validate_server_limits when application::server()
// accepted it; only what other configuration entries set is checked here, at run.
void validate_http_server_options(const http_server_options& options) {
    if (!std::has_single_bit(options.rate_limit_capacity_per_worker_)) {
        throw std::invalid_argument("rate-limit capacity per worker must be a power of two");
    }
    if (options.compression_.has_value()) {
        detail::ensure_positive_size(
            options.compression_->min_bytes_, "compression minimum size must be greater than zero");
        if (options.compression_->sync_bytes_ < options.compression_->min_bytes_) {
            throw std::invalid_argument(
                "compression synchronous size must not be smaller than the minimum size");
        }
        if (options.compression_->max_bytes_ < options.compression_->sync_bytes_) {
            throw std::invalid_argument(
                "compression maximum size must not be smaller than the synchronous size");
        }
    }
    validate_document_root_runtime_config(options);
}

void validate_http_server_listener(const http_server_listener_definition& listener_value) {
    if (const auto* redirect =
            std::get_if<http_server_listener_definition::redirect_http_to_https_type>(&listener_value.transport_)) {
        detail::ensure_non_zero_port(
            redirect->https_port_, "HTTP-to-HTTPS redirect requires a fixed HTTPS listen port");
    }
}

}  // namespace

[[nodiscard]] validated_http_server_configuration validate_http_server_configuration(
    std::span<const http_server_listener_definition> listeners, http_server_options&& options) {
    for (const auto& listener : listeners) {
        validate_http_server_listener(listener);
    }
    validate_http_server_options(options);
    return validated_http_server_configuration(listeners, std::move(options));
}

}  // namespace ruvia::detail
