#include <array>
#include <stdexcept>
#include <variant>

#include "ruvia/http/detail/field/http_connection_fields.h"
#include "ruvia/http/detail/field/http_field_list_grammar.h"
#include "ruvia/http/http1_client_response_parser.h"
#include "ruvia/http/http_base64.h"
#include "ruvia/http/http_protocol_managed_fields.h"
#include "ruvia/http/websocket_client_negotiation.h"

#include "client/http1_client_request_headers.h"
#include "parser/http_header_descriptor_storage.h"
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
            return detail::is_protocol_managed_request_field(kind, name) ||
                   http_ascii_equals_ignore_case(name, "sec-websocket-accept");
        default:
            return detail::is_protocol_managed_request_field(kind, name);
    }
}
}  // namespace

void validate_http1_websocket_client_handshake_configuration(std::span<const http_header_view> headers) {
    detail::http1_client_request_field_facts facts;
    http1_client_request_prepare_error error{};
    for (const auto& header : headers) {
        const auto kind = detail::classify_request_header(header.name());
        if (is_reserved_handshake_header(header.name(), kind)) {
            throw std::invalid_argument("invalid or reserved WebSocket handshake header");
        }
        if (!detail::analyze_http1_client_request_field(header, kind, facts, error)) {
            throw std::invalid_argument(http1_client_request_prepare_error_message(error).data());
        }
    }
    if (!detail::finish_http1_client_request_fields(facts, error)) {
        throw std::invalid_argument(http1_client_request_prepare_error_message(error).data());
    }
}

namespace {
template <typename header_type>
std::variant<prepared_http1_client_request, http1_client_request_buffer_too_small,
    http1_client_request_prepare_error>
prepare_websocket_request(const http_origin_view& origin, std::string_view target,
    std::span<const char, 24> encoded_key, const websocket_client_negotiation& negotiation,
    std::span<const header_type> headers, std::span<char> head_buffer, std::pmr::memory_resource* resource) {
    const auto negotiation_fields = negotiation.request_fields();
    detail::http_header_descriptor_storage<http_header_view> staged_headers(resource);
    staged_headers.reserve(headers.size() + 3U + negotiation_fields.size());
    for (const auto& header : headers) {
        staged_headers.append(http_header_view{header.name(), header.value()});
    }
    staged_headers.append(http_header_view{"Upgrade", "websocket"});
    staged_headers.append(
        http_header_view{detail::http_connection_field_name, detail::http_connection_upgrade_option});
    staged_headers.append(
        http_header_view{"Sec-WebSocket-Key", std::string_view(encoded_key.data(), encoded_key.size())});
    for (const auto& field : negotiation_fields) {
        staged_headers.append(http_header_view{field.name(), field.value()});
    }
    return http1_client_request_writer(resource)
        .prepare(origin, {.method_ = "GET", .target_ = target, .headers_ = staged_headers.view()}, head_buffer);
}

}  // namespace

std::variant<prepared_http1_client_request, http1_client_request_buffer_too_small,
    http1_client_request_prepare_error>
prepare_http1_websocket_client_request(const http_origin_view& origin, std::string_view target,
    std::span<const char, 24> encoded_key, const websocket_client_negotiation& negotiation,
    std::span<const http_header> headers, std::span<char> head_buffer, std::pmr::memory_resource* resource) {
    return prepare_websocket_request(origin, target, encoded_key, negotiation, headers, head_buffer, resource);
}

std::variant<prepared_http1_client_request, http1_client_request_buffer_too_small,
    http1_client_request_prepare_error>
prepare_http1_websocket_client_request(const http_origin_view& origin, std::string_view target,
    std::span<const char, 24> encoded_key, const websocket_client_negotiation& negotiation,
    std::span<const http_header_view> headers, std::span<char> head_buffer, std::pmr::memory_resource* resource) {
    return prepare_websocket_request(origin, target, encoded_key, negotiation, headers, head_buffer, resource);
}

std::variant<websocket_client_negotiation_result_view, websocket_client_handshake_error>
validate_http1_websocket_client_response(std::span<const char, 24> encoded_key,
    const websocket_client_negotiation& negotiation, const http1_parsed_client_response_head& response) {
    if (response.plan().protocol_upgrade() == nullptr ||
        response.head().status() != http_status::switching_protocols) {
        return websocket_client_handshake_error::response_status;
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
        return websocket_client_handshake_error::accept;
    }
    if (!has_upgrade) {
        return websocket_client_handshake_error::upgrade;
    }
    if (!has_connection) {
        return websocket_client_handshake_error::connection;
    }

    return detail::validate_websocket_client_selection(negotiation, response.head().headers(), false);
}
}  // namespace ruvia
