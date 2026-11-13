#pragma once

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory_resource>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/http/cookies.h"
#include "ruvia/http/http_ascii.h"
#include "ruvia/http/http_client.h"
#include "ruvia/http/http_field_syntax.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_request_target.h"
#include "ruvia/web/http_client_types.h"

#include "client/client_tls.h"
#include "integration/named_capability.h"
#include "util/config_validation.h"
#include "util/tcp_socket_policies.h"

namespace ruvia::detail {

// The configured User-Agent (RFC 9110 section 10.1.5) of the HTTP client and
// both WebSocket client protocols: sent when a request or handshake carries no
// User-Agent field of its own; empty sends none.
inline void validate_client_user_agent(std::string_view user_agent) {
    if (!user_agent.empty() && !is_valid_http_header_value(user_agent)) {
        throw std::invalid_argument("client user agent is invalid");
    }
}

// The scheme of an alias and its fixed origin, absent for a scheme-only
// base_url_ whose requests each name their authority.
struct http_client_origin_spec final {
    http_scheme scheme_;
    std::optional<http_origin_view> origin_;
};

// A nonempty set of compiled-in versions; HTTP/3 only over https.
inline void validate_http_client_versions(const http_client_versions& versions, http_scheme scheme) {
    if (!versions.http1_ && !versions.http2_ && !versions.http3_) {
        throw std::invalid_argument("HTTP client versions must not be empty");
    }
#ifndef RUVIA_ENABLE_HTTP2
    if (versions.http2_) {
        throw std::invalid_argument("HTTP/2 support is disabled");
    }
#endif
#ifndef RUVIA_ENABLE_HTTP3
    if (versions.http3_) {
        throw std::invalid_argument("HTTP/3 support is disabled");
    }
#endif
    if (versions.http3_ && scheme != http_scheme::https) {
        throw std::invalid_argument("HTTP/3 requires HTTPS");
    }
}

[[nodiscard]] inline http_client_origin_spec validate_http_client_config(const http_client_config& config) {
    auto url = config.base_url_.view();
    const bool tls = http_ascii_equals_ignore_case(url.substr(0, 8), "https://");
    if (!tls && !http_ascii_equals_ignore_case(url.substr(0, 7), "http://")) {
        throw std::invalid_argument("HTTP client base_url must be an http(s) origin");
    }
    url.remove_prefix(tls ? 8 : 7);
    const http_client_origin_spec spec{.scheme_ = tls ? http_scheme::https : http_scheme::http, .origin_ = std::nullopt};
    const auto scheme = spec.scheme_;
    if (url.empty()) {
        // An egress alias without a fixed origin: every destination comes
        // from a request, so the dial addresses must pass a validator, and no
        // origin-scoped state (cookies, Alt-Svc) exists.
        if (!config.address_validator_) {
            throw std::invalid_argument("HTTP client alias without a fixed origin requires an address_validator_");
        }
        if (!config.cookies_.empty() || config.received_cookies_ != http_client_received_cookie_policy::ignore) {
            throw std::invalid_argument("HTTP client alias without a fixed origin has no cookie jar");
        }
        if (config.tls_.server_name_) {
            throw std::invalid_argument("HTTP client alias without a fixed origin takes each TLS server name from its request");
        }
    }
    if (url.ends_with('/')) {
        url.remove_suffix(1);
    }
    std::optional<http_authority_view> authority;
    if (!url.empty()) {
        authority = parse_http_authority(url);
        if (!authority || authority->host().empty() || authority->port_kind() == http_authority_port_kind::empty ||
            authority->port() == 0 || url.find_first_of("/?#") != std::string_view::npos) {
            throw std::invalid_argument("HTTP client base_url must contain only a fixed origin or a scheme");
        }
    }
    auto result = spec;
    if (authority) {
        result.origin_ = tls ? http_origin_view::https({.host_ = authority->host(), .port_ = authority->effective_port(443)})
                             : http_origin_view::http({.host_ = authority->host(), .port_ = authority->effective_port(80)});
    }
    auto host = result.origin_ ? result.origin_->host() : std::string_view{};
    if (host.starts_with('[')) {
        host = host.substr(1, host.size() - 2);
    }
    if (config.push_.max_queued_pushes_ == 0 || config.push_.max_concurrent_pushes_ == 0 ||
        (config.push_.timeout_ && config.push_.timeout_->value().count() <= 0)) {
        throw std::invalid_argument("HTTP push bounds and configured timeout must be positive");
    }
    if (config.advertisements_.max_queued_advertisements_ == 0 || config.advertisements_.max_retained_bytes_ == 0) {
        throw std::invalid_argument("HTTP advertisement bounds must be positive");
    }
    if (config.advertisements_.receive_origins_ && scheme != http_scheme::https) {
        throw std::invalid_argument("ORIGIN advertisements require authenticated HTTPS");
    }
    validate_http_client_versions(config.versions_, scheme);
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
    if (config.dial_host_) {
        validate_client_origin_host(config.dial_host_->view(), "HTTP client dial host must not be empty", "HTTP client dial host is invalid");
    }
    if (config.dial_port_ && config.dial_port_->value() == 0) {
        throw std::invalid_argument("HTTP client dial port must be positive");
    }
    detail::ensure_positive_optional_durations("HTTP DNS refresh interval must be positive", config.dns_refresh_interval_);
    if (result.origin_) {
        validate_client_origin_host(
            host, "http client host must not be empty", "http client host is invalid");
    }
    if (config.connection_limits_.max_open_ && *config.connection_limits_.max_open_ == 0) {
        throw std::invalid_argument("http client open connection limit must be greater than zero");
    }
    detail::ensure_positive_size(config.max_concurrent_http2_streams_per_connection_,
        "http client HTTP/2 stream limit per connection must be greater than zero");
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
    return result;
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

struct http_client_config_storage final {
    http_client_config_storage(const http_client_config& source_value, std::pmr::memory_resource* resource,
        client_tls_files files = client_tls_files::parse)
        : http_client_config_storage(
              source_value, validate_http_client_config(source_value), pmr_resource_or_default(resource), files) {}

    http_client_config_storage(
        const http_client_config_storage& source_value, std::pmr::memory_resource* resource)
        : http_client_config_storage(source_value,
              http_client_origin_spec{.scheme_ = source_value.scheme_,
                  .origin_ = source_value.fixed_origin_ ? std::optional(source_value.origin()) : std::nullopt},
              pmr_resource_or_default(resource), client_tls_files::keep_paths) {}

    std::pmr::string host_;
    std::pmr::string dial_host_;
    std::uint16_t dial_port_;
    std::function<bool(std::string_view, std::uint16_t)> address_validator_;
    std::optional<timeout_duration> dns_refresh_interval_;
    bool decode_content_;
    // False for a scheme-only base_url_: host_, uri_host_ and authority_ stay
    // empty, each request names its authority, and dial_host_ / dial_port_
    // (empty / 0 unless configured) default to that authority.
    bool fixed_origin_;
    // RFC 3986 uri-host (IPv6 literals bracketed) and the default-port-omitted
    // authority of the fixed origin, computed once at startup.
    std::pmr::string uri_host_;
    http_scheme scheme_;
    std::uint16_t port_;
    std::pmr::string authority_;
    http_client_connection_limits connection_limits_;
    std::size_t max_concurrent_http2_streams_per_connection_;
    std::size_t max_buffered_requests_;
    std::size_t max_cookies_;
    std::size_t max_cookie_bytes_;
    timeout_duration connect_timeout_;
    std::optional<timeout_duration> write_timeout_;
    std::optional<timeout_duration> read_timeout_;
    std::optional<timeout_duration> request_timeout_;
    std::optional<timeout_duration> acquire_timeout_;
    std::optional<timeout_duration> idle_timeout_;
    std::optional<timeout_duration> http2_ping_interval_;
    std::size_t max_response_bytes_;
    http_client_versions versions_;
    http_client_advertisement_config advertisements_;
    http_client_push_config push_;
    http_client_http3_config http3_;
    client_tls_config_storage tls_;
    client_tcp_config tcp_;
    http_client_received_cookie_policy received_cookies_;
    std::pmr::string user_agent_;
    // Configured name/value cookie pairs, one allocation per pair; a later
    // configured entry replaces an earlier one of the same name in place.
    std::pmr::vector<http_header> cookies_;

    // Validating factory for the fixed origin. Owners that are never moved
    // cache the returned view, which borrows uri_host_. Requires fixed_origin_.
    [[nodiscard]] http_origin_view origin() const {
        const http_origin_options options{.host_ = uri_host_, .port_ = port_};
        return scheme_ == http_scheme::https ? http_origin_view::https(options) : http_origin_view::http(options);
    }

private:
    [[nodiscard]] static std::string_view transport_host(const std::optional<http_origin_view>& origin) noexcept {
        if (!origin) {
            return {};
        }
        auto host = origin->host();
        return host.starts_with('[') ? host.substr(1, host.size() - 2) : host;
    }
    [[nodiscard]] static std::string_view user_agent_view(const http_client_config& source_value) noexcept {
        return source_value.user_agent_.view();
    }
    [[nodiscard]] static std::string_view user_agent_view(const http_client_config_storage& source_value) noexcept {
        return source_value.user_agent_;
    }
    [[nodiscard]] static std::string_view dial_host_view(const http_client_config& source, const std::optional<http_origin_view>& origin) noexcept {
        return source.dial_host_ ? source.dial_host_->view() : transport_host(origin);
    }
    [[nodiscard]] static std::string_view dial_host_view(const http_client_config_storage& source, const std::optional<http_origin_view>&) noexcept {
        return source.dial_host_;
    }
    [[nodiscard]] static std::uint16_t dial_port_value(const http_client_config& source, const std::optional<http_origin_view>& origin) noexcept {
        if (source.dial_port_) {
            return source.dial_port_->value();
        }
        return origin ? origin->port() : std::uint16_t{0};
    }
    [[nodiscard]] static std::uint16_t dial_port_value(const http_client_config_storage& source, const std::optional<http_origin_view>&) noexcept {
        return source.dial_port_;
    }

    // Startup parses TLS files; asynchronous updates first own paths only.
    // Allocator-rebound copies share already prepared material.
    template <typename source_type>
    [[nodiscard]] static client_tls_config_storage tls_storage(
        const source_type& source_value, http_scheme scheme, std::pmr::memory_resource* resource, client_tls_files files) {
        if constexpr (std::same_as<source_type, http_client_config>) {
            return {source_value.tls_,
                scheme == http_scheme::https ? files : client_tls_files::keep_paths,
                resource};
        } else {
            return {source_value.tls_, resource};
        }
    }

    // Resolve representation differences at the boundary, then own every field
    // and cookie through one allocator-bound construction path.
    template <typename source_type>
        requires(std::same_as<source_type, http_client_config> ||
                    std::same_as<source_type, http_client_config_storage>)
    http_client_config_storage(
        const source_type& source_value, http_client_origin_spec spec, std::pmr::memory_resource* resource, client_tls_files files)
        : host_(transport_host(spec.origin_), resource),
          dial_host_(dial_host_view(source_value, spec.origin_), resource),
          dial_port_(dial_port_value(source_value, spec.origin_)),
          address_validator_(source_value.address_validator_),
          dns_refresh_interval_(source_value.dns_refresh_interval_),
          decode_content_(source_value.decode_content_),
          fixed_origin_(spec.origin_.has_value()),
          uri_host_(spec.origin_ ? spec.origin_->host() : std::string_view{}, resource),
          scheme_(spec.scheme_),
          port_(spec.origin_ ? spec.origin_->port() : (spec.scheme_ == http_scheme::https ? 443 : 80)),
          authority_(spec.origin_ ? make_http_origin_authority(origin(), resource) : std::pmr::string(resource)),
          connection_limits_(source_value.connection_limits_),
          max_concurrent_http2_streams_per_connection_(source_value.max_concurrent_http2_streams_per_connection_),
          max_buffered_requests_(source_value.max_buffered_requests_),
          max_cookies_(source_value.max_cookies_),
          max_cookie_bytes_(source_value.max_cookie_bytes_),
          connect_timeout_(source_value.connect_timeout_),
          write_timeout_(source_value.write_timeout_),
          read_timeout_(source_value.read_timeout_),
          request_timeout_(source_value.request_timeout_),
          acquire_timeout_(source_value.acquire_timeout_),
          idle_timeout_(source_value.idle_timeout_),
          http2_ping_interval_(source_value.http2_ping_interval_),
          max_response_bytes_(source_value.max_response_bytes_),
          versions_(source_value.versions_),
          advertisements_(source_value.advertisements_),
          push_(source_value.push_),
          http3_(source_value.http3_),
          tls_(tls_storage(source_value, scheme_, resource, files)),
          tcp_(source_value.tcp_),
          received_cookies_(source_value.received_cookies_),
          user_agent_(user_agent_view(source_value), resource),
          cookies_(resource) {
        cookies_.reserve(source_value.cookies_.size());
        for (const auto& cookie : source_value.cookies_) {
            if constexpr (std::same_as<source_type, http_client_config>) {
                const auto existing = std::ranges::find_if(cookies_,
                    [name = cookie.name()](const http_header& entry) { return entry.name() == name; });
                if (existing != cookies_.end()) {
                    *existing = http_header(cookie.name(), cookie.value(), resource);
                    continue;
                }
            }
            cookies_.emplace_back(cookie.name(), cookie.value(), resource);
        }
        if constexpr (std::same_as<source_type, http_client_config>) {
            validate_http_client_configured_cookie_capacity(cookies_, max_cookies_, max_cookie_bytes_);
        }
    }
};

using http_client_definition_type = named_capability_definition<http_client_config_storage>;

}  // namespace ruvia::detail
