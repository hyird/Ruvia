#include <cstdint>
#include <memory_resource>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>

#include "ruvia/http/http_request_target.h"

#include "parser/http_request_target.h"
#include "parser/http_uri_grammar.h"
#include "test_harness.h"

RUVIA_TEST(http_authority_host_public_parse_preserves_ip_literal_brackets) {
    RUVIA_CHECK_EQ(ruvia::parse_http_authority_host("[::1]:8080").value(), std::string_view("[::1]"));
    RUVIA_CHECK_EQ(ruvia::parse_http_authority_host("example.test:443").value(),
        std::string_view("example.test"));
    RUVIA_CHECK(!ruvia::parse_http_authority_host("user@example.test"));
    RUVIA_CHECK(ruvia::is_valid_http_ipv4_literal("127.0.0.1"));
    RUVIA_CHECK(!ruvia::is_valid_http_ipv4_literal("999.0.0.1"));
    RUVIA_CHECK(ruvia::is_valid_http_ipv6_literal("::1"));
    RUVIA_CHECK(!ruvia::is_valid_http_ipv6_literal("not-an-ip"));
}

RUVIA_TEST(uri_port_parser_returns_typed_values_and_errors) {
    using ruvia::detail::parse_port_value;
    RUVIA_CHECK_EQ(std::get<0>(parse_port_value("0")), std::uint16_t{0});
    RUVIA_CHECK_EQ(std::get<0>(parse_port_value("65535")), std::uint16_t{65535});
    RUVIA_CHECK_EQ(std::get<0>(parse_port_value("00080")), std::uint16_t{80});
    for (const std::string_view text : {"", "+80", "-1", " 80", "80 ", "80x"}) {
        const auto port = parse_port_value(text);
        RUVIA_CHECK(!(port.index() == 0));
        if ((port.index() != 0)) {
            RUVIA_CHECK_EQ(std::get<1>(port), std::errc::invalid_argument);
        }
    }
    for (const std::string_view text : {"65536", "99999999999999999999999999"}) {
        const auto port = parse_port_value(text);
        RUVIA_CHECK(!(port.index() == 0));
        if ((port.index() != 0)) {
            RUVIA_CHECK_EQ(std::get<1>(port), std::errc::result_out_of_range);
        }
    }
}

namespace {

using ruvia::http_known_method;
using ruvia::detail::authority_matches_host;
using ruvia::detail::http_authority_port_kind;
using ruvia::detail::http_request_target_form;
using ruvia::detail::http_uri_host_equals;
using ruvia::detail::http_uri_scheme_default_port;
using ruvia::detail::is_valid_host_header;
using ruvia::detail::is_valid_http_host;
using ruvia::detail::is_valid_origin_form_target;
using ruvia::detail::is_valid_origin_or_asterisk_form_target;
using ruvia::detail::is_valid_request_target_bytes;
using ruvia::detail::is_valid_uri_authority;
using ruvia::detail::is_valid_uri_scheme;
using ruvia::detail::parse_http_authority;
using ruvia::detail::parse_request_target;
using ruvia::detail::request_target_view;

}  // namespace

RUVIA_TEST(request_target_parsers_handle_deterministic_arbitrary_bytes) {
    std::uint64_t state_value = 0x5441'5247'4554'4655ULL;
    const auto next_value = [&state_value]() {
        state_value ^= state_value << 7U;
        state_value ^= state_value >> 9U;
        return state_value;
    };

    for (std::size_t sample = 0; sample < 4096; ++sample) {
        std::string input(static_cast<std::size_t>(next_value() % 513U), '\0');
        for (auto& byte : input) {
            byte = static_cast<char>(next_value());
        }

        const auto method = sample % 3 == 0   ? http_known_method::get
                            : sample % 3 == 1 ? http_known_method::options
                                              : http_known_method::connect;
        request_target_view target;
        const auto accepted = parse_request_target(method, input, target);
        const auto authority = parse_http_authority(input);
        RUVIA_CHECK_EQ(is_valid_host_header(input), input.empty() || authority.has_value());

        if (authority.has_value()) {
            RUVIA_CHECK(!authority->host().empty());
            RUVIA_CHECK(input.find(authority->host()) != std::string::npos);
            RUVIA_CHECK_EQ(authority->port().has_value(),
                authority->port_kind() == http_authority_port_kind::value);
        }
        if (!accepted) {
            continue;
        }

        RUVIA_CHECK(target.query_.empty() || input.find(target.query_) != std::string_view::npos);
        RUVIA_CHECK(
            target.authority_.empty() || input.find(target.authority_) != std::string_view::npos);
        switch (target.form_) {
            case http_request_target_form::origin:
                RUVIA_CHECK(target.scheme_.empty());
                RUVIA_CHECK(target.authority_.empty());
                RUVIA_CHECK(target.path_.starts_with('/'));
                break;
            case http_request_target_form::absolute:
                RUVIA_CHECK(!target.scheme_.empty());
                RUVIA_CHECK(input.starts_with(target.scheme_));
                RUVIA_CHECK_EQ(target.default_port_, http_uri_scheme_default_port(target.scheme_));
                RUVIA_CHECK(target.path_.empty() || target.path_ == "/" || target.path_ == "*" ||
                            input.find(target.path_) != std::string_view::npos);
                break;
            case http_request_target_form::authority:
                RUVIA_CHECK(method == http_known_method::connect);
                RUVIA_CHECK_EQ(target.path_, std::string_view(input));
                RUVIA_CHECK_EQ(target.authority_, std::string_view(input));
                break;
            case http_request_target_form::asterisk:
                RUVIA_CHECK(method == http_known_method::options);
                RUVIA_CHECK_EQ(input, std::string("*"));
                RUVIA_CHECK_EQ(target.path_, std::string_view("*"));
                break;
        }
    }
}

