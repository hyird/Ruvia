#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <type_traits>

#include "ruvia/http/detail/parser/http_parser_syntax.h"
#include "ruvia/http/detail/response/http_response_header_bits.h"
#include "ruvia/http/detail/response/http_response_known_headers.h"
#include "ruvia/http/http_ascii.h"
#include "ruvia/http/http_field_syntax.h"
#include "ruvia/http/http_header.h"

namespace ruvia::detail {

// RFC 9110 section 5.6.1 deliberately gives senders and recipients different
// list obligations: senders cannot generate empty members, while recipients
// must ignore a reasonable number of them. The HTTP/1 head-size limit bounds
// recipient work, so the tolerant path remains allocation-free and O(head).
enum class http_field_list_role : std::uint8_t { recipient,
    sender };

enum class http_field_list_parse_status : std::uint8_t { ok,
    malformed,
    rejected };

// Named reserved-field sets. Field names are case-insensitive (RFC 9110
// section 5.1), so every set is matched through http_field_name_in.
[[nodiscard]] inline bool http_field_name_in(
    std::string_view name, std::span<const std::string_view> fields) noexcept {
    return std::ranges::any_of(
        fields, [name](std::string_view field) noexcept { return http_ascii_equals_ignore_case(name, field); });
}

// Message framing (RFC 9110 sections 8.6 and 6.1): owned by the protocol
// layer, never by an application field list.
inline constexpr std::array<std::string_view, 2> http_message_framing_fields{
    "content-length", "transfer-encoding"};

// Connection-specific fields (RFC 9110 section 7.6.1). HTTP/2 forbids them
// outright (RFC 9113 section 8.2.2).
inline constexpr std::array<std::string_view, 5> http_connection_specific_fields{
    "connection", "keep-alive", "proxy-connection", "transfer-encoding", "upgrade"};

// A successful CONNECT or extended-CONNECT tunnel carries no representation
// (RFC 9110 section 9.3.6, RFC 9298 section 3): its head must not declare
// framing or representation metadata.
inline constexpr std::array<std::string_view, 5> http_connect_forbidden_fields{
    "content-length", "transfer-encoding", "content-type", "content-encoding", "trailer"};

// HTTP/2 applies the connection-specific field ban. Decoded-name
// lowercase validation and the request-only TE value exception belong to the
// protocol boundary; application-owned names are matched case-insensitively.
[[nodiscard]] inline bool is_forbidden_http_binary_connection_field(std::string_view name) noexcept {
    return http_field_name_in(name, http_connection_specific_fields);
}

[[nodiscard]] inline bool is_forbidden_http_binary_response_field(std::string_view name) noexcept {
    return is_forbidden_http_binary_connection_field(name) || http_ascii_equals_ignore_case(name, "te");
}

// The field identity of a response field line: classify_request_header(name),
// which also identifies response control fields (Trailer, TE, Upgrade). A
// field already indexed by classify_response_header_name (known_bit) needs no
// second name scan; indexed response-only fields have no shared identity.
[[nodiscard]] inline request_header_kind classify_response_field(
    std::string_view name, std::uint32_t known_bit) noexcept {
    switch (known_bit) {
        case 0:
            return classify_request_header(name);
        case response_header_connection:
            return request_header_kind::connection;
        case response_header_content_encoding:
            return request_header_kind::content_encoding;
        case response_header_content_length:
            return request_header_kind::content_length;
        case response_header_content_type:
            return request_header_kind::content_type;
        case response_header_transfer_encoding:
            return request_header_kind::transfer_encoding;
        default:
            return request_header_kind::other;
    }
}

// Spellings of every generated Connection field line. A sender of Upgrade
// lists the "Upgrade" connection option (RFC 9110 section 7.8); an HTTP/1.0
// persistent connection lists "keep-alive" (RFC 9112 section 9.3).
inline constexpr std::string_view http_connection_field_name = "Connection";
inline constexpr std::string_view http_connection_upgrade_option = "Upgrade";
inline constexpr std::string_view http_connection_keep_alive_option = "keep-alive";

enum class http_connection_option : std::uint8_t {
    close = 1U << 0,
    keep_alive = 1U << 1,
    upgrade = 1U << 2,
    te = 1U << 3
};

// A sender must not send an option naming a field intended for all recipients
// of the content (RFC 9110 section 7.6.1), nor one naming a field that frames
// (Content-Length, Transfer-Encoding) or routes (Host) the message: the
// recipient list (http_connection_option_names_protected_field) rejects those.
[[nodiscard]] inline bool http_connection_option_conflicts_with_managed_field(
    std::string_view option) noexcept {
    switch (classify_request_header(option)) {
        case request_header_kind::other:
        case request_header_kind::connection:
        case request_header_kind::upgrade:
        case request_header_kind::forwarded:
        case request_header_kind::x_forwarded_for:
        case request_header_kind::x_forwarded_proto:
        case request_header_kind::sec_websocket_extensions:
        case request_header_kind::te:
        case request_header_kind::proxy_authorization:
            break;
        case request_header_kind::accept:
        case request_header_kind::accept_encoding:
        case request_header_kind::access_control_request_headers:
        case request_header_kind::access_control_request_method:
        case request_header_kind::authorization:
        case request_header_kind::content_encoding:
        case request_header_kind::content_length:
        case request_header_kind::content_type:
        case request_header_kind::transfer_encoding:
        case request_header_kind::cookie:
        case request_header_kind::expect:
        case request_header_kind::host:
        case request_header_kind::if_match:
        case request_header_kind::if_modified_since:
        case request_header_kind::if_none_match:
        case request_header_kind::if_range:
        case request_header_kind::if_unmodified_since:
        case request_header_kind::origin:
        case request_header_kind::range:
        case request_header_kind::sec_websocket_key:
        case request_header_kind::sec_websocket_protocol:
        case request_header_kind::sec_websocket_version:
        case request_header_kind::user_agent:
        case request_header_kind::trailer:
            return true;
    }
    const auto known_bit = classify_response_header_name(option);
    return known_bit != 0 && known_bit != response_header_connection;
}

// Who parses a Connection field: its sender, or the recipient of a request or
// of a response, whose protected lists differ.
enum class http_connection_field_role : std::uint8_t {
    sender,
    request_recipient,
    response_recipient,
};

// The received options a recipient still refuses. RFC 9110 section 7.6.1
// forbids only sending an option that names an end-to-end field, but an
// intermediary removes every named field, so an option naming a field that
// changes how the recipient processes the message leaves that processing
// ambiguous and fails closed:
// - every message: the framing fields Content-Length and Transfer-Encoding,
//   and Set-Cookie (state);
// - a request also: Host (routing) and Authorization, Cookie, Origin (access
//   credentials, state, provenance).
// Request-only fields cannot change how a response is processed, so a
// response naming them is valid. Any other named field is kept and the option
// ignored.
[[nodiscard]] inline bool http_connection_option_names_protected_field(
    std::string_view option, http_connection_field_role recipient) noexcept {
    switch (classify_request_header(option)) {
        case request_header_kind::content_length:
        case request_header_kind::transfer_encoding:
            return true;
        case request_header_kind::host:
        case request_header_kind::authorization:
        case request_header_kind::cookie:
        case request_header_kind::origin:
            return recipient == http_connection_field_role::request_recipient;
        default:
            return classify_response_header_name(option) == response_header_set_cookie;
    }
}

// Incremental parser for the logical Connection field value. Repeated field
// lines extend the same state, so a caller cannot accidentally let a later
// occurrence erase an earlier close/Upgrade/TE signal. A sender's options must
// not name a field the protocol layer manages: a hop-by-hop declaration must
// not strip framing or end-to-end metadata from the message. A recipient
// rejects only options naming a protected field (see
// http_connection_option_names_protected_field). Empty list members are
// malformed for a sender and ignored by a recipient (RFC 9110 section 5.6.1).
class http_connection_options final {
public:
    [[nodiscard]] http_field_list_parse_status parse_field(
        std::string_view field_value, http_connection_field_role role) noexcept {
        auto parsed_bits = state_;
        std::size_t start = 0;
        while (start <= field_value.size()) {
            const auto comma = field_value.find(',', start);
            const auto end = comma == std::string_view::npos ? field_value.size() : comma;
            const auto option = http_trim_ows(field_value.substr(start, end - start));
            if (option.empty()) {
                // An explicitly present but empty Connection field is useless
                // and cannot be safely extended by a later generated option;
                // the strict writer contract therefore rejects it as well as
                // leading, trailing, and interior empty members.
                if (role == http_connection_field_role::sender) {
                    return http_field_list_parse_status::malformed;
                }
            } else {
                if (!is_valid_http_header_name(option)) {
                    return http_field_list_parse_status::malformed;
                }
                if (role == http_connection_field_role::sender
                        ? http_connection_option_conflicts_with_managed_field(option)
                        : http_connection_option_names_protected_field(option, role)) {
                    return http_field_list_parse_status::rejected;
                }
                if (http_ascii_equals_ignore_case(option, "close")) {
                    parsed_bits |= bit(http_connection_option::close);
                } else if (http_ascii_equals_ignore_case(option, http_connection_keep_alive_option)) {
                    parsed_bits |= bit(http_connection_option::keep_alive);
                } else if (http_ascii_equals_ignore_case(option, http_connection_upgrade_option)) {
                    parsed_bits |= bit(http_connection_option::upgrade);
                } else if (http_ascii_equals_ignore_case(option, "TE")) {
                    parsed_bits |= bit(http_connection_option::te);
                }
            }
            if (comma == std::string_view::npos) {
                break;
            }
            start = comma + 1;
        }

        state_ = static_cast<std::uint8_t>(parsed_bits | field_present_bit);
        return http_field_list_parse_status::ok;
    }

