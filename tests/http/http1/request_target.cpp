#include "test_harness.h"

#include <cstdint>
#include <memory_resource>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/http/detail/parser/HttpRequestTarget.h"

namespace {

using ruvia::HttpKnownMethod;
using ruvia::detail::authorityMatchesHost;
using ruvia::detail::HttpAuthorityPortKind;
using ruvia::detail::HttpRequestTargetForm;
using ruvia::detail::httpUriHostEquals;
using ruvia::detail::httpUriSchemeDefaultPort;
using ruvia::detail::isValidHostHeader;
using ruvia::detail::isValidHttpHost;
using ruvia::detail::isValidOriginFormTarget;
using ruvia::detail::isValidOriginOrAsteriskFormTarget;
using ruvia::detail::isValidRequestTargetBytes;
using ruvia::detail::isValidUriAuthority;
using ruvia::detail::isValidUriScheme;
using ruvia::detail::parseHttpAuthority;
using ruvia::detail::parseRequestTarget;
using ruvia::detail::RequestTargetView;

template <typename Input>
concept AcceptsTemporaryRequestTargetInput = requires(Input&& input, RequestTargetView& output) {
    parseRequestTarget(HttpKnownMethod::kGet, std::forward<Input>(input), output);
};

static_assert(!AcceptsTemporaryRequestTargetInput<std::string>);
static_assert(!AcceptsTemporaryRequestTargetInput<const std::string>);
static_assert(!AcceptsTemporaryRequestTargetInput<std::pmr::string>);
static_assert(AcceptsTemporaryRequestTargetInput<std::string&>);
static_assert(AcceptsTemporaryRequestTargetInput<std::pmr::string&>);
static_assert(AcceptsTemporaryRequestTargetInput<std::string_view>);

}  // namespace

RUVIA_TEST(request_target_parsers_handle_deterministic_arbitrary_bytes) {
    std::uint64_t state = 0x5441'5247'4554'4655ULL;
    const auto next = [&state]() {
        state ^= state << 7U;
        state ^= state >> 9U;
        return state;
    };

    for (std::size_t sample = 0; sample < 4096; ++sample) {
        std::string input(static_cast<std::size_t>(next() % 513U), '\0');
        for (auto& byte : input) {
            byte = static_cast<char>(next());
        }

        const auto method = sample % 3 == 0 ? HttpKnownMethod::kGet
                          : sample % 3 == 1 ? HttpKnownMethod::kOptions
                                            : HttpKnownMethod::kConnect;
        RequestTargetView target;
        const auto accepted = parseRequestTarget(method, input, target);
        const auto authority = parseHttpAuthority(input);
        RUVIA_CHECK_EQ(isValidHostHeader(input), input.empty() || authority.has_value());

        if (authority.has_value()) {
            RUVIA_CHECK(!authority->host().empty());
            RUVIA_CHECK(input.find(authority->host()) != std::string::npos);
            RUVIA_CHECK_EQ(authority->port().has_value(),
                authority->portKind() == HttpAuthorityPortKind::kValue);
        }
        if (!accepted) {
            continue;
        }

        RUVIA_CHECK(target.query.empty() || input.find(target.query) != std::string::npos);
        RUVIA_CHECK(
            target.authority.empty() || input.find(target.authority) != std::string::npos);
        switch (target.form) {
            case HttpRequestTargetForm::kOrigin:
                RUVIA_CHECK(target.scheme.empty());
                RUVIA_CHECK(target.authority.empty());
                RUVIA_CHECK(target.path.starts_with('/'));
                break;
            case HttpRequestTargetForm::kAbsolute:
                RUVIA_CHECK(!target.scheme.empty());
                RUVIA_CHECK(input.starts_with(target.scheme));
                RUVIA_CHECK_EQ(target.defaultPort, httpUriSchemeDefaultPort(target.scheme));
                RUVIA_CHECK(target.path.empty() || target.path == "/" || target.path == "*" ||
                            input.find(target.path) != std::string::npos);
                break;
            case HttpRequestTargetForm::kAuthority:
                RUVIA_CHECK(method == HttpKnownMethod::kConnect);
                RUVIA_CHECK_EQ(target.path, std::string_view(input));
                RUVIA_CHECK_EQ(target.authority, std::string_view(input));
                break;
            case HttpRequestTargetForm::kAsterisk:
                RUVIA_CHECK(method == HttpKnownMethod::kOptions);
                RUVIA_CHECK_EQ(input, std::string("*"));
                RUVIA_CHECK_EQ(target.path, std::string_view("*"));
                break;
        }
    }
}