RUVIA_TEST(uri_scheme_uses_complete_rfc3986_grammar) {
    RUVIA_CHECK(is_valid_uri_scheme("http"));
    RUVIA_CHECK(is_valid_uri_scheme("HTTPS"));
    RUVIA_CHECK(is_valid_uri_scheme("ftp"));
    RUVIA_CHECK(is_valid_uri_scheme("git+ssh"));
    RUVIA_CHECK(is_valid_uri_scheme("x-1.example"));
    RUVIA_CHECK(!is_valid_uri_scheme(""));
    RUVIA_CHECK(!is_valid_uri_scheme("1http"));
    RUVIA_CHECK(!is_valid_uri_scheme("bad scheme"));
    RUVIA_CHECK(!is_valid_uri_scheme("https:"));
    RUVIA_CHECK(!is_valid_uri_scheme("https/other"));

    RUVIA_CHECK_EQ(http_uri_scheme_default_port("HTTP"), std::uint16_t{80});
    RUVIA_CHECK_EQ(http_uri_scheme_default_port("hTtPs"), std::uint16_t{443});
    RUVIA_CHECK_EQ(http_uri_scheme_default_port("ftp"), std::uint16_t{0});
}

RUVIA_TEST(uri_components_preserve_their_literal_byte_grammars) {
    constexpr std::string_view unreserved = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~";
    constexpr std::string_view sub_delimiters = "!$&'()*+,;=";
    for (unsigned value = 0; value < 256; ++value) {
        const auto byte = static_cast<char>(value);
        const std::string_view literal(&byte, 1);
        const bool plain = unreserved.find(byte) != std::string_view::npos;
        const bool reg_name = plain || sub_delimiters.find(byte) != std::string_view::npos;
        const bool pchar = reg_name || byte == ':' || byte == '@';
        RUVIA_CHECK_EQ(ruvia::detail::is_unreserved_byte(static_cast<unsigned char>(byte)), plain);
        RUVIA_CHECK_EQ(ruvia::detail::is_uri_pchar(static_cast<unsigned char>(byte)), pchar);
        RUVIA_CHECK_EQ(ruvia::detail::is_valid_reg_name(literal), reg_name);
        RUVIA_CHECK_EQ(ruvia::detail::is_valid_uri_userinfo(literal), reg_name || byte == ':');
        RUVIA_CHECK_EQ(ruvia::detail::is_valid_uri_component(literal, false, false), pchar);
        RUVIA_CHECK_EQ(ruvia::detail::is_valid_uri_component(literal, false, true), pchar || byte == '?');
        RUVIA_CHECK_EQ(ruvia::detail::is_valid_uri_component(literal, true, false), pchar || byte == '/');
        RUVIA_CHECK_EQ(ruvia::detail::is_valid_uri_component(literal, true, true), pchar || byte == '/' || byte == '?');
        const std::string future = std::string("v1.") + byte;
        RUVIA_CHECK_EQ(ruvia::detail::is_valid_ipv_future(future), reg_name || byte == ':');
    }
}

