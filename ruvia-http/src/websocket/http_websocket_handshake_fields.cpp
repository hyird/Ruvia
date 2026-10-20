#include "websocket/http_websocket_handshake_fields.h"

#include <cstddef>
#include <span>
#include <stdexcept>
#include <utility>

#include "ruvia/http/detail/field/header_token_utils.h"
#include "ruvia/http/detail/field/http_connection_fields.h"
#include "ruvia/http/detail/parser/http_parser_syntax.h"
#include "ruvia/http/detail/util/pmr_resource.h"
#include "ruvia/http/http_field_section.h"
#include "ruvia/http/http_field_whitespace.h"
#include "ruvia/http/http_request.h"
#include "ruvia/http/websocket_handshake.h"
#include "ruvia/http/websocket_subprotocol_set.h"

#include "request/http_request_access.h"
#include "websocket/http_websocket_accept_key.h"
#include "websocket/http_websocket_permessage_deflate.h"
#include "websocket/websocket_server_negotiation.h"

namespace ruvia::detail {
namespace {

[[nodiscard]] std::pmr::vector<http_header> copy_websocket_response_headers(
    std::span<const http_header_view> headers, std::string_view subprotocol,
    std::string_view extension, std::pmr::memory_resource* resource) {
    if (headers.size() > max_http_header_fields - 5) {
        throw std::length_error("too many WebSocket handshake response headers");
    }
    // Account for the whole emitted head: the 101 status line, the generated
    // fields, the application field section, and the terminating empty line.
    http_field_section_budget size;
    constexpr auto status = http_status_code_token(http_status::switching_protocols);
    (void)size.add(std::string_view("HTTP/1.1 ").size() + status.size() + 1 +
                   http_reason_phrase(http_status::switching_protocols).size() + 2);
    (void)size.add(2);
    (void)size.add_field("upgrade", "websocket");
    (void)size.add_field("connection", "Upgrade");
    (void)size.add_field("sec-websocket-accept", "0000000000000000000000000000");
    if (!subprotocol.empty() && !size.add_field("sec-websocket-protocol", subprotocol)) {
        throw std::length_error("WebSocket handshake response headers are too large");
    }
    if (!extension.empty() && !size.add_field("sec-websocket-extensions", extension)) {
        throw std::length_error("WebSocket handshake response headers are too large");
    }
    std::pmr::vector<http_header> result(resource);
    result.reserve(headers.size());
    for (const auto& header : headers) {
        if (!is_valid_http_header_name(header.name()) || !is_valid_http_header_value(header.value())) {
            throw std::invalid_argument("invalid WebSocket handshake response header");
        }
        auto field = http_header::copy_with_lowercase_name(header.name(), header.value(), resource);
        const auto name = field.name();
        // Every Sec-WebSocket-* name is handshake-owned, including ones this
        // implementation does not know yet.
        const auto kind = classify_request_header(name);
        if (http_field_name_in(name, http_connection_specific_fields) ||
            http_field_name_in(name, http_message_framing_fields) || kind == request_header_kind::trailer ||
            kind == request_header_kind::te || name.starts_with("sec-websocket-")) {
            throw std::invalid_argument("WebSocket handshake controls this response header");
        }
        if (!size.add_field(name, header.value())) {
            throw std::length_error("WebSocket handshake response headers are too large");
        }
        result.push_back(std::move(field));
    }
    return result;
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

}  // namespace

bool append_websocket_extension_list(
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

std::string_view choose_websocket_subprotocol(
    const websocket_subprotocol_set& offered,
    std::span<const std::string_view> supported) noexcept {
    websocket_subprotocol_set configured;
    for (const auto protocol : supported) {
        if (!configured.append(protocol)) {
            return {};
        }
    }
    if (offered.empty()) {
        return {};
    }
    for (const auto protocol : supported) {
        if (offered.contains(protocol)) {
            return protocol;
        }
    }
    return {};
}

std::string_view choose_websocket_subprotocol(
    const http_request& request, std::span<const std::string_view> supported) noexcept {
    websocket_subprotocol_set offered;
    bool present = false;
    if (!append_websocket_subprotocol_offers(request, offered, present) || !present ||
        offered.empty()) {
        return {};
    }
    return choose_websocket_subprotocol(offered, supported);
}

bool websocket_request_header_validator::observe_header(
    request_header_kind kind, std::string_view value) noexcept {
    if (kind == request_header_kind::sec_websocket_version) {
        version_ = value;
        ++version_count_;
        if (version_count_ > 1) {
            valid_ = false;
        }
    } else if (kind == request_header_kind::sec_websocket_protocol) {
        protocols_present_ = true;
        if (valid_ && !protocols_.append_list(value)) {
            valid_ = false;
        }
    } else if (kind == request_header_kind::sec_websocket_extensions) {
        extensions_present_ = true;
        if (valid_ && !append_websocket_extension_list(value, has_extension_)) {
            valid_ = false;
        }
    }
    return valid_;
}

bool websocket_request_header_validator::offers_valid() const noexcept {
    return valid_ && version_count_ == 1 && (!protocols_present_ || !protocols_.empty()) &&
           (!extensions_present_ || has_extension_);
}

bool websocket_request_header_validator::client_offers_valid() const noexcept {
    return offers_valid() && version_ == "13";
}

websocket_handshake_validation_result websocket_request_header_validator::finish(
    websocket_handshake_validation_options options) const noexcept {
    if (!offers_valid()) {
        return websocket_handshake_validation_result::make_invalid_request();
    }
    if (http_trim_ows(version_) != "13") {
        return websocket_handshake_validation_result::make_unsupported_version();
    }
    if (options.require_subprotocol_ &&
        choose_websocket_subprotocol(protocols_, options.supported_subprotocols_).empty()) {
        return websocket_handshake_validation_result::make_unsupported_subprotocol();
    }
    return websocket_handshake_validation_result::make_accepted();
}

websocket_server_negotiation::websocket_server_negotiation(std::string_view subprotocol,
    websocket_compression compression, std::span<const http_header_view> response_headers_value,
    std::pmr::memory_resource* resource)
    : subprotocol_(subprotocol, http_pmr_resource_or_default(resource)),
      compression_(compression),
      extensions_(compression),
      response_headers_(copy_websocket_response_headers(response_headers_value, subprotocol,
          extensions_.view(), http_pmr_resource_or_default(resource))) {}

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
    auto negotiation = detail::make_websocket_server_negotiation(request,
        {
            .supported_subprotocols_ = options.supported_subprotocols_,
            .response_headers_ = options.response_headers_,
            .resource_ = options.resource_,
            .deflate_ = options.deflate_,
        });
    return websocket_server_handshake(accept, std::move(negotiation).take_subprotocol(),
        negotiation.compression(), std::move(negotiation).take_response_headers());
}

}  // namespace ruvia
