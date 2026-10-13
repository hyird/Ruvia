#pragma once
#include <stdexcept>
#include <string>
#include <string_view>

#include "ruvia/http/cookies.h"
#include "ruvia/http/http_header.h"
#include "ruvia/web/http_client_types.h"

#include "client/client_tls.h"
#include "util/config_validation.h"

namespace ruvia::detail {

inline void validate_http_client_user_agent(std::string_view user_agent) {
    if (!user_agent.empty() && !is_valid_http_header_value(user_agent)) {
        throw std::invalid_argument("http client user agent is invalid");
    }
}

inline void validate_http_client_config(const http_client_config& config) {
    const auto scheme = config.scheme_;
    const std::string_view host = config.host_;
    const auto port = config.port_.value_or(scheme == http_scheme::https ? 443 : 80);
    if (scheme != http_scheme::http && scheme != http_scheme::https) {
        throw std::invalid_argument("http client scheme is invalid");
    }
    validate_client_tls_config(config.tls_);
    validate_client_tcp_config(config.tcp_);
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
    detail::ensure_positive_size(
        config.max_buffered_requests_, "http client buffered request limit must be greater than zero");
    detail::ensure_positive_size(config.max_cookies_, "http client cookie limit must be greater than zero");
    detail::ensure_positive_size(
        config.max_cookie_bytes_, "http client cookie byte limit must be greater than zero");
    detail::ensure_positive_size(
        config.max_response_bytes_, "http client response byte limit must be greater than zero");
    detail::ensure_positive_optional_durations("configured http client timeouts must be greater than zero",
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
