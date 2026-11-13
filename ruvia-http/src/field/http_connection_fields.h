#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>

#include "ruvia/http/borrowed_text.h"
#include "ruvia/http/http_ascii.h"
#include "ruvia/http/http_base64.h"
#include "ruvia/http/http_client.h"
#include "ruvia/http/http_content_coding.h"
#include "ruvia/http/http_field_syntax.h"
#include "ruvia/http/http_field_values.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_interim_response.h"
#include "ruvia/http/http_limits.h"
#include "ruvia/http/http_request_target.h"

#include "http_wire_sink.h"
#include "parser/http_uri_grammar.h"

namespace ruvia::detail {

// IMF-fixdate month names (RFC 9110 section 5.6.7), in calendar order.
inline constexpr std::array<std::string_view, 12> http_month_names{
    "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

// Seconds since the epoch for UTC civil fields, or no value for an invalid
// calendar date, out-of-range clock fields or a time_t overflow. A second of 60
// is accepted as the leap second and counts into the following minute.
[[nodiscard]] std::optional<std::time_t> http_civil_to_time_t(
    int year, int month, int day, int hour, int minute, int second) noexcept;

// Full "Date: <value>\r\n" line for the current second, for HTTP/1 text response
// heads. Empty when the clock reports failure (-1) or a date outside the
// four-digit wire year range.
[[nodiscard]] std::string_view cached_date_header() noexcept;

// Bare date value (no field name, no CRLF) for HTTP/2 and HTTP/3 headers.
[[nodiscard]] std::string_view cached_date_value() noexcept;
}  // namespace ruvia::detail

namespace ruvia::detail {

// One identity for wire classification, cached lookup and descriptor tags.
// Other has no cache slot; the remaining identities map to compact slots.
enum class http_field_kind : std::uint8_t {
    other,
    accept,
    accept_encoding,
    access_control_request_headers,
    access_control_request_method,
    authorization,
    connection,
    content_encoding,
    content_length,
    content_type,
    cookie,
    expect,
    host,
    if_match,
    if_modified_since,
    if_none_match,
    if_range,
    if_unmodified_since,
    origin,
    range,
    sec_websocket_key,
    sec_websocket_protocol,
    sec_websocket_version,
    transfer_encoding,
    upgrade,
    user_agent,
    forwarded,
    x_forwarded_for,
    x_forwarded_proto,
    sec_websocket_extensions,
    te,
    trailer,
    proxy_authorization
};

inline constexpr std::size_t http_field_kind_count =
    static_cast<std::uint8_t>(http_field_kind::proxy_authorization) + 1;

[[nodiscard]] inline constexpr std::size_t http_field_kind_known_slot(
    http_field_kind kind) noexcept {
    const auto index = static_cast<std::size_t>(kind);
    return index == 0 ? http_field_kind_count : index - 1;
}

[[nodiscard]] inline constexpr std::uint32_t singleton_request_header_bit(
    http_field_kind kind) noexcept {
    switch (kind) {
        case http_field_kind::access_control_request_method:
        case http_field_kind::authorization:
        case http_field_kind::content_type:
        case http_field_kind::if_modified_since:
        case http_field_kind::if_range:
        case http_field_kind::if_unmodified_since:
        case http_field_kind::origin:
        case http_field_kind::range:
        case http_field_kind::sec_websocket_key:
        case http_field_kind::sec_websocket_version:
        case http_field_kind::user_agent:
            return 1U << static_cast<std::uint32_t>(kind);
        case http_field_kind::other:
        case http_field_kind::accept:
        case http_field_kind::accept_encoding:
        case http_field_kind::access_control_request_headers:
        case http_field_kind::connection:
        case http_field_kind::content_encoding:
        case http_field_kind::content_length:
        case http_field_kind::cookie:
        case http_field_kind::expect:
        case http_field_kind::host:
        case http_field_kind::if_match:
        case http_field_kind::if_none_match:
        case http_field_kind::sec_websocket_protocol:
        case http_field_kind::transfer_encoding:
        case http_field_kind::upgrade:
        case http_field_kind::forwarded:
        case http_field_kind::x_forwarded_for:
        case http_field_kind::x_forwarded_proto:
        case http_field_kind::sec_websocket_extensions:
        case http_field_kind::te:
        case http_field_kind::trailer:
        case http_field_kind::proxy_authorization:
            return 0;
    }
    return 0;
}

struct http_field_classification final {
    http_field_kind kind_{http_field_kind::other};
    std::uint32_t response_bit_{0};
};

[[nodiscard]] http_field_classification classify_http_field_name(std::string_view name) noexcept;
[[nodiscard]] http_field_kind classify_http_field(std::string_view name) noexcept;

}  // namespace ruvia::detail

namespace ruvia::detail {
struct http_structured_item {
    enum class kind_type {
        other,
        integer,
        boolean,
        token,
        string,
        inner_list
    } kind_{kind_type::other};
    std::int64_t integer_{0};
    bool boolean_{false};
    std::string_view text_{};
    bool has_escapes_{false};
};
struct http_structured_text_input {
    std::string_view remaining_;

