#pragma once

#include <algorithm>
#include <array>
#include <compare>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "ruvia/http/borrowed_text.h"

namespace ruvia {

// A validated HTTP status-code value. The complete 100..599 extension space is
// representable, while arbitrary integers cannot leak into protocol APIs.
class http_status_code final {
public:
    // Validate the original integer before narrowing to the stored wire value.
    template <std::integral integer_type>
    [[nodiscard]] static constexpr http_status_code from_value(integer_type value) {
        if (!is_valid_value(value)) {
            throw std::invalid_argument("HTTP status code must be in 100..599");
        }
        return http_status_code(static_cast<std::uint16_t>(value), validated_tag_type{});
    }

    template <std::integral integer_type>
    [[nodiscard]] static constexpr std::optional<http_status_code> try_from_value(
        integer_type value) noexcept {
        if (!is_valid_value(value)) {
            return std::nullopt;
        }
        return http_status_code(static_cast<std::uint16_t>(value), validated_tag_type{});
    }

    template <std::integral integer_type>
    [[nodiscard]] static constexpr bool is_valid_value(integer_type value) noexcept {
        const auto integer_value = +value;
        return integer_value >= 100 && integer_value <= 599;
    }

    [[nodiscard]] constexpr std::uint16_t value() const noexcept {
        return value_;
    }

    [[nodiscard]] constexpr bool is_informational() const noexcept {
        return value_ < 200;
    }

    [[nodiscard]] constexpr bool is_successful() const noexcept {
        return value_ >= 200 && value_ < 300;
    }

    [[nodiscard]] constexpr bool is_redirection() const noexcept {
        return value_ >= 300 && value_ < 400;
    }

    [[nodiscard]] constexpr bool is_client_error() const noexcept {
        return value_ >= 400 && value_ < 500;
    }

    [[nodiscard]] constexpr bool is_server_error() const noexcept {
        return value_ >= 500;
    }

    [[nodiscard]] constexpr bool is_error() const noexcept {
        return value_ >= 400;
    }

    [[nodiscard]] constexpr bool is_final() const noexcept {
        return value_ >= 200;
    }

    friend constexpr auto operator<=>(http_status_code, http_status_code) = default;

private:
    struct validated_tag_type {};

    constexpr http_status_code(std::uint16_t value, validated_tag_type) noexcept
        : value_(value) {}

    std::uint16_t value_;
};

static_assert(std::is_trivially_copyable_v<http_status_code>);
static_assert(sizeof(http_status_code) == sizeof(std::uint16_t));

namespace detail {

inline constexpr std::size_t http_status_code_token_size = 3;
using http_status_code_token_type = std::array<char, http_status_code_token_size>;

[[nodiscard]] inline constexpr http_status_code_token_type http_status_code_token(
    http_status_code status) noexcept {
    const auto value = status.value();
    return {static_cast<char>('0' + value / 100), static_cast<char>('0' + (value / 10) % 10),
        static_cast<char>('0' + value % 10)};
}

[[nodiscard]] inline constexpr std::string_view http_status_code_token_view(
    const http_status_code_token_type& token) noexcept {
    return std::string_view(token.data(), token.size());
}

[[nodiscard]] std::string_view http_status_code_token_view(http_status_code_token_type&&) = delete;

}  // namespace detail

// Stable RFC-assigned names are defined once here. Unknown extension codes and
// temporary draft registrations remain available through from_value() and
// try_from_value() without turning an unstable name into framework API.
namespace http_status {

inline constexpr auto continue_value = http_status_code::from_value(100);
inline constexpr auto switching_protocols = http_status_code::from_value(101);
inline constexpr auto processing = http_status_code::from_value(102);
inline constexpr auto early_hints = http_status_code::from_value(103);

inline constexpr auto ok = http_status_code::from_value(200);
inline constexpr auto created = http_status_code::from_value(201);
inline constexpr auto accepted = http_status_code::from_value(202);
inline constexpr auto non_authoritative_information = http_status_code::from_value(203);
inline constexpr auto no_content = http_status_code::from_value(204);
inline constexpr auto reset_content = http_status_code::from_value(205);
inline constexpr auto partial_content = http_status_code::from_value(206);
inline constexpr auto multi_status = http_status_code::from_value(207);
inline constexpr auto already_reported = http_status_code::from_value(208);
inline constexpr auto im_used = http_status_code::from_value(226);

inline constexpr auto multiple_choices = http_status_code::from_value(300);
inline constexpr auto moved_permanently = http_status_code::from_value(301);
inline constexpr auto found = http_status_code::from_value(302);
inline constexpr auto see_other = http_status_code::from_value(303);
inline constexpr auto not_modified = http_status_code::from_value(304);
inline constexpr auto use_proxy = http_status_code::from_value(305);
inline constexpr auto temporary_redirect = http_status_code::from_value(307);
inline constexpr auto permanent_redirect = http_status_code::from_value(308);

inline constexpr auto bad_request = http_status_code::from_value(400);
inline constexpr auto unauthorized = http_status_code::from_value(401);
inline constexpr auto payment_required = http_status_code::from_value(402);
inline constexpr auto forbidden = http_status_code::from_value(403);
inline constexpr auto not_found = http_status_code::from_value(404);
inline constexpr auto method_not_allowed = http_status_code::from_value(405);
inline constexpr auto not_acceptable = http_status_code::from_value(406);
inline constexpr auto proxy_authentication_required = http_status_code::from_value(407);
inline constexpr auto request_timeout = http_status_code::from_value(408);
inline constexpr auto conflict = http_status_code::from_value(409);
inline constexpr auto gone = http_status_code::from_value(410);
inline constexpr auto length_required = http_status_code::from_value(411);
inline constexpr auto precondition_failed = http_status_code::from_value(412);
inline constexpr auto content_too_large = http_status_code::from_value(413);
inline constexpr auto uri_too_long = http_status_code::from_value(414);
inline constexpr auto unsupported_media_type = http_status_code::from_value(415);
inline constexpr auto range_not_satisfiable = http_status_code::from_value(416);
inline constexpr auto expectation_failed = http_status_code::from_value(417);
inline constexpr auto misdirected_request = http_status_code::from_value(421);
inline constexpr auto unprocessable_content = http_status_code::from_value(422);
inline constexpr auto locked = http_status_code::from_value(423);
inline constexpr auto failed_dependency = http_status_code::from_value(424);
inline constexpr auto too_early = http_status_code::from_value(425);
inline constexpr auto upgrade_required = http_status_code::from_value(426);
inline constexpr auto precondition_required = http_status_code::from_value(428);
inline constexpr auto too_many_requests = http_status_code::from_value(429);
inline constexpr auto request_header_fields_too_large = http_status_code::from_value(431);
inline constexpr auto unavailable_for_legal_reasons = http_status_code::from_value(451);

inline constexpr auto internal_server_error = http_status_code::from_value(500);
inline constexpr auto not_implemented = http_status_code::from_value(501);
inline constexpr auto bad_gateway = http_status_code::from_value(502);
inline constexpr auto service_unavailable = http_status_code::from_value(503);
inline constexpr auto gateway_timeout = http_status_code::from_value(504);
inline constexpr auto http_version_not_supported = http_status_code::from_value(505);
inline constexpr auto variant_also_negotiates = http_status_code::from_value(506);
inline constexpr auto insufficient_storage = http_status_code::from_value(507);
inline constexpr auto loop_detected = http_status_code::from_value(508);
inline constexpr auto not_extended = http_status_code::from_value(510);
inline constexpr auto network_authentication_required = http_status_code::from_value(511);

}  // namespace http_status

// A status code received by a client. RFC 9110 section 15 makes a three-digit
// code outside 100..599 invalid for senders, but a client that receives one
// SHOULD process the response as if it had a 5xx status, so status() is 500
// for a received 000..099 or 600..999 while value() keeps the code as
// received; for 100..599 both agree.
class http_received_status final {
public:
    // Validate the original integer before narrowing the three-digit code.
    template <std::integral integer_type>
    [[nodiscard]] static constexpr std::optional<http_received_status> try_from_value(
        integer_type value) noexcept {
        if (std::cmp_less(+value, 0) || std::cmp_greater(+value, 999)) {
            return std::nullopt;
        }
        const auto wire_value = static_cast<std::uint16_t>(value);
        if (const auto status = http_status_code::try_from_value(wire_value)) {
            return http_received_status(*status, wire_value);
        }
        return http_received_status(http_status::internal_server_error, wire_value);
    }

