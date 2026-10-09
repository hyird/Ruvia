#pragma once

#include <concepts>
#include <memory_resource>
#include <string_view>

#include "ruvia/core/memory/pmr_resource.h"
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
    http_scheme scheme_;
    std::uint16_t port_;
    std::size_t connection_count_;
    std::size_t max_concurrent_http2_streams_per_connection_;
    std::size_t max_buffered_requests_;
    std::size_t max_cookies_;
    std::size_t max_cookie_bytes_;
    std::chrono::milliseconds connect_timeout_;
    std::optional<std::chrono::milliseconds> write_timeout_;
    std::optional<std::chrono::milliseconds> request_timeout_;
    std::optional<std::chrono::milliseconds> acquire_timeout_;
    std::size_t max_response_bytes_;
    http_client_protocol protocol_;
    quic_version initial_quic_version_;
    bool http3_early_data_;
    http3_qpack_config http3_qpack_;
    http_client_advertisement_config advertisements_;
    http_client_push_config push_;
    client_transport_config_storage transport_;
    http_client_received_cookie_policy received_cookies_;
    std::pmr::string user_agent_;
    std::pmr::vector<std::pair<std::pmr::string, std::pmr::string>> cookies_;

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

    template <typename source_type>
    [[nodiscard]] static const http3_qpack_config& qpack_config(const source_type& source_value) noexcept {
        if constexpr (std::same_as<source_type, http_client_config>) {
            return source_value.qpack_;
        } else {
            return source_value.http3_qpack_;
        }
    }

    // Resolve representation differences at the boundary, then own every field
    // and cookie through one allocator-bound construction path.
    template <typename source_type>
        requires(std::same_as<source_type, http_client_config> ||
                    std::same_as<source_type, http_client_config_storage>)
    http_client_config_storage(
        validated_config_tag_type, const source_type& source_value, std::pmr::memory_resource* resource)
        : host_(source_value.host_, resource),
          scheme_(source_value.scheme_),
          port_(resolved_port(source_value)),
          connection_count_(source_value.connection_count_),
          max_concurrent_http2_streams_per_connection_(source_value.max_concurrent_http2_streams_per_connection_),
          max_buffered_requests_(source_value.max_buffered_requests_),
          max_cookies_(source_value.max_cookies_),
          max_cookie_bytes_(source_value.max_cookie_bytes_),
          connect_timeout_(source_value.connect_timeout_),
          write_timeout_(source_value.write_timeout_),
          request_timeout_(source_value.request_timeout_),
          acquire_timeout_(source_value.acquire_timeout_),
          max_response_bytes_(source_value.max_response_bytes_),
          protocol_(source_value.protocol_),
          initial_quic_version_(source_value.initial_quic_version_),
          http3_early_data_(source_value.http3_early_data_),
          http3_qpack_(qpack_config(source_value)),
          advertisements_(source_value.advertisements_),
          push_(source_value.push_),
          transport_(make_client_transport_config_view(source_value), resource),
          received_cookies_(source_value.received_cookies_),
          user_agent_(source_value.user_agent_, resource),
          cookies_(resource) {
        cookies_.reserve(source_value.cookies_.size());
        for (const auto& [name, value] : source_value.cookies_) {
            cookies_.emplace_back(
                std::pmr::string(name, resource), std::pmr::string(value, resource));
        }
    }
};

[[nodiscard]] inline std::uint16_t http_client_port(const http_client_config_storage& config) noexcept {
    return config.port_;
}

using http_client_definition_type = named_capability_definition<http_client_config_storage>;

}  // namespace ruvia::detail