    [[nodiscard]] bool empty() const noexcept {
        return remaining_.empty();
    }
    [[nodiscard]] char peek() const noexcept {
        return remaining_.front();
    }
    [[nodiscard]] const char* data() const noexcept {
        return remaining_.data();
    }
    void advance() noexcept {
        remaining_.remove_prefix(1);
    }
};
template <typename iterator_type>
class http_structured_header_input final {
public:
    http_structured_header_input(
        iterator_type first, iterator_type last, std::string_view field_name) noexcept
        : next_(first),
          last_(last),
          field_name_(field_name) {
        (void)select_next(remaining_);
        normalize();
    }
    [[nodiscard]] bool empty() const noexcept {
        return remaining_.empty();
    }
    [[nodiscard]] char peek() const noexcept {
        return remaining_.front();
    }
    [[nodiscard]] const char* data() const noexcept {
        return remaining_.data();
    }
    void advance() noexcept {
        remaining_.remove_prefix(1);
        normalize();
    }

private:
    [[nodiscard]] bool select_next(std::string_view& value) noexcept {
        while (next_ != last_) {
            const auto& header_value = *next_++;
            if (http_ascii_equals_ignore_case(header_value.name(), field_name_)) {
                value = header_value.value();
                return true;
            }
        }
        return false;
    }
    void normalize() noexcept {
        while (remaining_.empty()) {
            if (separator_) {
                separator_ = false;
                remaining_ = pending_;
            } else if (select_next(pending_)) {
                remaining_ = ", ";
                separator_ = true;
            } else {
                return;
            }
        }
    }

    iterator_type next_;
    iterator_type last_;
    std::string_view field_name_;
    std::string_view remaining_;
    std::string_view pending_;
    bool separator_{false};
};

template <typename input_type>
struct http_structured_parser {
    input_type input_;

    [[nodiscard]] bool empty() const noexcept {
        return input_.empty();
    }
    bool take(char ch) {
        if (!input_.empty() && input_.peek() == ch) {
            input_.advance();
            return true;
        }
        return false;
    }
    void spaces() {
        while (take(' ')) {
        }
    }
    void ows() {
        while (!input_.empty() && is_http_ows_char(input_.peek())) {
            input_.advance();
        }
    }
    std::string_view key() {
        if (input_.empty() || !((input_.peek() >= 'a' && input_.peek() <= 'z') || input_.peek() == '*')) {
            return {};
        }
        const auto* const start = input_.data();
        std::size_t length = 1;
        input_.advance();
        while (!input_.empty()) {
            const auto ch = input_.peek();
            if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '-' || ch == '.' || ch == '*')) {
                break;
            }
            ++length;
            input_.advance();
        }
        // Field-line boundaries inject commas, so a key always stays contiguous.
        return {start, length};
    }
    bool item(http_structured_item& value) {
        if (input_.empty()) {
            return false;
        }
        if (take('?')) {
            value.kind_ = http_structured_item::kind_type::boolean;
            if (take('1')) {
                value.boolean_ = true;
                return true;
            }
            return take('0');
        }
        if (take('"')) {
            const auto* const start = input_.data();
            std::size_t length = 0;
            bool has_escapes = false;
            while (!input_.empty()) {
                const auto ch = static_cast<unsigned char>(input_.peek());
                input_.advance();
                if (ch == '"') {
                    value.kind_ = http_structured_item::kind_type::string;
                    value.text_ = std::string_view(start, length);
                    value.has_escapes_ = has_escapes;
                    return true;
                }
                if (ch == '\\') {
                    if (input_.empty() || (input_.peek() != '\\' && input_.peek() != '"')) {
                        return false;
                    }
                    has_escapes = true;
                    input_.advance();
                    length += 2;
                } else if (ch < 0x20 || ch > 0x7e) {
                    return false;
                } else {
                    ++length;
                }
            }
            return false;
        }
        if (take(':')) {
            std::size_t count = 0, padding = 0;
            while (!input_.empty() && input_.peek() != ':') {
                const auto ch = input_.peek();
                input_.advance();
                if (ch == '=') {
                    if (++padding > 2) {
                        return false;
                    }
                } else if (padding || http_base64_decode_char(ch) < 0) {
                    return false;
                }
                ++count;
            }
            // RFC 8941 section 4.2.7 allows missing padding, including the
            // final '=' of a partially padded sequence. Validate the data
            // quartet and supplied padding as if missing padding were added.
            const auto data_remainder = (count - padding) % 4;
            return take(':') && data_remainder != 1 && padding <= (4 - data_remainder) % 4;
        }
        if (input_.peek() == '-' || (input_.peek() >= '0' && input_.peek() <= '9')) {
            const bool negative = take('-');
            std::size_t digits = 0;
            std::int64_t number = 0;
            while (!input_.empty() && input_.peek() >= '0' && input_.peek() <= '9') {
                if (++digits > 15) {
                    return false;
                }
                number = number * 10 + input_.peek() - '0';
                input_.advance();
            }
            if (!digits) {
                return false;
            }
            if (take('.')) {
                if (digits > 12) {
                    return false;
                }
                std::size_t fraction = 0;
                while (!input_.empty() && input_.peek() >= '0' && input_.peek() <= '9') {
                    input_.advance();
                    if (++fraction > 3) {
                        return false;
                    }
                }
                return fraction != 0;
            }
            value.kind_ = http_structured_item::kind_type::integer;
            value.integer_ = negative ? -number : number;
            return true;
        }
        const auto* const start = input_.data();
        const auto first = input_.peek();
        input_.advance();
        if (!((first >= 'a' && first <= 'z') || (first >= 'A' && first <= 'Z') || first == '*')) {
            return false;
        }
        std::size_t length = 1;
        // sf-token = ( ALPHA / "*" ) *( tchar / ":" / "/" )
        while (!input_.empty()) {
            const auto ch = input_.peek();
            if (!is_http_token_char(static_cast<unsigned char>(ch)) && ch != ':' && ch != '/') {
                break;
            }
            ++length;
            input_.advance();
        }
        value.kind_ = http_structured_item::kind_type::token;
        value.text_ = std::string_view(start, length);
        return true;
    }
    bool parameters() {
        while (take(';')) {
            spaces();
            if (key().empty()) {
                return false;
            }
            if (take('=')) {
                http_structured_item ignored;
                if (!item(ignored)) {
                    return false;
                }
            }
        }
        return true;
    }
    bool member(http_structured_item& value) {
        if (!take('(')) {
            return item(value) && parameters();
        }
        value.kind_ = http_structured_item::kind_type::inner_list;
        spaces();
        while (!take(')')) {
            http_structured_item ignored;
            if (!item(ignored) || !parameters()) {
                return false;
            }
            if (input_.empty() || (input_.peek() != ' ' && input_.peek() != ')')) {
                return false;
            }
            spaces();
        }
        return parameters();
    }
};
}  // namespace ruvia::detail

