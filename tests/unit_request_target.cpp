#include "test_harness.h"

#include <string_view>

#include "parser/HttpRequestTarget.h"
#include "ruvia/http/HttpTypes.h"

namespace {

using ruvia::HttpMethod;
using ruvia::detail::RequestTargetView;
using ruvia::detail::authorityMatchesHost;
using ruvia::detail::isValidHostHeader;
using ruvia::detail::isValidOriginFormTarget;
using ruvia::detail::isValidRequestTargetBytes;
using ruvia::detail::parseRequestTarget;

}  // namespace

RUVIA_TEST(host_header_accepts_valid) {
    RUVIA_CHECK(isValidHostHeader("example.com"));
    RUVIA_CHECK(isValidHostHeader("example.com:8080"));
    RUVIA_CHECK(isValidHostHeader("localhost"));
    RUVIA_CHECK(isValidHostHeader("sub.domain.example.com"));
    RUVIA_CHECK(isValidHostHeader("192.0.2.1"));
    RUVIA_CHECK(isValidHostHeader("192.0.2.1:443"));
    RUVIA_CHECK(isValidHostHeader("example.com:65535"));   // the maximum valid port is inclusive
    RUVIA_CHECK(isValidHostHeader("[::1]"));
    RUVIA_CHECK(isValidHostHeader("[::1]:8080"));
    RUVIA_CHECK(isValidHostHeader("[2001:db8::1]"));
}