RUVIA_TEST(uri_scheme_uses_complete_rfc3986_grammar) {
    RUVIA_CHECK(isValidUriScheme("http"));
    RUVIA_CHECK(isValidUriScheme("HTTPS"));
    RUVIA_CHECK(isValidUriScheme("ftp"));
    RUVIA_CHECK(isValidUriScheme("git+ssh"));
    RUVIA_CHECK(isValidUriScheme("x-1.example"));
    RUVIA_CHECK(!isValidUriScheme(""));
    RUVIA_CHECK(!isValidUriScheme("1http"));
    RUVIA_CHECK(!isValidUriScheme("bad scheme"));
    RUVIA_CHECK(!isValidUriScheme("https:"));
    RUVIA_CHECK(!isValidUriScheme("https/other"));

    RUVIA_CHECK_EQ(httpUriSchemeDefaultPort("HTTP"), std::uint16_t{80});
    RUVIA_CHECK_EQ(httpUriSchemeDefaultPort("hTtPs"), std::uint16_t{443});
    RUVIA_CHECK_EQ(httpUriSchemeDefaultPort("ftp"), std::uint16_t{0});
}

RUVIA_TEST(uri_authority_uses_complete_rfc3986_generic_grammar) {
    RUVIA_CHECK(isValidUriAuthority(""));
    RUVIA_CHECK(isValidUriAuthority("example.com"));
    RUVIA_CHECK(isValidUriAuthority("user@example.com"));
    RUVIA_CHECK(isValidUriAuthority("user:secret@example.com:9418"));
    RUVIA_CHECK(isValidUriAuthority("name%3Avalue@example.com"));
    RUVIA_CHECK(isValidUriAuthority("@"));
    RUVIA_CHECK(isValidUriAuthority(":70000"));
    RUVIA_CHECK(isValidUriAuthority("[v1.future]:99999999999999999999"));

    RUVIA_CHECK(!isValidUriAuthority("user@@example.com"));
    RUVIA_CHECK(!isValidUriAuthority("bad%2@example.com"));
    RUVIA_CHECK(!isValidUriAuthority("bad user@example.com"));
    RUVIA_CHECK(!isValidUriAuthority("user@example.com:port"));
    RUVIA_CHECK(!isValidUriAuthority("user@[::1"));
    RUVIA_CHECK(!isValidUriAuthority("user@example.com/path"));
}

RUVIA_TEST(host_header_accepts_valid) {
    // RFC 9112 section 3.2 permits an empty Host field when the target URI has
    // no authority component.
    RUVIA_CHECK(isValidHostHeader(""));
    RUVIA_CHECK(isValidHostHeader("example.com"));
    RUVIA_CHECK(isValidHostHeader("example.com:8080"));
    RUVIA_CHECK(isValidHostHeader("localhost"));
    RUVIA_CHECK(isValidHostHeader("sub.domain.example.com"));
    RUVIA_CHECK(isValidHostHeader("192.0.2.1"));
    RUVIA_CHECK(isValidHostHeader("192.0.2.1:443"));
    RUVIA_CHECK(isValidHostHeader("example.com:65535"));  // the maximum valid port is inclusive
    RUVIA_CHECK(isValidHostHeader("example.com:0"));
    // RFC 3986 defines port as *DIGIT; RFC 9110 treats an empty HTTP port as
    // the scheme default rather than an invalid authority.
    RUVIA_CHECK(isValidHostHeader("example.com:"));
    RUVIA_CHECK(isValidHostHeader("[::1]"));
    RUVIA_CHECK(isValidHostHeader("[::1]:8080"));
    RUVIA_CHECK(isValidHostHeader("[::1]:"));
    RUVIA_CHECK(isValidHostHeader("[2001:db8::1]"));
    RUVIA_CHECK(isValidHostHeader("[::ffff:192.0.2.128]"));
    RUVIA_CHECK(isValidHostHeader("[2001:db8:0:0:0:0:192.0.2.1]"));
    RUVIA_CHECK(isValidHostHeader("[v1.future]"));
    RUVIA_CHECK(isValidHostHeader("[vF.a:b!c]:443"));
}

