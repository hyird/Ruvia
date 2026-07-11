#include "test_harness.h"

#include <cstdint>
#include <exception>
#include <memory_resource>
#include <string>
#include <string_view>

#include "ruvia/http/detail/client/HttpOrigin.h"
#include "ruvia/http/HttpClient.h"

namespace {

using ruvia::HttpOrigin;
using ruvia::HttpScheme;
using ruvia::detail::makeHttpOriginAuthority;

template <typename Fn>
bool throwsOn(Fn&& fn) {
    try {
        fn();
        return false;
    } catch (const std::exception&) {
        return true;
    }
}

HttpOrigin originFor(
    std::string_view host,
    std::uint16_t port,
    HttpScheme scheme = HttpScheme::kHttp) {
    return scheme == HttpScheme::kHttps
        ? HttpOrigin::https(host, port)
        : HttpOrigin::http(host, port);
}

std::string authorityFor(const HttpOrigin& origin) {
    const auto authority = makeHttpOriginAuthority(origin, std::pmr::get_default_resource());
    return std::string(authority.data(), authority.size());
}

}  // namespace

RUVIA_TEST(http_origin_factory_makes_an_invalid_host_unrepresentable) {
    RUVIA_CHECK(!throwsOn([] { (void)originFor("example.com", 80); }));
    RUVIA_CHECK(!throwsOn([] {
        (void)originFor("[::1]", 443, HttpScheme::kHttps);
    }));
    RUVIA_CHECK(!throwsOn([] { (void)originFor("[v1.future]", 80); }));
    // Port zero is a syntactically distinct URI origin. Whether a transport can
    // connect to it is deliberately outside this protocol-only value type.
    RUVIA_CHECK(!throwsOn([] { (void)originFor("example.com", 0); }));

    RUVIA_CHECK(throwsOn([] { (void)originFor("", 80); }));
    RUVIA_CHECK(throwsOn([] { (void)originFor("bad host", 80); }));
    RUVIA_CHECK(throwsOn([] { (void)originFor("::1", 80); }));
    RUVIA_CHECK(throwsOn([] { (void)originFor("[::1", 80); }));
    RUVIA_CHECK(throwsOn([] { (void)originFor("example.com:80", 80); }));
    RUVIA_CHECK(throwsOn([] { (void)originFor("bad?host", 80); }));
    RUVIA_CHECK(throwsOn([] { (void)originFor("[::::]", 80); }));
    RUVIA_CHECK(throwsOn([] { (void)originFor("[v.future]", 80); }));
    RUVIA_CHECK(throwsOn([] { (void)originFor("caf\xC3\xA9.example", 80); }));
    RUVIA_CHECK(throwsOn([] {
        (void)originFor(std::string_view("api\0.internal", 13), 80);
    }));
}

RUVIA_TEST(http_origin_authority_brackets_ipv6_and_omits_default_port) {
    // The default port for the scheme is omitted: 80 for http, 443 for https.
    RUVIA_CHECK_EQ(authorityFor(HttpOrigin::http("example.com")), std::string("example.com"));
    RUVIA_CHECK_EQ(authorityFor(HttpOrigin::https("example.com")), std::string("example.com"));

    // A non-default port is appended.
    RUVIA_CHECK_EQ(authorityFor(originFor("example.com", 8080)), std::string("example.com:8080"));

    // The default depends on the typed scheme: 80 is not default for https, and
    // 443 is not default for http, so each is included.
    RUVIA_CHECK_EQ(
        authorityFor(originFor("example.com", 80, HttpScheme::kHttps)),
        std::string("example.com:80"));
    RUVIA_CHECK_EQ(authorityFor(originFor("example.com", 443)), std::string("example.com:443"));

    // uri-host already carries the brackets required for an IP-literal; the
    // serializer no longer guesses host kind by searching for a colon.
    RUVIA_CHECK_EQ(authorityFor(originFor("[::1]", 80)), std::string("[::1]"));
    RUVIA_CHECK_EQ(authorityFor(originFor("[::1]", 8080)), std::string("[::1]:8080"));
    RUVIA_CHECK_EQ(
        authorityFor(originFor("[2001:db8::1]", 443, HttpScheme::kHttps)),
        std::string("[2001:db8::1]"));
    RUVIA_CHECK_EQ(
        authorityFor(originFor("[v1.future]", 8080)),
        std::string("[v1.future]:8080"));
    RUVIA_CHECK_EQ(
        authorityFor(originFor("example.com", 0)),
        std::string("example.com:0"));
}
