#pragma once
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

#include "ruvia/http/cookies.h"
#include "ruvia/http/http_ascii.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_request_target.h"
#include "ruvia/web/http_client_types.h"

#include "client/client_tls.h"
#include "client/client_tls_config_storage.h"
#include "client/client_user_agent.h"
#include "util/config_validation.h"
#include "util/tcp_socket_policies.h"

namespace ruvia::detail {

[[nodiscard]] inline http_origin_view validate_http_client_config(const http_client_config& config) {
    auto url = config.base_url_.view();
    const bool tls = http_ascii_equals_ignore_case(url.substr(0, 8), "https://");
    if (!tls && !http_ascii_equals_ignore_case(url.substr(0, 7), "http://")) {
        throw std::invalid_argument("HTTP client base_url must be an http(s) origin");
    }
    url.remove_prefix(tls ? 8 : 7);
    if (url.ends_with('/')) {
        url.remove_suffix(1);
    }
    const auto authority = parse_http_authority(url);
    if (!authority || authority->host().empty() || authority->port_kind() == http_authority_port_kind::empty ||
        authority->port() == 0 || url.find_first_of("/?#") != std::string_view::npos) {
        throw std::invalid_argument("HTTP client base_url must contain only a fixed origin");
    }
    const auto origin = tls ? http_origin_view::https({.host_ = authority->host(), .port_ = authority->effective_port(443)})
                            : http_origin_view::http({.host_ = authority->host(), .port_ = authority->effective_port(80)});
    const auto scheme = origin.scheme();
    auto host = origin.host();
    if (host.starts_with('[')) {
        host = host.substr(1, host.size() - 2);
    }
    if (config.push_.max_queued_pushes_ == 0 || config.push_.max_concurrent_pushes_ == 0 ||
        (config.push_.timeout_ && config.push_.timeout_->count() <= 0)) {
        throw std::invalid_argument("HTTP push bounds and configured timeout must be positive");
    }
    if (config.advertisements_.max_queued_advertisements_ == 0 || config.advertisements_.max_retained_bytes_ == 0) {
        throw std::invalid_argument("HTTP advertisement bounds must be positive");
    }
    if (config.advertisements_.receive_origins_ && scheme != http_scheme::https) {
        throw std::invalid_argument("ORIGIN advertisements require authenticated HTTPS");
    }
    if (config.protocol_ != http_client_protocol::negotiate &&
        config.protocol_ != http_client_protocol::http1_only &&
        config.protocol_ != http_client_protocol::http2_only &&
        config.protocol_ != http_client_protocol::http3_only) {
        throw std::invalid_argument("http client protocol is invalid");
    }
#ifndef RUVIA_ENABLE_HTTP2
    if (config.protocol_ == http_client_protocol::http2_only) {
        throw std::invalid_argument("HTTP/2 support is disabled");
    }
#endif
#ifndef RUVIA_ENABLE_HTTP3
    if (config.protocol_ == http_client_protocol::http3_only) {
        throw std::invalid_argument("HTTP/3 support is disabled");
    }
#endif
    if (config.protocol_ == http_client_protocol::http3_only && scheme != http_scheme::https) {
        throw std::invalid_argument("HTTP/3 requires HTTPS");
    }
    if (config.http3_.max_concurrent_streams_ == 0 || config.http3_.max_alt_svc_entries_ == 0 ||
        config.http3_.max_datagram_bytes_ == 0 || config.http3_.max_stream_output_bytes_ == 0) {
        throw std::invalid_argument("HTTP/3 resource bounds must be positive");
    }
    if (config.http3_.max_datagram_bytes_ < 1200 || config.http3_.max_datagram_bytes_ > 65527 ||
        config.http3_.max_concurrent_streams_ > (std::numeric_limits<std::uint32_t>::max)()) {
        throw std::invalid_argument("HTTP/3 transport limits exceed QUIC bounds");
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
    detail::ensure_positive_size(
        config.connection_count_, "http client connection count must be greater than zero");
    detail::ensure_positive_size(config.max_concurrent_http2_streams_per_connection_,
        "http client HTTP/2 stream limit per connection must be greater than zero");
    std::size_t stream_capacity = config.protocol_ == http_client_protocol::http1_only ? 1 : config.max_concurrent_http2_streams_per_connection_;
#ifdef RUVIA_ENABLE_HTTP3
    if (config.protocol_ == http_client_protocol::http3_only) {
        stream_capacity = config.http3_.max_concurrent_streams_;
    } else if (config.protocol_ == http_client_protocol::negotiate &&
               config.http3_.max_concurrent_streams_ > stream_capacity) {
        stream_capacity = config.http3_.max_concurrent_streams_;
    }
#endif
    if (stream_capacity > std::numeric_limits<std::size_t>::max() / config.connection_count_) {
        throw std::invalid_argument("HTTP client connection and stream capacity is too large");
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
    return origin;
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