RUVIA_TEST(host_header_rejects_invalid) {
    RUVIA_CHECK(!isValidHostHeader("example.com:65536"));    // one past the maximum port
    RUVIA_CHECK(!isValidHostHeader("example.com:70000"));    // port > 65535
    RUVIA_CHECK(!isValidHostHeader("example.com:8o80"));     // non-digit in port
    RUVIA_CHECK(!isValidHostHeader("example.com:80:80"));    // trailing junk after port
    RUVIA_CHECK(!isValidHostHeader("exa mple.com"));         // space is not a reg-name char
    RUVIA_CHECK(!isValidHostHeader("[::1"));                 // unclosed IPv6 bracket
    RUVIA_CHECK(!isValidHostHeader("[]"));                   // empty bracket
    RUVIA_CHECK(!isValidHostHeader("[::1]x"));               // junk after the bracket
    RUVIA_CHECK(!isValidHostHeader("[GG::1]"));              // non-hex byte in the IPv6 literal
    RUVIA_CHECK(!isValidHostHeader("[::::]"));               // not a valid IPv6 literal
    RUVIA_CHECK(!isValidHostHeader("[1:2:3:4:5:6:7:8:9]"));  // too many 16-bit groups
    RUVIA_CHECK(!isValidHostHeader("[::ffff:999.0.2.128]"));
    RUVIA_CHECK(
        !isValidHostHeader("[::ffff:192.168.001.1]"));  // IPv4 dec-octet has no leading zero
    RUVIA_CHECK(!isValidHostHeader("[v.future]"));      // missing version hex digit
    RUVIA_CHECK(!isValidHostHeader("[v1.]"));           // empty future address
    RUVIA_CHECK(!isValidHostHeader("[v1.a%20b]"));      // pct-encoding is not IPvFuture grammar
    // A zone/scope id is valid to getaddrinfo but not a legal URI host: it must
    // be rejected so a scoped address can never slip into host matching.
    RUVIA_CHECK(!isValidHostHeader("[fe80::1%eth0]"));
    // A CRLF-injection attempt must not validate.
    RUVIA_CHECK(!isValidHostHeader(std::string_view("example.com\r\nX", 14)));
}

RUVIA_TEST(http_host_component_reuses_reg_name_and_ipv6_validation) {
    RUVIA_CHECK(isValidHttpHost("example.com"));
    RUVIA_CHECK(isValidHttpHost("192.0.2.1"));
    RUVIA_CHECK(isValidHttpHost("[::1]"));
    RUVIA_CHECK(isValidHttpHost("[2001:db8::1]"));
    RUVIA_CHECK(isValidHttpHost("[v1.future]"));
    RUVIA_CHECK(!isValidHttpHost("::1"));
    RUVIA_CHECK(!isValidHttpHost("2001:db8::1"));
    RUVIA_CHECK(!isValidHttpHost("example.com:443"));
    RUVIA_CHECK(!isValidHttpHost("[::::]"));
    RUVIA_CHECK(!isValidHttpHost("bad?host"));
}

