#include "websocket/http_websocket_handshake_fields.h"

#include <algorithm>
#include <cstddef>
#include <span>
#include <stdexcept>
#include <utility>

#include "ruvia/http/detail/field/header_token_utils.h"
#include "ruvia/http/detail/field/http_header_section_size.h"
#include "ruvia/http/detail/parser/http_parser_syntax.h"
#include "ruvia/http/detail/util/http_ows.h"
#include "ruvia/http/detail/util/pmr_resource.h"
#include "ruvia/http/http_request.h"
#include "ruvia/http/websocket_handshake.h"

#include "http_header_access.h"
#include "request/http_request_access.h"
#include "websocket/http_websocket_accept_key.h"
#include "websocket/http_websocket_permessage_deflate.h"
#include "websocket/websocket_server_negotiation.h"
#include "websocket/websocket_subprotocol_set.h"

namespace ruvia::detail {
namespace {

[[nodiscard]] std::pmr::vector<http_header> copy_websocket_response_headers(
    std::span<const http_header_view> headers, std::string_view subprotocol,
    websocket_compression compression, std::pmr::memory_resource* resource) {
    if (headers.size() > max_http_header_fields - 5) {
        throw std::length_error("too many WebSocket handshake response headers");
    }
    // Account for generated fields as well. The HTTP/1 fields conservatively
    // cover HTTP/2's smaller :status + Date overhead, so both drivers share one
    // safe bound for the application field section.
    http_header_section_size size;
    (void)size.add("upgrade", "websocket");
    (void)size.add("connection", "Upgrade");
    (void)size.add("sec-websocket-accept", "0000000000000000000000000000");
    if (!subprotocol.empty() && !size.add("sec-websocket-protocol", subprotocol)) {
        throw std::length_error("WebSocket handshake response headers are too large");
    }
    const auto extension = get_websocket_compression_extension(compression);
    if (!extension.empty() && !size.add("sec-websocket-extensions", extension.view())) {
        throw std::length_error("WebSocket handshake response headers are too large");
    }
    std::pmr::vector<http_header> result(resource);
    result.reserve(headers.size());
    for (const auto& header : headers) {
        if (!is_valid_http_header_name(header.name()) || !is_valid_http_header_value(header.value()) ||
            http_trim_ows(header.value()).size() != header.value().size()) {
            throw std::invalid_argument("invalid WebSocket handshake response header");
        }
        std::pmr::string name(header.name(), resource);
        for (char& ch : name) {
            ch = static_cast<char>(http_ascii_to_lower(static_cast<unsigned char>(ch)));
        }
        if (name == "connection" || name == "upgrade" || name == "keep-alive" ||
            name == "proxy-connection" || name == "transfer-encoding" ||
            name == "content-length" || name == "trailer" || name == "te" ||
            name.starts_with("sec-websocket-")) {
            throw std::invalid_argument("WebSocket handshake controls this response header");
        }
        if (!size.add(name, header.value())) {
            throw std::length_error("WebSocket handshake response headers are too large");
        }
        result.push_back(http_header_access::make(std::move(name), std::pmr::string(header.value(), resource)));
    }
    return result;
}

[[nodiscard]] bool append_websocket_subprotocol_offers(std::span<const http_header_view> headers,
    websocket_subprotocol_set& protocols, bool& present) noexcept {
    for (const auto& header : headers) {
        if (!http_ascii_equals_ignore_case(header.name(), "Sec-WebSocket-Protocol")) {
            continue;
        }
        present = true;
        if (!protocols.append_list(header.value())) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool append_websocket_subprotocol_offers(const http_request& request,
    websocket_subprotocol_set& protocols, bool& present) noexcept {
    const auto headers = request.headers();
    for (std::size_t i = 0; i < headers.size(); ++i) {
        if (http_request_access::header_kind(request, i) !=
            static_cast<std::uint8_t>(request_header_kind::sec_websocket_protocol)) {
            continue;
        }
        present = true;
        if (!protocols.append_list(headers[i].value())) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool websocket_subprotocol_header_offers_valid(
    std::span<const http_header_view> headers) noexcept {
    websocket_subprotocol_set protocols;
    bool present = false;
    return append_websocket_subprotocol_offers(headers, protocols, present) &&
           (!present || !protocols.empty());
}

[[nodiscard]] bool websocket_protocol_token_valid(std::string_view protocol) noexcept {
    if (protocol.empty()) {
        return false;
    }
    return std::ranges::all_of(
        protocol, [](char ch) noexcept { return is_http_token_char(static_cast<unsigned char>(ch)); });
}

void skip_websocket_extension_ows(std::string_view value, std::size_t& cursor_value) noexcept {
    while (cursor_value < value.size() && (value[cursor_value] == ' ' || value[cursor_value] == '\t')) {
        ++cursor_value;
    }
}

[[nodiscard]] bool consume_websocket_extension_token(
    std::string_view value, std::size_t& cursor_value) noexcept {
    const auto start = cursor_value;
    while (cursor_value < value.size() && is_http_token_char(static_cast<unsigned char>(value[cursor_value]))) {
        ++cursor_value;
    }
    return cursor_value != start;
}

[[nodiscard]] bool consume_websocket_extension_quoted_token(
    std::string_view value, std::size_t& cursor_value) noexcept {
    if (cursor_value == value.size() || value[cursor_value] != '"') {
        return false;
    }
    ++cursor_value;
    bool decoded_any = false;
    while (cursor_value < value.size()) {
        auto ch = value[cursor_value++];
        if (ch == '"') {
            return decoded_any;
        }
        if (ch == '\\') {
            if (cursor_value == value.size()) {
                return false;
            }
            ch = value[cursor_value++];
        }
        // RFC 6455 section 4.3 narrows quoted-string extension values:
        // after quoted-pair unescaping, the result still has to be a token.
        if (!is_http_token_char(static_cast<unsigned char>(ch))) {
            return false;
        }
        decoded_any = true;
    }
    return false;
}

[[nodiscard]] bool append_websocket_extension_list(
    std::string_view value, bool& has_extension) noexcept {
    std::size_t cursor_value = 0;
    while (true) {
        skip_websocket_extension_ows(value, cursor_value);
        // RFC 2616's #rule permits null comma-list elements, but 1#extension
        // still requires at least one real extension across the logical field.
        while (cursor_value < value.size() && value[cursor_value] == ',') {
            ++cursor_value;
            skip_websocket_extension_ows(value, cursor_value);
        }
        if (cursor_value == value.size()) {
            return true;
        }
        if (!consume_websocket_extension_token(value, cursor_value)) {
            return false;
        }
        has_extension = true;

        while (true) {
            skip_websocket_extension_ows(value, cursor_value);
            if (cursor_value == value.size()) {
                return true;
            }
            if (value[cursor_value] == ',') {
                ++cursor_value;
                break;
            }
            if (value[cursor_value] != ';') {
                return false;
            }
            ++cursor_value;
            skip_websocket_extension_ows(value, cursor_value);
            if (!consume_websocket_extension_token(value, cursor_value)) {
                return false;
            }
            skip_websocket_extension_ows(value, cursor_value);
            if (cursor_value == value.size() || value[cursor_value] != '=') {
                continue;
            }
            ++cursor_value;
            skip_websocket_extension_ows(value, cursor_value);
            if (cursor_value == value.size()) {
                return false;
            }
            if (value[cursor_value] == '"') {
                if (!consume_websocket_extension_quoted_token(value, cursor_value)) {
                    return false;
                }
            } else if (!consume_websocket_extension_token(value, cursor_value)) {
                return false;
            }
        }
    }
}

[[nodiscard]] bool websocket_extension_header_offers_valid(
    std::span<const http_header_view> headers) noexcept {
    bool present = false;
    bool has_extension = false;
    for (const auto& header : headers) {
        if (!http_ascii_equals_ignore_case(header.name(), "Sec-WebSocket-Extensions")) {
            continue;
        }
        present = true;
        if (!append_websocket_extension_list(header.value(), has_extension)) {
            return false;
        }
    }
    return !present || has_extension;
}

[[nodiscard]] bool websocket_extension_header_offers_valid(const http_request& request) noexcept {
    bool present = false;
    bool has_extension = false;
    const auto headers = request.headers();
    for (std::size_t i = 0; i < headers.size(); ++i) {
        if (http_request_access::header_kind(request, i) !=
            static_cast<std::uint8_t>(request_header_kind::sec_websocket_extensions)) {
            continue;
        }
        present = true;
        if (!append_websocket_extension_list(headers[i].value(), has_extension)) {
            return false;
        }
    }
    return !present || has_extension;
}

}  // namespace

bool websocket_subprotocol_offers_valid(const http_request& request) noexcept {
    websocket_subprotocol_set protocols;
    bool present = false;
    return append_websocket_subprotocol_offers(request, protocols, present) &&
           (!present || !protocols.empty());
}

bool websocket_extension_offers_valid(const http_request& request) noexcept {
    return websocket_extension_header_offers_valid(request);
}

bool websocket_client_offer_headers_valid(std::span<const http_header_view> headers) noexcept {
    return websocket_subprotocol_header_offers_valid(headers) &&
           websocket_extension_header_offers_valid(headers);
}

bool websocket_protocol_offered(const http_request& request, std::string_view protocol) noexcept {
    if (!websocket_protocol_token_valid(protocol)) {
        return false;
    }
    websocket_subprotocol_set protocols;
    bool present = false;
    return append_websocket_subprotocol_offers(request, protocols, present) && present &&
           !protocols.empty() && protocols.contains(protocol);
}

std::string_view choose_websocket_subprotocol(
    const http_request& request, std::span<const std::string_view> supported) noexcept {
    websocket_subprotocol_set configured;
    for (const auto protocol : supported) {
        if (!configured.append(protocol)) {
            return {};
        }
    }

    websocket_subprotocol_set offered;
    bool present = false;
    if (!append_websocket_subprotocol_offers(request, offered, present) || !present ||
        offered.empty()) {
        return {};
    }
    for (const auto protocol : supported) {
        if (offered.contains(protocol)) {
            return protocol;
        }
    }
    return {};
}

websocket_server_negotiation::websocket_server_negotiation(std::string_view subprotocol,
    websocket_compression compression, std::span<const http_header_view> response_headers_value,
    std::pmr::memory_resource* resource)
    : subprotocol_(subprotocol, http_pmr_resource_or_default(resource)),
      compression_(compression),
      extensions_(resource != nullptr ? resource : std::pmr::get_default_resource()),
      response_headers_(copy_websocket_response_headers(response_headers_value, subprotocol, compression, http_pmr_resource_or_default(resource))) {
    const auto extension = get_websocket_compression_extension(compression);
    extensions_.assign(extension.view());
}

websocket_server_negotiation make_websocket_server_negotiation(
    const http_request& request, websocket_server_negotiation_options options) {
    return websocket_server_negotiation(
        choose_websocket_subprotocol(request, options.supported_subprotocols_),
        websocket_negotiate_permessage_deflate(request, options.deflate_), options.response_headers_, options.resource_);
}

}  // namespace ruvia::detail

namespace ruvia {

websocket_server_handshake make_websocket_server_handshake(
    const http_request& request, websocket_server_handshake_options options) {
    detail::websocket_accept_key_type accept;
    detail::encode_websocket_accept(
        accept, detail::request_known_header(request, detail::request_header_kind::sec_websocket_key));
    std::pmr::string subprotocol(
        detail::choose_websocket_subprotocol(request, options.supported_subprotocols_),
        detail::http_pmr_resource_or_default(options.resource_));
    const auto compression = detail::websocket_negotiate_permessage_deflate(request, options.deflate_);
    auto response_headers_value = detail::copy_websocket_response_headers(options.response_headers_,
        subprotocol, compression, detail::http_pmr_resource_or_default(options.resource_));
    return websocket_server_handshake(accept, std::move(subprotocol), compression, std::move(response_headers_value));
}

}  // namespace ruvia
