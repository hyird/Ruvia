#include "ruvia/http/http_connect_udp.h"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>

#include "ruvia/http/http_ascii.h"
#include "ruvia/http/http_datagram.h"
#include "ruvia/http/http_hex.h"
#include "ruvia/http/http_number_format.h"
#include "ruvia/http/http_request_target.h"
#include "ruvia/http/url_encoding.h"

#include "field/http_connection_fields.h"
#include "http_wire_sink.h"
#include "parser/http_uri_grammar.h"

namespace ruvia {

http_connect_udp_target::http_connect_udp_target(std::string host, port_number port) noexcept
    : host_(std::move(host)),
      port_(port.value()) {}

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
enum class capsule_protocol_state : std::uint8_t {
    absent,
    valid_true,
    invalid,
};
struct handshake_fields {
    capsule_protocol_state capsule_protocol_{capsule_protocol_state::absent};
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
            // field were absent; ?0 has the same meaning as absence. Only senders
            // require a single ?1 Capsule-Protocol field.
            if (list_role == detail::http_field_list_role::sender) {
                const auto value = parse_http_capsule_protocol(field.value());
                found.capsule_protocol_ =
                    (found.capsule_protocol_ == capsule_protocol_state::absent &&
                        value == std::optional<bool>(true))
                        ? capsule_protocol_state::valid_true
                        : capsule_protocol_state::invalid;
            }
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
std::variant<std::string, http_connect_udp_error> encode_http_connect_udp_path(http_connect_udp_target_view target) {
    if (!valid_host(target.host_) || target.port_ == 0) {
        return http_connect_udp_error::invalid_target;
    }
    std::size_t encoded_host_size = 0;
    for (const unsigned char c : target.host_.view()) {
        encoded_host_size += detail::is_unreserved_byte(c) ? 1U : 3U;
    }
    std::array<char, http_max_unsigned_decimal_size> port_buffer{};
    const auto port_digits = http_format_unsigned(port_buffer, target.port_.value());
    std::string result_value;
    result_value.reserve(prefix.size() + encoded_host_size + 1U + port_digits.size() + 1U);
    result_value.append(prefix);
    for (const unsigned char c : target.host_.view()) {
        if (detail::is_unreserved_byte(c)) {
            result_value.push_back(static_cast<char>(c));
        } else {
            http_append_percent_byte(result_value, c);
        }
    }
    result_value.push_back('/');
    result_value.append(port_digits);
    result_value.push_back('/');
    return result_value;
}
std::variant<http_connect_udp_target, http_connect_udp_error> parse_http_connect_udp_path(std::string_view path) {
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
    const auto port = detail::parse_port_value(path.substr(slash + 1));
    if ((port.index() != 0) || !std::get<0>(port)) {
        return http_connect_udp_error::invalid_target;
    }
    std::string host;
    const auto decoded_host = decode_url_component(raw_host, host);
    if (!decoded_host || !valid_host(*decoded_host)) {
        return http_connect_udp_error::invalid_target;
    }
    if (decoded_host->data() == raw_host.data()) {
        host.assign(raw_host);
    }
    return http_connect_udp_target{std::move(host), std::get<0>(port)};
}
namespace {
std::optional<http_connect_udp_error> validate_request(http_connect_udp_request_view request, bool sending) noexcept {
    const auto authority = parse_http_authority(request.authority_);
    if (!authority || authority->host().empty() || !is_valid_http_origin_form_target(request.path_)) {
        return http_connect_udp_error::invalid_request;
    }
    const auto parsed_value = fields(request.headers_.view(),
        sending ? handshake_message_role::request_sender : handshake_message_role::request_recipient,
        http_connect_udp_error::invalid_request);
    if (parsed_value.index() != 0) {
        return std::get<1>(parsed_value);
    }
    const auto& found = std::get<0>(parsed_value);
    if (sending && found.capsule_protocol_ != capsule_protocol_state::valid_true) {
        return http_connect_udp_error::invalid_capsule_protocol;
    }
    if (found.forbidden_ || request.authority_.empty() || request.path_.empty()) {
        return http_connect_udp_error::invalid_request;
    }
    if (request.version_ == http_protocol_version::http10) {
        return http_connect_udp_error::invalid_request;
    }
    if (found.host_) {
        const auto default_port = detail::http_uri_scheme_default_port(request.scheme_);
        if (!http_authorities_equal(request.authority_, found.host_value_, default_port)) {
            return http_connect_udp_error::invalid_request;
        }
    }
    if (request.version_ != http_protocol_version::http11) {
        if (request.method_.view() != "CONNECT" || request.protocol_.view() != "connect-udp" ||
            !is_http_or_https_scheme(request.scheme_) || found.connection_field_ ||
            found.upgrade_field_ || found.http2_forbidden_) {
            return http_connect_udp_error::invalid_request;
        }
    } else if (request.method_.view() != "GET" ||
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
        request.known_method() != http_known_method::get) {
        return false;
    }
    detail::http_upgrade_protocols upgrade_protocols;
    bool connect_udp = false;
    const bool valid = detail::http_request_access::visit_known_headers(
        request, detail::http_field_kind::upgrade, [&](std::string_view value) noexcept {
            return upgrade_protocols.parse_field(value, detail::http_field_list_role::recipient,
                       [&connect_udp](const detail::http_upgrade_protocol& protocol) noexcept {
                           if (protocol.version_.empty() &&
                               http_ascii_equals_ignore_case(protocol.name_, "connect-udp")) {
                               connect_udp = true;
                           }
                           return true;
                       }) == detail::http_field_list_parse_status::ok;
        });
    return valid && upgrade_protocols.has_protocol() && connect_udp;
}
std::optional<http_connect_udp_error> validate_http_connect_udp_request(const http_request& request) noexcept {
    if (request.target_form() == http_request_target_form::absolute &&
        !detail::is_valid_uri_component(request.query_string(), true, true)) {
        return http_connect_udp_error::invalid_request;
    }
    const auto path = request.target_form() == http_request_target_form::absolute
                          ? request.path()
                          : request.target();
    return validate_http_connect_udp_request({.version_ = request.protocol_version(), .method_ = request.method(), .protocol_ = request.protocol(), .scheme_ = request.scheme(), .authority_ = request.authority().empty() ? detail::http_request_access::known_header(request, detail::http_field_kind::host) : request.authority(), .path_ = path, .headers_ = request.headers()});
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
    if (found.capsule_protocol_ == capsule_protocol_state::invalid) {
        return http_connect_udp_error::invalid_capsule_protocol;
    }
    if (found.capsule_protocol_ == capsule_protocol_state::absent) {
        response.header_stable_view("Capsule-Protocol", "?1");
    }
    if (version == http_protocol_version::http11) {
        response.status_code_ = http_status::switching_protocols;
        response.header_stable_view(
            detail::http_connection_field_name, detail::http_connection_upgrade_option);
        response.header_stable_view("Upgrade", "connect-udp");
    }
    return response;
}
http1_response_head_plan prepare_http1_connect_udp_response_head(const http_response& response) {
    if (response.status() != http_status::switching_protocols || response.file_body() || !response.body_bytes().empty() ||
        validate_connect_udp_response(http_protocol_version::http11, response.status().value(),
            std::span(response.headers().begin(), response.headers().size()))) {
        throw http1_response_head_error("invalid HTTP/1 CONNECT-UDP response head");
    }
    return http1_close_delimited_response_stream_head_plan(plan_http_response_body(http_known_method::get, response.status()), http1_request_connection_plan::http11_close());
}
}  // namespace ruvia

namespace ruvia {
namespace {
// CONNECT-UDP context ID 0 (RFC 9298 section 5) as its one-byte variable-length integer.
constexpr std::string_view udp_payload_context_id{"\0", 1};
}  // namespace
// One DATAGRAM capsule (RFC 9297 section 3.5) whose value is `context_prefix`
// followed by the borrowed payload.
std::variant<http_datagram_write_plan, http_datagram_error> http_datagram_session::plan_datagram_capsule(
    std::span<const char> payload_value, std::string_view context_prefix) noexcept {
    if (payload_value.size() > detail::quic_varint_max - context_prefix.size()) {
        return http_datagram_error::payload_too_large;
    }
    http_datagram_write_plan plan;
    plan.payload_ = payload_value;
    const auto header = encode_http_capsule_header(
        plan.prefix_, http_datagram_capsule_type, context_prefix.size() + payload_value.size());
    if (header.index() != 0) {
        return http_datagram_error::payload_too_large;
    }
    plan.prefix_size_ = std::get<0>(header);
    std::ranges::copy(context_prefix, plan.prefix_.data() + plan.prefix_size_);
    plan.prefix_size_ += context_prefix.size();
    return plan;
}
std::optional<bool> parse_http_capsule_protocol(std::string_view value) noexcept {
    detail::http_structured_parser parser{detail::http_structured_text_input{value}};
    parser.spaces();
    detail::http_structured_item item;
    if (!parser.item(item) || !parser.parameters()) {
        return std::nullopt;
    }
    // RFC 8941 4.2: only SP surrounds a top-level Item; OWS (with HTAB)
    // belongs between List and Dictionary members.
    parser.spaces();
    if (!parser.empty() || item.kind_ != detail::http_structured_item::kind_type::boolean) {
        return std::nullopt;
    }
    return item.boolean_;
}

std::optional<http_udp_datagram_view> decode_http_udp_datagram(std::span<const char> input) noexcept {
    auto id = detail::decode_quic_varint(input);
    if (id.index() != 0) {
        return std::nullopt;
    }
    return http_udp_datagram_view{std::get<0>(id).value_, input.subspan(std::get<0>(id).encoded_bytes_)};
}
std::variant<std::size_t, http_capsule_codec_error> encode_http_udp_datagram_prefix(std::span<char> output, std::uint64_t context_id) noexcept {
    return detail::encode_quic_varint(output, context_id);
}
std::variant<std::size_t, http_capsule_codec_error> encode_http_capsule_header(std::span<char> output, std::uint64_t type, std::uint64_t length) noexcept {
    if (type > detail::quic_varint_max || length > detail::quic_varint_max) {
        return http_capsule_codec_error::value_out_of_range;
    }
    const auto type_size = detail::quic_varint_size(type);
    const auto length_size = detail::quic_varint_size(length);
    if (output.size() < type_size + length_size) {
        return http_capsule_codec_error::output_too_small;
    }
    (void)detail::encode_quic_varint(output, type);
    (void)detail::encode_quic_varint(output.subspan(type_size), length);
    return type_size + length_size;
}
http_capsule_decoder::http_capsule_decoder(http_capsule_config config)
    : config_(config) {
    if (config.max_capsule_length_ > detail::quic_varint_max) {
        throw std::invalid_argument("capsule length exceeds varint range");
    }
}
http_capsule_status http_capsule_decoder::feed(std::span<const char> input, bool fin, http_capsule_callback_type callback_value, void* context_value, http_capsule_retention retention) {
    return feed_impl(input, fin, callback_value, context_value, false, retention).status_;
}
http_capsule_feed_result http_capsule_decoder::feed_one(std::span<const char> input, bool fin, http_capsule_callback_type callback_value, void* context_value, http_capsule_retention retention) {
    return feed_impl(input, fin, callback_value, context_value, true, retention);
}
http_capsule_feed_result http_capsule_decoder::feed_impl(std::span<const char> input, bool fin, http_capsule_callback_type callback_value, void* context_value, bool one, http_capsule_retention retention) {
    if (feeding_) {
        throw std::logic_error("recursive capsule feed");
    }
    if (status_ != http_capsule_status::need_more_data) {
        return {.status_ = status_};
    }
    struct feeding_guard final {
        bool& value_;
        explicit feeding_guard(bool& value) noexcept
            : value_(value) {
            value_ = true;
        }
        ~feeding_guard() {
            value_ = false;
        }
    } feeding_scope(feeding_);
    const auto initial_size = input.size();
    bool completed{};
    const auto limit_reached = [&] {
        status_ = http_capsule_status::limit;
        return http_capsule_feed_result{.status_ = status_, .consumed_bytes_ = initial_size - input.size()};
    };
    try {
        while (!input.empty()) {
            if (!payload_) {
                header_[header_size_++] = input.front();
                input = input.subspan(1);
                if (type_size_ == 0) {
                    auto type = detail::decode_quic_varint(std::span<const char>(header_).first(header_size_));
                    if ((type.index() != 0)) {
                        continue;
                    }
                    type_ = std::get<0>(type).value_;
                    type_size_ = std::get<0>(type).encoded_bytes_;
                }
                auto length = detail::decode_quic_varint(std::span<const char>(header_).subspan(type_size_, header_size_ - type_size_));
                if ((length.index() != 0)) {
                    continue;
                }
                remaining_ = std::get<0>(length).value_;
                header_size_ = 0;
                type_size_ = 0;
                const bool kept_type = retention == http_capsule_retention::all || type_ == http_datagram_capsule_type;
                if (!kept_type) {
                    // RFC 9297 section 3.2: drop and skip over it, at any length.
                    mode_ = payload_mode::skip;
                } else if (retention == http_capsule_retention::udp_payloads && remaining_ != 0) {
                    // RFC 9298 section 4: an unknown Context ID drops the
                    // datagram, so the limit waits for the Context ID.
                    mode_ = payload_mode::context_id;
                    context_id_size_ = 0;
                } else if (remaining_ > config_.max_capsule_length_) {
                    return limit_reached();
                } else {
                    mode_ = payload_mode::deliver;
                }
                payload_ = remaining_ != 0;
                if (!payload_ && kept_type) {
                    if (callback_value) {
                        callback_value(context_value, {type_, {}, true});
                    }
                    completed = true;
                    if (one) {
                        break;
                    }
                }
            } else if (mode_ == payload_mode::skip) {
                const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(input.size(), remaining_));
                input = input.subspan(count);
                remaining_ -= count;
                payload_ = remaining_ != 0;
            } else if (mode_ == payload_mode::context_id) {
                context_id_[context_id_size_++] = input.front();
                input = input.subspan(1);
                --remaining_;
                const auto context_id = detail::decode_quic_varint(std::span<const char>(context_id_).first(context_id_size_));
                if (context_id.index() != 0 && remaining_ != 0) {
                    continue;
                }
                payload_ = remaining_ != 0;
                if (context_id.index() == 0 && std::get<0>(context_id).value_ != 0) {
                    mode_ = payload_mode::skip;
                    continue;
                }
                // Context ID 0, or a value too short to hold its Context ID.
                if (context_id_size_ + remaining_ > config_.max_capsule_length_) {
                    return limit_reached();
                }
                mode_ = payload_mode::deliver;
                if (callback_value) {
                    callback_value(context_value, {type_, std::span<const char>(context_id_).first(context_id_size_), !payload_});
                }
                if (!payload_) {
                    completed = true;
                    if (one) {
                        break;
                    }
                }
            } else {
                auto count = static_cast<std::size_t>(std::min<std::uint64_t>(input.size(), remaining_));
                auto part = input.first(count);
                input = input.subspan(count);
                remaining_ -= count;
                payload_ = remaining_ != 0;
                if (callback_value) {
                    callback_value(context_value, {type_, part, !payload_});
                }
                if (!payload_) {
                    completed = true;
                    if (one) {
                        break;
                    }
                }
            }
        }
        if (fin && input.empty()) {
            status_ = payload_ || header_size_ ? http_capsule_status::truncated : http_capsule_status::end;
        }
        return {.status_ = status_, .consumed_bytes_ = initial_size - input.size(), .capsule_complete_ = completed};
    } catch (...) {
        status_ = http_capsule_status::truncated;
        throw;
    }
}
std::variant<http_datagram_write_plan, http_datagram_error> http_datagram_session::prepare_datagram(
    std::span<const char> payload_value) const noexcept {
    if (!send_open_) {
        return http_datagram_error::send_closed;
    }
    return plan_datagram_capsule(payload_value, {});
}
std::optional<std::span<const char>> http_datagram_session::receive_datagram(
    std::span<const char> input) const noexcept {
    if (!receive_open_) {
        return std::nullopt;
    }
    return input;
}
std::variant<http_datagram_write_plan, http_datagram_error> http_datagram_session::prepare_udp_datagram(
    std::span<const char> payload_value) const noexcept {
    if (!send_open_) {
        return http_datagram_error::send_closed;
    }
    if (payload_value.size() > 65527) {
        return http_datagram_error::payload_too_large;
    }
    return plan_datagram_capsule(payload_value, udp_payload_context_id);
}
std::variant<std::optional<http_udp_datagram_view>, http_datagram_error> http_datagram_session::receive_udp_datagram(
    std::span<const char> input) const noexcept {
    if (!receive_open_) {
        return std::nullopt;
    }
    const auto udp = decode_http_udp_datagram(input);
    if (!udp.has_value()) {
        return http_datagram_error::malformed;
    }
    if (udp->context_id_ != 0) {
        return std::nullopt;
    }
    if (udp->payload_.size() > 65527) {
        return http_datagram_error::payload_too_large;
    }
    return *udp;
}
}  // namespace ruvia