RUVIA_TEST(authority_parser_preserves_absent_empty_and_numeric_ports) {
    const auto absent = parseHttpAuthority("example.com");
    RUVIA_CHECK(absent.has_value());
    RUVIA_CHECK(absent->portKind() == HttpAuthorityPortKind::kAbsent);
    RUVIA_CHECK(!absent->port().has_value());
    RUVIA_CHECK_EQ(absent->effectivePort(80), std::uint16_t{80});

    const auto empty = parseHttpAuthority("example.com:");
    RUVIA_CHECK(empty.has_value());
    RUVIA_CHECK(empty->portKind() == HttpAuthorityPortKind::kEmpty);
    RUVIA_CHECK(!empty->port().has_value());
    RUVIA_CHECK_EQ(empty->effectivePort(80), std::uint16_t{80});

    const auto zero = parseHttpAuthority("example.com:00000");
    RUVIA_CHECK(zero.has_value());
    RUVIA_CHECK(zero->portKind() == HttpAuthorityPortKind::kValue);
    RUVIA_CHECK_EQ(*zero->port(), std::uint16_t{0});
    RUVIA_CHECK_EQ(zero->effectivePort(80), std::uint16_t{0});
}

RUVIA_TEST(uri_host_comparison_normalizes_only_percent_encoded_unreserved_octets) {
    RUVIA_CHECK(httpUriHostEquals("EXAMPLE.com", "example.COM"));
    RUVIA_CHECK(httpUriHostEquals("exa%6Dple.com", "example.com"));
    RUVIA_CHECK(httpUriHostEquals("%65xample.com", "example.com"));
    RUVIA_CHECK(httpUriHostEquals("%2f.example", "%2F.example"));
    RUVIA_CHECK(httpUriHostEquals("[Vf.A:B]", "[vf.a:b]"));
    RUVIA_CHECK(!httpUriHostEquals("%21example", "!example"));
    RUVIA_CHECK(!httpUriHostEquals("example.com", "other.example"));
}

RUVIA_TEST(authority_matches_host_ports_and_case) {
    RUVIA_CHECK(authorityMatchesHost("example.com", "example.com", 80));
    RUVIA_CHECK(authorityMatchesHost("example.com:80", "example.com", 80));  // explicit == default
    RUVIA_CHECK(authorityMatchesHost("example.com", "example.com:80", 80));
    RUVIA_CHECK(authorityMatchesHost("example.com:", "example.com", 80));
    RUVIA_CHECK(authorityMatchesHost("exa%6dple.com", "example.com:", 80));
    RUVIA_CHECK(
        authorityMatchesHost("EXAMPLE.com", "example.COM", 80));   // host is case-insensitive
    RUVIA_CHECK(authorityMatchesHost("[::1]:443", "[::1]", 443));  // IPv6 default port
    RUVIA_CHECK(authorityMatchesHost("[v1.future]:", "[V1.FUTURE]", 80));
    RUVIA_CHECK(authorityMatchesHost("example.com", "example.com:", 0));
    RUVIA_CHECK(authorityMatchesHost("example.com:21", "example.com:21", 0));
}

RUVIA_TEST(authority_matches_host_rejects_mismatches) {
    RUVIA_CHECK(
        !authorityMatchesHost("example.com:8080", "example.com", 80));   // port mismatch vs default
    RUVIA_CHECK(!authorityMatchesHost("example.com", "other.com", 80));  // host mismatch
    RUVIA_CHECK(
        !authorityMatchesHost("example.com:8080", "example.com:80", 80));  // explicit port mismatch
    RUVIA_CHECK(!authorityMatchesHost("evil.com", "example.com", 80));
    RUVIA_CHECK(!authorityMatchesHost("example.com", "example.com:0", 0));
    RUVIA_CHECK(!authorityMatchesHost("example.com", "example.com:21", 0));
    RUVIA_CHECK(!authorityMatchesHost("example.com:21", "example.com", 0));
}

