#include "ruvia/http/http1_websocket_client_handshake.h"

#include <array>
#include <stdexcept>
#include <variant>

#include "ruvia/http/detail/field/header_token_utils.h"
#include "ruvia/http/detail/field/http_connection_fields.h"
#include "ruvia/http/http1_client_response_parser.h"
#include "ruvia/http/http_base64.h"

#include "client/http1_client_request_headers.h"
#include "websocket/http_websocket_accept_key.h"

namespace ruvia {
namespace {
// The one application-header policy of the client handshake: it generates
// Host, the Upgrade/Connection pair and every Sec-WebSocket-* field, and an
// Upgrade request carries no content, so Trailer and Expect have no place.
bool is_reserved_handshake_header(std::string_view name, detail::request_header_kind kind) noexcept {
    switch (kind) {
        case detail::request_header_kind::host:
        case detail::request_header_kind::trailer:
        case detail::request_header_kind::expect:
        case detail::request_header_kind::connection:
        case detail::request_header_kind::upgrade:
        case detail::request_header_kind::content_length:
        case detail::request_header_kind::transfer_encoding:
        case detail::request_header_kind::sec_websocket_key:
        case detail::request_header_kind::sec_websocket_version:
        case detail::request_header_kind::sec_websocket_protocol:
        case detail::request_header_kind::sec_websocket_extensions:
            return true;
        case detail::request_header_kind::other:
            return http_ascii_equals_ignore_case(name, "keep-alive") ||
                   http_ascii_equals_ignore_case(name, "proxy-connection") ||
                   http_ascii_equals_ignore_case(name, "sec-websocket-accept");
        default:
            return false;
    }
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
            throw std::invalid_argument(std::string(http1_client_request_prepare_error_message(error)));
        }
    }
    if (!finalize_headers(facts, error)) {
        throw std::invalid_argument(std::string(http1_client_request_prepare_error_message(error)));
    }
    if (!user_agent.empty()) {
        if (!analyze_header(
                http_header_view{"User-Agent", user_agent}, detail::request_header_kind::user_agent, facts, error)) {
            throw std::invalid_argument(std::string(http1_client_request_prepare_error_message(error)));
        }
    }
}

http1_websocket_client_handshake::http1_websocket_client_handshake(
    http1_websocket_client_handshake_config_view options, std::pmr::memory_resource* resource)
    : resource_(resource != nullptr ? resource : std::pmr::get_default_resource()),
      key_(resource_),
      negotiation_({.subprotocols_ = options.subprotocols_, .deflate_ = options.deflate_}, resource_),

      headers_(resource_),
      user_agent_(options.user_agent_, resource_) {
    validate_configuration(options.headers_, options.subprotocols_, options.user_agent_);
    std::array<char, 24> encoded{};
    http_base64_encode(encoded.data(), options.nonce_);
    key_.assign(encoded.data(), 24);
    headers_.reserve(options.headers_.size());
    for (auto header : options.headers_) {
        headers_.push_back(http_header::copy_of(header.name(), header.value(), resource_));
    }
}

http1_client_request_prepare_result http1_websocket_client_handshake::prepare_request(
    const http_origin_view& origin, std::string_view target, std::span<char> head_buffer) const {
    std::pmr::vector<http_header_view> headers(resource_);
    headers.reserve(headers_.size() + 6);
    for (const auto& header : headers_) {
        headers.emplace_back(header.name(), header.value());
    }
    headers.emplace_back("Upgrade", "websocket");
    headers.emplace_back("Connection", "Upgrade");
    headers.emplace_back("Sec-WebSocket-Key", key_);
    for (const auto& field : negotiation_.request_fields()) {
        headers.emplace_back(field.name(), field.value());
    }
    if (!user_agent_.empty()) {
        headers.emplace_back("User-Agent", user_agent_);
    }
    return http1_client_request_writer({.resource_ = resource_})
        .prepare(origin, {.method_ = "GET", .target_ = target, .headers_ = headers}, head_buffer);
}

std::variant<http1_websocket_client_handshake_result_view, http1_websocket_client_handshake_error>
http1_websocket_client_handshake::validate_response(const http1_parsed_client_response_head& response) const {
    if (response.plan().protocol_upgrade() == nullptr ||
        response.head().status() != http_status::switching_protocols) {
        return http1_websocket_client_handshake_error::response_status;
    }
    detail::websocket_accept_key_type expected{};
    detail::encode_websocket_accept(expected, key_);
    std::size_t accepts = 0;
    bool accept_matches = false, has_upgrade = false, has_connection = false;

    for (const auto& header : response.head().headers()) {
        if (http_ascii_equals_ignore_case(header.name(), "Sec-WebSocket-Accept")) {
            ++accepts;
            accept_matches = http_trim_ows(header.value()) ==
                             std::string_view(expected.data(), expected.size());
        } else if (http_ascii_equals_ignore_case(header.name(), "Upgrade")) {
            has_upgrade = has_upgrade || detail::http_has_token(header.value(), "websocket");
        } else if (http_ascii_equals_ignore_case(header.name(), "Connection")) {
            has_connection = has_connection || detail::http_has_token(header.value(), "upgrade");
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

    const auto negotiation = negotiation_.validate_fields(response.head().headers());
    if ((negotiation.index() != 0)) {
        return std::get<1>(negotiation) == websocket_client_negotiation_error::subprotocol ? http1_websocket_client_handshake_error::subprotocol : http1_websocket_client_handshake_error::extensions;
    }
    return http1_websocket_client_handshake_result_view{std::get<0>(negotiation).selected_subprotocol_, std::get<0>(negotiation).compression_};
}
}  // namespace ruvia