namespace ruvia::detail {

inline constexpr std::uint32_t response_header_content_length = 1U << 0;
inline constexpr std::uint32_t response_header_content_encoding = 1U << 1;
inline constexpr std::uint32_t response_header_content_type = 1U << 2;
inline constexpr std::uint32_t response_header_connection = 1U << 3;
inline constexpr std::uint32_t response_header_vary = 1U << 4;
inline constexpr std::uint32_t response_header_date = 1U << 5;
inline constexpr std::uint32_t response_header_server = 1U << 6;
inline constexpr std::uint32_t response_header_cache_control = 1U << 7;
inline constexpr std::uint32_t response_header_transfer_encoding = 1U << 8;
inline constexpr std::uint32_t response_header_allow = 1U << 9;
inline constexpr std::uint32_t response_header_access_control_allow_origin = 1U << 10;
inline constexpr std::uint32_t response_header_access_control_allow_credentials = 1U << 11;
inline constexpr std::uint32_t response_header_access_control_allow_methods = 1U << 12;
inline constexpr std::uint32_t response_header_access_control_allow_headers = 1U << 13;
inline constexpr std::uint32_t response_header_access_control_max_age = 1U << 14;
inline constexpr std::uint32_t response_header_access_control_expose_headers = 1U << 15;
inline constexpr std::uint32_t response_header_accept_ranges = 1U << 16;
inline constexpr std::uint32_t response_header_content_range = 1U << 17;
inline constexpr std::uint32_t response_header_etag = 1U << 18;
inline constexpr std::uint32_t response_header_last_modified = 1U << 19;
inline constexpr std::uint32_t response_header_location = 1U << 20;
inline constexpr std::uint32_t response_header_set_cookie = 1U << 21;
inline constexpr std::size_t response_known_header_count = 22;
inline constexpr std::uint32_t response_header_upgrade = 1U << 22;
inline constexpr std::uint32_t response_header_trailer = 1U << 23;
inline constexpr std::uint32_t response_header_keep_alive = 1U << 24;
inline constexpr std::uint32_t response_header_proxy_connection = 1U << 25;

[[nodiscard]] inline constexpr std::size_t response_known_header_slot(std::uint32_t bit) noexcept {
    constexpr std::uint32_t known_mask = (1U << response_known_header_count) - 1U;
    if (bit == 0 || (bit & ~known_mask) != 0 || (bit & (bit - 1U)) != 0) {
        return response_known_header_count;
    }
    return static_cast<std::size_t>(std::countr_zero(bit));
}

[[nodiscard]] inline bool response_header_append_forbidden(std::uint32_t bit) noexcept {
    return bit == response_header_content_length || bit == response_header_content_type ||
           bit == response_header_date || bit == response_header_server ||
           bit == response_header_transfer_encoding || bit == response_header_allow ||
           bit == response_header_access_control_allow_origin ||
           bit == response_header_access_control_allow_credentials ||
           bit == response_header_access_control_max_age || bit == response_header_accept_ranges ||
           bit == response_header_content_range || bit == response_header_etag ||
           bit == response_header_last_modified || bit == response_header_location;
}

[[nodiscard]] std::uint32_t classify_response_header_name(std::string_view name) noexcept;

// RFC 7541 §7.1.3 / RFC 9204 §4.2.1: credential-bearing fields use the
// never-indexed literal so an intermediary never places them in a shared dynamic table.
[[nodiscard]] inline bool http_field_never_indexed(std::string_view name) noexcept {
    const auto field = classify_http_field_name(name);
    return field.kind_ == http_field_kind::authorization ||
           field.kind_ == http_field_kind::proxy_authorization ||
           field.kind_ == http_field_kind::cookie ||
           field.response_bit_ == response_header_set_cookie;
}

[[nodiscard]] std::string_view response_known_header_lowercase_name(std::uint32_t known_bit) noexcept;

}  // namespace ruvia::detail

#pragma once

