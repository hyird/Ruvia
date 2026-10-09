#include <cstdint>
#include <memory_resource>
#include <string>
#include <string_view>

#include "ruvia/http/http_client.h"
#include "ruvia/http/http_origin.h"

#include "parser/http_request_target.h"
#include "test_harness.h"

namespace {

using ruvia::http_origin_view;
using ruvia::http_scheme;
using ruvia::make_http_origin_authority;
using ruvia::testing::throws_on;

http_origin_view origin_for(
    std::string_view host, std::uint16_t port, http_scheme scheme = http_scheme::http) {
    return scheme == http_scheme::https ? http_origin_view::https({.host_ = host, .port_ = port})
                                        : http_origin_view::http({.host_ = host, .port_ = port});
}

std::string authority_for(const http_origin_view& origin) {
    const auto authority = make_http_origin_authority(origin, std::pmr::get_default_resource());
    return std::string(authority.data(), authority.size());
}

}  // namespace

RUVIA_TEST(http_origin_factory_makes_an_invalid_host_unrepresentable) {
    RUVIA_CHECK(!throws_on([] { (void)origin_for("example.com", 80); }));
    RUVIA_CHECK(!throws_on([] { (void)origin_for("[::1]", 443, http_scheme::https); }));
    RUVIA_CHECK(!throws_on([] { (void)origin_for("[v1.future]", 80); }));
    // Port zero is a syntactically distinct URI origin. Whether a transport can
    // connect to it is deliberately outside this protocol-only value type.
    RUVIA_CHECK(!throws_on([] { (void)origin_for("example.com", 0); }));

    RUVIA_CHECK(throws_on([] { (void)origin_for("", 80); }));
    RUVIA_CHECK(throws_on([] { (void)origin_for("bad host", 80); }));
    RUVIA_CHECK(throws_on([] { (void)origin_for("::1", 80); }));
    RUVIA_CHECK(throws_on([] { (void)origin_for("[::1", 80); }));
    RUVIA_CHECK(throws_on([] { (void)origin_for("example.com:80", 80); }));
    RUVIA_CHECK(throws_on([] { (void)origin_for("bad?host", 80); }));
    RUVIA_CHECK(throws_on([] { (void)origin_for("[::::]", 80); }));
    RUVIA_CHECK(throws_on([] { (void)origin_for("[v.future]", 80); }));
    RUVIA_CHECK(throws_on([] { (void)origin_for("caf\xC3\xA9.example", 80); }));
    RUVIA_CHECK(throws_on([] { (void)origin_for(std::string_view("api\0.internal", 13), 80); }));
}

RUVIA_TEST(http_serialized_origin_matches_fetch_wire_grammar) {
    for (const auto value : {
             "https://example.com", "https://example.com.", "https://sub.example.com.:8443",
             "https://exa_mple.com", "https://-example.com", "https://example..com", "https://.",
             "http://127.0.0.1:8080", "https://[::]", "https://[::1]", "https://[0:0:1::1]",
             "custom+scheme://sub-domain.example:65535", "custom+scheme://sub-domain.example:0"}) {
        RUVIA_CHECK(ruvia::is_valid_http_serialized_origin(value));
    }
    for (const auto value : {
             "", "null", "HTTPS://example.com", "https://EXAMPLE.com", "https://example.com/",
             "https://exa%6dple.com", "https://123", "https://1.2.3", "https://127.0.0.1.",
             "https://example.123", "https://example.0x10", "https://[v1.future]", "https://[::A]",
             "https://[::0001]", "https://[1:2:3:4:5:6:7::]", "https://[1::0:0:1]", "https://[0:0:1::1:1:1]",
             "https://example.com:", "https://example.com:0443", "http://example.com:80",
             "https://example.com:443", "ws://example.com:80", "wss://example.com:443", "ftp://example.com:21",
             "https://example.com:65536", "custom+scheme://sub-domain.example:00001",
             "custom+scheme://sub-domain.example:99999", "https://example.com:123456"}) {
        RUVIA_CHECK(!ruvia::is_valid_http_serialized_origin(value));
    }
}

RUVIA_TEST(http_origin_authority_brackets_ipv6_and_omits_default_port) {
    // The default port for the scheme is omitted: 80 for http, 443 for https.
    RUVIA_CHECK_EQ(
        authority_for(http_origin_view::http({.host_ = "example.com"})), std::string("example.com"));
    RUVIA_CHECK_EQ(
        authority_for(http_origin_view::https({.host_ = "example.com"})), std::string("example.com"));

    // A non-default port is appended.
    RUVIA_CHECK_EQ(authority_for(origin_for("example.com", 8080)), std::string("example.com:8080"));

    // The default depends on the typed scheme: 80 is not default for https, and
    // 443 is not default for http, so each is included.
    RUVIA_CHECK_EQ(authority_for(origin_for("example.com", 80, http_scheme::https)),
        std::string("example.com:80"));
    RUVIA_CHECK_EQ(authority_for(origin_for("example.com", 443)), std::string("example.com:443"));

    // uri-host already carries the brackets required for an IP-literal; the
    // serializer no longer guesses host kind by searching for a colon.
    RUVIA_CHECK_EQ(authority_for(origin_for("[::1]", 80)), std::string("[::1]"));
    RUVIA_CHECK_EQ(authority_for(origin_for("[::1]", 8080)), std::string("[::1]:8080"));
    RUVIA_CHECK_EQ(authority_for(origin_for("[2001:db8::1]", 443, http_scheme::https)),
        std::string("[2001:db8::1]"));
    RUVIA_CHECK_EQ(authority_for(origin_for("[v1.future]", 8080)), std::string("[v1.future]:8080"));
    RUVIA_CHECK_EQ(authority_for(origin_for("example.com", 0)), std::string("example.com:0"));
}
