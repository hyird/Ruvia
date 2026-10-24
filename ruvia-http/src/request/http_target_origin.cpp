#include "ruvia/http/http_target_origin.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

#include "ruvia/http/http_ascii.h"
#include "ruvia/http/http_request.h"
#include "ruvia/http/http_request_target.h"

#include "parser/http_uri_grammar.h"
#include "request/http_request_access.h"

namespace ruvia {
namespace {

[[nodiscard]] http_target_origin certificate_check(std::string_view authority) noexcept {
    // The parsers admitted only valid Host syntax; anything else here is a
    // caller-built request, which fails closed.
    const auto parsed = parse_http_authority(authority);
    if (!parsed.has_value()) {
        return http_target_origin::misdirected();
    }
    const auto host = parsed->host();
    if (host.front() == '[') {
        const auto groups = detail::parse_ipv6_address(host.substr(1, host.size() - 2));
        if (!groups.has_value()) {
            // IPvFuture: no certificate identifies such a host.
            return http_target_origin::misdirected();
        }
        std::array<std::uint8_t, 16> address{};
        for (std::size_t i = 0; i < groups->size(); ++i) {
            address[2 * i] = static_cast<std::uint8_t>((*groups)[i] >> 8);
            address[2 * i + 1] = static_cast<std::uint8_t>((*groups)[i] & 0xFFU);
        }
        return http_target_origin::ip_address(http_target_host_kind::ipv6, address);
    }
    if (const auto address = detail::parse_ipv4_address(host)) {
        return http_target_origin::ip_address(http_target_host_kind::ipv4, *address);
    }
    // An absolute domain name names the same host as its relative spelling.
    const auto name = host.size() > 1 && host.back() == '.' ? host.substr(0, host.size() - 1) : host;
    return http_target_origin::dns_name(name);
}

}  // namespace

http_target_origin decide_http_target_origin(
    const http_request& request, http_target_origin_connection connection) noexcept {
    if (connection.trusted_gateway_) {
        return http_target_origin::accepted();
    }
    const auto form = request.target_form();
    const auto scheme = request.scheme();
    if (form == http_request_target_form::authority ||
        (form == http_request_target_form::http2 && scheme.empty())) {
        // A CONNECT tunnel target names a destination, not an "https" resource.
        return http_target_origin::accepted();
    }
    const bool https = scheme.empty() ? connection.secured_ : http_ascii_equals_ignore_case(scheme, "https");
    if (!https) {
        return http_target_origin::accepted();
    }
    if (!connection.secured_) {
        return http_target_origin::misdirected();
    }
    const auto authority = !request.authority().empty()
                               ? request.authority()
                               : detail::http_request_access::known_header(request, detail::request_header_kind::host);
    if (authority.empty()) {
        return http_target_origin::accepted();
    }
    return certificate_check(authority);
}

}  // namespace ruvia