    // Parses the received status-code = 3DIGIT of an HTTP/1 status line or an
    // HTTP/2 :status value: exactly three ASCII digits, no sign or spaces.
    [[nodiscard]] static constexpr std::optional<http_received_status> try_parse(
        std::string_view digits) noexcept {
        if (digits.size() != 3) {
            return std::nullopt;
        }
        std::uint16_t value = 0;
        for (const char digit : digits) {
            if (digit < '0' || digit > '9') {
                return std::nullopt;
            }
            value = static_cast<std::uint16_t>(value * 10 + (digit - '0'));
        }
        return try_from_value(value);
    }

    constexpr http_received_status(http_status_code status) noexcept
        : status_(status),
          value_(status.value()) {}

    // The status the response is processed as.
    [[nodiscard]] constexpr http_status_code status() const noexcept {
        return status_;
    }

    // The three-digit code as received.
    [[nodiscard]] constexpr std::uint16_t value() const noexcept {
        return value_;
    }

    friend constexpr bool operator==(http_received_status, http_received_status) = default;

private:
    constexpr http_received_status(http_status_code status, std::uint16_t value) noexcept
        : status_(status),
          value_(value) {}

    http_status_code status_;
    std::uint16_t value_;
};

static_assert(std::is_trivially_copyable_v<http_received_status>);

// RFC 9112 reason-phrase is optional HTTP/1 presentation text, not response
// semantics. Stable RFC-assigned codes get a conventional phrase; temporary,
// extension, and unassigned codes deliberately get an empty phrase.
[[nodiscard]] inline constexpr std::string_view http_reason_phrase(http_status_code status) noexcept {
    switch (status.value()) {
        case http_status::continue_value.value():
            return "Continue";
        case http_status::switching_protocols.value():
            return "Switching Protocols";
        case http_status::processing.value():
            return "Processing";
        case http_status::early_hints.value():
            return "Early Hints";
        case http_status::ok.value():
            return "OK";
        case http_status::created.value():
            return "Created";
        case http_status::accepted.value():
            return "Accepted";
        case http_status::non_authoritative_information.value():
            return "Non-Authoritative Information";
        case http_status::no_content.value():
            return "No Content";
        case http_status::reset_content.value():
            return "Reset Content";
        case http_status::partial_content.value():
            return "Partial Content";
        case http_status::multi_status.value():
            return "Multi-Status";
        case http_status::already_reported.value():
            return "Already Reported";
        case http_status::im_used.value():
            return "IM Used";
        case http_status::multiple_choices.value():
            return "Multiple Choices";
        case http_status::moved_permanently.value():
            return "Moved Permanently";
        case http_status::found.value():
            return "Found";
        case http_status::see_other.value():
            return "See Other";
        case http_status::not_modified.value():
            return "Not Modified";
        case http_status::use_proxy.value():
            return "Use Proxy";
        case http_status::temporary_redirect.value():
            return "Temporary Redirect";
        case http_status::permanent_redirect.value():
            return "Permanent Redirect";
        case http_status::bad_request.value():
            return "Bad Request";
        case http_status::unauthorized.value():
            return "Unauthorized";
        case http_status::payment_required.value():
            return "Payment Required";
        case http_status::forbidden.value():
            return "Forbidden";
        case http_status::not_found.value():
            return "Not Found";
        case http_status::method_not_allowed.value():
            return "Method Not Allowed";
        case http_status::not_acceptable.value():
            return "Not Acceptable";
        case http_status::proxy_authentication_required.value():
            return "Proxy Authentication Required";
        case http_status::request_timeout.value():
            return "Request Timeout";
        case http_status::conflict.value():
            return "Conflict";
        case http_status::gone.value():
            return "Gone";
        case http_status::length_required.value():
            return "Length Required";
        case http_status::precondition_failed.value():
            return "Precondition Failed";
        case http_status::content_too_large.value():
            return "Content Too Large";
        case http_status::uri_too_long.value():
            return "URI Too Long";
        case http_status::unsupported_media_type.value():
            return "Unsupported Media Type";
        case http_status::range_not_satisfiable.value():
            return "Range Not Satisfiable";
        case http_status::expectation_failed.value():
            return "Expectation Failed";
        case http_status::misdirected_request.value():
            return "Misdirected Request";
        case http_status::unprocessable_content.value():
            return "Unprocessable Content";
        case http_status::locked.value():
            return "Locked";
        case http_status::failed_dependency.value():
            return "Failed Dependency";
        case http_status::too_early.value():
            return "Too Early";
        case http_status::upgrade_required.value():
            return "Upgrade Required";
        case http_status::precondition_required.value():
            return "Precondition Required";
        case http_status::too_many_requests.value():
            return "Too Many Requests";
        case http_status::request_header_fields_too_large.value():
            return "Request Header Fields Too Large";
        case http_status::unavailable_for_legal_reasons.value():
            return "Unavailable For Legal Reasons";
        case http_status::internal_server_error.value():
            return "Internal Server Error";
        case http_status::not_implemented.value():
            return "Not Implemented";
        case http_status::bad_gateway.value():
            return "Bad Gateway";
        case http_status::service_unavailable.value():
            return "Service Unavailable";
        case http_status::gateway_timeout.value():
            return "Gateway Timeout";
        case http_status::http_version_not_supported.value():
            return "HTTP Version Not Supported";
        case http_status::variant_also_negotiates.value():
            return "Variant Also Negotiates";
        case http_status::insufficient_storage.value():
            return "Insufficient Storage";
        case http_status::loop_detected.value():
            return "Loop Detected";
        case http_status::not_extended.value():
            return "Not Extended";
        case http_status::network_authentication_required.value():
            return "Network Authentication Required";
        default:
            return {};
    }
}

// The redirect status codes whose Location a client may follow
// (RFC 9110 sections 15.4.2-15.4.4, 15.4.8, 15.4.9).
[[nodiscard]] inline constexpr bool is_http_redirect_status(http_status_code status) noexcept {
    return status == http_status::moved_permanently || status == http_status::found ||
           status == http_status::see_other || status == http_status::temporary_redirect ||
           status == http_status::permanent_redirect;
}

namespace detail {

// 101 is a protocol transition rather than an interim progress head. It is
// intentionally owned by a dedicated Upgrade driver instead of either generic
// response-head type.
[[nodiscard]] inline constexpr bool http_interim_status_code_valid(http_status_code status) noexcept {
    return status.is_informational() && status != http_status::switching_protocols;
}

}  // namespace detail

}  // namespace ruvia

