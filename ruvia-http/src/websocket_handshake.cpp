#include "websocket_handshake.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

#include "binary_text_encoding.h"
#include "http1_server_request.h"
#include "http_connection_fields.h"
#include "http_field_kind.h"
#include "http_field_validation.h"
#include "http_header.h"
#include "http_message.h"
#include "http_message_access.h"
#include "http_protocol.h"
#include "http_request.h"
#include "http_response.h"
#include "http_status.h"
#include "websocket_connection.h"
#include "websocket_negotiation.h"

namespace ruvia::detail {

// Decode RFC 7692 window parameters (8 through 15), including quoted tokens.
// The negotiated send and receive bounds are independent of takeover policy.
[[nodiscard]] inline std::optional<int> websocket_deflate_window_bits(std::string_view value) noexcept {
    value = http_trim_ows(value);
    int parsed_value = 0;
    std::size_t digits = 0;
    bool leading_zero = false;
    // RFC 7692 section 7.1.2 defines both max-window-bits parameters as
    // decimal integers without leading zeroes. Apply that grammar after
    // quoted-pair decoding as well as to tokens.
    const auto append_digit = [&](unsigned char ch) noexcept {
        if (ch < '0' || ch > '9' || ++digits > 2) {
            return false;
        }
        if (digits == 1) {
            leading_zero = ch == '0';
        } else if (leading_zero) {
            return false;
        }
        parsed_value = parsed_value * 10 + (ch - '0');
        return true;
    };
    if (!value.empty() && value.front() == '"') {
        http_quoted_string_cursor quoted(value);
        unsigned char byte = 0;
        while (quoted.next(byte)) {
            if (!append_digit(byte)) {
                return std::nullopt;
            }
        }
        if (!quoted.closed() || quoted.consumed() != value.size()) {
            return std::nullopt;
        }
    } else {
        for (const char ch : value) {
            if (!append_digit(static_cast<unsigned char>(ch))) {
                return std::nullopt;
            }
        }
    }
    return digits != 0 && parsed_value >= 8 && parsed_value <= 15 ? std::optional<int>(parsed_value) : std::nullopt;
}

std::optional<permessage_deflate_params> parse_permessage_deflate_params(std::string_view element) noexcept {
    const auto name = http_header_token_before_parameters(element);
    if (!http_ascii_equals_ignore_case(name, "permessage-deflate")) {
        return std::nullopt;
    }

    permessage_deflate_params params;
    const bool valid = http_all_parameters(element, [&params](std::string_view parameter) noexcept {
        const auto equals = parameter.find('=');
        const bool has_value = equals != std::string_view::npos;
        const auto parameter_name = http_trim_ows(parameter.substr(0, equals));
        const auto parameter_value = has_value ? http_trim_ows(parameter.substr(equals + 1)) : std::string_view{};

        switch (parameter_name.size()) {
            case 22:
                if (http_ascii_equals_ignore_case(parameter_name, "server_max_window_bits")) {
                    // A missing or empty value fails window decoding.
                    if (params.server_max_window_bits_.has_value()) {
                        return false;
                    }
                    params.server_max_window_bits_ = websocket_deflate_window_bits(parameter_value);
                    return params.server_max_window_bits_.has_value();
                }
                if (http_ascii_equals_ignore_case(parameter_name, "client_max_window_bits")) {
                    if (params.client_max_window_bits_present_) {
                        return false;
                    }
                    params.client_max_window_bits_present_ = true;
                    if (has_value) {
                        params.client_max_window_bits_ = websocket_deflate_window_bits(parameter_value);
                        return params.client_max_window_bits_.has_value();
                    }
                    return true;
                }
                return false;
            case 26:
                if (http_ascii_equals_ignore_case(parameter_name, "server_no_context_takeover")) {
                    if (params.server_no_context_takeover_ || has_value) {
                        return false;
                    }
                    params.server_no_context_takeover_ = true;
                    return true;
                }
                if (http_ascii_equals_ignore_case(parameter_name, "client_no_context_takeover")) {
                    if (params.client_no_context_takeover_ || has_value) {
                        return false;
                    }
                    params.client_no_context_takeover_ = true;
                    return true;
                }
                return false;
            default:
                // Includes an empty parameter, such as a trailing ';'.
                return false;
        }
    });
    return valid ? std::optional(params) : std::nullopt;
}

// Server role: an offered client_max_window_bits, with or without a value, is
// accepted but never echoed in the response.
[[nodiscard]] inline std::optional<websocket_compression> websocket_parse_deflate_offer(std::string_view offer, bool context_takeover) noexcept {
    const auto params = parse_permessage_deflate_params(offer);
    if (!params.has_value()) {
        return std::nullopt;
    }
    websocket_compression accepted{.enabled_ = true, .server_max_window_bits_ = params->server_max_window_bits_};
    if (context_takeover) {
        accepted.server_no_context_takeover_ = params->server_no_context_takeover_;
        accepted.client_no_context_takeover_ = params->client_no_context_takeover_;
    }
    return accepted;
}

[[nodiscard]] inline websocket_compression websocket_scan_deflate_offers(std::string_view offers, bool context_takeover) noexcept {
    std::optional<websocket_compression> accepted;
    http_visit_comma_separated_quoted_field_items(offers, [&accepted, context_takeover](std::string_view offer) noexcept {
        if (offer.empty()) {
            return true;
        }
        accepted = websocket_parse_deflate_offer(offer, context_takeover);
        return !accepted.has_value();
    });
    return accepted.value_or((websocket_compression{}));
}

[[nodiscard]] inline websocket_compression websocket_negotiate_permessage_deflate(const http_request& request, websocket_deflate_config config) {
    validate_websocket_deflater_config(config.deflater_);
    if (!config.enabled_) {
        return (websocket_compression{});
    }
    // RFC 6455 §9.1: extension declarations may be split across multiple
    // Sec-WebSocket-Extensions field lines, which RFC 9110 §5.3 makes equivalent to
    // one comma-joined list. Scan every line in order, validating the whole
    // extension list while honoring the first acceptable permessage-deflate offer.
    bool has_extension = false;
    websocket_compression negotiated{};
    const bool valid = http_request_access::visit_known_headers(request, http_field_kind::sec_websocket_extensions, [&has_extension, &negotiated, &config](std::string_view value) noexcept {
        if (!append_websocket_extension_list(value, has_extension)) {
            return false;
        }
        if (!negotiated.enabled_) {
            negotiated = websocket_scan_deflate_offers(value, config.context_takeover_);
        }
        return true;
    });
    return valid && has_extension ? negotiated : (websocket_compression{});
}
}  // namespace ruvia::detail

