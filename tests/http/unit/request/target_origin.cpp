#include <algorithm>
#include <array>
#include <cstdint>
#include <memory_resource>
#include <span>
#include <string_view>

#include "ruvia/http/http_header.h"
#include "ruvia/http/http_request.h"
#include "ruvia/http/http_target_origin.h"

#include "test_harness.h"

namespace {

using ruvia::http_target_host_kind;
using ruvia::http_target_origin;
using ruvia::http_target_origin_action;
using ruvia::http_target_origin_connection;

constexpr http_target_origin_connection cleartext{};
constexpr http_target_origin_connection secured{.secured_ = true};

[[nodiscard]] http_target_origin decide(std::string_view method, std::string_view target, std::string_view host,
    http_target_origin_connection connection) {
    std::pmr::monotonic_buffer_resource resource;
    const std::array headers{ruvia::http_header_view("Host", host)};
    const auto request = ruvia::make_parsed_http_request(method, target, headers, {}, &resource);
    return ruvia::decide_http_target_origin(request, connection);
}

}  // namespace

// RFC 9110 §7.4: an "https" target needs a secured connection whose
// certificate covers the target host; "http" targets and tunnel destinations
// impose nothing, and a trusted gateway vouches for the target itself.
RUVIA_TEST(target_origin_decision_follows_scheme_transport_and_gateway_trust) {
    RUVIA_CHECK(decide("GET", "https://a.example/x", "x", cleartext).action() == http_target_origin_action::misdirected);
    RUVIA_CHECK(decide("GET", "HTTPS://a.example/x", "x", cleartext).action() == http_target_origin_action::misdirected);
    RUVIA_CHECK(decide("GET", "https://a.example/x", "x", {.trusted_gateway_ = true}).action() == http_target_origin_action::accept);
    RUVIA_CHECK(decide("GET", "http://a.example/x", "x", cleartext).action() == http_target_origin_action::accept);
    RUVIA_CHECK(decide("GET", "/x", "a.example", cleartext).action() == http_target_origin_action::accept);
    RUVIA_CHECK(decide("GET", "http://a.example/x", "x", secured).action() == http_target_origin_action::accept);
    RUVIA_CHECK(decide("CONNECT", "a.example:443", "a.example:443", secured).action() == http_target_origin_action::accept);
    // RFC 9112 §3.3: an empty authority takes the configured default.
    RUVIA_CHECK(decide("GET", "/x", "", secured).action() == http_target_origin_action::accept);

    const auto name = decide("GET", "/x", "Example.COM.:8443", secured);
    RUVIA_CHECK(name.action() == http_target_origin_action::verify_certificate);
    RUVIA_CHECK(name.host_kind() == http_target_host_kind::dns_name);
    RUVIA_CHECK(name.host() == "Example.COM");
    RUVIA_CHECK(name.address().empty());

    // The absolute-form authority, not a conflicting Host, is the target.
    const auto absolute = decide("GET", "https://b.example/x", "a.example", secured);
    RUVIA_CHECK(absolute.action() == http_target_origin_action::verify_certificate);
    RUVIA_CHECK(absolute.host() == "b.example");

    const auto asterisk = decide("OPTIONS", "*", "a.example", secured);
    RUVIA_CHECK(asterisk.action() == http_target_origin_action::verify_certificate);
    RUVIA_CHECK(asterisk.host() == "a.example");

    const auto ipv4 = decide("GET", "/x", "192.0.2.7:443", secured);
    RUVIA_CHECK(ipv4.action() == http_target_origin_action::verify_certificate);
    RUVIA_CHECK(ipv4.host_kind() == http_target_host_kind::ipv4);
    constexpr std::array<std::uint8_t, 4> ipv4_bytes{192, 0, 2, 7};
    RUVIA_CHECK(std::ranges::equal(ipv4.address(), ipv4_bytes));

    const auto ipv6 = decide("GET", "/x", "[2001:db8::1]", secured);
    RUVIA_CHECK(ipv6.action() == http_target_origin_action::verify_certificate);
    RUVIA_CHECK(ipv6.host_kind() == http_target_host_kind::ipv6);
    constexpr std::array<std::uint8_t, 16> ipv6_bytes{0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
    RUVIA_CHECK(std::ranges::equal(ipv6.address(), ipv6_bytes));
    RUVIA_CHECK(ipv6.host().empty());

    // No certificate identifies an IPvFuture host.
    RUVIA_CHECK(decide("GET", "/x", "[v1.x]", secured).action() == http_target_origin_action::misdirected);
}