namespace ruvia {

inline constexpr std::size_t max_http_header_bytes = std::size_t{64} * 1024;
// Field lines per HTTP/1 head or trailer section (the http_field_section_budget
// rule), received and generated, beside the max_http_header_bytes byte cap. The
// smallest field line costs 4 bytes, so the byte cap alone admits ~16 K fields
// whose descriptors outweigh the head; 256 matches Pingora's parser limit.
inline constexpr std::size_t max_http_header_fields = 256;
inline constexpr std::size_t default_max_buffered_body_bytes = std::size_t{16} * 1024 * 1024;
inline constexpr std::size_t default_max_websocket_message_bytes = std::size_t{16} * 1024 * 1024;
// Bytes one SSE event may buffer while sse_parser assembles it.
inline constexpr std::size_t default_max_sse_event_bytes = std::size_t{16} * 1024 * 1024;

}  // namespace ruvia

namespace ruvia {

// A protocol byte ceiling with no numeric sentinel: the default is unlimited,
// while a configured limit is always strictly positive.
class protocol_byte_limit final {
public:
    protocol_byte_limit() noexcept = default;

    [[nodiscard]] static protocol_byte_limit unlimited() noexcept {
        return protocol_byte_limit();
    }

    // Validate the original integer before converting to the stored size.
    // Negative inputs must not become effectively unbounded positive ceilings.
    template <std::integral integer_type>
    [[nodiscard]] static protocol_byte_limit limited(integer_type bytes_value) {
        if (!std::in_range<std::size_t>(+bytes_value) || +bytes_value == 0) {
            throw std::invalid_argument("protocol byte limit must be positive and representable");
        }
        return protocol_byte_limit(static_cast<std::size_t>(bytes_value));
    }

    [[nodiscard]] bool is_limited() const noexcept {
        return maximum_.has_value();
    }

    [[nodiscard]] std::optional<std::size_t> maximum() const noexcept {
        return maximum_;
    }

    [[nodiscard]] bool exceeds(std::size_t bytes_value) const noexcept {
        return maximum_.has_value() && bytes_value > *maximum_;
    }

    [[nodiscard]] bool addition_exceeds(std::size_t current, std::size_t added) const noexcept {
        if (added > (std::numeric_limits<std::size_t>::max)() - current) {
            return true;
        }
        return maximum_.has_value() && (current > *maximum_ || added > *maximum_ - current);
    }

    [[nodiscard]] std::size_t read_ceiling() const noexcept {
        return maximum_.value_or((std::numeric_limits<std::size_t>::max)());
    }

private:
    explicit protocol_byte_limit(std::size_t bytes_value) noexcept
        : maximum_(bytes_value) {}

    std::optional<std::size_t> maximum_;
};

}  // namespace ruvia

namespace ruvia {

// An HTTP protocol primitive rejected input with a response status that a
// server driver may map onto its own application error model. `code` is a
// stable snake_case machine token naming the specific failure (for example
// "missing_host") and must have static storage duration: it is kept as a view.
// The human-readable message is copied into fixed exception storage, so
// reporting never allocates or borrows.
class http_protocol_error final : public std::exception {
public:
    http_protocol_error(
        http_status_code status, std::string_view code, std::string_view message) noexcept
        : status_(status),
          code_(code) {
        const auto size = (std::min)(message.size(), message_.size() - 1);
        if (size != 0) {
            std::memcpy(message_.data(), message.data(), size);
        }
        message_[size] = '\0';
    }

    [[nodiscard]] http_status_code status() const noexcept {
        return status_;
    }

    [[nodiscard]] std::string_view code() const noexcept {
        return code_;
    }

    [[nodiscard]] const char* what() const noexcept override {
        return message_.data();
    }

private:
    http_status_code status_;
    std::string_view code_;
    std::array<char, 128> message_{};
};

enum class http_parse_error : std::uint8_t {
    header_too_large,
    uri_too_long,
    invalid_request_line,
    unsupported_http_version,
    invalid_request_target,
    invalid_header,
    invalid_connection,
    invalid_upgrade,
    missing_host,
    invalid_host,
    invalid_content_length,
    conflicting_content_length,
    invalid_transfer_encoding,
    unsupported_transfer_encoding
};

[[nodiscard]] http_protocol_error http_parse_protocol_error(http_parse_error error) noexcept;

}  // namespace ruvia

namespace ruvia {

[[nodiscard]] constexpr unsigned char http_ascii_to_lower(unsigned char c) noexcept {
    return c >= 'A' && c <= 'Z' ? static_cast<unsigned char>(c + ('a' - 'A')) : c;
}

[[nodiscard]] constexpr bool http_ascii_equals_ignore_case(
    std::string_view left, std::string_view right) noexcept {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t i = 0; i < left.size(); ++i) {
        if (http_ascii_to_lower(static_cast<unsigned char>(left[i])) !=
            http_ascii_to_lower(static_cast<unsigned char>(right[i]))) {
            return false;
        }
    }
    return true;
}

}  // namespace ruvia

// The single owner of HTTP field-line syntax (RFC 9110 section 5): token and
// field-content byte classes and the normalized field name/value predicates.
// Field-specific grammars build on these; field-line OWS belongs to wire
// parsers, so normalized values never start or end with SP or HTAB.

