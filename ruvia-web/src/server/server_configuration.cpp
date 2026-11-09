#include <array>
#include <bit>
#include <chrono>
#include <cstdint>
#include <exception>
#include <memory>
#include <memory_resource>
#include <span>
#include <stdexcept>
#include <utility>
#include <variant>

#include "ruvia/core/failure_report.h"
#include "ruvia/core/memory/pmr_object.h"

#include "server/http_server_options.h"
#include "server/http_server_options_validation.h"
#include "server/inbound_transport_error.h"
#include "tls/tls_random.h"
#include "util/config_validation.h"

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

void connection_failure_sink::invoke(std::string_view remote_address, std::exception_ptr exception) const noexcept {
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

void http_server_options::set_server(const server_config& config) {
    validate_server_limits(config);
    server_ = config;
}

void http_server_options::set_compression(compression_config config) {
    ensure_positive_size(
        config.min_bytes_, "compression minimum size must be greater than zero");
    if (config.sync_bytes_ < config.min_bytes_) {
        throw std::invalid_argument(
            "compression synchronous size must not be smaller than the minimum size");
    }
    if (config.max_bytes_ < config.sync_bytes_) {
        throw std::invalid_argument(
            "compression maximum size must not be smaller than the synchronous size");
    }
    compression_ = std::move(config);
}

void http_server_options::set_cors(const cors_config& config) {
    cors_ = make_cors_options(config, process_resource());
}

void http_server_options::set_rate_limit(const rate_limit_config& config) {
    const auto& rule = config.rule_;
    if (rule.max_requests_ == 0) {
        throw std::invalid_argument("rate limit max requests must be greater than zero");
    }
    if (rule.window_.count() <= 0) {
        throw std::invalid_argument("rate limit window must be greater than zero");
    }
    if (rule.overflow_policy_ != rate_limit_overflow_policy::deny &&
        rule.overflow_policy_ != rate_limit_overflow_policy::allow) {
        throw std::invalid_argument("rate limit overflow policy is invalid");
    }
    if (!std::has_single_bit(config.capacity_per_worker_)) {
        throw std::invalid_argument(
            "rate-limit capacity per worker must be a power of two");
    }
    default_rate_limit_per_worker_ = config.rule_;
    rate_limit_capacity_per_worker_ = config.capacity_per_worker_;
}

void http_server_options::set_trusted_proxies(const trusted_proxy_config& config) {
    trusted_proxy_set parsed(process_resource());
    parsed.trust_x_forwarded_proto(config.trust_x_forwarded_proto_);
    for (const auto& cidr : config.cidrs_) {
        const auto block = parse_trusted_proxy_block(cidr);
        if (!block) {
            // A typo here would silently trust nothing and leave every
            // client identified as the proxy, so it fails startup instead.
            throw std::invalid_argument(
                "trusted proxy must be an IP address or CIDR block");
        }
        parsed.add(*block);
    }
    trusted_proxies_ = std::move(parsed);
}

validated_http_server_configuration::validated_http_server_configuration(
    std::span<const http_server_listener_definition> listeners, http_server_options&& options)
    : listeners_(process_resource()),
      options_(std::move(options)) {
    listeners_.reserve(listeners.size());
    // Every listener's key epochs start together, so all workers rotate them
    // at the same instants.
    const auto key_origin = std::chrono::steady_clock::now();
    for (const auto& listener : listeners) {
        auto& validated = listeners_.emplace_back(listener.clone(listeners_.get_allocator().resource()));
        // One ticket key ring per TLS listener for this configuration, so TLS
        // resumption does not depend on which worker accepts the connection.
        // Credentials were loaded once by listen(); every worker installs them.
        if (auto* tls = std::get_if<http_server_listener_definition::tls>(&validated.transport_)) {
            tls->session_ticket_keys_ = std::allocate_shared<tls_session_ticket_key_ring>(
                std::pmr::polymorphic_allocator<tls_session_ticket_key_ring>(listeners_.get_allocator().resource()),
                key_origin);
        }
        // One QUIC secret per HTTP/3 service, shared by every worker so any
        // worker can tell which one owns a connection ID.
        if (validated.http3_.has_value()) {
            auto secret = std::allocate_shared<std::array<std::uint8_t, 32>>(
                std::pmr::polymorphic_allocator<std::array<std::uint8_t, 32>>(listeners_.get_allocator().resource()));
            if (!fill_random_bytes(std::span<unsigned char>(secret->data(), secret->size()))) {
                throw std::runtime_error("failed to generate the HTTP/3 connection ID secret");
            }
            validated.http3_->secret_ = std::move(secret);
        }
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

void validate_http_server_listener(const http_server_listener_definition& listener_value) {
    if (const auto* redirect =
            std::get_if<http_server_listener_definition::redirect_http_to_https>(&listener_value.transport_)) {
        detail::ensure_non_zero_port(
            redirect->https_port_, "HTTP-to-HTTPS redirect requires a fixed HTTPS listen port");
    }
}

}  // namespace

[[nodiscard]] validated_http_server_configuration validate_http_server_configuration(
    std::span<const http_server_listener_definition> listeners, http_server_options&& options) {
    for (const auto& listener : listeners) {
        validate_http_server_listener(listener);
        // A QUIC connection ID names its owning worker in 16 bits.
        if (listener.http3_.has_value() && options.server_.worker_count_ > http3_max_worker_count) {
            throw std::invalid_argument("HTTP/3 supports at most 65536 workers");
        }
    }
    return validated_http_server_configuration(listeners, std::move(options));
}

}  // namespace ruvia::detail
