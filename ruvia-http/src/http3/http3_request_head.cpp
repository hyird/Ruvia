#include "ruvia/http/http3_request_head.h"

#include <array>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <utility>

#include "ruvia/http/http_ascii.h"
#include "ruvia/http/http_field_syntax.h"
#include "ruvia/http/http_known_method.h"
#include "ruvia/http/http_limits.h"
#include "ruvia/http/http_protocol_version.h"
#include "ruvia/http/http_request_target.h"

#include "coding/http_content_length.h"
#include "field/http_connection_fields.h"
#include "field/http_field_kind.h"
#include "field/http_request_field_validation.h"
#include "memory/geometric_reserve.h"
#include "parser/http_request_target.h"
#include "request/http_request_access.h"
#include "websocket/http_websocket_handshake_fields.h"

namespace ruvia {

http3_request_head::http3_request_head(std::int64_t stream_id, std::vector<http_header> fields,
    bool stream_ended, bool early_data)
    : fields_(std::move(fields)),
      stream_id_(stream_id),
      stream_ended_(stream_ended),
      early_data_(early_data) {}

std::optional<http3_request_head> http3_request_head::try_create(std::int64_t stream_id,
    std::vector<http_header> fields, bool stream_ended, bool early_data) {
    http3_request_head head(stream_id, std::move(fields), stream_ended, early_data);
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
    return detail::validate_extended_connect_websocket_headers(request, options);
}

bool http3_request_head::validate() {
    if (fields_.size() > (std::numeric_limits<std::uint16_t>::max)()) {
        return false;
    }
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
        if (!detail::is_valid_binary_field_name(name) || !is_valid_http_header_value(value) ||
            detail::is_forbidden_http_binary_connection_field(name)) {
            return false;
        }
        const auto kind = detail::classify_http_field(name);
        if (detail::validate_request_field(kind, value, detail::http_field_list_role::recipient, request_fields) !=
            detail::request_field_status::ok) {
            return false;
        }
        switch (kind) {
            case detail::http_field_kind::host:
                host_ = index;
                break;
            case detail::http_field_kind::cookie: {
                if (value.empty()) {
                    continue;
                }
                if (first_cookie_ == absent) {
                    if (value.size() > max_http_header_bytes) {
                        return false;
                    }
                    first_cookie_ = index;
                    continue;
                }
                if (!detail::append_binary_cookie_crumb(cookie_, value, true, field_value(first_cookie_))) {
                    return false;
                }
                continue;
            }
            case detail::http_field_kind::expect:
                // An extensible list: unsupported members stay for the Web
                // product's 417 policy.
                expectations_.parse_field(value);
                break;
            case detail::http_field_kind::content_length:
                if (content_length.parse_field(value) != detail::http_content_length_parse_status::ok) {
                    return false;
                }
                content_length_seen = true;
                break;
            default:
                break;
        }
        // The remaining suffix bounds every kept index without a second pass.
        if (regular_.empty()) {
            regular_.reserve(fields_.size() - index);
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
            if (!connect_method || authority_ == absent || !detail::is_valid_origin_form_target(path_value) ||
                !detail::is_valid_extended_connect_scheme(protocol(), scheme_value)) {
                return false;
            }
        } else if (!detail::is_valid_origin_or_asterisk_form_target(known_method, path_value)) {
            return false;
        }
    }
    if (!detail::validate_binary_request_authority_and_host(scheme_value, path_value,
            authority_ != absent, authority(), host_ != absent, field_value(host_))) {
        return false;
    }
    if (content_length_seen) {
        declared_content_length_ = content_length.value();
        // A HEADERS frame that ended the stream or a CONNECT request carries no content.
        if ((stream_ended_ || connect_method) && declared_content_length_ != std::uint64_t{0}) {
            return false;
        }
    }
    const bool synthesize_host = detail::binary_request_synthesizes_host(
        host_ != absent, authority_ != absent, scheme_value, authority());
    const bool has_cookie = first_cookie_ != absent;
    if (regular_.size() + static_cast<std::size_t>(synthesize_host) + static_cast<std::size_t>(has_cookie) >
        max_http_header_fields) {
        return false;
    }
    return true;
}

http_request http3_request_head::build_request(borrowed_text body) const& {
    const auto cookie_value = cookie_.empty() ? field_value(first_cookie_) : std::string_view(cookie_);
    const detail::binary_request_materialization_spec spec{
        .version_ = http_protocol_version::http3,
        .target_form_ = http_request_target_form::http3,
        .method_ = method(),
        .scheme_ = scheme(),
        .authority_ = authority(),
        .host_ = field_value(host_),
        .path_ = path(),
        .protocol_ = protocol(),
        .cookie_ = cookie_value,
        .body_ = body.view(),
        .regular_header_count_ = regular_.size(),
        .has_authority_ = authority_ != absent,
        .has_host_ = host_ != absent,
        .has_path_ = path_ != absent,
        .has_cookie_ = !cookie_value.empty(),
        .standard_connect_ = connect() && protocol_ == absent,
        .failure_message_ = "validated HTTP/3 request cannot be materialized",
    };
    return detail::materialize_binary_http_request(spec, [this](std::size_t index) noexcept {
        const auto& field = regular_[index];
        return std::pair{
            http_header_view{fields_[field.index_].name(), fields_[field.index_].value()},
            field.kind_};
    });
}

}  // namespace ruvia