RUVIA_TEST(parse_request_target_origin_form) {
    RequestTargetView out;
    RUVIA_CHECK(parseRequestTarget(HttpKnownMethod::kGet, "/path?q=1&r=2", out));
    RUVIA_CHECK_EQ(out.path, std::string_view("/path"));
    RUVIA_CHECK_EQ(out.query, std::string_view("q=1&r=2"));
    RUVIA_CHECK(out.form == HttpRequestTargetForm::kOrigin);

    RUVIA_CHECK(parseRequestTarget(HttpKnownMethod::kGet, "/only/path", out));
    RUVIA_CHECK_EQ(out.path, std::string_view("/only/path"));
    RUVIA_CHECK_EQ(out.query, std::string_view(""));
}

RUVIA_TEST(parse_request_target_absolute_form) {
    RequestTargetView out;

    // Absolute-form (proxy/smuggling surface): authority is split off and the
    // path/query recovered, with the scheme fixing the default port.
    RUVIA_CHECK(parseRequestTarget(HttpKnownMethod::kGet, "http://example.com/path?q=1", out));
    RUVIA_CHECK_EQ(out.authority, std::string_view("example.com"));
    RUVIA_CHECK_EQ(out.path, std::string_view("/path"));
    RUVIA_CHECK_EQ(out.query, std::string_view("q=1"));
    RUVIA_CHECK_EQ(out.defaultPort, std::uint16_t{80});
    RUVIA_CHECK(out.form == HttpRequestTargetForm::kAbsolute);

    // No path component defaults the path to "/".
    RUVIA_CHECK(parseRequestTarget(HttpKnownMethod::kGet, "http://example.com", out));
    RUVIA_CHECK_EQ(out.authority, std::string_view("example.com"));
    RUVIA_CHECK_EQ(out.path, std::string_view("/"));
    RUVIA_CHECK_EQ(out.query, std::string_view(""));

    // A query immediately after the authority still yields path "/".
    RUVIA_CHECK(parseRequestTarget(HttpKnownMethod::kGet, "http://example.com?q=1", out));
    RUVIA_CHECK_EQ(out.path, std::string_view("/"));
    RUVIA_CHECK_EQ(out.query, std::string_view("q=1"));

    // https fixes the default port to 443; an explicit port is kept in the authority.
    RUVIA_CHECK(parseRequestTarget(HttpKnownMethod::kGet, "https://example.com:8080/x", out));
    RUVIA_CHECK_EQ(out.authority, std::string_view("example.com:8080"));
    RUVIA_CHECK_EQ(out.path, std::string_view("/x"));
    RUVIA_CHECK_EQ(out.defaultPort, std::uint16_t{443});

    RUVIA_CHECK(parseRequestTarget(HttpKnownMethod::kGet, "http://example.com:/x", out));
    RUVIA_CHECK_EQ(out.authority, std::string_view("example.com:"));
    RUVIA_CHECK(parseRequestTarget(HttpKnownMethod::kGet, "http://[v1.future]/x", out));
    RUVIA_CHECK_EQ(out.authority, std::string_view("[v1.future]"));

    // Absolute-form is the complete RFC 3986 absolute-URI grammar, not only
    // HTTP(S). Unknown schemes retain an unknown default port.
    RUVIA_CHECK(parseRequestTarget(HttpKnownMethod::kGet, "ftp://example.com/pub/archive", out));
    RUVIA_CHECK_EQ(out.path, std::string_view("/pub/archive"));
    RUVIA_CHECK_EQ(out.authority, std::string_view("example.com"));
    RUVIA_CHECK_EQ(out.defaultPort, std::uint16_t{0});
    RUVIA_CHECK(out.form == HttpRequestTargetForm::kAbsolute);

    RUVIA_CHECK(parseRequestTarget(
        HttpKnownMethod::kGet, "custom://user:secret@example.com/resource", out));
    RUVIA_CHECK_EQ(out.path, std::string_view("/resource"));
    // RFC 9112 section 3.2 excludes userinfo from the effective Host value.
    RUVIA_CHECK_EQ(out.authority, std::string_view("example.com"));

    RUVIA_CHECK(parseRequestTarget(HttpKnownMethod::kGet, "urn:example:animal:ferret:nose", out));
    RUVIA_CHECK_EQ(out.path, std::string_view("example:animal:ferret:nose"));
    RUVIA_CHECK(out.query.empty());
    RUVIA_CHECK(out.authority.empty());
    RUVIA_CHECK_EQ(out.defaultPort, std::uint16_t{0});
    RUVIA_CHECK(out.form == HttpRequestTargetForm::kAbsolute);

    RUVIA_CHECK(parseRequestTarget(HttpKnownMethod::kGet, "file:///etc/hosts", out));
    RUVIA_CHECK_EQ(out.path, std::string_view("/etc/hosts"));
    RUVIA_CHECK(out.authority.empty());

    RUVIA_CHECK(parseRequestTarget(HttpKnownMethod::kGet, "custom:?name=value", out));
    RUVIA_CHECK(out.path.empty());
    RUVIA_CHECK_EQ(out.query, std::string_view("name=value"));
    RUVIA_CHECK(out.authority.empty());

    // Generic absolute-URI syntax does not make malformed HTTP(S) URI forms
    // valid: those schemes still require // followed by a non-empty authority.
    RUVIA_CHECK(!parseRequestTarget(HttpKnownMethod::kGet, "http:/x", out));
    RUVIA_CHECK(!parseRequestTarget(HttpKnownMethod::kGet, "https:x", out));
    RUVIA_CHECK(!parseRequestTarget(HttpKnownMethod::kGet, "1custom:/x", out));
    RUVIA_CHECK(!parseRequestTarget(HttpKnownMethod::kGet, "custom://user@@example.com/x", out));
    RUVIA_CHECK(!parseRequestTarget(HttpKnownMethod::kGet, "http://", out));
    RUVIA_CHECK(!parseRequestTarget(HttpKnownMethod::kGet, "http://exa@mple.com/x", out));
    RUVIA_CHECK(!parseRequestTarget(HttpKnownMethod::kGet, "http://example.com/[x]", out));
    RUVIA_CHECK(!parseRequestTarget(HttpKnownMethod::kGet, "http://example.com/?q={x}", out));
}

