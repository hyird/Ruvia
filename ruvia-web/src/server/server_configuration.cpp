#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <span>
#include <stdexcept>

#include "ruvia/core/memory/pmr_object.h"

#include "server/http_server_options_validation.h"
#include "tls/tls_random.h"
#include "util/config_validation.h"

namespace ruvia::detail {

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