namespace ruvia::detail {

// Case-insensitive field-name set membership (RFC 9110 section 5.1).
[[nodiscard]] inline bool http_field_name_in(
    std::string_view name, std::span<const std::string_view> fields) noexcept {
    return std::ranges::any_of(
        fields, [name](std::string_view field) noexcept { return http_ascii_equals_ignore_case(name, field); });
}

// A successful CONNECT or extended-CONNECT tunnel carries no representation
// (RFC 9110 section 9.3.6, RFC 9298 section 3): its head must not declare
// framing or representation metadata.
inline constexpr std::array<std::string_view, 5> http_connect_forbidden_fields{
    "content-length", "transfer-encoding", "content-type", "content-encoding", "trailer"};

// HTTP/2 applies the connection-specific field ban. Decoded-name
// lowercase validation and the request-only TE value exception belong to the
// protocol boundary; application-owned names are matched case-insensitively.
[[nodiscard]] inline bool is_forbidden_http_binary_connection_field(std::string_view name) noexcept {
    switch (name.size()) {
        case 7:
            return http_ascii_equals_ignore_case(name, "upgrade");
        case 10:
            return http_ascii_equals_ignore_case(name, "connection") ||
                   http_ascii_equals_ignore_case(name, "keep-alive");
        case 16:
            return http_ascii_equals_ignore_case(name, "proxy-connection");
        case 17:
            return http_ascii_equals_ignore_case(name, "transfer-encoding");
        default:
            return false;
    }
}

[[nodiscard]] bool is_forbidden_http_binary_response_field(std::string_view name) noexcept;

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
// (Content-Length, Transfer-Encoding) or routes the message: Host and the
// request-origin metadata Forwarded (RFC 7239), X-Forwarded-For,
// X-Forwarded-Proto and X-Forwarded-Host. A next hop honouring such an option
// strips the field before routing or client-address decisions consume it, so
// the nomination is refused as Pingora refuses it. Pseudo-header-shaped names
// are not tokens and already fail the list grammar. The recipient list
// (http_connection_option_names_protected_field) rejects the framing and Host
// nominations.
[[nodiscard]] inline bool http_connection_option_conflicts_with_managed_field(
    std::string_view option) noexcept {
    const auto field = classify_http_field_name(option);
    switch (field.kind_) {
        case http_field_kind::other:
            if (http_ascii_equals_ignore_case(option, "X-Forwarded-Host")) {
                return true;
            }
            break;
        case http_field_kind::connection:
        case http_field_kind::upgrade:
        case http_field_kind::sec_websocket_extensions:
        case http_field_kind::te:
        case http_field_kind::proxy_authorization:
            return false;
        case http_field_kind::forwarded:
        case http_field_kind::x_forwarded_for:
        case http_field_kind::x_forwarded_proto:
        case http_field_kind::accept:
        case http_field_kind::accept_encoding:
        case http_field_kind::access_control_request_headers:
        case http_field_kind::access_control_request_method:
        case http_field_kind::authorization:
        case http_field_kind::content_encoding:
        case http_field_kind::content_length:
        case http_field_kind::content_type:
        case http_field_kind::transfer_encoding:
        case http_field_kind::cookie:
        case http_field_kind::expect:
        case http_field_kind::host:
        case http_field_kind::if_match:
        case http_field_kind::if_modified_since:
        case http_field_kind::if_none_match:
        case http_field_kind::if_range:
        case http_field_kind::if_unmodified_since:
        case http_field_kind::origin:
        case http_field_kind::range:
        case http_field_kind::sec_websocket_key:
        case http_field_kind::sec_websocket_protocol:
        case http_field_kind::sec_websocket_version:
        case http_field_kind::user_agent:
        case http_field_kind::trailer:
            return true;
    }
    const auto known_bit = field.response_bit_;
    return (known_bit & ~(response_header_connection | response_header_upgrade |
                            response_header_keep_alive | response_header_proxy_connection)) != 0;
}

// Who parses a Connection field: its sender, or the recipient of a request or
// of a response, whose protected lists differ.
enum class http_connection_field_role : std::uint8_t {
    sender,
    request_recipient,
    response_recipient,
};

// The received options a recipient refuses. RFC 9110 section 7.6.1 forbids
// sending an option that names an end-to-end field, while a recipient only
// rejects options that make message framing or request target routing
// ambiguous if stripped by an intermediary:
// - every message: Content-Length and Transfer-Encoding;
// - a request also: Host.
// End-to-end fields such as Authorization, Cookie, Origin, or Set-Cookie
// remain prohibited for senders, not 400-rejected on receipt.
[[nodiscard]] inline bool http_connection_option_names_protected_field(
    std::string_view option, http_connection_field_role recipient) noexcept {
    switch (classify_http_field(option)) {
        case http_field_kind::content_length:
        case http_field_kind::transfer_encoding:
            return true;
        case http_field_kind::host:
            return recipient == http_connection_field_role::request_recipient;
        default:
            return false;
    }
}

// Incremental parser for the logical Connection field value. Repeated field
// lines extend the same state, so a caller cannot accidentally let a later
// occurrence erase an earlier close/Upgrade/TE signal. A sender's options must
// not name a field the protocol layer manages: a hop-by-hop declaration must
// not strip framing, routing or end-to-end metadata from the message. A recipient
// rejects only options naming a protected field (see
// http_connection_option_names_protected_field). Empty list members are
// malformed for a sender and ignored by a recipient (RFC 9110 section 5.6.1).
class http_connection_options final {
public:
    [[nodiscard]] http_field_list_parse_status parse_field(
        std::string_view field_value, http_connection_field_role role) noexcept {
        auto parsed_bits = state_;
        // An explicitly present but empty Connection field is useless and cannot
        // be safely extended by a later generated option; the strict writer
        // contract therefore rejects it as well as any empty member.
        const auto status = http_visit_token_list(field_value,
            role == http_connection_field_role::sender ? http_field_list_role::sender : http_field_list_role::recipient,
            [&parsed_bits, role](std::string_view option) noexcept {
                if (role == http_connection_field_role::sender
                        ? http_connection_option_conflicts_with_managed_field(option)
                        : http_connection_option_names_protected_field(option, role)) {
                    return false;
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
                return true;
            });
        if (status == http_field_list_parse_status::ok) {
            state_ = static_cast<std::uint8_t>(parsed_bits | field_present_bit);
        }
        return status;
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
        const auto status = http_visit_list_members(field_value, role,
            [&parsed_protocol, &visitor](std::string_view member) noexcept {
                http_upgrade_protocol protocol;
                if (!http_parse_upgrade_protocol(member, protocol)) {
                    return http_field_list_parse_status::malformed;
                }
                if (!visitor(protocol)) {
                    return http_field_list_parse_status::rejected;
                }
                parsed_protocol = true;
                return http_field_list_parse_status::ok;
            });
        if (status != http_field_list_parse_status::ok) {
            return status;
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

enum class http_response_control_field_status : std::uint8_t {
    ok,
    invalid_connection,
    invalid_upgrade,
    te_forbidden,
};

// Sender control-field grammar shared by insertion and final head analysis.
// Keep the parsed state across repeated lines, but do not check the Upgrade
// association until construction of all Connection fields has finished.
[[nodiscard]] inline http_response_control_field_status parse_http_response_control_field(
    http_field_kind kind, std::string_view value,
    http_connection_options& connection_options, http_upgrade_protocols& upgrade_protocols) noexcept {
    switch (kind) {
        case http_field_kind::connection:
            return connection_options.parse_field(value, http_connection_field_role::sender) ==
                           http_field_list_parse_status::ok
                       ? http_response_control_field_status::ok
                       : http_response_control_field_status::invalid_connection;
        case http_field_kind::upgrade:
            return upgrade_protocols.parse_field(value, http_field_list_role::sender) ==
                           http_field_list_parse_status::ok
                       ? http_response_control_field_status::ok
                       : http_response_control_field_status::invalid_upgrade;
        case http_field_kind::te:
            return http_response_control_field_status::te_forbidden;
        default:
            return http_response_control_field_status::ok;
    }
}

[[nodiscard]] inline bool http_response_upgrade_pair_valid(
    const http_connection_options& connection_options, const http_upgrade_protocols& upgrade_protocols) noexcept {
    return !upgrade_protocols.has_field() || connection_options.upgrade();
}

}  // namespace ruvia::detail

// Shared quality parsing (RFC 9110 section 12.4.2). Token fields use one
// optional weight; media ranges extract q from their parameters.

namespace ruvia::detail {

[[nodiscard]] inline int http_parse_quality_value(std::string_view value) noexcept {
    value = http_trim_ows(value);
    if (value == "1") {
        return 1000;
    }
    if (value == "0") {
        return 0;
    }
    if (value.size() >= 2 && value[1] == '.' && (value[0] == '0' || value[0] == '1')) {
        int quality = value[0] == '1' ? 1000 : 0;
        if (value[0] == '1') {
            for (std::size_t i = 2; i < value.size(); ++i) {
                if (i > 4 || value[i] != '0') {
                    return -1;
                }
            }
            return quality;
        }

        int scale = 100;
        for (std::size_t i = 2; i < value.size(); ++i) {
            if (i > 4 || value[i] < '0' || value[i] > '9') {
                return -1;
            }
            quality += (value[i] - '0') * scale;
            scale /= 10;
        }
        return quality;
    }
    return -1;
}

// Token-based Accept fields allow one optional weight, not media parameters.
// The list visitor has already trimmed surrounding OWS. Invalid weights are
// unacceptable rather than inheriting the unweighted default quality.
[[nodiscard]] inline int http_weight_parameter(std::string_view value) noexcept {
    const auto semicolon = value.find(';');
    if (semicolon == std::string_view::npos) {
        return 1000;
    }
    auto weight = http_trim_ows(value.substr(semicolon + 1));
    if (weight.size() < 3 || http_ascii_to_lower(static_cast<unsigned char>(weight[0])) != 'q' ||
        weight[1] != '=') {
        return 0;
    }
    const auto qvalue = weight.substr(2);
    if (qvalue != http_trim_ows(qvalue)) {
        return 0;
    }
    const auto parsed_value = http_parse_quality_value(qvalue);
    return parsed_value < 0 ? 0 : parsed_value;
}

}  // namespace ruvia::detail

namespace ruvia::detail {

enum class response_field_status : std::uint8_t {
    ok,
    invalid,
    repeated_singleton,
};

// One state per generated response header section. Content-Type's single media
// type cannot be combined into a list (RFC 9110 section 5.3).
class response_field_state final {
private:
    friend response_field_status validate_response_field(http_field_kind kind, std::string_view value,
        response_field_state& state) noexcept;

    bool content_type_seen_{false};
};

// Sender-only response metadata semantics. Callers validate field-name/value bytes
// and retain protocol-specific framing, connection, and forbidden-field rules.
// A fresh state also validates one value before an http_response setter commits
// its replacement; the setter owns the separate append/replace policy.
[[nodiscard]] response_field_status validate_response_field(http_field_kind kind, std::string_view value,
    response_field_state& state) noexcept;

[[nodiscard]] bool is_forbidden_response_trailer_name(std::string_view name) noexcept;

}  // namespace ruvia::detail

namespace ruvia::detail {

enum class http_interim_response_header_validation_status : std::uint8_t {
    ok,
    invalid_header,
    content_length_forbidden,
    transfer_encoding_forbidden,
    trailer_forbidden,
    repeated_singleton,
};

// Incremental sender validation for a version-neutral interim head. Recipients
// use their version's field syntax and message-boundary rules instead: 1xx
// framing advertisements do not establish content (RFC 9112 section 6.3 and
// RFC 9113 section 8.1.1).
class http_interim_response_sender_validator final {
public:
    // kind_out receives the field identity of an accepted line.
    [[nodiscard]] http_interim_response_header_validation_status validate(
        std::string_view name, std::string_view value,
        http_field_kind* kind_out = nullptr) noexcept {
        if (!is_valid_http_header_name(name) || !is_valid_http_header_value(value)) {
            return http_interim_response_header_validation_status::invalid_header;
        }

        const auto field = classify_http_field_name(name);
        const auto known_bit = field.response_bit_;
        const auto kind = field.kind_;
        switch (kind) {
            case http_field_kind::content_length:
                return http_interim_response_header_validation_status::content_length_forbidden;
            case http_field_kind::transfer_encoding:
                return http_interim_response_header_validation_status::transfer_encoding_forbidden;
            case http_field_kind::trailer:
                return http_interim_response_header_validation_status::trailer_forbidden;
            default:
                break;
        }
        switch (validate_response_field(kind, value, fields_)) {
            case response_field_status::ok:
                break;
            case response_field_status::invalid:
                return http_interim_response_header_validation_status::invalid_header;
            case response_field_status::repeated_singleton:
                return http_interim_response_header_validation_status::repeated_singleton;
        }
        if (known_bit != 0 && known_bit != response_header_content_type && (known_bits_ & known_bit) != 0 &&
            response_header_append_forbidden(known_bit)) {
            return http_interim_response_header_validation_status::repeated_singleton;
        }
        known_bits_ |= known_bit;
        if (kind_out != nullptr) {
            *kind_out = kind;
        }
        return http_interim_response_header_validation_status::ok;
    }

private:
    response_field_state fields_;
    std::uint32_t known_bits_{0};
};

}  // namespace ruvia::detail

namespace ruvia::detail {

[[nodiscard]] inline std::string_view http_trim_weak_etag_prefix(std::string_view value) noexcept {
    if (value.size() >= 2 && value[0] == 'W' && value[1] == '/') {
        value.remove_prefix(2);
    }
    return value;
}

template <http_temporary_owning_char_string value_type>
std::string_view http_trim_weak_etag_prefix(value_type&&) = delete;

[[nodiscard]] inline bool http_is_weak_etag(std::string_view value) noexcept {
    return value.size() >= 2 && value[0] == 'W' && value[1] == '/';
}

[[nodiscard]] inline bool http_is_strong_etag(std::string_view value) noexcept {
    return !value.empty() && value.front() == '"';
}

[[nodiscard]] inline bool http_strong_etag_equals(
    std::string_view left, std::string_view right) noexcept {
    return !http_is_weak_etag(left) && !http_is_weak_etag(right) && left == right;
}

[[nodiscard]] inline bool http_weak_etag_equals(
    std::string_view left, std::string_view right) noexcept {
    return http_trim_weak_etag_prefix(left) == http_trim_weak_etag_prefix(right);
}

struct http_etag_list_match_result final {
    bool valid_;
    bool matched_;
};

[[nodiscard]] inline http_etag_list_match_result http_parse_etag_list_matches(
    std::string_view values, std::string_view expected, bool strong) noexcept {
    bool matched = false;
    std::size_t offset = 0;

    while (offset < values.size()) {
        while (offset < values.size() && (is_http_ows_char(values[offset]) || values[offset] == ',')) {
            ++offset;
        }
        if (offset == values.size()) {
            break;
        }

        const std::size_t begin = offset;
        if (values.substr(offset).starts_with("W/")) {
            offset += 2;
        }
        if (offset == values.size() || values[offset] != '"') {
            return {false, false};
        }
        ++offset;
        while (offset < values.size() && values[offset] != '"') {
            const auto byte = static_cast<unsigned char>(values[offset]);
            if (byte != 0x21 && !(byte >= 0x23 && byte <= 0x7e) && byte < 0x80) {
                return {false, false};
            }
            ++offset;
        }
        if (offset == values.size()) {
            return {false, false};
        }
        ++offset;
        const auto entity_tag = values.substr(begin, offset - begin);
        matched = matched || (strong ? http_strong_etag_equals(entity_tag, expected)
                                     : http_weak_etag_equals(entity_tag, expected));

        while (offset < values.size() && is_http_ows_char(values[offset])) {
            ++offset;
        }
        if (offset < values.size() && values[offset] != ',') {
            return {false, false};
        }
    }
    return {true, matched};
}

}  // namespace ruvia::detail

#pragma once

namespace ruvia::detail {

[[nodiscard]] inline bool http_is_client_supported_te_transfer_coding(std::string_view coding) noexcept {
    return http_is_gzip_coding_token(coding) || http_ascii_equals_ignore_case(coding, "deflate");
}

[[nodiscard]] inline bool is_valid_http_te_field_item(
    std::string_view item, http_field_list_role role) noexcept {
    const auto first_semicolon = http_find_unquoted_delimiter(item, 0, ';');
    const auto coding = http_trim_ows(item.substr(0, first_semicolon));
    if (!is_valid_http_header_name(coding)) {
        return false;
    }
    if (http_ascii_equals_ignore_case(coding, "trailers")) {
        return first_semicolon == item.size();
    }
    // RFC 9112 section 7.4: a client MUST NOT list chunked in TE. That binds
    // the sender; a recipient treats it as an always-acceptable coding.
    if (role == http_field_list_role::sender &&
        !http_is_client_supported_te_transfer_coding(coding)) {
        return false;
    }

    // Validate transfer-parameter syntax and TE's weight in the same walk.
    // Unlike other parameters, q permits neither BWS around '=' nor a quoted
    // value. Recipients accept extension parameters; clients advertise only
    // the codings they can decode, with an optional weight.
    bool quality_seen = false;
    return http_all_parameters(item.substr(first_semicolon),
        [&quality_seen, role](std::string_view parameter) noexcept {
            const auto equals = parameter.find('=');
            if (equals == std::string_view::npos) {
                return false;
            }
            const auto raw_name = parameter.substr(0, equals);
            const auto raw_value = parameter.substr(equals + 1);
            const auto name = http_trim_ows(raw_name);
            const auto value = http_trim_ows(raw_value);
            if (http_ascii_equals_ignore_case(name, "q")) {
                if (quality_seen || raw_name != name || raw_value != value ||
                    http_parse_quality_value(value) < 0) {
                    return false;
                }
                quality_seen = true;
                return true;
            }
            return role == http_field_list_role::recipient && is_valid_http_transfer_parameter(parameter);
        });
}

[[nodiscard]] inline bool is_valid_http_te_field_value(
    std::string_view value, http_field_list_role role) noexcept {
    // RFC 9112 section 7.4 explicitly permits an empty TE field. It advertises
    // no optional transfer coding; chunked remains implicitly acceptable.
    if (http_trim_ows(value).empty()) {
        return true;
    }

    // A client generating TE is held to the sender's no-empty-member rule.
    return http_visit_list_members(value, role, [role](std::string_view item) noexcept {
        return is_valid_http_te_field_item(item, role) ? http_field_list_parse_status::ok
                                                       : http_field_list_parse_status::malformed;
    }) == http_field_list_parse_status::ok;
}

// HTTP/2 carries no transfer codings, so TE may only advertise
// "trailers" (RFC 9113 section 8.2.2). The member is an
// unprefixed ABNF literal and therefore case-insensitive (RFC 7405 section
// 2.1); list members are OWS-trimmed (RFC 9110 section 5.6.1). Recipients
// ignore empty members and accept empty lists (RFC 9110 section 10.1.4).
// This sender API requires a nonempty list without empty members.
[[nodiscard]] inline bool is_valid_binary_http_te_field_value(
    std::string_view value, http_field_list_role role) noexcept {
    bool has_member = false;
    const auto status = http_visit_list_members(value, role, [&has_member](std::string_view member) noexcept {
        has_member = true;
        return http_ascii_equals_ignore_case(member, "trailers") ? http_field_list_parse_status::ok
                                                                 : http_field_list_parse_status::malformed;
    });
    return status == http_field_list_parse_status::ok && (role == http_field_list_role::recipient || has_member);
}

}  // namespace ruvia::detail

namespace ruvia::detail {

// The standardized Expect field member is defined once so parsers and writers
// cannot drift on its wire spelling.
inline constexpr std::string_view http_continue_expectation_token = "100-continue";

// RFC 6454 section 7.1 permits either `null` or a space-delimited list of
// serialized origins. Fetch-generated CORS requests currently send one item,
// but the HTTP protocol primitive must retain the complete field grammar.
[[nodiscard]] inline bool is_valid_http_origin_field_value(std::string_view value) noexcept {
    value = http_trim_ows(value);
    if (value == "null") {
        return true;
    }
    std::size_t offset = 0;
    for (;;) {
        const auto separator = value.find(' ', offset);
        const auto end = separator == std::string_view::npos ? value.size() : separator;
        if (!is_valid_http_serialized_origin(value.substr(offset, end - offset))) {
            return false;
        }
        if (separator == std::string_view::npos) {
            return true;
        }
        offset = separator + 1;
    }
}

inline constexpr auto binary_field_name_char_table = [] {
    auto table_value = http_token_char_table;
    for (unsigned byte = 'A'; byte <= 'Z'; ++byte) {
        table_value[byte] = false;
    }
    return table_value;
}();

// HTTP/2 (RFC 9113 §8.2.1) and HTTP/3 (RFC 9114 §4.2) regular field names are
// nonempty lowercase tokens.
[[nodiscard]] inline bool is_valid_binary_field_name(std::string_view name) noexcept {
    if (name.empty()) {
        return false;
    }
    std::size_t index = 0;
    while (name.size() - index >= 4) {
        const auto* bytes_value = reinterpret_cast<const unsigned char*>(name.data() + index);
        const bool first = binary_field_name_char_table[bytes_value[0]];
        const bool second = binary_field_name_char_table[bytes_value[1]];
        const bool third = binary_field_name_char_table[bytes_value[2]];
        const bool fourth = binary_field_name_char_table[bytes_value[3]];
        if (!(first & second & third & fourth)) {
            return false;
        }
        index += 4;
    }
    while (index < name.size()) {
        if (!binary_field_name_char_table[static_cast<unsigned char>(name[index])]) {
            return false;
        }
        ++index;
    }
    return true;
}

[[nodiscard]] inline bool is_http_request_scheme(std::string_view scheme) noexcept {
    return http_ascii_equals_ignore_case(scheme, "http") || http_ascii_equals_ignore_case(scheme, "https");
}

// http(s) target URIs carry a mandatory, non-empty authority with Host
// syntax; other schemes may carry any RFC 3986 authority.
[[nodiscard]] inline bool is_valid_binary_request_authority(
    std::string_view scheme, std::string_view authority) noexcept {
    if (is_http_request_scheme(scheme)) {
        return !authority.empty() && is_valid_host_header(authority);
    }
    return is_valid_uri_authority(authority);
}

[[nodiscard]] inline bool binary_regular_request_requires_authority(
    std::string_view scheme, std::string_view path) noexcept {
    return path != "*" && is_http_request_scheme(scheme);
}

// HTTP(S) already uses Host syntax; other schemes can contain URI userinfo.
[[nodiscard]] inline bool binary_request_synthesizes_host(bool has_host, bool has_authority,
    std::string_view scheme, std::string_view authority) noexcept {
    return !has_host && has_authority &&
           (is_http_request_scheme(scheme) || is_valid_host_header(authority));
}

// RFC 8441 §4 / RFC 9220 §3: websocket extended CONNECT requires an HTTP(S) URI
// scheme; other extended protocols retain the full RFC 3986 space.
[[nodiscard]] inline bool is_valid_extended_connect_scheme(
    std::string_view protocol, std::string_view scheme) noexcept {
    return !http_ascii_equals_ignore_case(protocol, "websocket") || is_http_request_scheme(scheme);
}

[[nodiscard]] inline bool validate_binary_request_authority_and_host(
    std::string_view scheme, std::string_view path, bool has_authority, std::string_view authority,
    bool has_host, std::string_view host) noexcept {
    if (has_authority && !is_valid_binary_request_authority(scheme, authority)) {
        return false;
    }
    if (!has_authority && binary_regular_request_requires_authority(scheme, path) &&
        (!has_host || !is_valid_binary_request_authority(scheme, host))) {
        return false;
    }
    return !has_authority || !has_host ||
           authority_matches_host(authority, host, http_uri_scheme_default_port(scheme));
}

// Joins split binary (RFC 9113 §8.2.3, RFC 9114 §4.2.1) Cookie crumbs with "; "
// within max_http_header_bytes. When `first_crumb` is non-empty and `joined` is
// still empty, `first_crumb` is materialized before appending the second crumb.
[[nodiscard]] inline bool append_binary_cookie_crumb(std::string& joined,
    std::string_view value, bool has_existing_cookie, std::string_view first_crumb = {}) {
    if (value.empty()) {
        return true;
    }
    if (value.size() > max_http_header_bytes) {
        return false;
    }
    if (!has_existing_cookie) {
        reserve_geometric(joined, value.size());
        joined.append(value.data(), value.size());
        return true;
    }
    constexpr std::string_view cookie_separator = "; ";
    const auto prefix_bytes = joined.empty() ? first_crumb.size() : joined.size();
    if (prefix_bytes > max_http_header_bytes - cookie_separator.size() ||
        value.size() > max_http_header_bytes - cookie_separator.size() - prefix_bytes) {
        return false;
    }
    reserve_geometric(joined, prefix_bytes + cookie_separator.size() + value.size());
    if (joined.empty() && !first_crumb.empty()) {
        joined.append(first_crumb.data(), first_crumb.size());
    }
    joined.append(cookie_separator.data(), cookie_separator.size());
    joined.append(value.data(), value.size());
    return true;
}

[[nodiscard]] constexpr bool is_valid_http_request_field_source(http_request_field_source source) noexcept {
    return source == http_request_field_source::generated || source == http_request_field_source::forwarded;
}

// The only version-dependent request-field semantics: HTTP/1.1 negotiates
// transfer codings through TE, while HTTP/2 permits only "trailers".
enum class request_field_framing : std::uint8_t { http1,
    binary };

enum class request_field_status : std::uint8_t {
    ok,
    // Grammar violation, or a second instance of a field that may occur once.
    invalid,
    // A client generated the field itself. Expect and Content-Length are
    // derived from the request plan on every HTTP version, so a raw field
    // would be a second source of truth for the body gate or the framing.
    managed_by_sender,
};

// Per field section: which once-only fields were seen, plus the framing whose
// TE rules apply. One instance spans exactly one request header section.
class request_field_state final {
public:
    explicit request_field_state(request_field_framing framing) noexcept
        : framing_(framing) {}

    // True once a field that may occur only once (Host and every
    // singleton_request_header_bit() kind) has been validated.
    [[nodiscard]] bool seen(http_field_kind kind) const noexcept;

private:
    friend request_field_status validate_request_field(http_field_kind kind, std::string_view value,
        http_field_list_role role, request_field_state& state, http_request_field_source source) noexcept;

    std::uint32_t seen_{0};
    request_field_framing framing_;
};

// The authoritative semantic check for one request field, shared by the
// HTTP/1 and HTTP/2 request writers (role sender) and request
// recipients (role recipient). Callers classify the name once with
// classify_http_field() and keep version-specific concerns themselves:
// framing state (Content-Length values on receipt, Transfer-Encoding,
// Connection, Upgrade), Host/:authority agreement, cookie joining, and parsing
// accepted Expect members.
[[nodiscard]] request_field_status validate_request_field(http_field_kind kind, std::string_view value,
    http_field_list_role role, request_field_state& state,
    http_request_field_source source = http_request_field_source::generated) noexcept;

[[nodiscard]] bool is_protocol_managed_request_field(
    http_field_kind kind, std::string_view name) noexcept;
}  // namespace ruvia::detail
