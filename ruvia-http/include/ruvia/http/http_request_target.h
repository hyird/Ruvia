#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>

#include "ruvia/http/borrowed_text.h"
#include "ruvia/http/http_field_section.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_request_content_semantics.h"
#include "ruvia/http/ip_address.h"

namespace ruvia {

class http_request;

// Validates an HTTP origin-form request target without allocating. The same
// protocol rule applies to server routes and outbound HTTP/websocket clients.
[[nodiscard]] bool is_valid_http_origin_form_target(std::string_view target) noexcept;

// RFC 9110 CONNECT authority-form requires a nonempty host and explicit port.
[[nodiscard]] bool is_valid_http_connect_authority(std::string_view authority) noexcept;

// Numeric IP host syntax without URI brackets.
[[nodiscard]] bool is_valid_http_ipv4_literal(std::string_view value) noexcept;
[[nodiscard]] bool is_valid_http_ipv6_literal(std::string_view value) noexcept;

namespace detail {
struct http_authority_view_access;
}

enum class http_authority_port_kind : std::uint8_t {
    absent,
    empty,
    value,
};

// A validated, borrowed RFC 3986 authority without userinfo; IP-literal hosts
// keep their brackets. An explicit empty port stays distinct from an absent one
// for syntax-preserving parsing; both map to the scheme default when an HTTP
// origin is compared.
class http_authority_view final {
public:
    [[nodiscard]] constexpr std::string_view host() const noexcept {
        return host_;
    }

    [[nodiscard]] constexpr http_authority_port_kind port_kind() const noexcept {
        return port_kind_;
    }

    [[nodiscard]] constexpr std::optional<std::uint16_t> port() const noexcept {
        return port_kind_ == http_authority_port_kind::value ? std::optional<std::uint16_t>(port_)
                                                             : std::nullopt;
    }

    // Returns the explicit port or caller fallback. This numeric observation
    // alone does not preserve the absent/empty versus explicit-zero distinction.
    [[nodiscard]] constexpr std::uint16_t effective_port(port_number default_port) const noexcept {
        return port_kind_ == http_authority_port_kind::value ? port_ : default_port.value();
    }

private:
    friend struct detail::http_authority_view_access;

    constexpr http_authority_view(
        std::string_view host, http_authority_port_kind port_kind, std::uint16_t port) noexcept
        : host_(host),
          port_(port),
          port_kind_(port_kind) {}

    std::string_view host_;
    std::uint16_t port_{0};
    http_authority_port_kind port_kind_{http_authority_port_kind::absent};
};

[[nodiscard]] std::optional<http_authority_view> parse_http_authority(borrowed_text value) noexcept;

// Parses a Host-field authority without userinfo and borrows its host text.
// The host retains brackets around an IP literal. An invalid authority has no
// value; a syntactically valid empty host remains an engaged empty view.
[[nodiscard]] std::optional<std::string_view> parse_http_authority_host(borrowed_text value) noexcept;

// Compares HTTP authorities using case-insensitive host syntax and the supplied
// scheme default port, checked before narrowing like other port inputs.
// Zero means no known scheme default: absent/empty ports then compare equal
// only to absent/empty ports, never to an explicitly supplied zero.
// Invalid authorities never compare equal.
[[nodiscard]] bool http_authorities_equal(borrowed_text left, borrowed_text right, port_number default_port) noexcept;

// Validates a WHATWG serialized origin, not a general URI or origin list.
[[nodiscard]] bool is_valid_http_serialized_origin(std::string_view value) noexcept;

// Normalizes an incoming request target for proxy forwarding: converts
// absolute-form targets to origin-form (with "/" when the path is empty and
// preserving any query), converts an OPTIONS target with an empty path and no
// query to "*", and preserves all other forms verbatim.
[[nodiscard]] std::string normalize_http_proxy_request_target(
    std::string_view method, std::string_view target);
[[nodiscard]] std::string normalize_http_proxy_request_target(
    http_known_method method, std::string_view target);
[[nodiscard]] std::string normalize_http_proxy_request_target(
    const http_request& request);

enum class http_max_forwards_status : std::uint8_t {
    absent,
    valid,
    invalid,
};

// Parsed Max-Forwards hop count (RFC 9110 section 7.6.2).
class http_max_forwards_result final {
public:
    [[nodiscard]] static constexpr http_max_forwards_result absent() noexcept {
        return http_max_forwards_result(http_max_forwards_status::absent, 0);
    }
    [[nodiscard]] static constexpr http_max_forwards_result invalid() noexcept {
        return http_max_forwards_result(http_max_forwards_status::invalid, 0);
    }
    [[nodiscard]] static constexpr http_max_forwards_result valid(std::uint64_t current) noexcept {
        return http_max_forwards_result(http_max_forwards_status::valid, current);
    }