namespace ruvia {

// OWS / BWS byte: SP or HTAB (RFC 9110 section 5.6.3).
[[nodiscard]] constexpr bool is_http_ows_char(char c) noexcept {
    return c == ' ' || c == '\t';
}

// Removes only SP and HTAB from the ends of an HTTP field value (OWS,
// RFC 9110 section 5.6.3). The result borrows the caller's input.
[[nodiscard]] constexpr std::string_view http_trim_ows(borrowed_text text) noexcept {
    auto value = text.view();
    while (!value.empty() && is_http_ows_char(value.front())) {
        value.remove_prefix(1);
    }
    while (!value.empty() && is_http_ows_char(value.back())) {
        value.remove_suffix(1);
    }
    return value;
}

// 256-entry character class tables (picohttpparser/llhttp style): one load
// replaces multi-comparison chains and lets scan loops validate as they move.
inline constexpr std::array<bool, 256> http_token_char_table = [] {
    std::array<bool, 256> table_value{};
    for (unsigned c = '0'; c <= '9'; ++c) {
        table_value[c] = true;
    }
    for (unsigned c = 'A'; c <= 'Z'; ++c) {
        table_value[c] = true;
    }
    for (unsigned c = 'a'; c <= 'z'; ++c) {
        table_value[c] = true;
    }
    for (const unsigned char c :
        {'!', '#', '$', '%', '&', '\'', '*', '+', '-', '.', '^', '_', '`', '|', '~'}) {
        table_value[c] = true;
    }
    return table_value;
}();

// field-content bytes: HTAB, printable ASCII, and obs-text (0x80-0xFF).
// CR/LF/NUL/other controls and DEL are excluded.
inline constexpr std::array<bool, 256> http_field_value_char_table = [] {
    std::array<bool, 256> table_value{};
    for (unsigned c = 0; c < 256; ++c) {
        table_value[c] = c == '\t' || (c >= 0x20 && c != 0x7F);
    }
    return table_value;
}();

[[nodiscard]] constexpr bool is_http_token_char(unsigned char c) noexcept {
    return http_token_char_table[c];
}

[[nodiscard]] constexpr bool is_http_field_value_char(unsigned char c) noexcept {
    return http_field_value_char_table[c];
}

// Find the first non-token byte. Independent table loads let valid runs avoid
// one loop branch per byte; the final scan preserves the exact stop position.
[[nodiscard]] inline std::size_t http_token_prefix_size(std::string_view value) noexcept {
    std::size_t index = 0;
    while (value.size() - index >= 4) {
        const auto* bytes_value = value.data() + index;
        // Plain bool operands keep the bitwise '&' branchless without the
        // short-circuit warning on call operands.
        const bool first = is_http_token_char(static_cast<unsigned char>(bytes_value[0]));
        const bool second = is_http_token_char(static_cast<unsigned char>(bytes_value[1]));
        const bool third = is_http_token_char(static_cast<unsigned char>(bytes_value[2]));
        const bool fourth = is_http_token_char(static_cast<unsigned char>(bytes_value[3]));
        if (!(first & second & third & fourth)) {
            break;
        }
        index += 4;
    }
    while (index < value.size() && is_http_token_char(static_cast<unsigned char>(value[index]))) {
        ++index;
    }
    return index;
}

namespace detail {
// Keep bulk-scan register use outside inlined short-value callers.
[[nodiscard]] bool is_valid_long_http_field_value_bytes(const char* data, std::size_t size) noexcept;
}  // namespace detail

// Byte repertoire only. Field parsing separately handles leading/trailing OWS.
[[nodiscard]] inline bool is_valid_http_field_value_bytes(std::string_view value) noexcept {
    if (value.size() > 32) {
        return detail::is_valid_long_http_field_value_bytes(value.data(), value.size());
    }
    // Preserve a compile-time loop bound for short-value unrolling.
    const auto size = std::min<std::size_t>(32, value.size());
    for (std::size_t index = 0; index < size; ++index) {
        if (!is_http_field_value_char(static_cast<unsigned char>(value[index]))) {
            return false;
        }
    }
    return true;
}

// field-name = token.
[[nodiscard]] inline bool is_valid_http_header_name(std::string_view name) noexcept {
    return !name.empty() && http_token_prefix_size(name) == name.size();
}

// A normalized field value: field-content bytes without leading or trailing
// OWS. Wire parsers that accept field-line OWS trim it before this check.
[[nodiscard]] inline bool is_valid_http_header_value(std::string_view value) noexcept {
    if (value.empty()) {
        return true;
    }
    if (is_http_ows_char(value.front()) || is_http_ows_char(value.back())) {
        return false;
    }
    return is_valid_http_field_value_bytes(value);
}

// 校验单个出站响应字段：上述名称/规范化值语法及响应发送方的已知字段
// 语义（Content-Type、Content-Encoding、Trailer），不分配内存。
// 不应用请求专属字段语义或请求托管字段黑名单，不用于接收方校验。
// 调用方仍负责重复字段、版本相关连接/分帧规则、应用禁改字段政策和
// 完整响应约束；返回 true 不代表该字段在任意响应上下文中都可发送。
[[nodiscard]] bool is_valid_http_response_field(std::string_view name, std::string_view value) noexcept;

}  // namespace ruvia

#pragma once

namespace ruvia {

// Index of the next `delimiter` at or after `start` that is outside an RFC 9110
// section 5.6.4 quoted-string, or value.size() when there is none. A quoted-pair
// (`\"`) does not end the string. This is the only quote-aware delimiter scan;
// every list and parameter visitor below is built on it.
[[nodiscard]] constexpr std::size_t http_find_unquoted_delimiter(
    std::string_view value, std::size_t start, char delimiter) noexcept {
    if (start >= value.size()) {
        return value.size();
    }
    bool quoted = false;
    for (std::size_t index = 0; index < value.size(); ++index) {
        const char ch = value[index];
        if (quoted) {
            if (ch == '\\' && index + 1 < value.size()) {
                ++index;
            } else if (ch == '"') {
                quoted = false;
            }
        } else if (ch == '"') {
            quoted = true;
        } else if (ch == delimiter && index >= start) {
            return index;
        }
    }
    return value.size();
}

namespace detail {

// Splits one OWS-trimmed `name=value` segment on its first '=' and hands the
// OWS-trimmed pair to the visitor. A segment without '=' is not a parameter and
// is skipped. Returns the visitor's keep-going result.
template <typename visitor_type>
[[nodiscard]] inline bool http_emit_field_parameter(std::string_view part, visitor_type& visitor) {
    const auto equals = part.find('=');
    if (equals == std::string_view::npos) {
        return true;
    }
    return visitor(http_trim_ows(part.substr(0, equals)), http_trim_ows(part.substr(equals + 1)));
}

}  // namespace detail

// Visits comma-separated field members, treating commas inside quoted strings
// (including escaped quotes) as data. Members are OWS-trimmed and empty members
// are reported; framing-sensitive callers can reject them. Returning false from
// the visitor stops traversal. Views borrow the input field.
template <typename visitor_type>
inline void http_visit_comma_separated_quoted_field_items(
    std::string_view value, visitor_type&& visitor) {
    std::size_t start = 0;
    for (;;) {
        const auto end = start + http_find_unquoted_delimiter(value.substr(start), 0, ',');
        if (!visitor(http_trim_ows(value.substr(start, end - start))) || end == value.size()) {
            return;
        }
        start = end + 1;
    }
}

// Visits semicolon-separated field parameters while treating semicolons inside
// quoted strings (including escaped quotes) as data. Parameter names and values
// are trimmed of OWS; segments without '=' are ignored. Values keep their
// quotes. Do not use for Cookie: RFC 6265 gives '"' no special meaning there.
// Views borrow the input.
template <typename visitor_type>
inline void http_visit_semicolon_parameters_quoted_field(
    std::string_view value, visitor_type&& visitor) {
    std::size_t start = 0;
    for (;;) {
        const auto end = start + http_find_unquoted_delimiter(value.substr(start), 0, ';');
        if (!detail::http_emit_field_parameter(http_trim_ows(value.substr(start, end - start)), visitor) ||
            end == value.size()) {
            return;
        }
        start = end + 1;
    }
}

// Removes a surrounding pair of DQUOTE bytes when present; does not unescape
// quoted-pairs. The returned view borrows the input.
[[nodiscard]] constexpr std::string_view http_trim_quoted_field_value(borrowed_text text) noexcept {
    auto value = text.view();
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
        value.remove_prefix(1);
        value.remove_suffix(1);
    }
    return value;
}