RUVIA_TEST(uri_components_validate_percent_encoding_without_changing_component_delimiters) {
    for (const std::string_view hex : {"0123456789ABCDEF", "0123456789abcdef"}) {
        for (unsigned value = 0; value < 256; ++value) {
            const char bytes_value[]{'%', hex[value >> 4], hex[value & 15]};
            const std::string_view escaped(bytes_value, sizeof(bytes_value));
            RUVIA_CHECK(ruvia::detail::is_valid_reg_name(escaped));
            RUVIA_CHECK(ruvia::detail::is_valid_uri_userinfo(escaped));
            RUVIA_CHECK(ruvia::detail::is_valid_uri_component(escaped, false, false));
        }
    }
    RUVIA_CHECK(!ruvia::detail::is_valid_reg_name(""));
    RUVIA_CHECK(ruvia::detail::is_valid_uri_userinfo(""));
    RUVIA_CHECK(ruvia::detail::is_valid_uri_component("", false, false));
    for (const auto value : {"%00", "%2f", "%3F", "%40", "%7e", "%80", "%ff"}) {
        RUVIA_CHECK(ruvia::detail::is_valid_reg_name(value));
        RUVIA_CHECK(ruvia::detail::is_valid_uri_userinfo(value));
        RUVIA_CHECK(ruvia::detail::is_valid_uri_component(value, false, false));
    }
    for (const auto value : {"%", "%2", "%g0", "%0g", "name%2", "name%20%", "name%0g"}) {
        RUVIA_CHECK(!ruvia::detail::is_valid_reg_name(value));
        RUVIA_CHECK(!ruvia::detail::is_valid_uri_userinfo(value));
        RUVIA_CHECK(!ruvia::detail::is_valid_uri_component(value, true, true));
    }
    RUVIA_CHECK(!ruvia::detail::is_valid_reg_name("name@host"));
    RUVIA_CHECK(!ruvia::detail::is_valid_uri_userinfo("name@host"));
    RUVIA_CHECK(ruvia::detail::is_valid_uri_component("name@host", false, false));
    RUVIA_CHECK(!ruvia::detail::is_valid_uri_component("/path?query", true, false));
    RUVIA_CHECK(ruvia::detail::is_valid_uri_component("/path?query", true, true));
    RUVIA_CHECK(!ruvia::detail::is_valid_ipv_future("v1.name%20"));
}

RUVIA_TEST(uri_authority_uses_complete_rfc3986_generic_grammar) {
    RUVIA_CHECK(is_valid_uri_authority(""));
    RUVIA_CHECK(is_valid_uri_authority("example.com"));
    RUVIA_CHECK(is_valid_uri_authority("user@example.com"));
    RUVIA_CHECK(is_valid_uri_authority("user:secret@example.com:9418"));
    RUVIA_CHECK(is_valid_uri_authority("name%3Avalue@example.com"));
    RUVIA_CHECK(is_valid_uri_authority("@"));
    RUVIA_CHECK(is_valid_uri_authority(":70000"));
    RUVIA_CHECK(is_valid_uri_authority("[v1.future]:99999999999999999999"));

    RUVIA_CHECK(!is_valid_uri_authority("user@@example.com"));
    RUVIA_CHECK(!is_valid_uri_authority("bad%2@example.com"));
    RUVIA_CHECK(!is_valid_uri_authority("bad user@example.com"));
    RUVIA_CHECK(!is_valid_uri_authority("user@example.com:port"));
    RUVIA_CHECK(!is_valid_uri_authority("user@[::1"));
    RUVIA_CHECK(!is_valid_uri_authority("user@example.com/path"));
}

RUVIA_TEST(host_header_accepts_valid) {
    // RFC 9112 section 3.2 permits an empty Host field when the target URI has
    // no authority component.
    RUVIA_CHECK(is_valid_host_header(""));
    RUVIA_CHECK(is_valid_host_header("example.com"));
    RUVIA_CHECK(is_valid_host_header("example.com:8080"));
    RUVIA_CHECK(is_valid_host_header("localhost"));
    RUVIA_CHECK(is_valid_host_header("sub.domain.example.com"));
    RUVIA_CHECK(is_valid_host_header("192.0.2.1"));
    RUVIA_CHECK(is_valid_host_header("192.0.2.1:443"));
    RUVIA_CHECK(is_valid_host_header("example.com:65535"));  // the maximum valid port is inclusive
    RUVIA_CHECK(is_valid_host_header("example.com:0"));
    // RFC 3986 defines port as *DIGIT; RFC 9110 treats an empty HTTP port as
    // the scheme default rather than an invalid authority.
    RUVIA_CHECK(is_valid_host_header("example.com:"));
    RUVIA_CHECK(is_valid_host_header("[::1]"));
    RUVIA_CHECK(is_valid_host_header("[::1]:8080"));
    RUVIA_CHECK(is_valid_host_header("[::1]:"));
    RUVIA_CHECK(is_valid_host_header("[2001:db8::1]"));
    RUVIA_CHECK(is_valid_host_header("[::ffff:192.0.2.128]"));
    RUVIA_CHECK(is_valid_host_header("[2001:db8:0:0:0:0:192.0.2.1]"));
    RUVIA_CHECK(is_valid_host_header("[v1.future]"));
    RUVIA_CHECK(is_valid_host_header("[vF.a:b!c]:443"));
}