RUVIA_TEST(host_header_rejects_invalid) {
    RUVIA_CHECK(!isValidHostHeader(""));
    RUVIA_CHECK(!isValidHostHeader("example.com:"));        // empty port
    RUVIA_CHECK(!isValidHostHeader("example.com:65536"));   // one past the maximum port
    RUVIA_CHECK(!isValidHostHeader("example.com:70000"));   // port > 65535
    RUVIA_CHECK(!isValidHostHeader("example.com:8o80"));    // non-digit in port
    RUVIA_CHECK(!isValidHostHeader("example.com:80:80"));   // trailing junk after port
    RUVIA_CHECK(!isValidHostHeader("exa mple.com"));        // space is not a reg-name char
    RUVIA_CHECK(!isValidHostHeader("[::1"));                // unclosed IPv6 bracket
    RUVIA_CHECK(!isValidHostHeader("[]"));                  // empty bracket
    RUVIA_CHECK(!isValidHostHeader("[::1]x"));              // junk after the bracket
    RUVIA_CHECK(!isValidHostHeader("[GG::1]"));             // non-hex byte in the IPv6 literal
    RUVIA_CHECK(!isValidHostHeader("[::::]"));              // not a valid IPv6 literal
    // A zone/scope id is valid to getaddrinfo but not a legal URI host: it must
    // be rejected so a scoped address can never slip into host matching.
    RUVIA_CHECK(!isValidHostHeader("[fe80::1%eth0]"));
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

RUVIA_TEST(parse_request_target_absolute_form) {
    RequestTargetView out;

    // Absolute-form (proxy/smuggling surface): authority is split off and the
    // path/query recovered, with the scheme fixing the default port.
    RUVIA_CHECK(parseRequestTarget(HttpMethod::kGet, "http://example.com/path?q=1", out));
    RUVIA_CHECK_EQ(out.authority, std::string_view("example.com"));
    RUVIA_CHECK_EQ(out.path, std::string_view("/path"));
    RUVIA_CHECK_EQ(out.query, std::string_view("q=1"));
    RUVIA_CHECK_EQ(out.defaultPort, std::uint16_t{80});

    // No path component defaults the path to "/".
    RUVIA_CHECK(parseRequestTarget(HttpMethod::kGet, "http://example.com", out));
    RUVIA_CHECK_EQ(out.authority, std::string_view("example.com"));
    RUVIA_CHECK_EQ(out.path, std::string_view("/"));
    RUVIA_CHECK_EQ(out.query, std::string_view(""));

    // A query immediately after the authority still yields path "/".
    RUVIA_CHECK(parseRequestTarget(HttpMethod::kGet, "http://example.com?q=1", out));
    RUVIA_CHECK_EQ(out.path, std::string_view("/"));
    RUVIA_CHECK_EQ(out.query, std::string_view("q=1"));

    // https fixes the default port to 443; an explicit port is kept in the authority.
    RUVIA_CHECK(parseRequestTarget(HttpMethod::kGet, "https://example.com:8080/x", out));
    RUVIA_CHECK_EQ(out.authority, std::string_view("example.com:8080"));
    RUVIA_CHECK_EQ(out.path, std::string_view("/x"));
    RUVIA_CHECK_EQ(out.defaultPort, std::uint16_t{443});

    // Unknown scheme, empty authority, and an invalid authority are all rejected.
    RUVIA_CHECK(!parseRequestTarget(HttpMethod::kGet, "ftp://example.com/x", out));
    RUVIA_CHECK(!parseRequestTarget(HttpMethod::kGet, "http://", out));
    RUVIA_CHECK(!parseRequestTarget(HttpMethod::kGet, "http://exa@mple.com/x", out));
}

RUVIA_TEST(parse_request_target_connect_authority_form) {
    RequestTargetView out;

    RUVIA_CHECK(parseRequestTarget(HttpMethod::kConnect, "example.com:443", out));
    RUVIA_CHECK_EQ(out.authority, std::string_view("example.com:443"));
    RUVIA_CHECK_EQ(out.path, std::string_view("example.com:443"));
    RUVIA_CHECK_EQ(out.query, std::string_view(""));

    RUVIA_CHECK(parseRequestTarget(HttpMethod::kConnect, "[::1]:8443", out));
    RUVIA_CHECK_EQ(out.authority, std::string_view("[::1]:8443"));

    RUVIA_CHECK(!parseRequestTarget(HttpMethod::kConnect, "/", out));
    RUVIA_CHECK(!parseRequestTarget(HttpMethod::kConnect, "/tunnel", out));
    RUVIA_CHECK(!parseRequestTarget(HttpMethod::kConnect, "example.com", out));
    RUVIA_CHECK(!parseRequestTarget(HttpMethod::kConnect, "http://example.com:443", out));
    RUVIA_CHECK(!parseRequestTarget(HttpMethod::kConnect, "*", out));
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
    RUVIA_CHECK(!parseRequestTarget(HttpMethod::kGet, "/bad\\path", out));   // backslash
    RUVIA_CHECK(!parseRequestTarget(HttpMethod::kGet, std::string_view("/a\r\nb", 5), out));  // CRLF
    RUVIA_CHECK(!parseRequestTarget(HttpMethod::kGet, "/bad%zz", out));      // malformed pct-encoded
    RUVIA_CHECK(!parseRequestTarget(HttpMethod::kGet, "/bad%", out));        // truncated pct-encoded
    RUVIA_CHECK(!parseRequestTarget(HttpMethod::kGet, "/bad%2", out));       // truncated pct-encoded
    RUVIA_CHECK(parseRequestTarget(HttpMethod::kGet, "/ok%2F?q=%7B%7D", out));
    RUVIA_CHECK_EQ(out.path, std::string_view("/ok%2F"));
    RUVIA_CHECK_EQ(out.query, std::string_view("q=%7B%7D"));
}

RUVIA_TEST(request_target_bytes_reject_smuggling_and_control_chars) {
    // The low-level byte validator underpins both the HTTP/1 request target and the
    // HTTP/2 :path. It is a request-smuggling / path-injection defense, so anything
    // that could confuse downstream parsing or split the target must be rejected.
    RUVIA_CHECK(isValidRequestTargetBytes("/index.html"));
    RUVIA_CHECK(isValidRequestTargetBytes("/search?q=a+b&x=1"));
    RUVIA_CHECK(isValidRequestTargetBytes("/a/b/c"));

    RUVIA_CHECK(!isValidRequestTargetBytes(""));              // empty is never a valid target
    RUVIA_CHECK(!isValidRequestTargetBytes("/a b"));          // raw space (0x20) splits the request line
    RUVIA_CHECK(!isValidRequestTargetBytes("/a\tb"));         // HTAB is a control char
    RUVIA_CHECK(!isValidRequestTargetBytes("/a\rb"));         // CR -- header/line injection
    RUVIA_CHECK(!isValidRequestTargetBytes("/a\nb"));         // LF -- request smuggling
    RUVIA_CHECK(!isValidRequestTargetBytes(std::string_view("/a\0b", 4)));  // NUL
    RUVIA_CHECK(!isValidRequestTargetBytes("/a\x7f" "b"));    // DEL (0x7F); split literal so 'b' is not eaten by the hex escape
    RUVIA_CHECK(!isValidRequestTargetBytes("/page#frag"));    // '#' -- fragment must not reach the origin
    RUVIA_CHECK(!isValidRequestTargetBytes("/a\\b"));         // backslash -- path-normalization confusion
}

RUVIA_TEST(request_target_bytes_validate_percent_encoding) {
    // A '%' must be followed by exactly two hex digits, else it is malformed.
    RUVIA_CHECK(isValidRequestTargetBytes("/%2Fpath"));       // %2F is well-formed
    RUVIA_CHECK(isValidRequestTargetBytes("/a%20b"));         // encoded space is fine (raw is not)
    RUVIA_CHECK(isValidRequestTargetBytes("/%ff"));           // lowercase hex accepted

    RUVIA_CHECK(!isValidRequestTargetBytes("/%"));            // truncated at string end
    RUVIA_CHECK(!isValidRequestTargetBytes("/%2"));           // only one hex digit
    RUVIA_CHECK(!isValidRequestTargetBytes("/%2G"));          // second digit not hex
    RUVIA_CHECK(!isValidRequestTargetBytes("/%g0"));          // first digit not hex
    RUVIA_CHECK(!isValidRequestTargetBytes("/a%"));           // trailing bare '%'
}

RUVIA_TEST(origin_form_target_shape) {
    // Origin-form must start with '/'; the sole exception is the asterisk-form ("*",
    // used by OPTIONS) which is valid on its own but not as a prefix.
    RUVIA_CHECK(isValidOriginFormTarget("/"));
    RUVIA_CHECK(isValidOriginFormTarget("/path?q=1"));
    RUVIA_CHECK(isValidOriginFormTarget("*"));

    RUVIA_CHECK(!isValidOriginFormTarget(""));                // empty
    RUVIA_CHECK(!isValidOriginFormTarget("path"));            // missing leading '/'
    RUVIA_CHECK(!isValidOriginFormTarget("http://x/y"));      // absolute-form is not origin-form
    RUVIA_CHECK(!isValidOriginFormTarget("*/"));              // '*' is valid only as the whole target
    RUVIA_CHECK(!isValidOriginFormTarget("/bad path"));       // inherits byte validation (raw space)
    RUVIA_CHECK(!isValidOriginFormTarget("/x#y"));            // inherits fragment rejection
}