    [[nodiscard]] constexpr http_max_forwards_status status() const noexcept {
        return status_;
    }
    [[nodiscard]] constexpr bool is_absent() const noexcept {
        return status_ == http_max_forwards_status::absent;
    }
    [[nodiscard]] constexpr bool is_valid() const noexcept {
        return status_ == http_max_forwards_status::valid;
    }
    [[nodiscard]] constexpr bool is_invalid() const noexcept {
        return status_ == http_max_forwards_status::invalid;
    }
    [[nodiscard]] constexpr std::optional<std::uint64_t> current() const noexcept {
        return is_valid() ? std::optional<std::uint64_t>(current_) : std::nullopt;
    }
    [[nodiscard]] constexpr bool exhausted() const noexcept {
        return is_valid() && current_ == 0;
    }
    [[nodiscard]] constexpr std::optional<std::uint64_t> decremented() const noexcept {
        return (is_valid() && current_ > 0) ? std::optional<std::uint64_t>(current_ - 1)
                                            : std::nullopt;
    }
    [[nodiscard]] std::string format_decremented() const;

private:
    constexpr http_max_forwards_result(
        http_max_forwards_status status, std::uint64_t current) noexcept
        : current_(current),
          status_(status) {}

    std::uint64_t current_{0};
    http_max_forwards_status status_{http_max_forwards_status::absent};
};

[[nodiscard]] std::optional<std::uint64_t> parse_http_max_forwards_value(
    std::string_view field_value,
    std::uint64_t max_saturation = (std::numeric_limits<std::uint64_t>::max)()) noexcept;
[[nodiscard]] http_max_forwards_result parse_http_max_forwards(
    std::string_view field_value,
    std::uint64_t max_saturation = (std::numeric_limits<std::uint64_t>::max)()) noexcept;
[[nodiscard]] http_max_forwards_result parse_http_max_forwards(
    std::span<const http_header> headers,
    std::uint64_t max_saturation = (std::numeric_limits<std::uint64_t>::max)()) noexcept;
[[nodiscard]] http_max_forwards_result parse_http_max_forwards(
    std::span<const http_header_view> headers,
    std::uint64_t max_saturation = (std::numeric_limits<std::uint64_t>::max)()) noexcept;
[[nodiscard]] http_max_forwards_result parse_http_max_forwards(
    const http_request& request,
    std::uint64_t max_saturation = (std::numeric_limits<std::uint64_t>::max)()) noexcept;
}  // namespace ruvia

namespace ruvia {

class http_request;

// The connection facts RFC 9110 §7.4 weighs against a request's target URI.
// The server runtime supplies them; the request alone cannot know them.
struct http_target_origin_connection final {
    // The request arrived over a TLS-secured connection.
    bool secured_{false};
    // The peer is a trusted gateway (a configured reverse proxy). It, not this
    // connection, vouches for the target's scheme and authority, so §7.4
    // exempts the request from the connection checks.
    bool trusted_gateway_{false};
};

enum class http_target_origin_action : std::uint8_t {
    // Nothing on this connection must be verified before processing.
    accept,
    // An "https" target on a secured connection: process it only when the
    // certificate this connection presented is valid for host().
    verify_certificate,
    // The connection cannot carry this target: answer 421 (Misdirected
    // Request, RFC 9110 §15.5.20).
    misdirected,
};

enum class http_target_host_kind : std::uint8_t {
    // A reg-name, without the trailing dot of an absolute domain name.
    dns_name,
    ipv4,
    ipv6,
};

// What an origin server must establish about one request's target origin.
// Meaningful host data only for verify_certificate.
class http_target_origin final {
public:
    static constexpr std::size_t max_dns_name_size = 253;

    [[nodiscard]] constexpr http_target_origin_action action() const noexcept {
        return action_;
    }

    [[nodiscard]] constexpr http_target_host_kind host_kind() const noexcept {
        return host_kind_;
    }