RUVIA_TEST(parse_request_target_absolute_empty_path_preserves_method_semantics) {
    RequestTargetView out;

    // RFC 9112 section 3.2.4 turns the no-query OPTIONS form into server-wide
    // asterisk-form at the final hop. With a query, the target remains an
    // HTTP(S) URI resource target and its empty path normalizes to "/".
    RUVIA_CHECK(parseRequestTarget(HttpKnownMethod::kOptions, "http://example.com", out));
    RUVIA_CHECK_EQ(out.path, std::string_view("*"));
    RUVIA_CHECK(out.query.empty());
    RUVIA_CHECK_EQ(out.authority, std::string_view("example.com"));
    RUVIA_CHECK(out.form == HttpRequestTargetForm::kAbsolute);

    RUVIA_CHECK(parseRequestTarget(HttpKnownMethod::kOptions, "http://example.com?scope=all", out));
    RUVIA_CHECK_EQ(out.path, std::string_view("/"));
    RUVIA_CHECK_EQ(out.query, std::string_view("scope=all"));

    // Generic URI schemes have no HTTP(S) rule that rewrites an empty path.
    RUVIA_CHECK(parseRequestTarget(HttpKnownMethod::kGet, "ftp://archive.example", out));
    RUVIA_CHECK(out.path.empty());
    RUVIA_CHECK(out.query.empty());

    RUVIA_CHECK(parseRequestTarget(HttpKnownMethod::kOptions, "ftp://archive.example", out));
    RUVIA_CHECK_EQ(out.path, std::string_view("*"));
}