RUVIA_TEST(host_header_rejects_invalid) {
    RUVIA_CHECK(!is_valid_host_header("example.com:65536"));    // one past the maximum port
    RUVIA_CHECK(!is_valid_host_header("example.com:70000"));    // port > 65535
    RUVIA_CHECK(!is_valid_host_header("example.com:8o80"));     // non-digit in port
    RUVIA_CHECK(!is_valid_host_header("example.com:80:80"));    // trailing junk after port
    RUVIA_CHECK(!is_valid_host_header("exa mple.com"));         // space is not a reg-name char
    RUVIA_CHECK(!is_valid_host_header("[::1"));                 // unclosed IPv6 bracket
    RUVIA_CHECK(!is_valid_host_header("[]"));                   // empty bracket
    RUVIA_CHECK(!is_valid_host_header("[::1]x"));               // junk after the bracket
    RUVIA_CHECK(!is_valid_host_header("[GG::1]"));              // non-hex byte in the IPv6 literal
    RUVIA_CHECK(!is_valid_host_header("[::::]"));               // not a valid IPv6 literal
    RUVIA_CHECK(!is_valid_host_header("[1:2:3:4:5:6:7:8:9]"));  // too many 16-bit groups
    RUVIA_CHECK(!is_valid_host_header("[::ffff:999.0.2.128]"));
    RUVIA_CHECK(
        !is_valid_host_header("[::ffff:192.168.001.1]"));  // IPv4 dec-octet has no leading zero
    RUVIA_CHECK(!is_valid_host_header("[v.future]"));      // missing version hex digit
    RUVIA_CHECK(!is_valid_host_header("[v1.]"));           // empty future address
    RUVIA_CHECK(!is_valid_host_header("[v1.a%20b]"));      // pct-encoding is not IPvFuture grammar
    // A zone/scope id is valid to getaddrinfo but not a legal URI host: it must
    // be rejected so a scoped address can never slip into host matching.
    RUVIA_CHECK(!is_valid_host_header("[fe80::1%eth0]"));
    // A CRLF-injection attempt must not validate.
    RUVIA_CHECK(!is_valid_host_header(std::string_view("example.com\r\nX", 14)));
}

RUVIA_TEST(http_host_component_reuses_reg_name_and_ipv6_validation) {
    RUVIA_CHECK(is_valid_http_host("example.com"));
    RUVIA_CHECK(is_valid_http_host("192.0.2.1"));
    RUVIA_CHECK(is_valid_http_host("[::1]"));
    RUVIA_CHECK(is_valid_http_host("[2001:db8::1]"));
    RUVIA_CHECK(is_valid_http_host("[v1.future]"));
    RUVIA_CHECK(!is_valid_http_host("::1"));
    RUVIA_CHECK(!is_valid_http_host("2001:db8::1"));
    RUVIA_CHECK(!is_valid_http_host("example.com:443"));
    RUVIA_CHECK(!is_valid_http_host("[::::]"));
    RUVIA_CHECK(!is_valid_http_host("bad?host"));
}

RUVIA_TEST(authority_parser_preserves_absent_empty_and_numeric_ports) {
    const auto absent = parse_http_authority("example.com");
    RUVIA_CHECK(absent.has_value());
    RUVIA_CHECK(absent->port_kind() == http_authority_port_kind::absent);
    RUVIA_CHECK(!absent->port().has_value());
    RUVIA_CHECK_EQ(absent->effective_port(80), std::uint16_t{80});

    const auto empty = parse_http_authority("example.com:");
    RUVIA_CHECK(empty.has_value());
    RUVIA_CHECK(empty->port_kind() == http_authority_port_kind::empty);
    RUVIA_CHECK(!empty->port().has_value());
    RUVIA_CHECK_EQ(empty->effective_port(80), std::uint16_t{80});

    const auto zero = parse_http_authority("example.com:00000");
    RUVIA_CHECK(zero.has_value());
    RUVIA_CHECK(zero->port_kind() == http_authority_port_kind::value);
    RUVIA_CHECK_EQ(*zero->port(), std::uint16_t{0});
    RUVIA_CHECK_EQ(zero->effective_port(80), std::uint16_t{0});
}

RUVIA_TEST(uri_host_comparison_normalizes_only_percent_encoded_unreserved_octets) {
    RUVIA_CHECK(http_uri_host_equals("EXAMPLE.com", "example.COM"));
    RUVIA_CHECK(http_uri_host_equals("exa%6Dple.com", "example.com"));
    RUVIA_CHECK(http_uri_host_equals("%65xample.com", "example.com"));
    RUVIA_CHECK(http_uri_host_equals("%2f.example", "%2F.example"));
    RUVIA_CHECK(http_uri_host_equals("[Vf.A:B]", "[vf.a:b]"));
    RUVIA_CHECK(!http_uri_host_equals("%21example", "!example"));
    RUVIA_CHECK(!http_uri_host_equals("example.com", "other.example"));
}