// Allocation-free RFC 9110 section 5.6.4 quoted-string reader. The input starts
// at the opening DQUOTE; next() validates and decodes one content byte at a time
// (qdtext or the byte of a quoted-pair: HTAB, SP, VCHAR, obs-text) and returns
// false at the closing DQUOTE or on malformed input. closed() tells the two
// apart; consumed() then counts the bytes read, both quotes included. Bytes after
// the closing DQUOTE are never read.
class http_quoted_string_cursor final {
public:
    explicit constexpr http_quoted_string_cursor(borrowed_text input) noexcept
        : input_(input.view()),
          state_(!input_.empty() && input_.front() == '"' ? state_type::open : state_type::failed) {}

    [[nodiscard]] constexpr bool next(unsigned char& out) noexcept {
        if (state_ != state_type::open) {
            return false;
        }
        if (position_ == input_.size()) {
            state_ = state_type::failed;
            return false;
        }
        auto byte = static_cast<unsigned char>(input_[position_++]);
        if (byte == '"') {
            state_ = state_type::closed;
            return false;
        }
        if (byte == '\\') {
            if (position_ == input_.size()) {
                state_ = state_type::failed;
                return false;
            }
            byte = static_cast<unsigned char>(input_[position_++]);
        }
        if (!is_http_field_value_char(byte)) {
            state_ = state_type::failed;
            return false;
        }
        out = byte;
        return true;
    }

    [[nodiscard]] constexpr bool closed() const noexcept {
        return state_ == state_type::closed;
    }

    [[nodiscard]] constexpr std::size_t consumed() const noexcept {
        return position_;
    }

private:
    enum class state_type : unsigned char { open,
        closed,
        failed };

    std::string_view input_;
    std::size_t position_{1};
    state_type state_;
};

// Byte length of the well-formed quoted-string that starts `input`, quotes
// included, or 0 when `input` does not start with one.
[[nodiscard]] constexpr std::size_t http_quoted_string_size(std::string_view input) noexcept {
    http_quoted_string_cursor cursor(input);
    unsigned char byte = 0;
    while (cursor.next(byte)) {
    }
    return cursor.closed() ? cursor.consumed() : 0;
}

// True when the whole of `value` is exactly one well-formed quoted-string.
[[nodiscard]] constexpr bool http_is_quoted_string(std::string_view value) noexcept {
    return !value.empty() && http_quoted_string_size(value) == value.size();
}

// Iterates a comma-delimited HTTP field-name list without allocating. Empty
// members are ignored, including an entirely empty list. Each returned view
// borrows the input. Drain next() until it returns nullopt, then check valid()
// to distinguish end from malformed input.
class http_field_name_list final {
public:
    explicit http_field_name_list(borrowed_text field) noexcept
        : remaining_(field.view()) {}

    [[nodiscard]] std::optional<std::string_view> next() noexcept;

    [[nodiscard]] bool valid() const noexcept {
        return finished_ && valid_;
    }

private:
    std::string_view remaining_;
    bool finished_{false};
    bool valid_{false};
};

}  // namespace ruvia

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

// The one role-aware list walk (RFC 9110 section 5.6.1) over the quote-aware
// splitter: an empty member is malformed for a sender and skipped for a
// recipient; every other member goes to `visitor`, whose non-ok status stops
// the walk and is returned.
template <typename visitor_type>
[[nodiscard]] inline http_field_list_parse_status http_visit_list_members(
    std::string_view value, http_field_list_role role, visitor_type&& visitor) {
    auto status = http_field_list_parse_status::ok;
    http_visit_comma_separated_quoted_field_items(value, [&status, role, &visitor](std::string_view member) {
        if (member.empty()) {
            if (role == http_field_list_role::sender) {
                status = http_field_list_parse_status::malformed;
            }
        } else {
            status = visitor(member);
        }
        return status == http_field_list_parse_status::ok;
    });
    return status;
}

// `#token` lists: a member that is not a token is malformed; a visitor that
// returns false rejects the field.
template <typename visitor_type>
[[nodiscard]] inline http_field_list_parse_status http_visit_token_list(
    std::string_view value, http_field_list_role role, visitor_type&& visitor) {
    return http_visit_list_members(value, role, [&visitor](std::string_view member) {
        if (!is_valid_http_header_name(member)) {
            return http_field_list_parse_status::malformed;
        }
        return visitor(member) ? http_field_list_parse_status::ok : http_field_list_parse_status::rejected;
    });
}

// The first nonempty list member accepted by `predicate`, or an empty view.
// Members are not validated; a quoted-string member is never split.
template <typename predicate_type>
[[nodiscard]] inline std::string_view http_find_header_token(
    std::string_view value, predicate_type&& predicate) noexcept {
    std::string_view found;
    http_visit_comma_separated_quoted_field_items(value, [&found, &predicate](std::string_view member) noexcept {
        if (!member.empty() && predicate(member)) {
            found = member;
            return false;
        }
        return true;
    });
    return found;
}

template <http_temporary_owning_char_string value_type, typename predicate_type>
std::string_view http_find_header_token(value_type&&, predicate_type&&) = delete;

// Accept-style negotiation fields skip empty list items without a role rule;
// grammar-checked lists use http_visit_list_members.
template <typename visitor_type>
inline void http_visit_comma_separated_quoted(std::string_view value, visitor_type&& visitor) {
    http_visit_comma_separated_quoted_field_items(
        value, [&visitor](std::string_view item) { return item.empty() || visitor(item); });
}

