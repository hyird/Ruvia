#include <algorithm>
#include <array>
#include <cstdint>
#include <memory_resource>
#include <span>
#include <string>
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

RUVIA_TEST(target_origin_dns_reference_names_follow_label_and_full_name_boundaries) {
    const auto maximal = std::string(63, 'a') + "." + std::string(63, 'b') + "." +
                         std::string(63, 'c') + "." + std::string(61, 'd');
    for (const std::string_view host : {std::string_view(maximal), std::string_view("7.example"),
             std::string_view("xn--bcher-kva.example")}) {
        const auto origin = decide("GET", "/", host, secured);
        RUVIA_CHECK(origin.action() == http_target_origin_action::verify_certificate);
        RUVIA_CHECK(origin.host_kind() == http_target_host_kind::dns_name);
        RUVIA_CHECK_EQ(origin.host(), host);
    }
    const auto rooted_name = maximal + ".";
    const auto rooted = decide("GET", "/", rooted_name, secured);
    RUVIA_CHECK(rooted.action() == http_target_origin_action::verify_certificate);
    RUVIA_CHECK_EQ(rooted.host(), std::string_view(maximal));

    for (const auto& host : {maximal + "d", std::string(64, 'a') + ".example"}) {
        RUVIA_CHECK(decide("GET", "/", host, secured).action() == http_target_origin_action::misdirected);
    }
}

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

    // RFC 3986 §3.2.2 / §6.2.2.2 and RFC 9110 §4.2.3: percent-encoded
    // unreserved octets in reg-name are decoded before host classification.
    const auto encoded_dns = decide("GET", "/x", "%6Cocalhost%2E:8443", secured);
    RUVIA_CHECK(encoded_dns.action() == http_target_origin_action::verify_certificate);
    RUVIA_CHECK(encoded_dns.host_kind() == http_target_host_kind::dns_name);
    RUVIA_CHECK(encoded_dns.host() == "localhost");

    const auto encoded_abs = decide("GET", "https://%6Cocalhost/x", "localhost", secured);
    RUVIA_CHECK(encoded_abs.action() == http_target_origin_action::verify_certificate);
    RUVIA_CHECK(encoded_abs.host_kind() == http_target_host_kind::dns_name);
    RUVIA_CHECK(encoded_abs.host() == "localhost");

    const auto encoded_ipv4 = decide("GET", "/x", "192.0.2.%37:443", secured);
    RUVIA_CHECK(encoded_ipv4.action() == http_target_origin_action::verify_certificate);
    RUVIA_CHECK(encoded_ipv4.host_kind() == http_target_host_kind::ipv4);
    RUVIA_CHECK(std::ranges::equal(encoded_ipv4.address(), std::array<std::uint8_t, 4>{192, 0, 2, 7}));

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

    // RFC 1034/1123 and RFC 9525 §6.2: leading dots, wildcards, multi-dot
    // suffixes, invalid LDH labels, and trailing-dot IPv4 literals are not
    // valid DNS reference identifiers.
    RUVIA_CHECK(decide("GET", "/x", ".example.com", secured).action() == http_target_origin_action::misdirected);
    RUVIA_CHECK(decide("GET", "/x", "%2Eexample.com", secured).action() == http_target_origin_action::misdirected);
    RUVIA_CHECK(decide("GET", "/x", "*.wild.test", secured).action() == http_target_origin_action::misdirected);
    RUVIA_CHECK(decide("GET", "/x", "Example.COM..", secured).action() == http_target_origin_action::misdirected);
    RUVIA_CHECK(decide("GET", "/x", "example..com", secured).action() == http_target_origin_action::misdirected);
    RUVIA_CHECK(decide("GET", "/x", "-example.com", secured).action() == http_target_origin_action::misdirected);
    RUVIA_CHECK(decide("GET", "/x", "example-.com", secured).action() == http_target_origin_action::misdirected);
    RUVIA_CHECK(decide("GET", "/x", "192.0.2.7.", secured).action() == http_target_origin_action::misdirected);
    RUVIA_CHECK(decide("GET", "/x", "192.0.2.7%2E", secured).action() == http_target_origin_action::misdirected);
}
