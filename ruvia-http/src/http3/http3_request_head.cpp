#include "ruvia/http/http3_request_head.h"

#include <array>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <utility>

#include "ruvia/http/detail/field/http_connection_fields.h"
#include "ruvia/http/detail/field/http_trailer_fields.h"
#include "ruvia/http/detail/parser/http_parser_syntax.h"
#include "ruvia/http/detail/util/pmr_resource.h"
#include "ruvia/http/http_ascii.h"
#include "ruvia/http/http_field_syntax.h"
#include "ruvia/http/http_known_method.h"
#include "ruvia/http/http_limits.h"
#include "ruvia/http/http_protocol_version.h"
#include "ruvia/http/http_request_target.h"

#include "coding/http_content_length.h"
#include "field/http_request_field_validation.h"
#include "parser/http_request_target.h"
#include "request/http_request_access.h"
#include "util/geometric_reserve.h"
#include "websocket/http_websocket_handshake_fields.h"

namespace ruvia {
namespace {

[[nodiscard]] bool is_http_request_scheme(std::string_view scheme) noexcept {
    return http_ascii_equals_ignore_case(scheme, "http") || http_ascii_equals_ignore_case(scheme, "https");
}

// RFC 9114 §4.2: field names are lowercase tokens.
[[nodiscard]] bool is_valid_lowercase_field_name(std::string_view name) noexcept {
    if (!is_valid_http_header_name(name)) {
        return false;
    }
    for (const char byte : name) {
        if (byte >= 'A' && byte <= 'Z') {
            return false;
        }
    }
    return true;
}

// http(s) target URIs carry a mandatory, non-empty authority with Host
// syntax; other schemes may carry any RFC 3986 authority.
[[nodiscard]] bool is_valid_request_authority(std::string_view scheme, std::string_view authority) noexcept {
    if (is_http_request_scheme(scheme)) {
        return !authority.empty() && detail::is_valid_host_header(authority);
    }
    return detail::is_valid_uri_authority(authority);
}

}  // namespace

http3_request_head::http3_request_head(http3_headers_event event, std::pmr::memory_resource* resource)
    : fields_(std::move(event.fields_)),
      regular_(resource),
      cookie_(resource),
      stream_id_(event.stream_id_),
      stream_ended_(event.stream_ended_),
      early_data_(event.early_data_) {}

std::optional<http3_request_head> http3_request_head::parse(
    http3_headers_event event, std::pmr::memory_resource* resource) {
    http3_request_head head(std::move(event), detail::http_pmr_resource_or_default(resource));
    if (!head.validate()) {
        return std::nullopt;
    }
    return head;
}

std::string_view http3_request_head::field_value(std::uint32_t index) const noexcept {
    return index == absent ? std::string_view{} : fields_[index].value();
}

std::string_view http3_request_head::method() const& noexcept {
    return field_value(method_);
}

std::string_view http3_request_head::scheme() const& noexcept {
    return field_value(scheme_);
}

std::string_view http3_request_head::authority() const& noexcept {
    return field_value(authority_);
}

std::string_view http3_request_head::path() const& noexcept {
    return field_value(path_);
}

std::string_view http3_request_head::protocol() const& noexcept {
    return field_value(protocol_);
}

bool http3_request_head::connect() const noexcept {
    return classify_http_method(method()) == http_known_method::connect;
}

websocket_handshake_validation_result validate_http3_websocket_handshake(
    const http3_request_head& head, const http_request& request,
    websocket_handshake_validation_options options) noexcept {
    if (!head.connect() || head.protocol() != "websocket" || head.stream_ended() ||
        (head.declared_content_length().has_value() && *head.declared_content_length() != 0)) {
        return websocket_handshake_validation_result::make_invalid_request();
    }
    detail::websocket_request_header_validator validator(detail::http_field_list_role::recipient);
    const auto headers = request.headers();
    for (std::size_t index = 0; index < headers.size(); ++index) {
        if (!validator.observe_header(detail::http_request_access::header_kind(request, index), headers[index].value())) {
            return websocket_handshake_validation_result::make_invalid_request();
        }
    }
    return validator.finish(options);
}

bool http3_request_head::validate() {
    if (fields_.size() > (std::numeric_limits<std::uint16_t>::max)()) {
        return false;
    }
    regular_.reserve(fields_.size());
    detail::request_field_state request_fields{detail::request_field_framing::binary};
    detail::http_content_length_state content_length;
    bool content_length_seen = false;
    bool regular_seen = false;
    const auto assign_pseudo = [](std::uint32_t& slot, std::uint32_t index) noexcept {
        if (slot != absent) {
            return false;
        }
        slot = index;
        return true;
    };

    for (std::uint32_t index = 0; index < fields_.size(); ++index) {
        const auto name = fields_[index].name();
        const auto value = fields_[index].value();
        if (!name.empty() && name.front() == ':') {
            // RFC 9114 §4.3: pseudo-header fields precede every regular field.
            if (regular_seen) {
                return false;
            }
            bool assigned = false;
            if (name == ":method") {
                assigned = is_valid_http_method_token(value) && assign_pseudo(method_, index);
            } else if (name == ":scheme") {
                assigned = detail::is_valid_uri_scheme(value) && assign_pseudo(scheme_, index);
            } else if (name == ":authority") {
                assigned = detail::is_valid_uri_authority(value) && assign_pseudo(authority_, index);
            } else if (name == ":path") {
                assigned = !value.empty() && assign_pseudo(path_, index);
            } else if (name == ":protocol") {
                assigned = is_valid_http_header_name(value) && assign_pseudo(protocol_, index);
            }
            if (!assigned) {
                return false;
            }
            continue;
        }
        regular_seen = true;
        if (!is_valid_lowercase_field_name(name) || !is_valid_http_header_value(value) ||
            detail::is_forbidden_http_binary_connection_field(name)) {
            return false;
        }
        const auto kind = detail::classify_request_header(name);
        if (detail::validate_request_field(kind, value, detail::http_field_list_role::recipient, request_fields) !=
            detail::request_field_status::ok) {
            return false;
        }
        switch (kind) {
            case detail::request_header_kind::host:
                host_ = index;
                break;
            case detail::request_header_kind::cookie:
                // RFC 9114 §4.2.1 joins split crumbs with "; ". An empty crumb
                // is no cookie-pair (RFC 6265 §4.2.1) and contributes nothing.
                if (!value.empty()) {
                    constexpr std::string_view cookie_separator = "; ";
                    const auto separator_bytes = cookie_.empty() ? std::size_t{0} : cookie_separator.size();
                    if (value.size() > max_http_header_bytes ||
                        cookie_.size() > max_http_header_bytes - separator_bytes ||
                        cookie_.size() + separator_bytes > max_http_header_bytes - value.size()) {
                        return false;
                    }
                    detail::reserve_geometric(cookie_, cookie_.size() + separator_bytes + value.size());
                    if (separator_bytes != 0) {
                        cookie_.append(cookie_separator);
                    }
                    cookie_.append(value);
                }
                continue;
            case detail::request_header_kind::expect:
                // An extensible list: unsupported members stay for the Web
                // product's 417 policy.
                expectations_.parse_field(value);
                break;
            case detail::request_header_kind::content_length:
                if (content_length.parse_field(value) != detail::http_content_length_parse_status::ok) {
                    return false;
                }
                content_length_seen = true;
                break;
            default:
                break;
        }
        regular_.push_back(regular_field{index, kind});
    }

    if (method_ == absent) {
        return false;
    }
    const auto method_value = method();
    const auto known_method = classify_http_method(method_value);
    const bool connect_method = known_method == http_known_method::connect;
    const auto scheme_value = scheme();
    const auto path_value = path();

    if (connect_method && protocol_ == absent) {
        // RFC 9114 §4.4: CONNECT names only its authority.
        const auto target = authority();
        const auto parsed = parse_http_authority(target);
        if (scheme_ != absent || path_ != absent || authority_ == absent ||
            !parsed || parsed->host().empty() || parsed->port_kind() != http_authority_port_kind::value) {
            return false;
        }
    } else {
        if (scheme_ == absent || path_ == absent) {
            return false;
        }
        if (protocol_ != absent) {
            // RFC 9220 §3: only an extended CONNECT carries :protocol.
            if (!connect_method || authority_ == absent || !detail::is_valid_origin_form_target(path_value)) {
                return false;
            }
        } else if (!detail::is_valid_origin_or_asterisk_form_target(known_method, path_value)) {
            return false;
        }
        if (is_http_request_scheme(scheme_value) && path_value != "*") {
            // RFC 9114 §4.3.1: a scheme with a mandatory authority needs
            // :authority or Host, and both name the same origin.
            if (authority_ != absent) {
                if (!is_valid_request_authority(scheme_value, authority())) {
                    return false;
                }
            } else if (host_ == absent || !is_valid_request_authority(scheme_value, field_value(host_))) {
                return false;
            }
        }
    }
    if (authority_ != absent && host_ != absent &&
        !detail::authority_matches_host(
            authority(), field_value(host_), detail::http_uri_scheme_default_port(scheme_value))) {
        return false;
    }
    if (content_length_seen) {
        declared_content_length_ = content_length.value();
        // A HEADERS frame that ended the stream carries no content.
        if (stream_ended_ && declared_content_length_ != std::uint64_t{0}) {
            return false;
        }
    }
    const bool synthesize_host = host_ == absent && authority_ != absent &&
                                 (is_http_request_scheme(scheme_value) || detail::is_valid_host_header(authority()));
    const bool has_cookie = !cookie_.empty();
    if (regular_.size() + static_cast<std::size_t>(synthesize_host) + static_cast<std::size_t>(has_cookie) >
        max_http_header_fields) {
        return false;
    }
    return true;
}

http_request http3_request_head::build_request(std::pmr::memory_resource* resource, borrowed_text body) const& {
    auto request = detail::http_request_access::make();
    detail::http_request_access::set_resource(request, resource);
    const auto method_value = method();
    const bool standard_connect = connect() && protocol_ == absent;
    const auto target = standard_connect ? authority() : path();
    std::string_view request_path = target;
    std::string_view query_string;
    if (!standard_connect && target != "*") {
        if (const auto query = target.find('?'); query != std::string_view::npos) {
            request_path = target.substr(0, query);
            query_string = target.substr(query + 1);
        }
    }
    const auto host_value = field_value(host_);
    const auto effective_authority = authority_ != absent ? authority() : host_value;

    detail::http_request_access::set_method(request, method_value);
    detail::http_request_access::set_protocol_version(request, http_protocol_version::http3);
    detail::http_request_access::set_target(request, target);
    detail::http_request_access::set_scheme(request, scheme());
    detail::http_request_access::set_authority(request, effective_authority);
    detail::http_request_access::set_protocol(request, protocol());
    detail::http_request_access::set_target_form(request, http_request_target_form::http3);
    detail::http_request_access::set_path(request, request_path);
    detail::http_request_access::set_query_string(request, query_string);
    detail::http_request_access::set_body(request, std::as_bytes(std::span(body.data(), body.size())));

    // A non-HTTP target can carry an authority that is not legal Host syntax;
    // never manufacture an invalid regular field from that distinct grammar.
    const bool synthesize_host = host_ == absent && authority_ != absent &&
                                 (is_http_request_scheme(scheme()) || detail::is_valid_host_header(authority()));
    const bool has_cookie = !cookie_.empty();
    detail::http_request_access::reserve_headers(request,
        regular_.size() + static_cast<std::size_t>(synthesize_host) + static_cast<std::size_t>(has_cookie));
    for (const auto& field : regular_) {
        if (!detail::http_request_access::add_header(request,
                http_header_view{fields_[field.index_].name(), fields_[field.index_].value()}, field.kind_)) {
            throw std::logic_error("validated HTTP/3 request cannot be materialized");
        }
    }
    if (synthesize_host &&
        !detail::http_request_access::add_header(
            request, http_header_view{"host", authority()}, detail::request_header_kind::host)) {
        throw std::logic_error("validated HTTP/3 request cannot be materialized");
    }
    if (has_cookie &&
        !detail::http_request_access::add_header(
            request, http_header_view{"cookie", cookie_}, detail::request_header_kind::cookie)) {
        throw std::logic_error("validated HTTP/3 request cannot be materialized");
    }
    return request;
}

std::optional<http_request_trailer_error> append_http3_request_trailers(
    http_request_trailers& trailers, std::span<const http3_field> fields) {
    auto budget = http_trailer_section_budget();
    for (const auto& existing : trailers.fields()) {
        if (!budget.add_field(existing.name(), existing.value())) {
            return http_request_trailer_error::section_too_large;
        }
    }
    std::array<http_header_view, max_http_header_fields> retained{};
    std::size_t retained_count = 0;
    for (const auto& field : fields) {
        const auto name = field.name();
        const auto value = field.value();
        if (name.empty() || name.front() == ':' || !is_valid_lowercase_field_name(name) ||
            !is_valid_http_header_value(value)) {
            return http_request_trailer_error::invalid_field;
        }
        if (!budget.add_field(name, value)) {
            return http_request_trailer_error::section_too_large;
        }
        if (!detail::is_forbidden_http_request_trailer_name(name)) {
            retained[retained_count++] = http_header_view{name, value};
        }
    }
    if (retained_count == 0) {
        return std::nullopt;
    }
    return trailers.append(std::span<const http_header_view>(retained.data(), retained_count));
}

}  // namespace ruvia
