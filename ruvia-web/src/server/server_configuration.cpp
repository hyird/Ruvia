#include <array>
#include <bit>
#include <chrono>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>
#include <variant>

#include "ruvia/core/failure_report.h"

#include "ratelimit/rate_limiter.h"
#include "server/config_validation.h"
#include "server/http_server_options.h"
#include "server/http_server_options_validation.h"
#include "server/inbound_transport_error.h"
#include "tls/tls_random.h"

namespace ruvia::detail {

namespace {

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
            throw std::invalid_argument(
                "compression synchronous size must not be smaller than the minimum size");
        }
        if (compression->max_bytes_ < compression->sync_bytes_) {
            throw std::invalid_argument(
                "compression maximum size must not be smaller than the synchronous size");
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

http_server_listener_definition validate_run_listener(const http_server_listener_definition& listener,
    const http_server_options& options, std::chrono::steady_clock::time_point key_origin) {
    const auto worker_count = options.server_.worker_count_;
    if (const auto* redirect = std::get_if<http_server_listener_definition::redirect_http_to_https>(&listener.transport_)) {
        detail::ensure_non_zero_port(redirect->https_port_, "HTTP-to-HTTPS redirect requires a fixed HTTPS listen port");
    }
    if (const auto* tls = std::get_if<http_server_listener_definition::tls>(&listener.transport_)) {
        switch (tls->tcp_mode_) {
            case http_server_listener_definition::tls_tcp_mode::tls_only:
                if (tls->plaintext_redirect_) {
                    throw std::invalid_argument("a plaintext redirect requires a mixed HTTP/HTTPS listener");
                }
                break;
            case http_server_listener_definition::tls_tcp_mode::detect_plain_or_tls:
                if (tls->plaintext_redirect_) {
                    detail::ensure_non_zero_port(
                        tls->plaintext_redirect_->https_port_, "HTTP-to-HTTPS redirect requires a fixed HTTPS listen port");
                }
                break;
            default:
                throw std::invalid_argument("invalid TLS TCP transport mode");
        }
    }
    if (listener.connection_.has_value() && !options.connection_) {
        throw std::invalid_argument("listener_dispatch::connection requires application::on_connection()");
    }
    if (listener.request_stream_ && !options.request_stream_) {
        throw std::invalid_argument("listener_dispatch::request_stream requires application::on_request_stream()");
    }
    // A QUIC connection ID names its owning worker in 16 bits.
    if (listener.http3_.has_value() && worker_count > http3_max_worker_count) {
        throw std::invalid_argument("HTTP/3 supports at most 65536 workers");
    }
#if !defined(_WIN32)
    // Worker i serves inherited socket i; listen() admitted one per service on Windows.
    if (const auto& inherited = listener.inherited_sockets_; inherited != nullptr &&
                                                             ((!inherited->tcp().empty() && inherited->tcp().size() != worker_count) ||
                                                                 (!inherited->udp().empty() && inherited->udp().size() != worker_count))) {
        throw std::invalid_argument("inherited listener sockets must number one per worker");
    }
#endif
    auto validated = listener.clone();
    // One ticket key ring per TLS listener for this run, so TLS resumption
    // does not depend on which worker accepts the connection. Credentials
    // were loaded once by listen(); every worker installs them.
    if (auto* tls = std::get_if<http_server_listener_definition::tls>(&validated.transport_)) {
        tls->session_ticket_keys_ = std::make_shared<tls_session_ticket_key_ring>(key_origin);
    }
    // One QUIC secret per HTTP/3 service, shared by every worker so any
    // worker can tell which one owns a connection ID.
    if (validated.http3_.has_value()) {
        auto secret = std::make_shared<std::array<std::uint8_t, 32>>();
        if (!fill_random_bytes(std::span<unsigned char>(secret->data(), secret->size()))) {
            throw std::runtime_error("failed to generate the HTTP/3 connection ID secret");
        }
        validated.http3_->secret_ = std::move(secret);
    }
    return validated;
}

validated_http_server_configuration::validated_http_server_configuration(
    std::span<const http_server_listener_definition> listeners, http_server_options&& options)
    : listeners_(),
      options_(std::move(options)),
      // Every listener's key epochs start together, so all workers rotate them
      // at the same instants.
      tls_key_origin_(std::chrono::steady_clock::now()) {
    listeners_.reserve(listeners.size());
    for (const auto& listener : listeners) {
        listeners_.push_back(
            validate_run_listener(listener, options_, tls_key_origin_));
    }
}

namespace {

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
    detail::ensure_positive_optional_durations("configured server timeouts must be greater than zero",
        config.idle_timeout_, config.request_header_timeout_, config.request_body_timeout_,
        config.write_timeout_);
    detail::ensure_positive_duration(
        config.connection_scan_interval_, "connection scan interval must be greater than 0");
    validate_worker_queue_capacity(config.worker_queue_capacity_);
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
    detail::ensure_positive_size(config.http_client_worker_.result_budget_.max_retained_bytes_,
        "HTTP client retained result byte budget must be greater than zero");
    detail::ensure_positive_size(config.http_client_worker_.result_budget_.max_in_flight_bytes_,
        "HTTP client in-flight response budget must be greater than zero");
    detail::ensure_positive_size(config.http_client_worker_.max_concurrent_dns_refreshes_,
        "HTTP client DNS refresh concurrency must be greater than zero");
}

[[nodiscard]] validated_http_server_configuration validate_http_server_configuration(
    std::span<const http_server_listener_definition> listeners, http_server_options&& options) {
    return validated_http_server_configuration(listeners, std::move(options));
}

}  // namespace ruvia::detail