RUVIA_TEST(parse_request_target_connect_authority_form) {
    RequestTargetView out;

    RUVIA_CHECK(parseRequestTarget(HttpKnownMethod::kConnect, "example.com:443", out));
    RUVIA_CHECK_EQ(out.authority, std::string_view("example.com:443"));
    RUVIA_CHECK(out.form == HttpRequestTargetForm::kAuthority);
    RUVIA_CHECK_EQ(out.path, std::string_view("example.com:443"));
    RUVIA_CHECK_EQ(out.query, std::string_view(""));

    RUVIA_CHECK(parseRequestTarget(HttpKnownMethod::kConnect, "[::1]:8443", out));
    RUVIA_CHECK_EQ(out.authority, std::string_view("[::1]:8443"));

    RUVIA_CHECK(!parseRequestTarget(HttpKnownMethod::kConnect, "/", out));
    RUVIA_CHECK(!parseRequestTarget(HttpKnownMethod::kConnect, "/tunnel", out));
    RUVIA_CHECK(!parseRequestTarget(HttpKnownMethod::kConnect, "example.com", out));
    RUVIA_CHECK(!parseRequestTarget(HttpKnownMethod::kConnect, "example.com:", out));
    RUVIA_CHECK(!parseRequestTarget(HttpKnownMethod::kConnect, "example.com:0", out));
    RUVIA_CHECK(!parseRequestTarget(HttpKnownMethod::kConnect, "[::1]:0", out));
    RUVIA_CHECK(!parseRequestTarget(HttpKnownMethod::kConnect, "http://example.com:443", out));
    RUVIA_CHECK(!parseRequestTarget(HttpKnownMethod::kConnect, "*", out));
}

RUVIA_TEST(parse_request_target_asterisk_and_rejections) {
    RequestTargetView out;
    // Asterisk-form is valid only for OPTIONS.
    RUVIA_CHECK(parseRequestTarget(HttpKnownMethod::kOptions, "*", out));
    RUVIA_CHECK_EQ(out.path, std::string_view("*"));
    RUVIA_CHECK(out.form == HttpRequestTargetForm::kAsterisk);
    RUVIA_CHECK(!parseRequestTarget(HttpKnownMethod::kGet, "*", out));
    // Empty, control/whitespace bytes and fragments are rejected.
    RUVIA_CHECK(!parseRequestTarget(HttpKnownMethod::kGet, "", out));
    RUVIA_CHECK(!parseRequestTarget(HttpKnownMethod::kGet, "/pa th", out));      // space
    RUVIA_CHECK(!parseRequestTarget(HttpKnownMethod::kGet, "/path#frag", out));  // '#' fragment
    RUVIA_CHECK(!parseRequestTarget(HttpKnownMethod::kGet, "/bad\\path", out));  // backslash
    RUVIA_CHECK(
        !parseRequestTarget(HttpKnownMethod::kGet, std::string_view("/a\r\nb", 5), out));  // CRLF
    RUVIA_CHECK(
        !parseRequestTarget(HttpKnownMethod::kGet, "/bad%zz", out));        // malformed pct-encoded
    RUVIA_CHECK(!parseRequestTarget(HttpKnownMethod::kGet, "/bad%", out));  // truncated pct-encoded
    RUVIA_CHECK(
        !parseRequestTarget(HttpKnownMethod::kGet, "/bad%2", out));  // truncated pct-encoded
    RUVIA_CHECK(!parseRequestTarget(HttpKnownMethod::kGet, "/raw{brace}", out));
    RUVIA_CHECK(
        !parseRequestTarget(HttpKnownMethod::kGet, std::string_view("/caf\xC3\xA9", 6), out));
    RUVIA_CHECK(parseRequestTarget(HttpKnownMethod::kGet, "/ok%2F?q=%7B%7D", out));
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
    RUVIA_CHECK(isValidRequestTargetBytes("http://[::1]/x?y=1"));

    RUVIA_CHECK(!isValidRequestTargetBytes(""));                            // empty is never a valid target
    RUVIA_CHECK(!isValidRequestTargetBytes("/a b"));                        // raw space (0x20) splits the request line
    RUVIA_CHECK(!isValidRequestTargetBytes("/a\tb"));                       // HTAB is a control char
    RUVIA_CHECK(!isValidRequestTargetBytes("/a\rb"));                       // CR -- header/line injection
    RUVIA_CHECK(!isValidRequestTargetBytes("/a\nb"));                       // LF -- request smuggling
    RUVIA_CHECK(!isValidRequestTargetBytes(std::string_view("/a\0b", 4)));  // NUL
    RUVIA_CHECK(!isValidRequestTargetBytes(
        "/a\x7f"
        "b"));  // DEL (0x7F); split literal so 'b' is not eaten by the hex escape
    RUVIA_CHECK(
        !isValidRequestTargetBytes("/page#frag"));     // '#' -- fragment must not reach the origin
    RUVIA_CHECK(!isValidRequestTargetBytes("/a\\b"));  // backslash -- path-normalization confusion
    RUVIA_CHECK(
        !isValidRequestTargetBytes("/raw{brace}"));  // not in the RFC 3986 URI character set
    RUVIA_CHECK(!isValidRequestTargetBytes("/raw|pipe"));
    RUVIA_CHECK(!isValidRequestTargetBytes(
        std::string_view("/caf\xC3\xA9", 6)));  // raw UTF-8 must be encoded
}

