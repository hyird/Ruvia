#include "ruvia/http/http_connect_udp.h"

#include <array>
#include <charconv>
#include <variant>

#include "ruvia/http/detail/field/http_connection_fields.h"
#include "ruvia/http/http_ascii.h"
#include "ruvia/http/http_datagram.h"
#include "ruvia/http/http_request_target.h"
#include "ruvia/http/url_encoding.h"

#include "parser/http_uri_grammar.h"

namespace ruvia {
namespace {
constexpr std::string_view prefix{"/.well-known/masque/udp/"};
bool valid_host(std::string_view host) {
    return !host.empty() && host.find('%') == std::string_view::npos &&
           (host.find(':') != std::string_view::npos ? detail::is_valid_ipv6_literal(host) : detail::is_valid_reg_name(host));
}
bool same(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; };
        if (lower(a[i]) != lower(b[i])) {
            return false;
        }
    }
    return true;
}
struct handshake_fields {
    bool capsule_{false}, upgrade_{false}, connection_{false}, connection_field_{false}, host_{false}, forbidden_{false};
};
std::variant<handshake_fields, http_connect_udp_error> fields(std::span<const http_header_view> headers) {
    handshake_fields found;
    for (const auto& field : headers) {
        if (!is_valid_http_header_name(field.name()) || !is_valid_http_header_value(field.value())) {
            return http_connect_udp_error::invalid_request;
        }
        if (same(field.name(), "capsule-protocol")) {
            const auto value = parse_http_capsule_protocol(field.value());
            if (found.capsule_ || (value.index() != 0) || !std::get<0>(value)) {
                return http_connect_udp_error::invalid_capsule_protocol;
            }
            found.capsule_ = true;
        } else if (same(field.name(), "upgrade")) {
            if (found.upgrade_ || !same(field.value(), "connect-udp")) {
                return http_connect_udp_error::invalid_request;
            }
            found.upgrade_ = true;
        } else if (same(field.name(), "connection")) {
            found.connection_field_ = true;
            detail::http_connection_options options;
            if (options.parse_field(field.value(), detail::http_field_list_role::recipient) != detail::http_field_list_parse_status::ok) {
                return http_connect_udp_error::invalid_request;
            }
            found.connection_ = found.connection_ || options.contains(detail::http_connection_option::upgrade);
        } else if (same(field.name(), "host")) {
            if (found.host_ || field.value().empty()) {
                return http_connect_udp_error::invalid_request;
            }
            found.host_ = true;
        } else if (same(field.name(), "content-length") || same(field.name(), "transfer-encoding") ||
                   same(field.name(), "content-type") || same(field.name(), "content-encoding") || same(field.name(), "trailer")) {
            found.forbidden_ = true;
        }
    }
    return found;
}
}  // namespace
std::variant<std::pmr::string, http_connect_udp_error> encode_http_connect_udp_path(http_connect_udp_target_view target, std::pmr::memory_resource* resource) {
    if (!valid_host(target.host_) || !target.port_) {
        return http_connect_udp_error::invalid_target;
    }
    std::pmr::string result_value(prefix, resource ? resource : std::pmr::get_default_resource());
    constexpr char hex[]{"0123456789ABCDEF"};
    for (const unsigned char c : target.host_) {
        if (detail::is_unreserved_byte(c)) {
            result_value.push_back(static_cast<char>(c));
        } else {
            result_value.push_back('%');
            result_value.push_back(hex[c >> 4]);
            result_value.push_back(hex[c & 15]);
        }
    }
    std::array<char, 5> port{};
    const auto number = std::to_chars(port.data(), port.data() + port.size(), target.port_);
    result_value.push_back('/');
    result_value.append(port.data(), number.ptr);
    result_value.push_back('/');
    return result_value;
}
std::variant<http_connect_udp_target, http_connect_udp_error> parse_http_connect_udp_path(std::string_view path, std::pmr::memory_resource* resource) {
    if (!path.starts_with(prefix) || !path.ends_with('/')) {
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
std::variant<std::monostate, http_connect_udp_error> validate_http_connect_udp_request(http_connect_udp_request_view request) noexcept {
    const auto authority = parse_http_authority_host(request.authority_);
    if (!authority || authority->empty() || !is_valid_http_origin_form_target(request.path_)) {
        return http_connect_udp_error::invalid_request;
    }
    const auto parsed_value = fields(request.headers_);
    if ((parsed_value.index() != 0)) {
        return std::get<1>(parsed_value);
    }
    if (!std::get<0>(parsed_value).capsule_ || std::get<0>(parsed_value).forbidden_ || request.authority_.empty() || request.path_.empty()) {
        return http_connect_udp_error::invalid_request;
    }
    if (request.version_ == http_protocol_version::http10) {
        return http_connect_udp_error::invalid_request;
    }
    if (request.version_ != http_protocol_version::http11) {
        if (request.method_ != "CONNECT" || request.protocol_ != "connect-udp" || request.scheme_.empty() || std::get<0>(parsed_value).connection_field_ || std::get<0>(parsed_value).upgrade_) {
            return http_connect_udp_error::invalid_request;
        }
    } else if (request.method_ != "GET" || !std::get<0>(parsed_value).upgrade_ || !std::get<0>(parsed_value).connection_ || !std::get<0>(parsed_value).host_) {
        return http_connect_udp_error::invalid_request;
    }
    return {};
}
std::variant<std::monostate, http_connect_udp_error> validate_http_connect_udp_response(http_protocol_version version, std::uint16_t status, std::span<const http_header_view> headers) noexcept {
    if (version == http_protocol_version::http10) {
        return http_connect_udp_error::invalid_response;
    }
    const bool extended_connect = version != http_protocol_version::http11;
    const auto parsed_value = fields(headers);
    if ((parsed_value.index() != 0)) {
        return std::get<1>(parsed_value);
    }
    if (!std::get<0>(parsed_value).capsule_ || std::get<0>(parsed_value).forbidden_ ||
        (extended_connect ? status < 200 || status >= 300 || std::get<0>(parsed_value).upgrade_ || std::get<0>(parsed_value).connection_field_ : status != 101 || !std::get<0>(parsed_value).upgrade_ || !std::get<0>(parsed_value).connection_)) {
        return http_connect_udp_error::invalid_response;
    }
    return {};
}
bool is_http_connect_udp_upgrade_request(const http_request& request) noexcept {
    return request.known_method() == http_known_method::get &&
           same(detail::http_trim_ows(request.header("upgrade").value_or("")), "connect-udp");
}
std::variant<std::monostate, http_connect_udp_error> validate_http_connect_udp_request(const http_request& request) noexcept {
    return validate_http_connect_udp_request({.version_ = request.protocol_version(), .method_ = request.method(), .scheme_ = request.scheme(), .authority_ = request.authority().empty() ? request.header("host").value_or("") : request.authority(), .path_ = request.target(), .headers_ = request.headers()});
}
std::variant<http_response, http_connect_udp_error> prepare_http_connect_udp_response(http_response response, http_protocol_version version) {
    if (version == http_protocol_version::http10 || !response.status().is_successful() ||
        response.file_body() || !response.body_bytes().empty()) {
        return http_connect_udp_error::invalid_response;
    }
    for (const auto& field : response.headers()) {
        if (same(field.name(), "connection") || same(field.name(), "upgrade") ||
            same(field.name(), "content-length") || same(field.name(), "transfer-encoding") ||
            same(field.name(), "content-type") || same(field.name(), "content-encoding") || same(field.name(), "trailer")) {
            return http_connect_udp_error::invalid_response;
        }
    }
    if (const auto capsule = response.header("capsule-protocol")) {
        const auto enabled = parse_http_capsule_protocol(*capsule);
        if ((enabled.index() != 0) || !std::get<0>(enabled)) {
            return http_connect_udp_error::invalid_capsule_protocol;
        }
    }
    response.header("Capsule-Protocol", "?1");
    if (version == http_protocol_version::http11) {
        response.status_code_ = http_status::switching_protocols;
        response.header("Connection", "Upgrade");
        response.header("Upgrade", "connect-udp");
    }
    std::pmr::vector<http_header_view> fields_value(response.resource());
    for (const auto& field : response.headers()) {
        fields_value.emplace_back(field.name(), field.value());
    }
    if (const auto valid = validate_http_connect_udp_response(version, response.status().value(), fields_value); (valid.index() != 0)) {
        return std::get<1>(valid);
    }
    return response;
}
std::variant<http1_response_head_plan, http_connect_udp_error> prepare_http1_connect_udp_response_head(const http_response& response) noexcept {
    if (response.status() != http_status::switching_protocols || response.file_body() || !response.body_bytes().empty()) {
        return http_connect_udp_error::invalid_response;
    }
    bool capsule{}, upgrade{}, connection{};
    for (const auto& field : response.headers()) {
        if (same(field.name(), "capsule-protocol")) {
            const auto enabled = parse_http_capsule_protocol(field.value());
            if (capsule || (enabled.index() != 0) || !std::get<0>(enabled)) {
                return http_connect_udp_error::invalid_capsule_protocol;
            }
            capsule = true;
        } else if (same(field.name(), "upgrade")) {
            if (upgrade || !same(field.value(), "connect-udp")) {
                return http_connect_udp_error::invalid_response;
            }
            upgrade = true;
        } else if (same(field.name(), "connection")) {
            if (connection || !same(field.value(), "Upgrade")) {
                return http_connect_udp_error::invalid_response;
            }
            connection = true;
        } else if (same(field.name(), "content-length") || same(field.name(), "transfer-encoding") ||
                   same(field.name(), "content-type") || same(field.name(), "content-encoding") || same(field.name(), "trailer")) {
            return http_connect_udp_error::invalid_response;
        }
    }
    if (!capsule || !upgrade || !connection) {
        return http_connect_udp_error::invalid_response;
    }
    return http1_close_delimited_response_stream_head_plan(plan_http_response_body(http_known_method::get, response.status()), http1_request_connection_plan::http11_close());
}
}  // namespace ruvia