RUVIA_TEST(authority_matches_host_ports_and_case) {
    RUVIA_CHECK(authority_matches_host("example.com", "example.com", 80));
    RUVIA_CHECK(authority_matches_host("example.com:80", "example.com", 80));  // explicit == default
    RUVIA_CHECK(authority_matches_host("example.com", "example.com:80", 80));
    RUVIA_CHECK(authority_matches_host("example.com:", "example.com", 80));
    RUVIA_CHECK(authority_matches_host("exa%6dple.com", "example.com:", 80));
    RUVIA_CHECK(
        authority_matches_host("EXAMPLE.com", "example.COM", 80));   // host is case-insensitive
    RUVIA_CHECK(authority_matches_host("[::1]:443", "[::1]", 443));  // IPv6 default port
    RUVIA_CHECK(authority_matches_host("[v1.future]:", "[V1.FUTURE]", 80));
    RUVIA_CHECK(authority_matches_host("example.com", "example.com:", 0));
    RUVIA_CHECK(authority_matches_host("example.com:21", "example.com:21", 0));
}

RUVIA_TEST(authority_matches_host_rejects_mismatches) {
    RUVIA_CHECK(
        !authority_matches_host("example.com:8080", "example.com", 80));   // port mismatch vs default
    RUVIA_CHECK(!authority_matches_host("example.com", "other.com", 80));  // host mismatch
    RUVIA_CHECK(
        !authority_matches_host("example.com:8080", "example.com:80", 80));  // explicit port mismatch
    RUVIA_CHECK(!authority_matches_host("evil.com", "example.com", 80));
    RUVIA_CHECK(!authority_matches_host("example.com", "example.com:0", 0));
    RUVIA_CHECK(!authority_matches_host("example.com", "example.com:21", 0));
    RUVIA_CHECK(!authority_matches_host("example.com:21", "example.com", 0));
}

RUVIA_TEST(parse_request_target_origin_form) {
    request_target_view out;
    RUVIA_CHECK(parse_request_target(http_known_method::get, "/path?q=1&r=2", out));
    RUVIA_CHECK_EQ(out.path_, std::string_view("/path"));
    RUVIA_CHECK_EQ(out.query_, std::string_view("q=1&r=2"));
    RUVIA_CHECK(out.form_ == http_request_target_form::origin);

    RUVIA_CHECK(parse_request_target(http_known_method::get, "/only/path", out));
    RUVIA_CHECK_EQ(out.path_, std::string_view("/only/path"));
    RUVIA_CHECK_EQ(out.query_, std::string_view(""));
}

