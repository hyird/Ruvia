#pragma once

#include <cstdint>
#include <string_view>

// RFC 3986 syntax, one level below any HTTP-specific rule: which byte sequences
// form a legal URI component, userinfo, port, IPv4 / IPv6 / IPvFuture literal or
// reg-name. Nothing here knows about request targets, Host fields or origins --
// those rules live in HttpRequestTarget.h and HttpSerializedOrigin.h.

namespace ruvia::detail {

[[nodiscard]] inline bool isDecimalDigit(char c) noexcept {
    return c >= '0' && c <= '9';
}

// RFC 3986 section 2.3 unreserved. Percent-encoding one of these octets is
// equivalent to spelling it literally, which host comparison relies on.
[[nodiscard]] inline bool isUnreservedByte(unsigned char byte) noexcept {
    return (byte >= '0' && byte <= '9') || (byte >= 'A' && byte <= 'Z') ||
           (byte >= 'a' && byte <= 'z') || byte == '-' || byte == '.' || byte == '_' || byte == '~';
}

// Parse a decimal port, rejecting anything that does not fit 16 bits.
// RFC 3986 pchar = unreserved / pct-encoded / sub-delims / ":" / "@" (the
// percent sign itself is admitted; the encoding is checked by the caller).
[[nodiscard]] bool isUriPchar(unsigned char byte) noexcept;

[[nodiscard]] bool parsePortValue(std::string_view value, std::uint16_t& port) noexcept;

// Validate a percent-encoded component: unreserved / sub-delims / pct-encoded,
// plus ':' and '@', with '/' and '?' admitted only where the component allows them.
[[nodiscard]] bool isValidUriComponent(
    std::string_view value, bool allowSlash, bool allowQuestion) noexcept;
[[nodiscard]] bool isValidUriUserinfo(std::string_view value) noexcept;
[[nodiscard]] bool isValidUriPort(std::string_view value) noexcept;

[[nodiscard]] bool parseIpv4Address(std::string_view value) noexcept;
// Both take the literal without its surrounding brackets.
[[nodiscard]] bool isValidIpv6Literal(std::string_view literal) noexcept;
[[nodiscard]] bool isValidIpvFuture(std::string_view literal) noexcept;
[[nodiscard]] bool isValidRegName(std::string_view value) noexcept;

}  // namespace ruvia::detail