// Plain comma-delimited token scan for fields whose grammar never permits
// quoted-string (such as Connection and Vary). Empty segments are skipped.
template <typename visitor_type>
inline void http_visit_comma_separated_tokens(std::string_view value, visitor_type&& visitor) {
    for (;;) {
        const auto comma = value.find(',');
        const auto item = http_trim_ows(value.substr(0, comma));
        if (!item.empty() && !visitor(item)) {
            return;
        }
        if (comma == std::string_view::npos) {
            return;
        }
        value.remove_prefix(comma + 1);
    }
}

// True when `predicate` accepts every ';'-separated parameter that follows a
// field value's leading token — the media type, coding or extension name, which
// carries no '=' and is never a parameter itself. The scan is quote-aware, so a
// ';' inside a quoted parameter value does not split, and each parameter reaches
// the predicate with its surrounding whitespace trimmed. A value with no ';' has
// no parameters and trivially satisfies the predicate.
//
// Unlike http_visit_semicolon_parameters_quoted_field this reports rejection: a predicate
// that returns false stops the walk and fails the whole field value. Use it
// where a malformed parameter must invalidate the field rather than be skipped.
template <typename predicate_type>
[[nodiscard]] inline bool http_all_parameters(std::string_view value, predicate_type&& predicate) {
    auto start = http_find_unquoted_delimiter(value, 0, ';');
    if (start >= value.size()) {
        return true;
    }
    ++start;
    while (start <= value.size()) {
        const auto end = start + http_find_unquoted_delimiter(value.substr(start), 0, ';');
        if (!predicate(http_trim_ows(value.substr(start, end - start)))) {
            return false;
        }
        if (end >= value.size()) {
            return true;
        }
        start = end + 1;
    }
    return true;
}

[[nodiscard]] inline bool http_has_token(std::string_view value, std::string_view expected) noexcept {
    if (expected.empty()) {
        return false;
    }
    const auto expected_first = http_ascii_to_lower(static_cast<unsigned char>(expected.front()));
    return !http_find_header_token(value, [expected, expected_first](std::string_view token) noexcept {
        return token.size() == expected.size() &&
               http_ascii_to_lower(static_cast<unsigned char>(token.front())) == expected_first &&
               http_ascii_equals_ignore_case(token, expected);
    }).empty();
}

[[nodiscard]] inline bool is_http_token_or_quoted_string(std::string_view value) noexcept {
    if (value.empty()) {
        return false;
    }
    return value.front() == '"' ? http_is_quoted_string(value) : is_valid_http_header_name(value);
}

// RFC 9112 section 7 transfer-parameter: `token BWS "=" BWS ( token / quoted-string )`.
// `part` is one OWS-trimmed segment as produced by http_all_parameters.
[[nodiscard]] inline bool is_valid_http_transfer_parameter(std::string_view part) noexcept {
    const auto equals = part.find('=');
    return equals != std::string_view::npos && is_valid_http_header_name(http_trim_ows(part.substr(0, equals))) &&
           is_http_token_or_quoted_string(http_trim_ows(part.substr(equals + 1)));
}

[[nodiscard]] inline std::string_view http_header_token_before_parameters(
    std::string_view value) noexcept {
    const auto semicolon = value.find(';');
    return http_trim_ows(semicolon == std::string_view::npos ? value : value.substr(0, semicolon));
}

template <http_temporary_owning_char_string value_type>
std::string_view http_header_token_before_parameters(value_type&&) = delete;

}  // namespace ruvia::detail

namespace ruvia {

// Protocol version is HTTP message control data, not a borrowed wire token.
// HTTP/1 carries it in the start-line; HTTP/2 and HTTP/3 establish it from
// connection framing even though no textual version appears in a field block.
enum class http_protocol_version : std::uint8_t {
    http10,
    http11,
    http2,
    http3,
};

}  // namespace ruvia

namespace ruvia {
enum class http_scheme : std::uint8_t {
    http,
    https,
};

enum class http_known_method : std::uint8_t {
    get,
    post,
    put,
    delete_value,
    patch,
    head,
    options,
    connect,
    unknown
};

[[nodiscard]] constexpr std::uint32_t http_known_method_bit(http_known_method method) noexcept {
    return method == http_known_method::unknown
               ? 0U
               : (std::uint32_t{1} << static_cast<std::uint32_t>(method));
}

// HTTP methods are an extensible, case-sensitive token space. http_known_method is
// only the framework's fixed semantic classification; it is never the wire value.
[[nodiscard]] http_known_method classify_http_method(std::string_view method) noexcept;
[[nodiscard]] std::string_view known_http_method_token(http_known_method method) noexcept;
[[nodiscard]] bool is_valid_http_method_token(std::string_view method) noexcept;

// RFC 9110 method properties over the exact, case-sensitive wire token. Unknown
// extension methods are conservatively neither safe nor idempotent; TRACE is
// recognized even though it is intentionally not a routable fixed enum member.
[[nodiscard]] bool is_http_method_safe(std::string_view method) noexcept;
[[nodiscard]] bool is_http_method_idempotent(std::string_view method) noexcept;

}  // namespace ruvia

namespace ruvia {

// An owned field returned by protocol parsers and events. The same value type
// represents initial fields and trailers for requests and responses.
class http_header final {
public:
    http_header(std::string_view name, std::string_view value)
        : name_size_(name.size()),
          value_size_(value.size()),
          owned_(true) {
        if (value.size() > (std::numeric_limits<std::size_t>::max)() - name.size()) {
            throw std::length_error("HTTP header storage size overflows size_t");
        }
        const auto size = name.size() + value.size();
        if (size != 0) {
            bytes_ = new char[size];
            if (!name.empty()) {
                std::memcpy(bytes_, name.data(), name.size());
            }
            if (!value.empty()) {
                std::memcpy(bytes_ + name.size(), value.data(), value.size());
            }
        }
    }

    http_header(const http_header& other)
        : bytes_(!other.owned_ ? other.bytes_ : nullptr),
          name_size_(other.name_size_),
          value_size_(other.value_size_),
          owned_(other.owned_) {
        if (owned_) {
            const auto size = name_size_ + value_size_;
            if (size != 0) {
                bytes_ = new char[size];
                std::memcpy(bytes_, other.bytes_, size);
            }
        }
    }

    http_header(http_header&& other) noexcept
        : bytes_(std::exchange(other.bytes_, nullptr)),
          name_size_(std::exchange(other.name_size_, 0)),
          value_size_(std::exchange(other.value_size_, 0)),
          owned_(std::exchange(other.owned_, false)) {}

    http_header& operator=(const http_header& other) {
        if (this != &other) {
            http_header replacement(other);
            swap_storage(replacement);
        }
        return *this;
    }

    http_header& operator=(http_header&& other) noexcept {
        if (this != &other) {
            http_header retired(std::move(*this));
            bytes_ = std::exchange(other.bytes_, nullptr);
            name_size_ = std::exchange(other.name_size_, 0);
            value_size_ = std::exchange(other.value_size_, 0);
            owned_ = std::exchange(other.owned_, false);
        }
        return *this;
    }

    ~http_header() noexcept {
        release();
    }

