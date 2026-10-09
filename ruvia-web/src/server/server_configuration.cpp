#include "ruvia/core/memory/process_resource.h"

#include "server/http_server_options_validation.h"

namespace ruvia::detail {

validated_http_server_configuration::validated_http_server_configuration(
    std::span<const http_server_listener_definition> listeners, http_server_options&& options)
    : listeners_(process_resource()),
      options_(std::move(options)) {
    listeners_.reserve(listeners.size());
    for (const auto& listener : listeners) {
        listeners_.push_back(listener.clone(listeners_.get_allocator().resource()));
    }
}

const server_config& server_config_defaults() noexcept {
    static const server_config defaults;
    return defaults;
}

http_server_options normalize_server_options(const server_config& config, http_server_options options) {
    ruvia::ensure_positive_size(config.worker_count_, "worker count must be greater than zero");
    if (config.process_signal_handlers_ != process_signal_handler_policy::external_owner &&
        config.process_signal_handlers_ != process_signal_handler_policy::install) {
        throw std::invalid_argument("process signal handler policy is invalid");
    }
    options.worker_queue_capacity_ = config.worker_queue_capacity_;
    options.idle_timeout_ = config.idle_timeout_;
    options.scan_interval_ = config.connection_scan_interval_;
    options.request_header_timeout_ = config.request_header_timeout_;
    options.request_body_timeout_ = config.request_body_timeout_;
    options.write_timeout_ = config.write_timeout_;
    options.max_connections_ = config.max_connections_per_worker_;
    options.max_requests_per_connection_ = config.max_requests_per_connection_;
    options.max_buffered_body_bytes_ = config.max_buffered_body_bytes_;
    options.max_inbound_buffer_bytes_per_worker_ = config.max_inbound_buffer_bytes_per_worker_;
    options.max_inbound_buffer_bytes_per_connection_ = config.max_inbound_buffer_bytes_per_connection_;
    options.header_completion_timeout_ = config.header_completion_timeout_;
    options.body_completion_timeout_ = config.body_completion_timeout_;
    options.max_stream_body_bytes_ = config.max_stream_body_bytes_;
    options.max_websocket_message_bytes_ = config.max_websocket_message_bytes_;
    options.memory_config_ = config.memory_pool_;
    options.http_client_result_budget_ = config.http_client_result_budget_;
    validate_server_limits(options);
    return options;
}

void validate_document_root_runtime_config(const http_server_options& options) {
    const auto* refresh = options.document_root_.refresh_options();
    if (refresh == nullptr) {
        return;
    }
    ruvia::ensure_positive_duration(
        refresh->refresh_interval_, "document root refresh interval must be greater than zero");
    if (options.blocking_pool_ == nullptr) {
        throw std::invalid_argument(
            "document root refresh cannot run while the blocking pool is disabled");
    }
    const auto* precompression = options.document_root_.precompression_options();
    if (precompression == nullptr) {
        return;
    }
    ruvia::ensure_positive_size(precompression->min_bytes_,
        "document root precompression minimum size must be greater than zero");
    if (precompression->max_bytes_ < precompression->min_bytes_) {
        throw std::invalid_argument(
            "document root precompression maximum size must not be smaller than the minimum size");
    }
}

void validate_worker_queue_capacity(std::size_t capacity) {
    ruvia::ensure_positive_size(capacity, "worker queue capacity must be greater than zero");
    if (capacity == std::numeric_limits<std::size_t>::max()) {
        throw std::invalid_argument("worker queue capacity must leave a reserved execution slot");
    }
}

void validate_server_limits(const http_server_options& options) {
    ruvia::ensure_positive_optional_durations("configured server timeouts must be greater than zero",
        options.idle_timeout_, options.request_header_timeout_, options.request_body_timeout_,
        options.write_timeout_);
    ruvia::ensure_positive_duration(options.scan_interval_, "connection scan interval must be greater than 0");
    validate_worker_queue_capacity(options.worker_queue_capacity_);
    ruvia::ensure_positive_size(options.memory_config_.request_initial_buffer_bytes_,
        "memory pool config values must be greater than 0");
    ruvia::ensure_positive_size(options.max_buffered_body_bytes_, "buffered body limit must be greater than 0");
    ruvia::ensure_positive_optional_size(
        options.max_stream_body_bytes_, "configured stream body limit must be greater than zero");
    ruvia::ensure_positive_size(
        options.max_websocket_message_bytes_, "websocket message limit must be greater than 0");
    ruvia::ensure_positive_optional_size(
        options.max_connections_, "configured connection limit must be greater than zero");
    ruvia::ensure_positive_optional_size(options.max_requests_per_connection_,
        "configured requests-per-connection limit must be greater than zero");
    ruvia::ensure_positive_size(options.max_inbound_buffer_bytes_per_worker_,
        "worker inbound buffer budget must be greater than zero");
    ruvia::ensure_positive_size(options.max_inbound_buffer_bytes_per_connection_,
        "connection inbound buffer budget must be greater than zero");
    ruvia::ensure_positive_optional_durations("completion deadlines must be greater than zero",
        options.header_completion_timeout_, options.body_completion_timeout_);
    ruvia::ensure_positive_size(options.http_client_result_budget_.max_retained_bytes_,
        "HTTP client retained result byte budget must be greater than zero");
    ruvia::ensure_positive_size(options.http_client_result_budget_.max_in_flight_bytes_,
        "HTTP client in-flight response budget must be greater than zero");
}

void validate_http_server_options(const http_server_options& options) {
    validate_server_limits(options);
    if (!std::has_single_bit(options.rate_limit_capacity_per_worker_)) {
        throw std::invalid_argument("rate-limit capacity per worker must be a power of two");
    }
    if (options.compression_.has_value()) {
        ruvia::ensure_positive_size(
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

void validate_http_server_tls_options(const http_server_listener_definition::tls_type& tls) {
    validate_http_server_tls_identity(tls.identity_);
    if (tls.http3_early_data_ && tls.client_certificates_.has_value()) {
        throw std::invalid_argument("HTTP/3 early data is unavailable with TLS client certificates");
    }
    if (tls.client_certificates_.has_value()) {
        validate_http_server_tls_client_certificate_policy(*tls.client_certificates_);
    }
    for (std::size_t i = 0; i < tls.sni_identities_.size(); ++i) {
        const auto& sni = tls.sni_identities_[i];
        ensure_sni_host(sni.host_, "SNI host must not be empty", "SNI host is invalid");
        validate_http_server_tls_identity(sni.identity_);
        for (std::size_t j = 0; j < i; ++j) {
            if (http_ascii_equals_ignore_case(tls.sni_identities_[j].host_, sni.host_)) {
                throw std::invalid_argument("SNI hosts must be unique");
            }
        }
    }
}

[[nodiscard]] std::size_t http3_worker_tracked_stream_capacity(
    std::size_t max_requests_per_connection) {
    constexpr auto allowance = http3_peer_unidirectional_stream_allowance + http3_server_push_allowance;
    if (max_requests_per_connection > std::numeric_limits<std::size_t>::max() - allowance) {
        throw std::invalid_argument("HTTP/3 worker stream capacity is not representable");
    }
    return max_requests_per_connection + allowance;
}

[[nodiscard]] std::size_t http3_transport_lifetime_stream_capacity(
    std::size_t max_requests_per_connection) {
    constexpr auto allowance = http3_peer_unidirectional_stream_allowance +
                               http3_post_goaway_request_allowance;
    if (max_requests_per_connection > std::numeric_limits<std::size_t>::max() - allowance) {
        throw std::invalid_argument("HTTP/3 transport stream capacity is not representable");
    }
    return max_requests_per_connection + allowance;
}

void validate_http3_server_limits(
    std::optional<std::size_t> max_connections, const http3_listen_config& config,
    std::optional<std::size_t> max_requests_per_connection, std::size_t worker_count) {
    if (worker_count == 0) {
        throw std::invalid_argument("HTTP/3 worker count must be greater than zero");
    }
    if (!max_connections.has_value()) {
        throw std::invalid_argument("HTTP/3 requires a finite per-worker connection limit");
    }
    ruvia::ensure_positive_size(*max_connections,
        "HTTP/3 per-worker connection limit must be greater than zero");

    // Keep the aggregate startup-allocated slot count representable as a
    // container difference across all worker-local protocol drivers.
    const auto max_connection_capacity =
        static_cast<std::size_t>(std::numeric_limits<std::ptrdiff_t>::max());
    if (*max_connections > max_connection_capacity / worker_count) {
        throw std::invalid_argument(
            "HTTP/3 aggregate connection capacity is not representable");
    }

    (void)normalize_http3_capacity(config, worker_count);

    if (!max_requests_per_connection.has_value()) {
        throw std::invalid_argument(
            "HTTP/3 requires a finite per-connection request limit");
    }
    ruvia::ensure_positive_size(*max_requests_per_connection,
        "HTTP/3 per-connection request limit must be greater than zero");

    // The request-stream boundary is a client-bidi QUIC stream ID: 4 * N.
    constexpr std::uint64_t max_goaway_id = (std::uint64_t{1} << 62) - 4;
    if constexpr (sizeof(std::size_t) > sizeof(std::uint64_t)) {
        if (*max_requests_per_connection > std::numeric_limits<std::uint64_t>::max()) {
            throw std::invalid_argument("HTTP/3 request limit does not fit a GOAWAY varint");
        }
    }
    const auto request_limit = static_cast<std::uint64_t>(*max_requests_per_connection);
    if (request_limit > max_goaway_id / 4) {
        throw std::invalid_argument("HTTP/3 GOAWAY request boundary is not representable");
    }

    const auto tracked_streams =
        http3_worker_tracked_stream_capacity(*max_requests_per_connection);
    const auto lifetime_streams =
        http3_transport_lifetime_stream_capacity(*max_requests_per_connection);
    constexpr auto max_power_of_two =
        std::size_t{1} << (std::numeric_limits<std::size_t>::digits - 1);
    if (tracked_streams > max_power_of_two / 2 || lifetime_streams > max_power_of_two) {
        throw std::invalid_argument("HTTP/3 stream tracking capacity is not representable");
    }
    const std::vector<std::size_t> slots;
    if (tracked_streams > slots.max_size() / 2 || lifetime_streams > slots.max_size()) {
        throw std::invalid_argument("HTTP/3 stream capacity exceeds container limits");
    }
}

void validate_http3_listen_config(const http3_listen_config& config) {
    (void)normalize_http3_capacity(config, 1);
    validate_http3_qpack_config(config.qpack_);
    ruvia::ensure_positive_duration(config.handshake_timeout_,
        "HTTP/3 handshake timeout must be greater than zero");
    ruvia::ensure_positive_duration(config.drain_timeout_,
        "HTTP/3 drain timeout must be greater than zero");
    if (std::chrono::duration<long double>(config.drain_timeout_) >
        std::chrono::duration<long double>(std::chrono::steady_clock::duration::max())) {
        throw std::invalid_argument("HTTP/3 drain timeout is not representable");
    }
}

void validate_http_server_listener(const http_server_listener_definition& listener_value) {
    if (const auto* tls = std::get_if<http_server_listener_definition::tls_type>(&listener_value.transport_)) {
        validate_http_server_tls_options(*tls);
    } else if (listener_value.http3_.has_value()) {
        throw std::invalid_argument("HTTP/3 listener requires TLS");
    }
    if (listener_value.http3_) {
        validate_http3_listen_config(*listener_value.http3_);
    }
    if (const auto* redirect =
            std::get_if<http_server_listener_definition::redirect_http_to_https_type>(&listener_value.transport_)) {
        ruvia::ensure_non_zero_port(
            redirect->https_port_, "HTTP-to-HTTPS redirect requires a fixed HTTPS listen port");
    }
}

[[nodiscard]] validated_http_server_configuration validate_http_server_configuration(
    std::span<const http_server_listener_definition> listeners, http_server_options&& options) {
    if (listeners.empty()) {
        throw std::invalid_argument("HTTP server worker requires at least one listener");
    }
    const http3_listen_config* http3 = nullptr;
    for (const auto& listener : listeners) {
        validate_http_server_listener(listener);
        if (listener.http3_.has_value()) {
            if (http3) {
                throw std::invalid_argument(
                    "only one HTTP/3 listener is supported by the App runtime");
            }
            http3 = &*listener.http3_;
        }
    }
    validate_http_server_options(options);
    if (http3) {
        validate_http3_server_limits(
            options.max_connections_, *http3,
            options.max_requests_per_connection_, 1);
    }
    return validated_http_server_configuration(listeners, std::move(options));
}

}  // namespace ruvia::detail
