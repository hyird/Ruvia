#include <gtest/gtest.h>

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

template <typename origin_type>
concept target_origin_allows_host = requires(origin_type&& origin) {
    std::forward<origin_type>(origin).host();
};

template <typename origin_type>
concept target_origin_allows_address = requires(origin_type&& origin) {
    std::forward<origin_type>(origin).address();
};

static_assert(target_origin_allows_host<const http_target_origin&>);
static_assert(!target_origin_allows_host<http_target_origin>);
static_assert(target_origin_allows_address<const http_target_origin&>);
static_assert(!target_origin_allows_address<http_target_origin>);
}  // namespace

TEST(Ruvia, target_origin_dns_reference_names_follow_label_and_full_name_boundaries) {
    const auto maximal = std::string(63, 'a') + "." + std::string(63, 'b') + "." +
                         std::string(63, 'c') + "." + std::string(61, 'd');
    for (const std::string_view host : {std::string_view(maximal), std::string_view("7.example"),
             std::string_view("xn--bcher-kva.example")}) {
        const auto origin = decide("GET", "/", host, secured);
        EXPECT_TRUE(origin.action() == http_target_origin_action::verify_certificate);
        EXPECT_TRUE(origin.host_kind() == http_target_host_kind::dns_name);
        EXPECT_EQ(origin.host(), host);
    }
    const auto rooted_name = maximal + ".";
    const auto rooted = decide("GET", "/", rooted_name, secured);
    EXPECT_TRUE(rooted.action() == http_target_origin_action::verify_certificate);
    EXPECT_EQ(rooted.host(), std::string_view(maximal));

    for (const auto& host : {maximal + "d", std::string(64, 'a') + ".example"}) {
        EXPECT_TRUE(decide("GET", "/", host, secured).action() == http_target_origin_action::misdirected);
    }
}