    [[nodiscard]] bool has_field() const noexcept {
        return (state_ & field_present_bit) != 0;
    }

    [[nodiscard]] bool contains(http_connection_option option) const noexcept {
        return (state_ & bit(option)) != 0;
    }

    [[nodiscard]] bool close() const noexcept {
        return contains(http_connection_option::close);
    }

    [[nodiscard]] bool keep_alive() const noexcept {
        return contains(http_connection_option::keep_alive);
    }

    [[nodiscard]] bool upgrade() const noexcept {
        return contains(http_connection_option::upgrade);
    }

    [[nodiscard]] bool te() const noexcept {
        return contains(http_connection_option::te);
    }

private:
    static constexpr std::uint8_t field_present_bit = 1U << 7;

    [[nodiscard]] static constexpr std::uint8_t bit(http_connection_option option) noexcept {
        return static_cast<std::uint8_t>(option);
    }

    // Connection owns four recognised-token bits plus one orthogonal field
    // presence bit. Keeping them in one committed byte makes absent, present
    // empty/unknown, and present with recognised options impossible to tear.
    std::uint8_t state_{0};
};

static_assert(std::is_trivially_copyable_v<http_connection_options>);
static_assert(sizeof(http_connection_options) == 1);

struct http_upgrade_protocol final {
    std::string_view name_;
    std::string_view version_;
};

[[nodiscard]] inline bool http_parse_upgrade_protocol(
    std::string_view value, http_upgrade_protocol& output) noexcept {
    const auto slash = value.find('/');
    const auto name = slash == std::string_view::npos ? value : value.substr(0, slash);
    const auto version =
        slash == std::string_view::npos ? std::string_view{} : value.substr(slash + 1);
    if (!is_valid_http_header_name(name) ||
        (slash != std::string_view::npos &&
            (!is_valid_http_header_name(version) || (version.find('/') != std::string_view::npos)))) {
        return false;
    }
    output = http_upgrade_protocol{.name_ = name, .version_ = version};
    return true;
}

template <http_temporary_owning_char_string value_type>
bool http_parse_upgrade_protocol(value_type&&, http_upgrade_protocol&) = delete;

[[nodiscard]] inline bool http_upgrade_protocol_equals(
    const http_upgrade_protocol& left, const http_upgrade_protocol& right) noexcept {
    // RFC 9110 section 7.8: protocol-name is case-insensitive, while an
    // optional protocol-version remains an exact token.
    return http_ascii_equals_ignore_case(left.name_, right.name_) && left.version_ == right.version_;
}

// Incremental Upgrade list parser. It owns repeated-field/list syntax, while
// the visitor owns policy such as whether a selected protocol was offered.
enum class http_upgrade_field_state : std::uint8_t {
    absent,
    present_without_protocol,
    present_with_protocol,
};

class http_upgrade_protocols final {
public:
    [[nodiscard]] http_field_list_parse_status parse_field(
        std::string_view field_value, http_field_list_role role) noexcept {
        return parse_field(field_value, role, [](const http_upgrade_protocol&) noexcept { return true; });
    }

