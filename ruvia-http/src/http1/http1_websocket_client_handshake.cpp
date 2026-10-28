#include <array>
#include <stdexcept>
#include <variant>

#include "ruvia/http/detail/field/header_token_utils.h"
#include "ruvia/http/detail/field/http_connection_fields.h"
#include "ruvia/http/http1_client_response_parser.h"
#include "ruvia/http/http_base64.h"
#include "ruvia/http/http_protocol_managed_fields.h"
#include "ruvia/http/websocket_client_negotiation.h"

#include "client/http1_client_request_headers.h"
#include "websocket/http_websocket_accept_key.h"

namespace ruvia {
namespace {
// The one application-header policy of the client handshake: the request
// writer owns every protocol-managed field (Host, framing, Expect and the
// Upgrade/Connection pair) and the handshake generates every Sec-WebSocket-*
// field.
bool is_reserved_handshake_header(std::string_view name, detail::request_header_kind kind) noexcept {
    switch (kind) {
        case detail::request_header_kind::sec_websocket_key:
        case detail::request_header_kind::sec_websocket_version:
        case detail::request_header_kind::sec_websocket_protocol:
        case detail::request_header_kind::sec_websocket_extensions:
            return true;
        case detail::request_header_kind::other:
            return is_http_protocol_managed_request_field(name) ||
                   http_ascii_equals_ignore_case(name, "sec-websocket-accept");
        default:
            return is_http_protocol_managed_request_field(name);
    }
}

std::pmr::memory_resource* validated_handshake_resource(
    const http1_websocket_client_handshake_config_view& options,
    std::pmr::memory_resource* resource) {
    http1_websocket_client_handshake::validate_configuration(
        options.headers_, options.subprotocols_, options.user_agent_);
    return resource != nullptr ? resource : std::pmr::get_default_resource();
}
}  // namespace

void http1_websocket_client_handshake::validate_configuration(
    std::span<const http_header_view> headers, std::span<const std::string_view>,
    std::string_view user_agent) {
    request_header_facts facts;
    http1_client_request_prepare_error error{};
    for (const auto& header : headers) {
        const auto kind = detail::classify_request_header(header.name());
        if (is_reserved_handshake_header(header.name(), kind)) {
            throw std::invalid_argument("invalid or reserved WebSocket handshake header");
        }
        if (!analyze_header(header, kind, facts, error)) {
            throw std::invalid_argument(http1_client_request_prepare_error_message(error).data());
        }
    }
    if (!finalize_headers(facts, error)) {
        throw std::invalid_argument(http1_client_request_prepare_error_message(error).data());
    }
    if (!user_agent.empty()) {
        if (!analyze_header(
                http_header_view{"User-Agent", user_agent}, detail::request_header_kind::user_agent, facts, error)) {
            throw std::invalid_argument(http1_client_request_prepare_error_message(error).data());
        }
    }
}

http1_websocket_client_handshake::http1_websocket_client_handshake(
    http1_websocket_client_handshake_config_view options, std::pmr::memory_resource* resource)
    : resource_(validated_handshake_resource(options, resource)),
      negotiation_({.subprotocols_ = options.subprotocols_, .deflate_ = options.deflate_}, resource_),
      headers_(resource_),
      user_agent_(options.user_agent_, resource_) {
    http_base64_encode(key_.data(), options.nonce_);
    headers_.reserve(options.headers_.size());
    for (auto header : options.headers_) {
        headers_.emplace_back(header.name(), header.value(), resource_);
    }
}

std::variant<prepared_http1_client_request, http1_client_request_buffer_too_small,
    http1_client_request_prepare_error>
http1_websocket_client_handshake::prepare_request(const http_origin_view& origin,
    std::string_view target, std::span<const char, 24> encoded_key,
    const websocket_client_negotiation& negotiation, std::span<const http_header> headers,
    std::string_view user_agent, std::span<char> head_buffer, std::pmr::memory_resource* resource) {
    static constexpr std::size_t inline_header_capacity = 16;
    const auto negotiation_fields = negotiation.request_fields();
    const std::size_t total_headers =
        headers.size() + 3U + negotiation_fields.size() + (user_agent.empty() ? 0U : 1U);
    std::array<http_header_view, inline_header_capacity> inline_headers{};
    std::pmr::vector<http_header_view> heap_headers(resource);
    std::span<const http_header_view> active_headers{};
    if (total_headers <= inline_headers.size()) {
        std::size_t index = 0;
        for (const auto& header : headers) {
            inline_headers[index++] = http_header_view{header.name(), header.value()};
        }
        inline_headers[index++] = http_header_view{"Upgrade", "websocket"};
        inline_headers[index++] =
            http_header_view{detail::http_connection_field_name, detail::http_connection_upgrade_option};
        inline_headers[index++] =
            http_header_view{"Sec-WebSocket-Key", std::string_view(encoded_key.data(), encoded_key.size())};
        for (const auto& field : negotiation_fields) {
            inline_headers[index++] = http_header_view{field.name(), field.value()};
        }
        if (!user_agent.empty()) {
            inline_headers[index++] = http_header_view{"User-Agent", user_agent};
        }
        active_headers = std::span<const http_header_view>(inline_headers.data(), total_headers);
    } else {
        heap_headers.reserve(total_headers);
        for (const auto& header : headers) {
            heap_headers.emplace_back(header.name(), header.value());
        }
        heap_headers.emplace_back("Upgrade", "websocket");
        heap_headers.emplace_back(
            detail::http_connection_field_name, detail::http_connection_upgrade_option);
        heap_headers.emplace_back(
            "Sec-WebSocket-Key", std::string_view(encoded_key.data(), encoded_key.size()));
        for (const auto& field : negotiation_fields) {
            heap_headers.emplace_back(field.name(), field.value());
        }
        if (!user_agent.empty()) {
            heap_headers.emplace_back("User-Agent", user_agent);
        }
        active_headers = heap_headers;
    }
    return http1_client_request_writer(resource)
        .prepare(origin, {.method_ = "GET", .target_ = target, .headers_ = active_headers}, head_buffer);
}

std::variant<prepared_http1_client_request, http1_client_request_buffer_too_small,
    http1_client_request_prepare_error>
http1_websocket_client_handshake::prepare_request(
    const http_origin_view& origin, std::string_view target, std::span<char> head_buffer) const {
    return prepare_request(
        origin, target, key_, negotiation_, headers_, user_agent_, head_buffer, resource_);
}

std::variant<websocket_client_negotiation_result_view, http1_websocket_client_handshake_error>
http1_websocket_client_handshake::validate_response(std::span<const char, 24> encoded_key,
    const websocket_client_negotiation& negotiation,
    const http1_parsed_client_response_head& response) {
    if (response.plan().protocol_upgrade() == nullptr ||
        response.head().status() != http_status::switching_protocols) {
        return http1_websocket_client_handshake_error::response_status;
    }
    detail::websocket_accept_key_type expected{};
    detail::encode_websocket_accept(
        expected, std::string_view(encoded_key.data(), encoded_key.size()));
    std::size_t accepts = 0;
    bool accept_matches = false, has_upgrade = false, has_connection = false;
    for (const auto& header : response.head().headers()) {
        switch (detail::classify_request_header(header.name())) {
            case detail::request_header_kind::upgrade:
                has_upgrade = has_upgrade || detail::http_has_token(header.value(), "websocket");
                break;
            case detail::request_header_kind::connection:
                has_connection = has_connection ||
                                 detail::http_has_token(header.value(), detail::http_connection_upgrade_option);
                break;
            case detail::request_header_kind::other:
                if (http_ascii_equals_ignore_case(header.name(), "Sec-WebSocket-Accept")) {
                    ++accepts;
                    accept_matches = http_trim_ows(header.value()) ==
                                     std::string_view(expected.data(), expected.size());
                }
                break;
            default:
                break;
        }
    }
    if (accepts != 1 || !accept_matches) {
        return http1_websocket_client_handshake_error::accept;
    }
    if (!has_upgrade) {
        return http1_websocket_client_handshake_error::upgrade;
    }
    if (!has_connection) {
        return http1_websocket_client_handshake_error::connection;
    }

    const auto negotiated = negotiation.validate_fields(response.head().headers());
    if ((negotiated.index() != 0)) {
        return std::get<1>(negotiated) == websocket_client_negotiation_error::subprotocol
                   ? http1_websocket_client_handshake_error::subprotocol
                   : http1_websocket_client_handshake_error::extensions;
    }
    return std::get<0>(negotiated);
}

std::variant<websocket_client_negotiation_result_view, http1_websocket_client_handshake_error>
http1_websocket_client_handshake::validate_response(
    const http1_parsed_client_response_head& response) const {
    return validate_response(key_, negotiation_, response);
}
}  // namespace ruvia