RUVIA_TEST(request_target_bytes_validate_percent_encoding) {
    // A '%' must be followed by exactly two hex digits, else it is malformed.
    RUVIA_CHECK(isValidRequestTargetBytes("/%2Fpath"));  // %2F is well-formed
    RUVIA_CHECK(isValidRequestTargetBytes("/a%20b"));    // encoded space is fine (raw is not)
    RUVIA_CHECK(isValidRequestTargetBytes("/%ff"));      // lowercase hex accepted

    RUVIA_CHECK(!isValidRequestTargetBytes("/%"));    // truncated at string end
    RUVIA_CHECK(!isValidRequestTargetBytes("/%2"));   // only one hex digit
    RUVIA_CHECK(!isValidRequestTargetBytes("/%2G"));  // second digit not hex
    RUVIA_CHECK(!isValidRequestTargetBytes("/%g0"));  // first digit not hex
    RUVIA_CHECK(!isValidRequestTargetBytes("/a%"));   // trailing bare '%'
}

RUVIA_TEST(origin_form_target_shape) {
    // Origin-form and asterisk-form are distinct request-target forms.
    RUVIA_CHECK(isValidOriginFormTarget("/"));
    RUVIA_CHECK(isValidOriginFormTarget("/path?q=1"));
    RUVIA_CHECK(!isValidOriginFormTarget("*"));
    RUVIA_CHECK(isValidOriginOrAsteriskFormTarget("*"));
    RUVIA_CHECK(isValidOriginOrAsteriskFormTarget(HttpKnownMethod::kOptions, "*"));
    RUVIA_CHECK(!isValidOriginOrAsteriskFormTarget(HttpKnownMethod::kGet, "*"));
    RUVIA_CHECK(isValidOriginOrAsteriskFormTarget(HttpKnownMethod::kGet, "/"));

    RUVIA_CHECK(!isValidOriginFormTarget(""));            // empty
    RUVIA_CHECK(!isValidOriginFormTarget("path"));        // missing leading '/'
    RUVIA_CHECK(!isValidOriginFormTarget("http://x/y"));  // absolute-form is not origin-form
    RUVIA_CHECK(!isValidOriginFormTarget("*/"));          // '*' is valid only as the whole target
    RUVIA_CHECK(!isValidOriginFormTarget("/bad path"));   // inherits byte validation (raw space)
    RUVIA_CHECK(!isValidOriginFormTarget("/x#y"));        // inherits fragment rejection
    RUVIA_CHECK(
        !isValidOriginFormTarget("/[x]"));             // brackets belong only to an IP-literal authority
    RUVIA_CHECK(!isValidOriginFormTarget("/?q={x}"));  // braces must be percent-encoded
    RUVIA_CHECK(isValidOriginFormTarget("/!$&'()*+,-._~:@/x?y=/?:@"));
}