    // Owning boundaries that normalize field names lower-case them while
    // copying, so the field still costs one allocation.
    [[nodiscard]] static http_header copy_with_lowercase_name(
        std::string_view name, std::string_view value) {
        auto result = http_header(name, value);
        for (std::size_t index = 0; index < result.name_size_; ++index) {
            result.bytes_[index] =
                static_cast<char>(http_ascii_to_lower(static_cast<unsigned char>(result.bytes_[index])));
        }
        return result;
    }

    [[nodiscard]] std::string_view name() const& noexcept RUVIA_LIFETIMEBOUND {
        return name_size_ != 0 ? std::string_view(bytes_, name_size_) : std::string_view{};
    }
    std::string_view name() const&& = delete;
    [[nodiscard]] std::string_view value() const& noexcept RUVIA_LIFETIMEBOUND {
        return value_size_ != 0 ? std::string_view(bytes_ + name_size_, value_size_) : std::string_view{};
    }
    std::string_view value() const&& = delete;

private:
    friend class websocket_client_negotiation;

    constexpr http_header() noexcept = default;

    constexpr http_header(
        const char* static_bytes, std::size_t name_size, std::size_t value_size) noexcept
        : bytes_(const_cast<char*>(static_bytes)),
          name_size_(name_size),
          value_size_(value_size),
          owned_(false) {}

    void release() noexcept {
        auto* const bytes = std::exchange(bytes_, nullptr);
        name_size_ = 0;
        value_size_ = 0;
        if (std::exchange(owned_, false) && bytes != nullptr) {
            delete[] bytes;
        }
    }

    void swap_storage(http_header& other) noexcept {
        std::swap(bytes_, other.bytes_);
        std::swap(name_size_, other.name_size_);
        std::swap(value_size_, other.value_size_);
        std::swap(owned_, other.owned_);
    }

    char* bytes_{nullptr};
    std::size_t name_size_{};
    std::size_t value_size_{};
    bool owned_{false};
};

class http_header_view final {
public:
    constexpr http_header_view() noexcept = default;

    constexpr http_header_view(std::string_view name, std::string_view value) noexcept
        : name_(name),
          value_(value) {}

    template <typename name_type, typename value_type>
        requires(detail::http_temporary_owning_char_string<name_type> ||
                    detail::http_temporary_owning_char_string<value_type>)
    http_header_view(name_type&& name, value_type&& value) = delete;

    [[nodiscard]] constexpr std::string_view name() const noexcept {
        return name_;
    }

    [[nodiscard]] constexpr std::string_view value() const noexcept {
        return value_;
    }

private:
    std::string_view name_;
    std::string_view value_;
};

// Zero-copy HTTP header-view span held by a request, response, handshake, or
// client option struct. Like borrowed_text, it accepts borrowed spans and
// contiguous owning-container lvalues while rejecting temporary owning
// containers (std::vector, std::array) and initializer_list
// values whose backing storage would expire before the struct is consumed.
using borrowed_http_headers = borrowed_span<http_header_view>;

static_assert(sizeof(borrowed_http_headers) == sizeof(std::span<const http_header_view>));

template <typename field_type>
concept http_header_field = requires(const std::remove_reference_t<field_type>& field) {
    { field.name() } -> std::convertible_to<std::string_view>;
    { field.value() } -> std::convertible_to<std::string_view>;
};

template <typename range_type>
concept http_header_field_range =
    std::ranges::contiguous_range<const std::remove_cvref_t<range_type>> &&
    http_header_field<std::ranges::range_reference_t<const std::remove_cvref_t<range_type>>>;

namespace detail {
class http_header_range_view;
}  // namespace detail

template <typename source_type>
concept http_header_field_source =
    std::same_as<std::remove_cvref_t<source_type>, detail::http_header_range_view> ||
    http_header_field_range<source_type> ||
    requires(const std::remove_cvref_t<source_type>& source) {
        { source.headers() } -> http_header_field_range;
    };
namespace detail {

class http_header_range_view final {
public:
    using accessor_type = http_header_view (*)(const void*, std::size_t) noexcept;

    constexpr http_header_range_view() noexcept = default;

    template <http_header_field_source source_type>
        requires(!std::same_as<std::remove_cvref_t<source_type>, http_header_range_view>)
    constexpr http_header_range_view(const source_type& source) noexcept {
        if constexpr (http_header_field_range<source_type>) {
            using value_type = std::remove_cv_t<std::ranges::range_value_t<const source_type>>;
            data_ = std::ranges::data(source);
            size_ = std::ranges::size(source);
            at_ = +[](const void* base, std::size_t index) noexcept -> http_header_view {
                const auto& field = static_cast<const value_type*>(base)[index];
                return {field.name(), field.value()};
            };
        } else {
            *this = http_header_range_view(source.headers());
        }
    }

    [[nodiscard]] constexpr std::size_t size() const noexcept {
        return size_;
    }

    [[nodiscard]] constexpr bool empty() const noexcept {
        return size_ == 0;
    }

    [[nodiscard]] constexpr http_header_view operator[](std::size_t index) const noexcept {
        return at_(data_, index);
    }

    class iterator final {
    public:
        using iterator_concept = std::forward_iterator_tag;
        using iterator_category = std::input_iterator_tag;
        using value_type = http_header_view;
        using difference_type = std::ptrdiff_t;

        constexpr iterator() noexcept = default;
        constexpr iterator(const void* data, accessor_type at, std::size_t index) noexcept
            : data_(data),
              at_(at),
              index_(index) {}

        [[nodiscard]] constexpr http_header_view operator*() const noexcept {
            return at_(data_, index_);
        }
        constexpr iterator& operator++() noexcept {
            ++index_;
            return *this;
        }
        constexpr iterator operator++(int) noexcept {
            auto copy = *this;
            ++index_;
            return copy;
        }
        [[nodiscard]] friend constexpr bool operator==(
            const iterator& left, const iterator& right) noexcept {
            return left.data_ == right.data_ && left.index_ == right.index_;
        }

    private:
        const void* data_{nullptr};
        accessor_type at_{nullptr};
        std::size_t index_{0};
    };

    [[nodiscard]] constexpr iterator begin() const noexcept {
        return {data_, at_, 0};
    }

    [[nodiscard]] constexpr iterator end() const noexcept {
        return {data_, at_, size_};
    }

private:
    const void* data_{nullptr};
    std::size_t size_{0};
    accessor_type at_{nullptr};
};
}  // namespace detail

}  // namespace ruvia

namespace ruvia {

// RFC 9218 section 4.1: urgency is 0..7, defaulting to 3. Local numeric
// inputs are checked before narrowing. Peer fields instead use the tolerant
// parser below, which ignores out-of-range values as required by section 4.
class http_priority_urgency final {
public:
    constexpr http_priority_urgency() noexcept = default;