RUVIA_TEST(parse_request_target_absolute_form) {
    request_target_view out;

    // Absolute-form (proxy/smuggling surface): authority is split off and the
    // path/query recovered, with the scheme fixing the default port.
    RUVIA_CHECK(parse_request_target(http_known_method::get, "http://example.com/path?q=1", out));
    RUVIA_CHECK_EQ(out.authority_, std::string_view("example.com"));
    RUVIA_CHECK_EQ(out.path_, std::string_view("/path"));
    RUVIA_CHECK_EQ(out.query_, std::string_view("q=1"));
    RUVIA_CHECK_EQ(out.default_port_, std::uint16_t{80});
    RUVIA_CHECK(out.form_ == http_request_target_form::absolute);

    // No path component defaults the path to "/".
    RUVIA_CHECK(parse_request_target(http_known_method::get, "http://example.com", out));
    RUVIA_CHECK_EQ(out.authority_, std::string_view("example.com"));
    RUVIA_CHECK_EQ(out.path_, std::string_view("/"));
    RUVIA_CHECK_EQ(out.query_, std::string_view(""));

    // A query immediately after the authority still yields path "/".
    RUVIA_CHECK(parse_request_target(http_known_method::get, "http://example.com?q=1", out));
    RUVIA_CHECK_EQ(out.path_, std::string_view("/"));
    RUVIA_CHECK_EQ(out.query_, std::string_view("q=1"));

    // https fixes the default port to 443; an explicit port is kept in the authority.
    RUVIA_CHECK(parse_request_target(http_known_method::get, "https://example.com:8080/x", out));
    RUVIA_CHECK_EQ(out.authority_, std::string_view("example.com:8080"));
    RUVIA_CHECK_EQ(out.path_, std::string_view("/x"));
    RUVIA_CHECK_EQ(out.default_port_, std::uint16_t{443});

    RUVIA_CHECK(parse_request_target(http_known_method::get, "http://example.com:/x", out));
    RUVIA_CHECK_EQ(out.authority_, std::string_view("example.com:"));
    RUVIA_CHECK(parse_request_target(http_known_method::get, "http://[v1.future]/x", out));
    RUVIA_CHECK_EQ(out.authority_, std::string_view("[v1.future]"));

    // Absolute-form is the complete RFC 3986 absolute-URI grammar, not only
    // HTTP(S). Unknown schemes retain an unknown default port.
    RUVIA_CHECK(parse_request_target(http_known_method::get, "ftp://example.com/pub/archive", out));
    RUVIA_CHECK_EQ(out.path_, std::string_view("/pub/archive"));
    RUVIA_CHECK_EQ(out.authority_, std::string_view("example.com"));
    RUVIA_CHECK_EQ(out.default_port_, std::uint16_t{0});
    RUVIA_CHECK(out.form_ == http_request_target_form::absolute);

    RUVIA_CHECK(parse_request_target(
        http_known_method::get, "custom://user:secret@example.com/resource", out));
    RUVIA_CHECK_EQ(out.path_, std::string_view("/resource"));
    // RFC 9112 section 3.2 excludes userinfo from the effective Host value.
    RUVIA_CHECK_EQ(out.authority_, std::string_view("example.com"));

    RUVIA_CHECK(parse_request_target(http_known_method::get, "urn:example:animal:ferret:nose", out));
    RUVIA_CHECK_EQ(out.path_, std::string_view("example:animal:ferret:nose"));
    RUVIA_CHECK(out.query_.empty());
    RUVIA_CHECK(out.authority_.empty());
    RUVIA_CHECK_EQ(out.default_port_, std::uint16_t{0});
    RUVIA_CHECK(out.form_ == http_request_target_form::absolute);

    RUVIA_CHECK(parse_request_target(http_known_method::get, "file:///etc/hosts", out));
    RUVIA_CHECK_EQ(out.path_, std::string_view("/etc/hosts"));
    RUVIA_CHECK(out.authority_.empty());

    RUVIA_CHECK(parse_request_target(http_known_method::get, "custom:?name=value", out));
    RUVIA_CHECK(out.path_.empty());
    RUVIA_CHECK_EQ(out.query_, std::string_view("name=value"));
    RUVIA_CHECK(out.authority_.empty());

    // Generic absolute-URI syntax does not make malformed HTTP(S) URI forms
    // valid: those schemes still require // followed by a non-empty authority.
    RUVIA_CHECK(!parse_request_target(http_known_method::get, "http:/x", out));
    RUVIA_CHECK(!parse_request_target(http_known_method::get, "https:x", out));
    RUVIA_CHECK(!parse_request_target(http_known_method::get, "1custom:/x", out));
    RUVIA_CHECK(!parse_request_target(http_known_method::get, "custom://user@@example.com/x", out));
    RUVIA_CHECK(!parse_request_target(http_known_method::get, "http://", out));
    RUVIA_CHECK(!parse_request_target(http_known_method::get, "http://exa@mple.com/x", out));
    RUVIA_CHECK(!parse_request_target(http_known_method::get, "http://example.com/[x]", out));
    RUVIA_CHECK(!parse_request_target(http_known_method::get, "http://example.com/?q={x}", out));
}

RUVIA_TEST(parse_request_target_absolute_empty_path_preserves_method_semantics) {
    request_target_view out;

    // RFC 9112 section 3.2.4 turns the no-query OPTIONS form into server-wide
    // asterisk-form at the final hop. With a query, the target remains an
    // HTTP(S) URI resource target and its empty path normalizes to "/".
    RUVIA_CHECK(parse_request_target(http_known_method::options, "http://example.com", out));
    RUVIA_CHECK_EQ(out.path_, std::string_view("*"));
    RUVIA_CHECK(out.query_.empty());
    RUVIA_CHECK_EQ(out.authority_, std::string_view("example.com"));
    RUVIA_CHECK(out.form_ == http_request_target_form::absolute);

    RUVIA_CHECK(parse_request_target(http_known_method::options, "http://example.com?scope=all", out));
    RUVIA_CHECK_EQ(out.path_, std::string_view("/"));
    RUVIA_CHECK_EQ(out.query_, std::string_view("scope=all"));

    // A present but empty query is distinct from an absent query component.
    for (const std::string_view target : {"http://example.com?", "https://example.com?"}) {
        RUVIA_CHECK(parse_request_target(http_known_method::options, target, out));
        RUVIA_CHECK_EQ(out.path_, std::string_view("/"));
        RUVIA_CHECK(out.query_.empty());
    }

    // Generic URI schemes have no HTTP(S) rule that rewrites an empty path.
    RUVIA_CHECK(parse_request_target(http_known_method::get, "ftp://archive.example", out));
    RUVIA_CHECK(out.path_.empty());
    RUVIA_CHECK(out.query_.empty());

    RUVIA_CHECK(parse_request_target(http_known_method::options, "ftp://archive.example", out));
    RUVIA_CHECK_EQ(out.path_, std::string_view("*"));
    RUVIA_CHECK(parse_request_target(http_known_method::options, "ftp://archive.example?", out));
    RUVIA_CHECK(out.path_.empty());
    RUVIA_CHECK(out.query_.empty());
}

