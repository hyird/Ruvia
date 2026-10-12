#pragma once

#include <concepts>
#include <memory_resource>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/core/memory/pmr_resource.h"
#include "ruvia/http/http_client.h"
#include "ruvia/web/http_client_types.h"

#include "client/client_transport.h"
#include "client/http_client_config_validation.h"
#include "integration/named_capability.h"

namespace ruvia::detail {

struct http_client_config_storage final {
    http_client_config_storage(const http_client_config& source_value, std::pmr::memory_resource* resource)
        : http_client_config_storage(
              validated_config_tag_type{}, validate(source_value), pmr_resource_or_default(resource)) {}

    http_client_config_storage(
        const http_client_config_storage& source_value, std::pmr::memory_resource* resource)
        : http_client_config_storage(validated_config_tag_type{}, source_value, pmr_resource_or_default(resource)) {}

    std::pmr::string host_;
    // RFC 3986 uri-host (IPv6 literals bracketed) and the default-port-omitted
    // authority of the fixed origin, computed once at startup.
    std::pmr::string uri_host_;
    http_scheme scheme_;
    std::uint16_t port_;
    std::pmr::string authority_;
    std::size_t connection_count_;
    std::size_t max_buffered_requests_;
    std::size_t max_cookies_;
    std::size_t max_cookie_bytes_;
    std::chrono::milliseconds connect_timeout_;
    std::optional<std::chrono::milliseconds> write_timeout_;
    std::optional<std::chrono::milliseconds> request_timeout_;
    std::optional<std::chrono::milliseconds> acquire_timeout_;
    std::size_t max_response_bytes_;
    client_tls_config_storage tls_;
    client_tcp_config tcp_;
    http_client_received_cookie_policy received_cookies_;
    std::pmr::string user_agent_;
    // Configured name/value cookie pairs, one allocation per pair.
    std::pmr::vector<http_header> cookies_;

    // Validating factory for the fixed origin. Owners that are never moved
    // cache the returned view, which borrows uri_host_.
    [[nodiscard]] http_origin_view origin() const {
        const http_origin_options options{.host_ = uri_host_, .port_ = port_};
        return scheme_ == http_scheme::https ? http_origin_view::https(options) : http_origin_view::http(options);
    }

private:
    struct validated_config_tag_type final {};

    [[nodiscard]] static const http_client_config& validate(const http_client_config& source_value) {
        validate_http_client_config(source_value);
        return source_value;
    }

    template <typename source_type>
    [[nodiscard]] static std::uint16_t resolved_port(const source_type& source_value) noexcept {
        if constexpr (std::same_as<source_type, http_client_config>) {
            return source_value.port_.value_or(source_value.scheme_ == http_scheme::https ? 443 : 80);
        } else {
            return source_value.port_;
        }
    }

    // Public https configuration parses the TLS files every pool's context
    // installs; allocator-rebound copies share them.
    template <typename source_type>
    [[nodiscard]] static client_tls_config_storage tls_storage(
        const source_type& source_value, std::pmr::memory_resource* resource) {
        if constexpr (std::same_as<source_type, http_client_config>) {
            return {source_value.tls_,
                source_value.scheme_ == http_scheme::https ? client_tls_files::parse : client_tls_files::keep_paths,
                resource};
        } else {
            return {source_value.tls_, resource};
        }
    }

    [[nodiscard]] static std::string_view cookie_name(const std::pair<std::string, std::string>& cookie) noexcept {
        return cookie.first;
    }
    [[nodiscard]] static std::string_view cookie_name(const http_header& cookie) noexcept {
        return cookie.name();
    }
    [[nodiscard]] static std::string_view cookie_value(const std::pair<std::string, std::string>& cookie) noexcept {
        return cookie.second;
    }
    [[nodiscard]] static std::string_view cookie_value(const http_header& cookie) noexcept {
        return cookie.value();
    }

    // Resolve representation differences at the boundary, then own every field
    // and cookie through one allocator-bound construction path.
    template <typename source_type>
        requires(std::same_as<source_type, http_client_config> ||
                    std::same_as<source_type, http_client_config_storage>)
    http_client_config_storage(
        validated_config_tag_type, const source_type& source_value, std::pmr::memory_resource* resource)
        : host_(source_value.host_, resource),
          uri_host_(client_uri_host(source_value.host_, resource)),
          scheme_(source_value.scheme_),
          port_(resolved_port(source_value)),
          authority_(make_http_origin_authority(origin(), resource)),
          connection_count_(source_value.connection_count_),
          max_buffered_requests_(source_value.max_buffered_requests_),
          max_cookies_(source_value.max_cookies_),
          max_cookie_bytes_(source_value.max_cookie_bytes_),
          connect_timeout_(source_value.connect_timeout_),
          write_timeout_(source_value.write_timeout_),
          request_timeout_(source_value.request_timeout_),
          acquire_timeout_(source_value.acquire_timeout_),
          max_response_bytes_(source_value.max_response_bytes_),
          tls_(tls_storage(source_value, resource)),
          tcp_(source_value.tcp_),
          received_cookies_(source_value.received_cookies_),
          user_agent_(source_value.user_agent_, resource),
          cookies_(resource) {
        cookies_.reserve(source_value.cookies_.size());
        for (const auto& cookie : source_value.cookies_) {
            cookies_.push_back(http_header::copy_of(cookie_name(cookie), cookie_value(cookie), resource));
        }
    }
};

using http_client_definition_type = named_capability_definition<http_client_config_storage>;

}  // namespace ruvia::detail
