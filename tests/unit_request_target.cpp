#include "test_harness.h"

#include <string_view>

#include "http/parser/HttpRequestTarget.h"
#include "ruvia/http/HttpTypes.h"

namespace {

using ruvia::HttpMethod;
using ruvia::detail::RequestTargetView;
using ruvia::detail::authorityMatchesHost;
using ruvia::detail::isValidHostHeader;
using ruvia::detail::parseRequestTarget;

}  // namespace

RUVIA_TEST(host_header_accepts_valid) {
    RUVIA_CHECK(isValidHostHeader("example.com"));
    RUVIA_CHECK(isValidHostHeader("example.com:8080"));
    RUVIA_CHECK(isValidHostHeader("localhost"));
    RUVIA_CHECK(isValidHostHeader("sub.domain.example.com"));
    RUVIA_CHECK(isValidHostHeader("192.0.2.1"));
    RUVIA_CHECK(isValidHostHeader("192.0.2.1:443"));
    RUVIA_CHECK(isValidHostHeader("[::1]"));
    RUVIA_CHECK(isValidHostHeader("[::1]:8080"));
    RUVIA_CHECK(isValidHostHeader("[2001:db8::1]"));
}

RUVIA_TEST(host_header_rejects_invalid) {
    RUVIA_CHECK(!isValidHostHeader(""));
    RUVIA_CHECK(!isValidHostHeader("example.com:"));        // empty port
    RUVIA_CHECK(!isValidHostHeader("example.com:70000"));   // port > 65535
    RUVIA_CHECK(!isValidHostHeader("example.com:8o80"));    // non-digit in port
    RUVIA_CHECK(!isValidHostHeader("example.com:80:80"));   // trailing junk after port
    RUVIA_CHECK(!isValidHostHeader("exa mple.com"));        // space is not a reg-name char
    RUVIA_CHECK(!isValidHostHeader("[::1"));                // unclosed IPv6 bracket
    RUVIA_CHECK(!isValidHostHeader("[]"));                  // empty bracket
    RUVIA_CHECK(!isValidHostHeader("[::1]x"));              // junk after the bracket
    RUVIA_CHECK(!isValidHostHeader("[GG::1]"));             // non-hex byte in the IPv6 literal
    // A CRLF-injection attempt must not validate.
    RUVIA_CHECK(!isValidHostHeader(std::string_view("example.com\r\nX", 14)));
}

RUVIA_TEST(authority_matches_host_ports_and_case) {
    RUVIA_CHECK(authorityMatchesHost("example.com", "example.com", 80));
    RUVIA_CHECK(authorityMatchesHost("example.com:80", "example.com", 80));  // explicit == default
    RUVIA_CHECK(authorityMatchesHost("example.com", "example.com:80", 80));
    RUVIA_CHECK(authorityMatchesHost("EXAMPLE.com", "example.COM", 80));     // host is case-insensitive
    RUVIA_CHECK(authorityMatchesHost("[::1]:443", "[::1]", 443));            // IPv6 default port
}

RUVIA_TEST(authority_matches_host_rejects_mismatches) {
    RUVIA_CHECK(!authorityMatchesHost("example.com:8080", "example.com", 80));      // port mismatch vs default
    RUVIA_CHECK(!authorityMatchesHost("example.com", "other.com", 80));            // host mismatch
    RUVIA_CHECK(!authorityMatchesHost("example.com:8080", "example.com:80", 80));  // explicit port mismatch
    RUVIA_CHECK(!authorityMatchesHost("evil.com", "example.com", 80));
}

RUVIA_TEST(parse_request_target_origin_form) {
    RequestTargetView out;
    RUVIA_CHECK(parseRequestTarget(HttpMethod::kGet, "/path?q=1&r=2", out));
    RUVIA_CHECK_EQ(out.path, std::string_view("/path"));
    RUVIA_CHECK_EQ(out.query, std::string_view("q=1&r=2"));

    RUVIA_CHECK(parseRequestTarget(HttpMethod::kGet, "/only/path", out));
    RUVIA_CHECK_EQ(out.path, std::string_view("/only/path"));
    RUVIA_CHECK_EQ(out.query, std::string_view(""));
}

RUVIA_TEST(parse_request_target_asterisk_and_rejections) {
    RequestTargetView out;
    // Asterisk-form is valid only for OPTIONS.
    RUVIA_CHECK(parseRequestTarget(HttpMethod::kOptions, "*", out));
    RUVIA_CHECK_EQ(out.path, std::string_view("*"));
    RUVIA_CHECK(!parseRequestTarget(HttpMethod::kGet, "*", out));
    // Empty, control/whitespace bytes and fragments are rejected.
    RUVIA_CHECK(!parseRequestTarget(HttpMethod::kGet, "", out));
    RUVIA_CHECK(!parseRequestTarget(HttpMethod::kGet, "/pa th", out));       // space
    RUVIA_CHECK(!parseRequestTarget(HttpMethod::kGet, "/path#frag", out));   // '#' fragment
    RUVIA_CHECK(!parseRequestTarget(HttpMethod::kGet, std::string_view("/a\r\nb", 5), out));  // CRLF
}
