#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/http/http_client.h"
#include "ruvia/http/websocket_client_negotiation.h"
#include "ruvia/web/websocket_client.h"

#include "client/client_tls.h"
#include "client/websocket_client_config_validation.h"

namespace ruvia::detail {

// Owned in place by one websocket_client_state and never copied or moved: the
// stored views and the negotiation borrow or copy the strings above them.
struct websocket_client_config_storage final {
    websocket_client_config_storage(
        const websocket_client_config& source_value, std::pmr::memory_resource* resource)
        : websocket_client_config_storage(
              validated_config_tag_type{}, validate(source_value), pmr_resource_or_default(resource)) {}

    websocket_client_config_storage(const websocket_client_config_storage&) = delete;
    websocket_client_config_storage& operator=(const websocket_client_config_storage&) = delete;

    websocket_scheme scheme_;
    websocket_client_protocol protocol_;
    std::pmr::string host_;
    std::uint16_t port_;
    // RFC 3986 uri-host (IPv6 literals bracketed), the fixed origin over it and
    // its default-port-omitted authority, computed once at startup.
    std::pmr::string uri_host_;
    http_origin_view origin_;
    std::pmr::string authority_;
    std::pmr::string target_;
    std::pmr::vector<http_header> headers_;
    std::pmr::vector<std::pmr::string> subprotocols_;
    std::pmr::vector<http_header_view> header_views_;
    std::pmr::vector<std::string_view> subprotocol_views_;
    websocket_client_deflate_offer deflate_{};
    int compression_level_{6};
    std::size_t max_message_bytes_;
    std::chrono::milliseconds connect_timeout_;
    std::optional<std::chrono::milliseconds> read_timeout_;
    std::optional<std::chrono::milliseconds> write_timeout_;
    std::optional<std::chrono::milliseconds> close_handshake_timeout_;
    websocket_heartbeat_config heartbeat_;
    client_tls_config_storage tls_;
    client_tcp_config tcp_;
    std::pmr::string user_agent_;
    // Extended CONNECT (HTTP/2) request fields and response
    // validation, built once from the headers, user agent and offers above.
    websocket_client_negotiation negotiation_;
    std::pmr::vector<http_header_view> extended_headers_;

private:
    struct validated_config_tag_type final {};

    [[nodiscard]] static const websocket_client_config& validate(
        const websocket_client_config& source_value) {
        validate_websocket_client_config(source_value);
        return source_value;
    }

    [[nodiscard]] static http_origin_view make_origin(
        websocket_scheme scheme, std::string_view uri_host, std::uint16_t port) {
        const http_origin_options options{.host_ = uri_host, .port_ = port};
        return scheme == websocket_scheme::wss ? http_origin_view::https(options) : http_origin_view::http(options);
    }

    [[nodiscard]] static std::pmr::vector<http_header> copy_headers(
        const websocket_client_config& source_value, std::pmr::memory_resource* resource) {
        std::pmr::vector<http_header> result_value(resource);
        result_value.reserve(source_value.headers_.size());
        for (const auto& header : source_value.headers_) {
            result_value.push_back(source_value.protocol_ == websocket_client_protocol::http2
                                       ? http_header::copy_with_lowercase_name(header.name(), header.value(), resource)
                                       : http_header(header.name(), header.value(), resource));
        }
        return result_value;
    }

    [[nodiscard]] static std::pmr::vector<std::pmr::string> copy_subprotocols(
        const websocket_client_config& source_value, std::pmr::memory_resource* resource) {
        std::pmr::vector<std::pmr::string> result_value(resource);
        result_value.reserve(source_value.subprotocols_.size());
        for (const auto& subprotocol : source_value.subprotocols_) {
            result_value.emplace_back(subprotocol.view());
        }
        return result_value;
    }

    [[nodiscard]] static std::pmr::vector<http_header_view> view_headers(
        const std::pmr::vector<http_header>& headers, std::pmr::memory_resource* resource) {
        std::pmr::vector<http_header_view> result_value(resource);
        result_value.reserve(headers.size());
        for (const auto& header : headers) {
            result_value.emplace_back(header.name(), header.value());
        }
        return result_value;
    }

    [[nodiscard]] static std::pmr::vector<std::string_view> view_subprotocols(
        const std::pmr::vector<std::pmr::string>& subprotocols, std::pmr::memory_resource* resource) {
        std::pmr::vector<std::string_view> result_value(resource);
        result_value.reserve(subprotocols.size());
        for (const auto& subprotocol : subprotocols) {
            result_value.emplace_back(subprotocol);
        }
        return result_value;
    }

    [[nodiscard]] static websocket_client_negotiation make_negotiation(std::span<const http_header_view> headers,
        std::span<const std::string_view> subprotocols, std::string_view user_agent,
        const websocket_client_deflate_offer& deflate, std::pmr::memory_resource* resource) {
        validate_websocket_client_protocols_and_headers(headers, subprotocols, user_agent, deflate);
        return websocket_client_negotiation(
            {.subprotocols_ = subprotocols, .deflate_ = deflate}, resource);
    }

    // The views borrow members initialized before them; element storage is
    // heap-owned, so views built from a member stay valid.
    websocket_client_config_storage(validated_config_tag_type, const websocket_client_config& source_value,
        std::pmr::memory_resource* resource)
        : scheme_(source_value.scheme_),
          protocol_(source_value.protocol_),
          host_(source_value.host_.view(), resource),
          port_(source_value.port_.value_or(source_value.scheme_ == websocket_scheme::wss ? 443 : 80)),
          uri_host_(client_uri_host(source_value.host_.view(), resource)),
          origin_(make_origin(scheme_, uri_host_, port_)),
          authority_(make_http_origin_authority(origin_, resource)),
          target_(source_value.target_.view(), resource),
          headers_(copy_headers(source_value, resource)),
          subprotocols_(copy_subprotocols(source_value, resource)),
          header_views_(view_headers(headers_, resource)),
          subprotocol_views_(view_subprotocols(subprotocols_, resource)),
          deflate_(source_value.deflate_),
          compression_level_(source_value.compression_level_),
          max_message_bytes_(source_value.max_message_bytes_),
          connect_timeout_(source_value.connect_timeout_),
          read_timeout_(source_value.read_timeout_),
          write_timeout_(source_value.write_timeout_),
          close_handshake_timeout_(source_value.close_handshake_timeout_),
          heartbeat_(normalize_websocket_heartbeat_config(source_value.heartbeat_)),
          tls_(source_value.tls_,
              source_value.scheme_ == websocket_scheme::wss ? client_tls_files::parse : client_tls_files::keep_paths,
              resource),
          tcp_(source_value.tcp_),
          user_agent_(source_value.user_agent_.view(), resource),
          negotiation_(make_negotiation(header_views_, subprotocol_views_, user_agent_, deflate_, resource)),
          extended_headers_(header_views_.begin(), header_views_.end(), resource) {
        if (!user_agent_.empty()) {
            extended_headers_.emplace_back("user-agent", user_agent_);
        }
    }
};

}  // namespace ruvia::detail
