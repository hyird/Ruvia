#pragma once

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <type_traits>

namespace ruvia {

// A validated HTTP status-code value. The complete 100..599 extension space is
// representable, while arbitrary integers cannot leak into protocol APIs.
class http_status_code final {
public:
    [[nodiscard]] static constexpr http_status_code from_value(std::uint16_t value) {
        if (!is_valid_value(value)) {
            throw std::invalid_argument("HTTP status code must be in 100..599");
        }
        return http_status_code(value, validated_tag_type{});
    }

    [[nodiscard]] static constexpr std::optional<http_status_code> try_from_value(
        std::uint16_t value) noexcept {
        if (!is_valid_value(value)) {
            return std::nullopt;
        }
        return http_status_code(value, validated_tag_type{});
    }

    [[nodiscard]] static constexpr bool is_valid_value(std::uint16_t value) noexcept {
        return value >= 100 && value <= 599;
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

namespace detail {

[[nodiscard]] inline constexpr bool http_final_status_code_valid(http_status_code status) noexcept {
    return status.is_final();
}

// 101 is a protocol transition rather than an interim progress head. It is
// intentionally owned by a dedicated Upgrade driver instead of either generic
// response-head type.
[[nodiscard]] inline constexpr bool http_interim_status_code_valid(http_status_code status) noexcept {
    return status.is_informational() && status != http_status::switching_protocols;
}

}  // namespace detail

}  // namespace ruvia
