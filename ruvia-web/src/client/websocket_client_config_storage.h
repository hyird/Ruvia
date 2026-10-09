#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ruvia/core/memory/pmr_resource.h"
#include "ruvia/web/websocket_client.h"

#include "client/client_transport.h"
#include "client/websocket_client_config_validation.h"

namespace ruvia::detail {

struct websocket_client_stored_header final {
    websocket_client_stored_header(
        std::string_view name, std::string_view value, std::pmr::memory_resource* resource)
        : name_(name, resource),
          value_(value, resource) {}

    std::pmr::string name_;
    std::pmr::string value_;
};

struct websocket_client_config_storage final {
    websocket_client_config_storage(
        const websocket_client_config& source_value, std::pmr::memory_resource* resource)
        : websocket_client_config_storage(
              validated_config_tag_type{}, validate(source_value), pmr_resource_or_default(resource)) {}

    websocket_scheme scheme_;
    websocket_client_protocol protocol_;
    std::pmr::string host_;
    std::optional<std::uint16_t> port_;
    std::pmr::string target_;
    std::pmr::vector<websocket_client_stored_header> headers_;
    std::pmr::vector<std::pmr::string> subprotocols_;
    websocket_client_deflate_offer deflate_{};
    int compression_level_{6};
    http3_qpack_config qpack_{};
    std::size_t max_message_bytes_;
    std::chrono::milliseconds connect_timeout_;
    std::optional<std::chrono::milliseconds> read_timeout_;
    std::optional<std::chrono::milliseconds> write_timeout_;
    std::optional<std::chrono::milliseconds> close_handshake_timeout_;
    websocket_heartbeat_config heartbeat_;
    client_transport_config_storage transport_;
    std::pmr::string user_agent_;

private:
    struct validated_config_tag_type final {};

    [[nodiscard]] static const websocket_client_config& validate(
        const websocket_client_config& source_value) {
        validate_websocket_client_config(source_value);
        return source_value;
    }

    websocket_client_config_storage(validated_config_tag_type, const websocket_client_config& source_value,
        std::pmr::memory_resource* resource)
        : scheme_(source_value.scheme_),
          protocol_(source_value.protocol_),
          host_(source_value.host_, resource),
          port_(source_value.port_),
          target_(source_value.target_, resource),
          headers_(resource),
          subprotocols_(resource),
          deflate_(source_value.deflate_),
          compression_level_(source_value.compression_level_),
          qpack_(source_value.qpack_),
          max_message_bytes_(source_value.max_message_bytes_),
          connect_timeout_(source_value.connect_timeout_),
          read_timeout_(source_value.read_timeout_),
          write_timeout_(source_value.write_timeout_),
          close_handshake_timeout_(source_value.close_handshake_timeout_),
          heartbeat_(normalize_websocket_heartbeat_config(source_value.heartbeat_)),
          transport_(make_client_transport_config_view(source_value), resource),
          user_agent_(source_value.user_agent_, resource) {
        headers_.reserve(source_value.headers_.size());
        for (const auto& [name, value] : source_value.headers_) {
            headers_.emplace_back(name, value, resource);
        }
        subprotocols_.reserve(source_value.subprotocols_.size());
        for (const auto& subprotocol : source_value.subprotocols_) {
            subprotocols_.emplace_back(subprotocol);
        }
        std::pmr::vector<http_header_view> header_views(resource);
        header_views.reserve(headers_.size());
        for (const auto& header : headers_) {
            header_views.emplace_back(header.name_, header.value_);
        }
        std::pmr::vector<std::string_view> protocol_views(resource);
        protocol_views.reserve(subprotocols_.size());
        for (const auto& subprotocol : subprotocols_) {
            protocol_views.emplace_back(subprotocol);
        }
        validate_websocket_client_protocols_and_headers(header_views, protocol_views, user_agent_, deflate_);
    }
};

}  // namespace ruvia::detail