RUVIA_TEST(parse_request_target_connect_authority_form) {
    request_target_view out;

    RUVIA_CHECK(parse_request_target(http_known_method::connect, "example.com:443", out));
    RUVIA_CHECK_EQ(out.authority_, std::string_view("example.com:443"));
    RUVIA_CHECK(out.form_ == http_request_target_form::authority);
    RUVIA_CHECK_EQ(out.path_, std::string_view("example.com:443"));
    RUVIA_CHECK_EQ(out.query_, std::string_view(""));

    RUVIA_CHECK(parse_request_target(http_known_method::connect, "[::1]:8443", out));
    RUVIA_CHECK_EQ(out.authority_, std::string_view("[::1]:8443"));

    RUVIA_CHECK(!parse_request_target(http_known_method::connect, "/", out));
    RUVIA_CHECK(!parse_request_target(http_known_method::connect, "/tunnel", out));
    RUVIA_CHECK(!parse_request_target(http_known_method::connect, "example.com", out));
    RUVIA_CHECK(!parse_request_target(http_known_method::connect, "example.com:", out));
    RUVIA_CHECK(!parse_request_target(http_known_method::connect, "example.com:0", out));
    RUVIA_CHECK(!parse_request_target(http_known_method::connect, "[::1]:0", out));
    RUVIA_CHECK(!parse_request_target(http_known_method::connect, "http://example.com:443", out));
    RUVIA_CHECK(!parse_request_target(http_known_method::connect, "*", out));
}

RUVIA_TEST(parse_request_target_asterisk_and_rejections) {
    request_target_view out;
    // Asterisk-form is valid only for OPTIONS.
    RUVIA_CHECK(parse_request_target(http_known_method::options, "*", out));
    RUVIA_CHECK_EQ(out.path_, std::string_view("*"));
    RUVIA_CHECK(out.form_ == http_request_target_form::asterisk);
    RUVIA_CHECK(!parse_request_target(http_known_method::get, "*", out));
    // Empty, control/whitespace bytes and fragments are rejected.
    RUVIA_CHECK(!parse_request_target(http_known_method::get, "", out));
    RUVIA_CHECK(!parse_request_target(http_known_method::get, "/pa th", out));      // space
    RUVIA_CHECK(!parse_request_target(http_known_method::get, "/path#frag", out));  // '#' fragment
    RUVIA_CHECK(!parse_request_target(http_known_method::get, "/bad\\path", out));  // backslash
    RUVIA_CHECK(
        !parse_request_target(http_known_method::get, std::string_view("/a\r\nb", 5), out));  // CRLF
    RUVIA_CHECK(
        !parse_request_target(http_known_method::get, "/bad%zz", out));        // malformed pct-encoded
    RUVIA_CHECK(!parse_request_target(http_known_method::get, "/bad%", out));  // truncated pct-encoded
    RUVIA_CHECK(
        !parse_request_target(http_known_method::get, "/bad%2", out));  // truncated pct-encoded
    RUVIA_CHECK(!parse_request_target(http_known_method::get, "/?safe=1&bad=%zz", out));
    RUVIA_CHECK(!parse_request_target(http_known_method::get, "/?bad=%2", out));
    RUVIA_CHECK(!parse_request_target(http_known_method::get, "/raw{brace}", out));
    RUVIA_CHECK(
        !parse_request_target(http_known_method::get, std::string_view("/caf\xC3\xA9", 6), out));
    RUVIA_CHECK(parse_request_target(http_known_method::get, "/ok%2F?q=%7B%7D", out));
    RUVIA_CHECK_EQ(out.path_, std::string_view("/ok%2F"));
    RUVIA_CHECK_EQ(out.query_, std::string_view("q=%7B%7D"));
}