namespace ruvia::detail {
namespace {

[[nodiscard]] std::vector<http_header> copy_websocket_response_headers(std::span<const http_header_view> headers, std::string_view subprotocol, std::string_view extension) {
    // Account for the whole emitted head: the fixed 101 prefix, the accept
    // key line, the negotiated and application fields, and the terminating
    // empty line.
    http_field_section_budget size;
    (void)size.add(websocket_switching_protocols_head_prefix.size() + websocket_accept_key_type{}.size() + std::string_view("\r\n").size());
    (void)size.add(std::string_view("\r\n").size());
    if (!subprotocol.empty() && !size.add_field("sec-websocket-protocol", subprotocol)) {
        throw std::length_error("WebSocket handshake response headers are too large");
    }
    if (!extension.empty() && !size.add_field("sec-websocket-extensions", extension)) {
        throw std::length_error("WebSocket handshake response headers are too large");
    }
    static constexpr std::string_view sec_websocket_prefix = "sec-websocket-";
    for (const auto& header : headers) {
        const auto name = header.name();
        if (!is_valid_http_header_name(name) || !is_valid_http_header_value(header.value())) {
            throw std::invalid_argument("invalid WebSocket handshake response header");
        }
        // Every Sec-WebSocket-* name is handshake-owned, including ones this
        // implementation does not know yet.
        const auto kind = classify_http_field(name);
        const bool is_sec_websocket = name.size() >= sec_websocket_prefix.size() && http_ascii_equals_ignore_case(name.substr(0, sec_websocket_prefix.size()), sec_websocket_prefix);
        if ((kind != http_field_kind::host && kind != http_field_kind::expect && is_protocol_managed_request_field(kind, name)) || is_sec_websocket) {
            throw std::invalid_argument("WebSocket handshake controls this response header");
        }
        if (!size.add_field(name, header.value())) {
            throw std::length_error("WebSocket handshake response headers are too large");
        }
    }
    std::vector<http_header> result;
    result.reserve(headers.size());
    for (const auto& header : headers) {
        result.push_back(http_header::copy_with_lowercase_name(header.name(), header.value()));
    }
    return result;
}

void skip_websocket_extension_ows(std::string_view value, std::size_t& cursor_value) noexcept {
    while (cursor_value < value.size() && is_http_ows_char(value[cursor_value])) {
        ++cursor_value;
    }
}

[[nodiscard]] bool consume_websocket_extension_token(std::string_view value, std::size_t& cursor_value) noexcept {
    const auto start = cursor_value;
    while (cursor_value < value.size() && is_http_token_char(static_cast<unsigned char>(value[cursor_value]))) {
        ++cursor_value;
    }
    return cursor_value != start;
}

// RFC 6455 section 4.3 narrows quoted-string extension values: after
// quoted-pair unescaping, the result still has to be a nonempty token.
[[nodiscard]] bool consume_websocket_extension_quoted_token(std::string_view value, std::size_t& cursor_value) noexcept {
    http_quoted_string_cursor quoted(value.substr(cursor_value));
    bool decoded_any = false;
    unsigned char byte = 0;
    while (quoted.next(byte)) {
        if (!is_http_token_char(byte)) {
            return false;
        }
        decoded_any = true;
    }
    if (!quoted.closed()) {
        return false;
    }
    cursor_value += quoted.consumed();
    return decoded_any;
}

}  // namespace

bool append_websocket_extension_list(std::string_view value, bool& has_extension) noexcept {
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

std::string_view choose_websocket_subprotocol(const websocket_subprotocol_set& offered, std::span<const std::string_view> supported) noexcept {
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

std::string_view choose_websocket_subprotocol(const http_request& request, std::span<const std::string_view> supported) noexcept {
    if (supported.empty()) {
        return {};
    }
    websocket_subprotocol_set offered;
    if (!http_request_access::visit_known_headers(request, http_field_kind::sec_websocket_protocol, [&offered](std::string_view value) noexcept {
        return offered.append_offered_list(value);
    })) {
        return {};
    }
    return choose_websocket_subprotocol(offered, supported);
}

bool websocket_request_header_validator::observe_header(http_field_kind kind, std::string_view value) noexcept {
    if (kind == http_field_kind::sec_websocket_version) {
        version_ = value;
        if (version_present_) {
            valid_ = false;
        }
        version_present_ = true;
    } else if (kind == http_field_kind::sec_websocket_protocol) {
        protocols_present_ = true;
        if (valid_ && !(role_ == http_field_list_role::recipient ? protocols_.append_offered_list(value) : protocols_.append_list(value))) {
            valid_ = false;
        }
    } else if (kind == http_field_kind::sec_websocket_extensions) {
        extensions_present_ = true;
        if (valid_ && !append_websocket_extension_list(value, has_extension_)) {
            valid_ = false;
        }
    }
    return valid_;
}

bool websocket_request_header_validator::offers_valid() const noexcept {
    return valid_ && version_present_ && (!protocols_present_ || !protocols_.empty()) && (!extensions_present_ || has_extension_);
}

bool websocket_request_header_validator::client_offers_valid() const noexcept {
    return offers_valid() && version_ == "13";
}

websocket_handshake_validation_result websocket_request_header_validator::finish(websocket_handshake_validation_options options) const noexcept {
    if (!offers_valid()) {
        return websocket_handshake_validation_result::make_invalid_request();
    }
    if (http_trim_ows(version_) != "13") {
        return websocket_handshake_validation_result::make_unsupported_version();
    }
    if (options.require_subprotocol_ && choose_websocket_subprotocol(protocols_, options.supported_subprotocols_).empty()) {
        return websocket_handshake_validation_result::make_unsupported_subprotocol();
    }
    return websocket_handshake_validation_result::make_accepted();
}

websocket_server_negotiation::websocket_server_negotiation(std::string_view subprotocol, websocket_compression compression, std::span<const http_header_view> response_headers_value) :
    subprotocol_(subprotocol),
    compression_(compression),
    extensions_(compression),
    response_headers_(copy_websocket_response_headers(response_headers_value, subprotocol, extensions_.view())) {}

websocket_server_negotiation make_websocket_server_negotiation(const http_request& request, const websocket_server_handshake_options& options) {
    return websocket_server_negotiation(choose_websocket_subprotocol(request, options.supported_subprotocols_), websocket_negotiate_permessage_deflate(request, options.deflate_), options.response_headers_);
}

websocket_handshake_validation_result validate_extended_connect_websocket_headers(const http_request& request, websocket_handshake_validation_options options) noexcept {
    websocket_request_header_validator validator(http_field_list_role::recipient);
    const auto headers = request.headers();
    for (std::size_t index = 0; index < headers.size(); ++index) {
        if (!validator.observe_header(http_request_access::header_kind(request, index), headers[index].value())) {
            return validator.finish(options);
        }
    }
    return validator.finish(options);
}

}  // namespace ruvia::detail

namespace ruvia {

websocket_server_handshake make_websocket_server_handshake(const http_request& request, websocket_server_handshake_options options) {
    detail::websocket_accept_key_type accept;
    detail::encode_websocket_accept(accept, detail::http_request_access::known_header(request, detail::http_field_kind::sec_websocket_key));
    auto negotiation = detail::make_websocket_server_negotiation(request, options);
    return websocket_server_handshake(accept, std::move(negotiation).take_subprotocol(), negotiation.compression(), std::move(negotiation).take_response_headers());
}

}  // namespace ruvia

namespace ruvia::detail {
namespace {

// A Sec-WebSocket-Key is the canonical padded base64 of a 16-byte nonce
// (RFC 6455 section 4.1), so exactly 24 characters.
[[nodiscard]] std::optional<std::array<std::uint8_t, 16>> decode_websocket_key(std::string_view key) noexcept {
    key = http_trim_ows(key);
    std::array<std::uint8_t, 16> nonce{};
    if (key.size() != 24) {
        return std::nullopt;
    }
    const auto decoded = http_base64_decode(key, nonce, http_base64_charset::standard, http_base64_padding::required);
    if (!decoded || *decoded != nonce.size()) {
        return std::nullopt;
    }
    return nonce;
}

}  // namespace

}  // namespace ruvia::detail

namespace ruvia {

http_protocol_error websocket_handshake_failure::protocol_error() const noexcept {
    switch (kind_) {
        case kind_type::invalid_request:
            return http_protocol_error(http_status::bad_request, "invalid_websocket_handshake", "invalid WebSocket handshake");
        case kind_type::unsupported_version:
            // RFC 6455 section 4.4 requires Sec-websocket-Version and uses 400
            // in its example. 426 would instead require HTTP Upgrade fields.
            return http_protocol_error(http_status::bad_request, "unsupported_websocket_version", "unsupported WebSocket version");
        case kind_type::unsupported_subprotocol:
            return http_protocol_error(http_status::bad_request, "unsupported_websocket_subprotocol", "no supported WebSocket subprotocol offered");
    }
    return http_protocol_error(http_status::bad_request, "invalid_websocket_handshake", "invalid WebSocket handshake");
}

void websocket_handshake_failure::apply_required_response_headers(http_response& response) const {
    if (kind_ == kind_type::unsupported_version) {
        response.header_stable_view("Sec-WebSocket-Version", "13");
    }
}

websocket_handshake_validation_result validate_websocket_handshake(const http_request& request, const http1_request_body_plan& body_plan, websocket_handshake_validation_options options) noexcept {
    if (request.known_method() != http_known_method::get || request.protocol_version() != http_protocol_version::http11 ||
        // RFC 6455 does not forbid Content-Length on the HTTP Upgrade request.
        // The parser-owned framing plan is the authoritative distinction:
        // Content-Length: 0 carries no content, while a positive length or
        // chunked coding still has bytes that must be consumed before the
        // connection can change protocols.
        body_plan.requires_consumption()) {
        return websocket_handshake_validation_result::make_invalid_request();
    }

    detail::http_connection_options connection_options;
    detail::http_upgrade_protocols upgrade_protocols;
    detail::websocket_request_header_validator websocket_headers(detail::http_field_list_role::recipient);
    std::string_view key;
    std::size_t key_count = 0;
    bool websocket_upgrade = false;

    const auto headers = request.headers();
    for (std::size_t i = 0; i < headers.size(); ++i) {
        const auto kind = detail::http_request_access::header_kind(request, i);
        const auto& header_value = headers[i];
        if (kind == detail::http_field_kind::connection) {
            if (connection_options.parse_field(header_value.value(), detail::http_connection_field_role::request_recipient) != detail::http_field_list_parse_status::ok) {
                return websocket_handshake_validation_result::make_invalid_request();
            }
        } else if (kind == detail::http_field_kind::upgrade) {
            if (upgrade_protocols.parse_field(header_value.value(), detail::http_field_list_role::recipient, [&websocket_upgrade](const detail::http_upgrade_protocol& protocol) noexcept {
                if (protocol.version_.empty() && http_ascii_equals_ignore_case(protocol.name_, "websocket")) {
                    websocket_upgrade = true;
                }
                return true;
            }) != detail::http_field_list_parse_status::ok) {
                return websocket_handshake_validation_result::make_invalid_request();
            }
        } else if (kind == detail::http_field_kind::sec_websocket_key) {
            key = header_value.value();
            ++key_count;
        } else if (!websocket_headers.observe_header(kind, header_value.value())) {
            return websocket_handshake_validation_result::make_invalid_request();
        }
    }

    if (!connection_options.upgrade() || !upgrade_protocols.has_protocol() || !websocket_upgrade || key_count != 1 || !detail::decode_websocket_key(key).has_value()) {
        return websocket_handshake_validation_result::make_invalid_request();
    }
    return websocket_headers.finish(options);
}

}  // namespace ruvia