    template <typename visitor_type>
    [[nodiscard]] http_field_list_parse_status parse_field(
        std::string_view field_value, http_field_list_role role, visitor_type&& visitor) noexcept {
        bool parsed_protocol = false;
        std::size_t start = 0;
        while (start <= field_value.size()) {
            const auto comma = field_value.find(',', start);
            const auto end = comma == std::string_view::npos ? field_value.size() : comma;
            const auto item = http_trim_ows(field_value.substr(start, end - start));
            if (item.empty()) {
                if (role == http_field_list_role::sender) {
                    return http_field_list_parse_status::malformed;
                }
            } else {
                http_upgrade_protocol protocol;
                if (!http_parse_upgrade_protocol(item, protocol)) {
                    return http_field_list_parse_status::malformed;
                }
                if (!visitor(protocol)) {
                    return http_field_list_parse_status::rejected;
                }
                parsed_protocol = true;
            }
            if (comma == std::string_view::npos) {
                break;
            }
            start = comma + 1;
        }

        if (parsed_protocol) {
            state_ = http_upgrade_field_state::present_with_protocol;
        } else if (state_ == http_upgrade_field_state::absent) {
            state_ = http_upgrade_field_state::present_without_protocol;
        }
        return http_field_list_parse_status::ok;
    }

    [[nodiscard]] bool has_field() const noexcept {
        return state_ != http_upgrade_field_state::absent;
    }

    [[nodiscard]] bool has_protocol() const noexcept {
        return state_ == http_upgrade_field_state::present_with_protocol;
    }

private:
    http_upgrade_field_state state_{http_upgrade_field_state::absent};
};

static_assert(std::is_trivially_copyable_v<http_upgrade_protocols>);
static_assert(sizeof(http_upgrade_protocols) == 1);

}  // namespace ruvia::detail