    template <std::integral integer_type>
    constexpr http_priority_urgency(integer_type value)
        : value_(is_valid_value(value)
                     ? static_cast<std::uint8_t>(value)
                     : throw std::invalid_argument("HTTP priority urgency must be between 0 and 7")) {}

    template <std::integral integer_type>
    [[nodiscard]] static constexpr bool is_valid_value(integer_type value) noexcept {
        return std::cmp_greater_equal(+value, 0) && std::cmp_less_equal(+value, 7);
    }

    template <std::integral integer_type>
    [[nodiscard]] static constexpr std::optional<http_priority_urgency> try_from_value(integer_type value) noexcept {
        if (!is_valid_value(value)) {
            return std::nullopt;
        }
        return http_priority_urgency(static_cast<std::uint8_t>(value), validated_tag_type{});
    }

    [[nodiscard]] constexpr std::uint8_t value() const noexcept {
        return value_;
    }

    friend constexpr auto operator<=>(http_priority_urgency, http_priority_urgency) = default;

private:
    struct validated_tag_type {};

    constexpr http_priority_urgency(std::uint8_t value, validated_tag_type) noexcept
        : value_(value) {}

    std::uint8_t value_{3};
};

static_assert(std::is_trivially_copyable_v<http_priority_urgency>);
static_assert(sizeof(http_priority_urgency) == sizeof(std::uint8_t));

struct http_priority final {
    http_priority_urgency urgency_{};
    bool incremental_{false};
};
struct http_priority_fields final {
    std::optional<http_priority_urgency> urgency_{};
    std::optional<bool> incremental_{};
    [[nodiscard]] constexpr http_priority request_priority() const noexcept {
        return {urgency_.value_or(http_priority_urgency{}), incremental_.value_or(false)};
    }
};
enum class http_priority_error : std::uint8_t { invalid_syntax,
    invalid_value,
    invalid_frame,
    output_too_small };
struct http_priority_update final {
    std::uint64_t element_id_{0};
    bool push_{false};
    http_priority_fields fields_{};
};

// RFC 9218 / RFC 8941 Dictionary parsing. Unknown members, invalid parameter
// types and out-of-range values are ignored; malformed structured syntax fails.
// Missing response parameters remain absent for intermediary merging.
[[nodiscard]] std::variant<http_priority_fields, http_priority_error> parse_http_priority(std::string_view value) noexcept;
namespace detail {
[[nodiscard]] std::variant<http_priority_fields, http_priority_error> parse_http_priority_headers(
    http_header_range_view headers) noexcept;
}  // namespace detail

// Reads all Priority field lines in wire order without allocating. A later
// dictionary member replaces the earlier member, including an invalid value.
template <http_header_field_source headers_type>
[[nodiscard]] std::variant<http_priority_fields, http_priority_error> parse_http_priority(
    const headers_type& headers) noexcept {
    return detail::parse_http_priority_headers(headers);
}
[[nodiscard]] std::variant<std::size_t, http_priority_error> encode_http_priority(std::span<char> output, http_priority_fields fields_value) noexcept;
[[nodiscard]] std::variant<http_priority_update, http_priority_error> decode_http2_priority_update(std::span<const char> payload_value) noexcept;
[[nodiscard]] std::variant<std::size_t, http_priority_error> encode_http2_priority_update(std::span<char> output, std::uint32_t stream_id, http_priority_fields fields_value) noexcept;

}  // namespace ruvia

namespace ruvia {

struct http_push_request_view final {
    borrowed_text method_{"GET"};
    borrowed_text scheme_{"https"};
    borrowed_text authority_{};
    borrowed_text path_{"/"};
    borrowed_http_headers headers_{};
};

// Detached promise metadata owns its decoded bytes independently of feed input.
// Accepting a promise does
// not establish transport authority: the driver validates the URI origin using
// its authenticated connection before consuming the pushed response.
struct http_push_request final {
    http_push_request() noexcept = default;
    http_push_request(const http_push_request&) = delete;
    http_push_request& operator=(const http_push_request&) = delete;
    http_push_request(http_push_request&&) noexcept = default;
    http_push_request& operator=(http_push_request&&) = delete;
    std::string method_;
    std::string scheme_;
    std::string authority_;
    std::string path_;
    std::vector<http_header> headers_;
};

struct http2_push_promise_event final {
    std::uint32_t associated_stream_id_;
    std::uint32_t promised_stream_id_;
    http_push_request request_;
};

enum class http2_push_submit_error : std::uint8_t {
    invalid_state,
    push_disabled,
    stream_limit,
    invalid_request,
};

}  // namespace ruvia

namespace ruvia {
// Encoders validate limits before allocation and write into the final owned result.
struct http_origin_advertisement final {
    std::vector<std::string> origins_;
    http_origin_advertisement() noexcept = default;
    http_origin_advertisement(const http_origin_advertisement&) = default;
    http_origin_advertisement(http_origin_advertisement&&) noexcept = default;
    http_origin_advertisement& operator=(const http_origin_advertisement&) = default;
    http_origin_advertisement& operator=(http_origin_advertisement&&) noexcept = default;
};
struct http_alternative_service_advertisement final {
    std::uint32_t stream_id_{0};
    std::string origin_;
    std::string field_value_;
    http_alternative_service_advertisement() noexcept = default;
    http_alternative_service_advertisement(const http_alternative_service_advertisement&) = default;
    http_alternative_service_advertisement(http_alternative_service_advertisement&&) noexcept = default;
    http_alternative_service_advertisement& operator=(const http_alternative_service_advertisement&) = default;
    http_alternative_service_advertisement& operator=(http_alternative_service_advertisement&&) noexcept = default;
};
enum class http_connection_advertisement_error : std::uint8_t {
    malformed_payload,
    invalid_origin,
    invalid_field,
    invalid_stream,
    limit,
};
// RFC 8336 ORIGIN payload decoding. Invalid serialized
// origins are ignored; truncated length-delimited entries reject the payload.
// Origin authority and connection coalescing require the runtime's TLS identity.
[[nodiscard]] std::variant<http_origin_advertisement, http_connection_advertisement_error>
decode_http_origin_advertisement(std::span<const char> payload_value);
[[nodiscard]] std::variant<std::vector<char>, http_connection_advertisement_error>
encode_http2_origin_frame(std::span<const std::string_view> origins,
    std::uint32_t max_frame_size = 16384);
// RFC 7838 ALTSVC. Stream 0 carries an explicit origin; a request stream
// carries an empty origin and inherits the request origin. The value is an
// Alt-Svc field value; transport authority and cache policy are external.
[[nodiscard]] std::variant<http_alternative_service_advertisement, http_connection_advertisement_error>
decode_http2_alternative_service(std::uint32_t stream_id, std::span<const char> payload_value);
[[nodiscard]] std::variant<std::vector<char>, http_connection_advertisement_error>
encode_http2_alternative_service_frame(std::uint32_t stream_id, std::string_view origin, std::string_view field_value,
    std::uint32_t max_frame_size = 16384);
}  // namespace ruvia