    // dns_name: the host text (percent-encoded unreserved octets decoded, a
    // single trailing dot stripped, letter case preserved). ipv4/ipv6: empty.
    [[nodiscard]] constexpr std::string_view host() const& noexcept RUVIA_LIFETIMEBOUND {
        return decoded_host_size_ != 0 ? std::string_view(decoded_host_.data(), decoded_host_size_) : host_;
    }
    std::string_view host() const&& = delete;

    // ipv4/ipv6: the address in network byte order (4 or 16 bytes).
    // dns_name: empty.
    [[nodiscard]] std::span<const std::uint8_t> address() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::span(address_.data(), address_size_);
    }
    std::span<const std::uint8_t> address() const&& = delete;

    [[nodiscard]] static constexpr http_target_origin accepted() noexcept {
        return http_target_origin(http_target_origin_action::accept);
    }

    [[nodiscard]] static constexpr http_target_origin misdirected() noexcept {
        return http_target_origin(http_target_origin_action::misdirected);
    }

    [[nodiscard]] static constexpr http_target_origin dns_name(std::string_view host) noexcept {
        if (host.empty() || host.size() > max_dns_name_size) {
            return misdirected();
        }
        http_target_origin origin(http_target_origin_action::verify_certificate);
        origin.host_ = host;
        return origin;
    }

    template <detail::http_temporary_owning_char_string host_type>
    static void dns_name(host_type&&) = delete;

    [[nodiscard]] static constexpr http_target_origin decoded_dns_name(std::string_view host) noexcept {
        if (host.empty() || host.size() > max_dns_name_size) {
            return misdirected();
        }
        http_target_origin origin(http_target_origin_action::verify_certificate);
        origin.decoded_host_size_ = static_cast<std::uint8_t>(host.size());
        for (std::size_t i = 0; i < host.size(); ++i) {
            origin.decoded_host_[i] = host[i];
        }
        return origin;
    }

    [[nodiscard]] static constexpr http_target_origin ip_address(
        http_target_host_kind kind, std::span<const std::uint8_t> address) noexcept {
        if ((kind == http_target_host_kind::ipv4 && address.size() != 4) ||
            (kind == http_target_host_kind::ipv6 && address.size() != 16) ||
            (kind != http_target_host_kind::ipv4 && kind != http_target_host_kind::ipv6)) {
            return misdirected();
        }
        http_target_origin origin(http_target_origin_action::verify_certificate);
        origin.host_kind_ = kind;
        origin.address_size_ = static_cast<std::uint8_t>(address.size());
        for (std::size_t i = 0; i < address.size(); ++i) {
            origin.address_[i] = address[i];
        }
        return origin;
    }

private:
    explicit constexpr http_target_origin(http_target_origin_action action) noexcept
        : action_(action) {
        if (std::is_constant_evaluated()) {
            decoded_host_.fill('\0');
            address_.fill(0);
        }
    }

    std::string_view host_;
    std::array<char, max_dns_name_size> decoded_host_;
    std::array<std::uint8_t, 16> address_;
    std::uint8_t decoded_host_size_{0};
    std::uint8_t address_size_{0};
    http_target_origin_action action_;
    http_target_host_kind host_kind_{http_target_host_kind::dns_name};
};

// RFC 9110 §7.4: unless the connection is from a trusted gateway, an origin
// server MUST reject a request whose target URI's scheme-specific requirements
// the connection does not meet; an "https" resource only over a connection
// secured by a certificate valid for the target's origin. Decides, for a
// parsed request of any HTTP version:
// - trusted gateway: accept;
// - an "https" target (absolute-form scheme or HTTP/2 :scheme, compared
//   case-insensitively) on a cleartext connection: misdirected;
// - a secured connection whose target scheme is "https" -- explicit, or
//   implied for origin-form and asterisk-form (RFC 9112 §3.3): verify the
//   target host (:authority, else Host) against the certificate. An
//   IPvFuture host, which no certificate names, is misdirected;
// - everything else accepts: "http" targets on either transport (RFC 9110
//   imposes no certificate requirement on them), CONNECT tunnel targets
//   (authority-form, or HTTP/2 CONNECT without :scheme), and targets with an
//   empty authority, which RFC 9112 §3.3 completes with the server's
//   configured default rather than a client-chosen name.
// The port is not compared: certificates name hosts, not ports. Allocation free.
[[nodiscard]] http_target_origin decide_http_target_origin(
    const http_request& request, http_target_origin_connection connection) noexcept;

}  // namespace ruvia
