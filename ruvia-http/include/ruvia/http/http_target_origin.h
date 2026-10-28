#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <type_traits>

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
    [[nodiscard]] constexpr std::string_view host() const noexcept {
        return decoded_host_size_ != 0 ? std::string_view(decoded_host_.data(), decoded_host_size_) : host_;
    }

    // ipv4/ipv6: the address in network byte order (4 or 16 bytes).
    // dns_name: empty.
    [[nodiscard]] std::span<const std::uint8_t> address() const noexcept {
        return std::span(address_.data(), address_size_);
    }

    [[nodiscard]] static constexpr http_target_origin accepted() noexcept {
        return http_target_origin(http_target_origin_action::accept);
    }

    [[nodiscard]] static constexpr http_target_origin misdirected() noexcept {
        return http_target_origin(http_target_origin_action::misdirected);
    }

    [[nodiscard]] static constexpr http_target_origin dns_name(std::string_view host) noexcept {
        http_target_origin origin(http_target_origin_action::verify_certificate);
        origin.host_ = host;
        return origin;
    }

    [[nodiscard]] static constexpr http_target_origin decoded_dns_name(std::string_view host) noexcept {
        if (host.size() > max_dns_name_size) {
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
