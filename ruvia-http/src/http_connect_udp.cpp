#include "ruvia/http/http_connect_udp.h"

#include <variant>

#include "ruvia/http/detail/field/http_connection_fields.h"
#include "ruvia/http/http_ascii.h"
#include "ruvia/http/http_datagram.h"
#include "ruvia/http/http_hex.h"
#include "ruvia/http/http_number_format.h"
#include "ruvia/http/http_request_target.h"
#include "ruvia/http/url_encoding.h"

#include "field/http_te_fields.h"
#include "parser/http_uri_grammar.h"
#include "request/http_request_access.h"

namespace ruvia {
namespace {
constexpr std::string_view prefix{"/.well-known/masque/udp/"};
bool valid_host(std::string_view host) {
    return !host.empty() && host.find('%') == std::string_view::npos &&
           (host.find(':') != std::string_view::npos ? detail::parse_ipv6_address(host).has_value() : detail::is_valid_reg_name(host));
}
// RFC 9297 section 3.2: these statuses cannot start the Capsule Protocol.
constexpr bool forbids_capsule_protocol(std::uint16_t status) noexcept {
    return status == 204 || status == 205 || status == 206;
}
[[nodiscard]] bool is_http_or_https_scheme(std::string_view scheme) noexcept {
    return http_ascii_equals_ignore_case(scheme, "http") ||
           http_ascii_equals_ignore_case(scheme, "https");
}
enum class handshake_message_role : std::uint8_t {
    request_recipient,
    request_sender,
    response_recipient,
    response_sender,
};
struct handshake_fields {
    // capsule_ is true only for exactly one Capsule-Protocol field holding ?1.
    bool capsule_{false}, capsule_field_{false}, capsule_invalid_{false};
    bool upgrade_{false}, upgrade_field_{false}, connection_{false}, connection_field_{false};
    bool host_{false}, forbidden_{false}, http2_forbidden_{false};
    std::string_view host_value_{};
};
template <typename header_type>
std::variant<handshake_fields, http_connect_udp_error> fields(std::span<const header_type> headers,
    handshake_message_role role, http_connect_udp_error syntax_error) noexcept {
    const auto connection_role =
        role == handshake_message_role::request_recipient
            ? detail::http_connection_field_role::request_recipient
            : (role == handshake_message_role::response_recipient
                      ? detail::http_connection_field_role::response_recipient
                      : detail::http_connection_field_role::sender);
    const auto list_role =
        (role == handshake_message_role::request_recipient ||
            role == handshake_message_role::response_recipient)
            ? detail::http_field_list_role::recipient
            : detail::http_field_list_role::sender;
    handshake_fields found;
    detail::http_connection_options connection_options;
    detail::http_upgrade_protocols upgrade_protocols;
    bool connect_udp_offered = false;
    for (const auto& field : headers) {
        if (!is_valid_http_header_name(field.name()) || !is_valid_http_header_value(field.value())) {
            return syntax_error;
        }
        if (detail::is_forbidden_http_binary_connection_field(field.name())) {
            found.http2_forbidden_ = true;
        } else if (http_ascii_equals_ignore_case(field.name(), "te")) {
            if (role == handshake_message_role::response_recipient ||
                role == handshake_message_role::response_sender ||
                !detail::is_valid_binary_http_te_field_value(field.value(), list_role)) {
                found.http2_forbidden_ = true;
            }
        }
        if (http_ascii_equals_ignore_case(field.name(), "capsule-protocol")) {
            // RFC 9297 section 3.4: recipients handle a non-Boolean value as if the
            // field were absent; ?0 has the same meaning as absence.
            const auto value = parse_http_capsule_protocol(field.value());
            found.capsule_invalid_ = found.capsule_invalid_ || found.capsule_field_ || !value.has_value() || !*value;
            found.capsule_field_ = true;
            found.capsule_ = !found.capsule_invalid_;
        } else if (http_ascii_equals_ignore_case(field.name(), "upgrade")) {
            found.upgrade_field_ = true;
            if (upgrade_protocols.parse_field(field.value(), list_role,
                    [&](const detail::http_upgrade_protocol& protocol) noexcept {
                        const bool is_connect_udp =
                            protocol.version_.empty() &&
                            http_ascii_equals_ignore_case(protocol.name_, "connect-udp");
                        if (role == handshake_message_role::request_recipient) {
                            if (is_connect_udp) {
                                connect_udp_offered = true;
                            }
                            return true;
                        }
                        if (!is_connect_udp ||
                            (role != handshake_message_role::response_recipient && connect_udp_offered)) {
                            return false;
                        }
                        connect_udp_offered = true;
                        return true;
                    }) != detail::http_field_list_parse_status::ok) {
                return syntax_error;
            }
        } else if (http_ascii_equals_ignore_case(field.name(), "connection")) {
            found.connection_field_ = true;
            if (connection_options.parse_field(field.value(), connection_role) !=
                detail::http_field_list_parse_status::ok) {
                return syntax_error;
            }
            found.connection_ = connection_options.contains(detail::http_connection_option::upgrade);
        } else if (http_ascii_equals_ignore_case(field.name(), "host")) {
            const auto trimmed_host = http_trim_ows(field.value());
            if (found.host_ || !parse_http_authority(trimmed_host).has_value()) {
                return syntax_error;
            }
            found.host_ = true;
            found.host_value_ = trimmed_host;
        } else if (detail::http_field_name_in(field.name(), detail::http_connect_forbidden_fields)) {
            // RFC 9297 Section 3.2: the Capsule Protocol MUST NOT be used with
            // messages carrying Content-Length, Content-Type, or Transfer-Encoding
            // (the list also covers Content-Encoding and Trailer); a receiver that
            // observes one treats the message as malformed.
            found.forbidden_ = true;
        }
    }
    found.upgrade_ = upgrade_protocols.has_protocol() && connect_udp_offered;
    if (found.upgrade_field_ && !found.upgrade_) {
        return syntax_error;
    }
    return found;
}
template <typename header_type>
std::optional<http_connect_udp_error> validate_connect_udp_response(
    http_protocol_version version, std::uint16_t status, std::span<const header_type> headers) noexcept {
    if (version == http_protocol_version::http10) {
        return http_connect_udp_error::invalid_response;
    }
    const bool extended_connect = version != http_protocol_version::http11;
    const auto parsed_value =
        fields(headers, handshake_message_role::response_recipient, http_connect_udp_error::invalid_response);
    if (parsed_value.index() != 0) {
        return std::get<1>(parsed_value);
    }
    const auto& found = std::get<0>(parsed_value);
    // The connect-udp token, not Capsule-Protocol, selects the Capsule Protocol
    // (RFC 9298 section 3); RFC 9297 section 3.2 makes 204/205/206 malformed.
    if (found.forbidden_ ||
        (extended_connect
                ? status < 200 || status >= 300 || forbids_capsule_protocol(status) || found.upgrade_field_ ||
                      found.connection_field_ || found.http2_forbidden_
                : status != 101 || !found.upgrade_ || !found.connection_)) {
        return http_connect_udp_error::invalid_response;
    }
    return std::nullopt;
}
}  // namespace
std::variant<std::pmr::string, http_connect_udp_error> encode_http_connect_udp_path(http_connect_udp_target_view target, std::pmr::memory_resource* resource) {
    if (!valid_host(target.host_) || !target.port_) {
        return http_connect_udp_error::invalid_target;
    }
    std::pmr::string result_value(prefix, resource ? resource : std::pmr::get_default_resource());
    for (const unsigned char c : target.host_) {
        if (detail::is_unreserved_byte(c)) {
            result_value.push_back(static_cast<char>(c));
        } else {
            http_append_percent_byte(result_value, c);
        }
    }
    result_value.push_back('/');
    http_append_unsigned(result_value, target.port_);
    result_value.push_back('/');
    return result_value;
}
std::variant<http_connect_udp_target, http_connect_udp_error> parse_http_connect_udp_path(std::string_view path, std::pmr::memory_resource* resource) {
    if (!path.starts_with(prefix) || path.size() <= prefix.size() || !path.ends_with('/')) {
        return http_connect_udp_error::invalid_path;
    }
    path.remove_prefix(prefix.size());
    path.remove_suffix(1);
    const auto slash = path.find('/');
    if (slash == std::string_view::npos || path.find('/', slash + 1) != std::string_view::npos) {
        return http_connect_udp_error::invalid_path;
    }
    const auto raw_host = path.substr(0, slash);
    // Colons in IPv6 addresses must be escaped in this URI-template variable.
    if (raw_host.find(':') != std::string_view::npos) {
        return http_connect_udp_error::invalid_target;
    }
    auto host = decode_url_component(raw_host, {.resource_ = resource});
    const auto port = detail::parse_port_value(path.substr(slash + 1));
    if (!host || !valid_host(*host) || (port.index() != 0) || !std::get<0>(port)) {
        return http_connect_udp_error::invalid_target;
    }
    return http_connect_udp_target{std::move(*host), std::get<0>(port)};
}
namespace {
std::optional<http_connect_udp_error> validate_request(http_connect_udp_request_view request, bool sending) noexcept {
    const auto authority = parse_http_authority(request.authority_);
    if (!authority || authority->host().empty() || !is_valid_http_origin_form_target(request.path_)) {
        return http_connect_udp_error::invalid_request;
    }
    const auto parsed_value = fields(request.headers_,
        sending ? handshake_message_role::request_sender : handshake_message_role::request_recipient,
        http_connect_udp_error::invalid_request);
    if (parsed_value.index() != 0) {
        return std::get<1>(parsed_value);
    }
    const auto& found = std::get<0>(parsed_value);
    if (sending && !found.capsule_) {
        return http_connect_udp_error::invalid_capsule_protocol;
    }
    if (found.forbidden_ || request.authority_.empty() || request.path_.empty()) {
        return http_connect_udp_error::invalid_request;
    }
    if (request.version_ == http_protocol_version::http10) {
        return http_connect_udp_error::invalid_request;
    }
    if (found.host_) {
        const std::uint16_t default_port =
            http_ascii_equals_ignore_case(request.scheme_, "https")  ? 443
            : http_ascii_equals_ignore_case(request.scheme_, "http") ? 80
                                                                     : 0;
        if (!http_authorities_equal(request.authority_, found.host_value_, default_port)) {
            return http_connect_udp_error::invalid_request;
        }
    }
    if (request.version_ != http_protocol_version::http11) {
        if (request.method_ != "CONNECT" || request.protocol_ != "connect-udp" ||
            !is_http_or_https_scheme(request.scheme_) || found.connection_field_ ||
            found.upgrade_field_ || found.http2_forbidden_) {
            return http_connect_udp_error::invalid_request;
        }
    } else if (request.method_ != "GET" ||
               (!request.scheme_.empty() && !is_http_or_https_scheme(request.scheme_)) ||
               !found.upgrade_ || !found.connection_ || !found.host_) {
        return http_connect_udp_error::invalid_request;
    }
    return std::nullopt;
}
}  // namespace
std::optional<http_connect_udp_error> validate_http_connect_udp_request(http_connect_udp_request_view request) noexcept {
    return validate_request(request, false);
}
std::optional<http_connect_udp_error> validate_http_connect_udp_request_to_send(
    http_connect_udp_request_view request) noexcept {
    return validate_request(request, true);
}
std::optional<http_connect_udp_error> validate_http_connect_udp_response(
    http_protocol_version version, std::uint16_t status, std::span<const http_header_view> headers) noexcept {
    return validate_connect_udp_response(version, status, headers);
}
std::optional<http_connect_udp_error> validate_http_connect_udp_response(
    http_protocol_version version, std::uint16_t status, std::span<const http_header> headers) noexcept {
    return validate_connect_udp_response(version, status, headers);
}
bool is_http_connect_udp_upgrade_request(const http_request& request) noexcept {
    if (request.protocol_version() != http_protocol_version::http11 ||
        request.known_method() != http_known_method::get ||
        !detail::request_has_known_header(request, detail::request_header_kind::upgrade)) {
        return false;
    }
    detail::http_upgrade_protocols upgrade_protocols;
    bool connect_udp = false;
    const auto headers = request.headers();
    for (std::size_t i = 0; i < headers.size(); ++i) {
        if (static_cast<detail::request_header_kind>(detail::http_request_access::header_kind(request, i)) !=
            detail::request_header_kind::upgrade) {
            continue;
        }
        if (upgrade_protocols.parse_field(headers[i].value(), detail::http_field_list_role::recipient,
                [&connect_udp](const detail::http_upgrade_protocol& protocol) noexcept {
                    if (protocol.version_.empty() &&
                        http_ascii_equals_ignore_case(protocol.name_, "connect-udp")) {
                        connect_udp = true;
                    }
                    return true;
                }) != detail::http_field_list_parse_status::ok) {
            return false;
        }
    }
    return upgrade_protocols.has_protocol() && connect_udp;
}
std::optional<http_connect_udp_error> validate_http_connect_udp_request(const http_request& request) noexcept {
    const auto path = request.target_form() == http_request_target_form::absolute
                          ? request.path()
                          : request.target();
    return validate_http_connect_udp_request({.version_ = request.protocol_version(), .method_ = request.method(), .protocol_ = request.protocol(), .scheme_ = request.scheme(), .authority_ = request.authority().empty() ? detail::request_known_header(request, detail::request_header_kind::host) : request.authority(), .path_ = path, .headers_ = request.headers()});
}
std::variant<http_response, http_connect_udp_error> prepare_http_connect_udp_response(http_response response, http_protocol_version version) {
    if (version == http_protocol_version::http10 || !response.status().is_successful() ||
        forbids_capsule_protocol(response.status().value()) || response.file_body() || !response.body_bytes().empty()) {
        return http_connect_udp_error::invalid_response;
    }
    // The response's own fields go through the one CONNECT-UDP handshake walk;
    // the protocol, not the application, generates the Upgrade pair.
    const auto parsed_value = fields(std::span(response.headers().begin(), response.headers().size()),
        handshake_message_role::response_sender, http_connect_udp_error::invalid_response);
    if (parsed_value.index() != 0) {
        return std::get<1>(parsed_value);
    }
    const auto& found = std::get<0>(parsed_value);
    if (found.upgrade_field_ || found.connection_field_ || found.forbidden_ ||
        (version != http_protocol_version::http11 && found.http2_forbidden_)) {
        return http_connect_udp_error::invalid_response;
    }
    // As the sender, the planner only lets a single ?1 through.
    if (found.capsule_invalid_) {
        return http_connect_udp_error::invalid_capsule_protocol;
    }
    if (!found.capsule_) {
        response.header("Capsule-Protocol", "?1");
    }
    if (version == http_protocol_version::http11) {
        response.status_code_ = http_status::switching_protocols;
        response.header(detail::http_connection_field_name, detail::http_connection_upgrade_option);
        response.header("Upgrade", "connect-udp");
    }
    return response;
}
std::variant<http1_response_head_plan, http_connect_udp_error> prepare_http1_connect_udp_response_head(const http_response& response) noexcept {
    if (response.status() != http_status::switching_protocols || response.file_body() || !response.body_bytes().empty()) {
        return http_connect_udp_error::invalid_response;
    }
    if (const auto error = validate_connect_udp_response(http_protocol_version::http11, response.status().value(),
            std::span(response.headers().begin(), response.headers().size()))) {
        return *error;
    }
    return http1_close_delimited_response_stream_head_plan(plan_http_response_body(http_known_method::get, response.status()), http1_request_connection_plan::http11_close());
}
}  // namespace ruvia