// RFC 9110 §7.4: an "https" target needs a secured connection whose
// certificate covers the target host; "http" targets and tunnel destinations
// impose nothing, and a trusted gateway vouches for the target itself.
TEST(Ruvia, target_origin_decision_follows_scheme_transport_and_gateway_trust) {
    EXPECT_TRUE(decide("GET", "https://a.example/x", "x", cleartext).action() == http_target_origin_action::misdirected);
    EXPECT_TRUE(decide("GET", "HTTPS://a.example/x", "x", cleartext).action() == http_target_origin_action::misdirected);
    EXPECT_TRUE(decide("GET", "https://a.example/x", "x", {.trusted_gateway_ = true}).action() == http_target_origin_action::accept);
    EXPECT_TRUE(decide("GET", "http://a.example/x", "x", cleartext).action() == http_target_origin_action::accept);
    EXPECT_TRUE(decide("GET", "/x", "a.example", cleartext).action() == http_target_origin_action::accept);
    EXPECT_TRUE(decide("GET", "http://a.example/x", "x", secured).action() == http_target_origin_action::accept);
    EXPECT_TRUE(decide("CONNECT", "a.example:443", "a.example:443", secured).action() == http_target_origin_action::accept);
    // RFC 9112 §3.3: an empty authority takes the configured default.
    EXPECT_TRUE(decide("GET", "/x", "", secured).action() == http_target_origin_action::accept);

    const auto name = decide("GET", "/x", "Example.COM.:8443", secured);
    EXPECT_TRUE(name.action() == http_target_origin_action::verify_certificate);
    EXPECT_TRUE(name.host_kind() == http_target_host_kind::dns_name);
    EXPECT_TRUE(name.host() == "Example.COM");
    EXPECT_TRUE(name.address().empty());

    // The absolute-form authority, not a conflicting Host, is the target.
    const auto absolute = decide("GET", "https://b.example/x", "a.example", secured);
    EXPECT_TRUE(absolute.action() == http_target_origin_action::verify_certificate);
    EXPECT_TRUE(absolute.host() == "b.example");

    const auto asterisk = decide("OPTIONS", "*", "a.example", secured);
    EXPECT_TRUE(asterisk.action() == http_target_origin_action::verify_certificate);
    EXPECT_TRUE(asterisk.host() == "a.example");

    // RFC 3986 §3.2.2 / §6.2.2.2 and RFC 9110 §4.2.3: percent-encoded
    // unreserved octets in reg-name are decoded before host classification.
    const auto encoded_dns = decide("GET", "/x", "%6Cocalhost%2E:8443", secured);
    EXPECT_TRUE(encoded_dns.action() == http_target_origin_action::verify_certificate);
    EXPECT_TRUE(encoded_dns.host_kind() == http_target_host_kind::dns_name);
    EXPECT_TRUE(encoded_dns.host() == "localhost");

    const auto encoded_abs = decide("GET", "https://%6Cocalhost/x", "localhost", secured);
    EXPECT_TRUE(encoded_abs.action() == http_target_origin_action::verify_certificate);
    EXPECT_TRUE(encoded_abs.host_kind() == http_target_host_kind::dns_name);
    EXPECT_TRUE(encoded_abs.host() == "localhost");

    const auto encoded_ipv4 = decide("GET", "/x", "192.0.2.%37:443", secured);
    EXPECT_TRUE(encoded_ipv4.action() == http_target_origin_action::verify_certificate);
    EXPECT_TRUE(encoded_ipv4.host_kind() == http_target_host_kind::ipv4);
    EXPECT_TRUE(std::ranges::equal(encoded_ipv4.address(), std::array<std::uint8_t, 4>{192, 0, 2, 7}));

    const auto ipv4 = decide("GET", "/x", "192.0.2.7:443", secured);
    EXPECT_TRUE(ipv4.action() == http_target_origin_action::verify_certificate);
    EXPECT_TRUE(ipv4.host_kind() == http_target_host_kind::ipv4);
    constexpr std::array<std::uint8_t, 4> ipv4_bytes{192, 0, 2, 7};
    EXPECT_TRUE(std::ranges::equal(ipv4.address(), ipv4_bytes));

    const auto ipv6 = decide("GET", "/x", "[2001:db8::1]", secured);
    EXPECT_TRUE(ipv6.action() == http_target_origin_action::verify_certificate);
    EXPECT_TRUE(ipv6.host_kind() == http_target_host_kind::ipv6);
    constexpr std::array<std::uint8_t, 16> ipv6_bytes{0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
    EXPECT_TRUE(std::ranges::equal(ipv6.address(), ipv6_bytes));
    EXPECT_TRUE(ipv6.host().empty());

    // No certificate identifies an IPvFuture host.
    EXPECT_TRUE(decide("GET", "/x", "[v1.x]", secured).action() == http_target_origin_action::misdirected);

    // RFC 1034/1123 and RFC 9525 §6.2: leading dots, wildcards, multi-dot
    // suffixes, invalid LDH labels, and trailing-dot IPv4 literals are not
    // valid DNS reference identifiers.
    EXPECT_TRUE(decide("GET", "/x", ".example.com", secured).action() == http_target_origin_action::misdirected);
    EXPECT_TRUE(decide("GET", "/x", "%2Eexample.com", secured).action() == http_target_origin_action::misdirected);
    EXPECT_TRUE(decide("GET", "/x", "*.wild.test", secured).action() == http_target_origin_action::misdirected);
    EXPECT_TRUE(decide("GET", "/x", "Example.COM..", secured).action() == http_target_origin_action::misdirected);
    EXPECT_TRUE(decide("GET", "/x", "example..com", secured).action() == http_target_origin_action::misdirected);
    EXPECT_TRUE(decide("GET", "/x", "-example.com", secured).action() == http_target_origin_action::misdirected);
    EXPECT_TRUE(decide("GET", "/x", "example-.com", secured).action() == http_target_origin_action::misdirected);
    EXPECT_TRUE(decide("GET", "/x", "192.0.2.7.", secured).action() == http_target_origin_action::misdirected);
    EXPECT_TRUE(decide("GET", "/x", "192.0.2.7%2E", secured).action() == http_target_origin_action::misdirected);
}
