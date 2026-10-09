#pragma once
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

#include "ruvia/core/config_validation.h"
#include "ruvia/http/cookies.h"
#include "ruvia/http/http_header.h"
#include "ruvia/web/http_client_types.h"

#include "client/client_transport.h"
#include "http3/http3_qpack_config_validation.h"

namespace ruvia::detail {

inline void validate_http_client_user_agent(std::string_view user_agent) {
    if (!user_agent.empty() && !is_valid_http_header_value(user_agent)) {
        throw std::invalid_argument("http client user agent is invalid");
    }
}

inline void validate_http_client_config(const http_client_config& config) {
    validate_http3_qpack_config(config.qpack_);
    if (config.push_.max_queued_pushes_ == 0 || config.push_.max_concurrent_pushes_ == 0 ||
        (config.push_.timeout_ && config.push_.timeout_->count() <= 0)) {
        throw std::invalid_argument("HTTP push bounds and configured timeout must be positive");
    }
    if (config.push_.enabled_ && config.protocol_ == http_client_protocol::http3_only && config.push_.max_concurrent_pushes_ > 25) {
        throw std::invalid_argument("HTTP/3 push concurrency must leave room for critical streams and requests");
    }
    if (config.push_.enabled_ && config.scheme_ == http_scheme::https &&
        config.tls_peer_verification_ != tls_peer_verification_policy::verify) {
        throw std::invalid_argument("HTTPS push requires authenticated origin authority");
    }
    if (config.advertisements_.max_queued_advertisements_ == 0 || config.advertisements_.max_retained_bytes_ == 0) {
        throw std::invalid_argument("HTTP advertisement bounds must be positive");
    }
    if (config.advertisements_.receive_origins_ && (config.scheme_ != http_scheme::https || config.tls_peer_verification_ != tls_peer_verification_policy::verify)) {
        throw std::invalid_argument("ORIGIN advertisements require authenticated HTTPS");
    }
    const auto scheme = config.scheme_;
    const std::string_view host = config.host_;
    const auto port = config.port_.value_or(scheme == http_scheme::https ? 443 : 80);
    if (scheme != http_scheme::http && scheme != http_scheme::https) {
        throw std::invalid_argument("http client scheme is invalid");
    }
    if (config.initial_quic_version_ != quic_version::v1 &&
        config.initial_quic_version_ != quic_version::v2) {
        throw std::invalid_argument("HTTP client QUIC version is invalid");
    }
    if (config.protocol_ != http_client_protocol::negotiate &&
        config.protocol_ != http_client_protocol::http1_only &&
        config.protocol_ != http_client_protocol::http2_only &&
        config.protocol_ != http_client_protocol::http3_only) {
        throw std::invalid_argument("http client protocol is invalid");
    }
    if (config.protocol_ == http_client_protocol::http3_only && scheme != http_scheme::https) {
        throw std::invalid_argument("HTTP/3 client requires the HTTPS scheme");
    }
    if (config.http3_early_data_ &&
        (scheme != http_scheme::https || config.protocol_ == http_client_protocol::http1_only ||
            config.protocol_ == http_client_protocol::http2_only)) {
        throw std::invalid_argument("HTTP/3 early data requires an HTTPS client with HTTP/3 enabled");
    }
    validate_client_transport_config(make_client_transport_config_view(config));
    if (config.received_cookies_ != http_client_received_cookie_policy::ignore &&
        config.received_cookies_ != http_client_received_cookie_policy::retain_and_send) {
        throw std::invalid_argument("http client received cookie policy is invalid");
    }
    validate_client_origin_host(
        host, "http client host must not be empty", "http client host is invalid");
    if (port == 0) {
        throw std::invalid_argument("http client port must be greater than zero");
    }
    ruvia::ensure_positive_size(
        config.connection_count_, "http client connection count must be greater than zero");
    ruvia::ensure_positive_size(config.max_concurrent_http2_streams_per_connection_,
        "http client HTTP/2 stream limit per connection must be greater than zero");
    if (config.max_concurrent_http2_streams_per_connection_ >
        std::numeric_limits<std::size_t>::max() / config.connection_count_) {
        throw std::invalid_argument(
            "HTTP client connection and HTTP/2 stream capacity is too large");
    }
    constexpr std::size_t http3_concurrent_requests_per_connection = 29;
    if (config.protocol_ == http_client_protocol::http3_only &&
        config.connection_count_ >
            std::numeric_limits<std::size_t>::max() /
                http3_concurrent_requests_per_connection) {
        throw std::invalid_argument("HTTP/3 client connection capacity is too large");
    }
    ruvia::ensure_positive_size(
        config.max_buffered_requests_, "http client buffered request limit must be greater than zero");
    ruvia::ensure_positive_size(config.max_cookies_, "http client cookie limit must be greater than zero");
    ruvia::ensure_positive_size(
        config.max_cookie_bytes_, "http client cookie byte limit must be greater than zero");
    ruvia::ensure_positive_size(
        config.max_response_bytes_, "http client response byte limit must be greater than zero");
    constexpr std::size_t max_http3_response_bytes = std::size_t{64} * 1024 * 1024;
    if (config.protocol_ == http_client_protocol::http3_only &&
        config.max_response_bytes_ > max_http3_response_bytes) {
        throw std::invalid_argument(
            "HTTP/3 client response byte limit must not exceed 64 MiB");
    }
    ruvia::ensure_positive_optional_durations("configured http client timeouts must be greater than zero",
        std::optional{config.connect_timeout_}, config.write_timeout_, config.request_timeout_,
        config.acquire_timeout_);
    validate_http_client_user_agent(config.user_agent_);
    if (config.cookies_.size() > config.max_cookies_) {
        throw std::invalid_argument(
            "configured HTTP client cookies exceed the client cookie limit");
    }
    std::size_t cookie_bytes = 0;
    for (const auto& [name, value] : config.cookies_) {
        if (!is_valid_http_header_name(name) || !::ruvia::is_valid_cookie_value(value)) {
            throw std::invalid_argument("configured HTTP client cookie is invalid");
        }
        if (name.size() > config.max_cookie_bytes_ - cookie_bytes) {
            throw std::invalid_argument(
                "configured HTTP client cookies exceed the client byte limit");
        }
        cookie_bytes += name.size();
        if (value.size() > config.max_cookie_bytes_ - cookie_bytes) {
            throw std::invalid_argument(
                "configured HTTP client cookies exceed the client byte limit");
        }
        cookie_bytes += value.size();
        if (cookie_bytes == config.max_cookie_bytes_) {
            throw std::invalid_argument(
                "configured HTTP client cookies exceed the client byte limit");
        }
        ++cookie_bytes;  // Every configured cookie is stored with the default "/" path.
    }
}

}  // namespace ruvia::detail
