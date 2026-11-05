#pragma once
#include <cstddef>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

#include "ruvia/http/cookies.h"
#include "ruvia/http/http_header.h"
#include "ruvia/web/http_client_types.h"

#include "client/client_tls.h"
#include "client/client_tls_config_storage.h"
#include "client/client_user_agent.h"
#include "util/config_validation.h"
#include "util/tcp_socket_policies.h"

namespace ruvia::detail {

inline void validate_http_client_config(const http_client_config& config) {
    if (config.push_.max_queued_pushes_ == 0 || config.push_.max_concurrent_pushes_ == 0 ||
        (config.push_.timeout_ && config.push_.timeout_->count() <= 0)) {
        throw std::invalid_argument("HTTP push bounds and configured timeout must be positive");
    }
    if (config.advertisements_.max_queued_advertisements_ == 0 || config.advertisements_.max_retained_bytes_ == 0) {
        throw std::invalid_argument("HTTP advertisement bounds must be positive");
    }
    if (config.advertisements_.receive_origins_ && config.scheme_ != http_scheme::https) {
        throw std::invalid_argument("ORIGIN advertisements require authenticated HTTPS");
    }
    const auto scheme = config.scheme_;
    const std::string_view host = config.host_.view();
    const auto port = config.port_.value_or(scheme == http_scheme::https ? 443 : 80);
    if (scheme != http_scheme::http && scheme != http_scheme::https) {
        throw std::invalid_argument("http client scheme is invalid");
    }
    if (config.protocol_ != http_client_protocol::negotiate &&
        config.protocol_ != http_client_protocol::http1_only &&
        config.protocol_ != http_client_protocol::http2_only) {
        throw std::invalid_argument("http client protocol is invalid");
    }
    validate_client_tls_config(config.tls_);
    if (scheme == http_scheme::https && config.tls_.mode_ == client_tls_mode::disabled) {
        throw std::invalid_argument("HTTPS client requires TLS; use the http scheme for plaintext");
    }
    if (config.received_cookies_ != http_client_received_cookie_policy::ignore &&
        config.received_cookies_ != http_client_received_cookie_policy::retain_and_send) {
        throw std::invalid_argument("http client received cookie policy is invalid");
    }
    validate_client_origin_host(
        host, "http client host must not be empty", "http client host is invalid");
    if (port == 0) {
        throw std::invalid_argument("http client port must be greater than zero");
    }
    detail::ensure_positive_size(
        config.connection_count_, "http client connection count must be greater than zero");
    detail::ensure_positive_size(config.max_concurrent_http2_streams_per_connection_,
        "http client HTTP/2 stream limit per connection must be greater than zero");
    if (config.max_concurrent_http2_streams_per_connection_ >
        std::numeric_limits<std::size_t>::max() / config.connection_count_) {
        throw std::invalid_argument(
            "HTTP client connection and HTTP/2 stream capacity is too large");
    }
    detail::ensure_positive_size(
        config.max_buffered_requests_, "http client buffered request limit must be greater than zero");
    detail::ensure_positive_size(config.max_cookies_, "http client cookie limit must be greater than zero");
    detail::ensure_positive_size(
        config.max_cookie_bytes_, "http client cookie byte limit must be greater than zero");
    detail::ensure_positive_size(
        config.max_response_bytes_, "http client response byte limit must be greater than zero");
    detail::ensure_positive_optional_durations("configured http client timeouts must be greater than zero",
        std::optional{config.connect_timeout_}, config.write_timeout_, config.read_timeout_, config.request_timeout_,
        config.acquire_timeout_, config.idle_timeout_, config.http2_ping_interval_);
    validate_client_user_agent(config.user_agent_.view());
    validate_client_tcp_config(config.tcp_);
    // Every configured cookie is validated, including one a later entry with
    // the same name replaces; capacity applies to the deduplicated set.
    for (const auto& cookie : config.cookies_) {
        if (!is_valid_http_header_name(cookie.name()) || !::ruvia::is_valid_cookie_value(cookie.value())) {
            throw std::invalid_argument("configured HTTP client cookie is invalid");
        }
    }
}

// Capacity of the deduplicated configured cookies, each stored with the
// default "/" path like a retained cookie.
inline void validate_http_client_configured_cookie_capacity(
    std::span<const http_header> cookies, std::size_t max_cookies, std::size_t max_cookie_bytes) {
    if (cookies.size() > max_cookies) {
        throw std::invalid_argument("configured HTTP client cookies exceed the client cookie limit");
    }
    std::size_t cookie_bytes = 0;
    for (const auto& cookie : cookies) {
        for (const auto bytes : {cookie.name().size(), cookie.value().size(), std::size_t{1}}) {
            if (bytes > max_cookie_bytes - cookie_bytes) {
                throw std::invalid_argument("configured HTTP client cookies exceed the client byte limit");
            }
            cookie_bytes += bytes;
        }
    }
}

}  // namespace ruvia::detail