RUVIA_TEST(request_target_bytes_reject_smuggling_and_control_chars) {
    // The low-level byte validator underpins both the HTTP/1 request target and the
    // HTTP/2 :path. It is a request-smuggling / path-injection defense, so anything
    // that could confuse downstream parsing or split the target must be rejected.
    RUVIA_CHECK(is_valid_request_target_bytes("/index.html"));
    RUVIA_CHECK(is_valid_request_target_bytes("/search?q=a+b&x=1"));
    RUVIA_CHECK(is_valid_request_target_bytes("/a/b/c"));
    RUVIA_CHECK(is_valid_request_target_bytes("http://[::1]/x?y=1"));

    RUVIA_CHECK(!is_valid_request_target_bytes(""));                            // empty is never a valid target
    RUVIA_CHECK(!is_valid_request_target_bytes("/a b"));                        // raw space (0x20) splits the request line
    RUVIA_CHECK(!is_valid_request_target_bytes("/a\tb"));                       // HTAB is a control char
    RUVIA_CHECK(!is_valid_request_target_bytes("/a\rb"));                       // CR -- header/line injection
    RUVIA_CHECK(!is_valid_request_target_bytes("/a\nb"));                       // LF -- request smuggling
    RUVIA_CHECK(!is_valid_request_target_bytes(std::string_view("/a\0b", 4)));  // NUL
    RUVIA_CHECK(!is_valid_request_target_bytes(
        "/a\x7f"
        "b"));  // DEL (0x7F); split literal so 'b' is not eaten by the hex escape
    RUVIA_CHECK(
        !is_valid_request_target_bytes("/page#frag"));     // '#' -- fragment must not reach the origin
    RUVIA_CHECK(!is_valid_request_target_bytes("/a\\b"));  // backslash -- path-normalization confusion
    RUVIA_CHECK(
        !is_valid_request_target_bytes("/raw{brace}"));  // not in the RFC 3986 URI character set
    RUVIA_CHECK(!is_valid_request_target_bytes("/raw|pipe"));
    RUVIA_CHECK(!is_valid_request_target_bytes(
        std::string_view("/caf\xC3\xA9", 6)));  // raw UTF-8 must be encoded
}

RUVIA_TEST(request_target_bytes_validate_percent_encoding) {
    // A '%' must be followed by exactly two hex digits, else it is malformed.
    RUVIA_CHECK(is_valid_request_target_bytes("/%2Fpath"));  // %2F is well-formed
    RUVIA_CHECK(is_valid_request_target_bytes("/a%20b"));    // encoded space is fine (raw is not)
    RUVIA_CHECK(is_valid_request_target_bytes("/%ff"));      // lowercase hex accepted

    RUVIA_CHECK(!is_valid_request_target_bytes("/%"));    // truncated at string end
    RUVIA_CHECK(!is_valid_request_target_bytes("/%2"));   // only one hex digit
    RUVIA_CHECK(!is_valid_request_target_bytes("/%2G"));  // second digit not hex
    RUVIA_CHECK(!is_valid_request_target_bytes("/%g0"));  // first digit not hex
    RUVIA_CHECK(!is_valid_request_target_bytes("/a%"));   // trailing bare '%'
}

RUVIA_TEST(origin_form_target_shape) {
    // Origin-form and asterisk-form are distinct request-target forms.
    RUVIA_CHECK(is_valid_origin_form_target("/"));
    RUVIA_CHECK(is_valid_origin_form_target("/path?q=1"));
    RUVIA_CHECK(!is_valid_origin_form_target("*"));
    RUVIA_CHECK(is_valid_origin_or_asterisk_form_target("*"));
    RUVIA_CHECK(is_valid_origin_or_asterisk_form_target(http_known_method::options, "*"));
    RUVIA_CHECK(!is_valid_origin_or_asterisk_form_target(http_known_method::get, "*"));
    RUVIA_CHECK(is_valid_origin_or_asterisk_form_target(http_known_method::get, "/"));

    RUVIA_CHECK(!is_valid_origin_form_target(""));            // empty
    RUVIA_CHECK(!is_valid_origin_form_target("path"));        // missing leading '/'
    RUVIA_CHECK(!is_valid_origin_form_target("http://x/y"));  // absolute-form is not origin-form
    RUVIA_CHECK(!is_valid_origin_form_target("*/"));          // '*' is valid only as the whole target
    RUVIA_CHECK(!is_valid_origin_form_target("/bad path"));   // inherits byte validation (raw space)
    RUVIA_CHECK(!is_valid_origin_form_target("/x#y"));        // inherits fragment rejection
    RUVIA_CHECK(
        !is_valid_origin_form_target("/[x]"));             // brackets belong only to an IP-literal authority
    RUVIA_CHECK(!is_valid_origin_form_target("/?q={x}"));  // braces must be percent-encoded
    RUVIA_CHECK(is_valid_origin_form_target("/!$&'()*+,-._~:@/x?y=/?:@"));
}
