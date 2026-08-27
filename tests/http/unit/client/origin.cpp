#include "test_harness.h"

#include <cstdint>
#include <exception>
#include <memory_resource>
#include <string>
#include <string_view>

#include "ruvia/http/detail/client/HttpOriginView.h"
#include "ruvia/http/detail/parser/HttpRequestTarget.h"
#include "ruvia/http/detail/parser/HttpSerializedOrigin.h"
#include "ruvia/http/HttpClient.h"

namespace {

using ruvia::HttpOriginView;
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

HttpOriginView originFor(
    std::string_view host, std::uint16_t port, HttpScheme scheme = HttpScheme::kHttp) {
    return scheme == HttpScheme::kHttps ? HttpOriginView::https({.host = host, .port = port})
                                        : HttpOriginView::http({.host = host, .port = port});
}

std::string authorityFor(const HttpOriginView& origin) {
    const auto authority = makeHttpOriginAuthority(origin, std::pmr::get_default_resource());
    return std::string(authority.data(), authority.size());
}

}  // namespace

RUVIA_TEST(http_origin_factory_makes_an_invalid_host_unrepresentable) {
    RUVIA_CHECK(!throwsOn([] { (void)originFor("example.com", 80); }));
    RUVIA_CHECK(!throwsOn([] { (void)originFor("[::1]", 443, HttpScheme::kHttps); }));
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
    RUVIA_CHECK(throwsOn([] { (void)originFor(std::string_view("api\0.internal", 13), 80); }));
}

RUVIA_TEST(http_serialized_origin_matches_fetch_wire_grammar) {
    using ruvia::detail::isValidHttpSerializedOrigin;

    RUVIA_CHECK(isValidHttpSerializedOrigin("https://example.com"));
    RUVIA_CHECK(isValidHttpSerializedOrigin("https://example.com."));
    RUVIA_CHECK(isValidHttpSerializedOrigin("https://sub.example.com.:8443"));
    RUVIA_CHECK(isValidHttpSerializedOrigin("https://exa_mple.com"));
    RUVIA_CHECK(isValidHttpSerializedOrigin("https://-example.com"));
    RUVIA_CHECK(isValidHttpSerializedOrigin("https://example..com"));
    RUVIA_CHECK(isValidHttpSerializedOrigin("https://."));
    RUVIA_CHECK(isValidHttpSerializedOrigin("http://127.0.0.1:8080"));
    RUVIA_CHECK(isValidHttpSerializedOrigin("https://[::]"));
    RUVIA_CHECK(isValidHttpSerializedOrigin("https://[::1]"));
    RUVIA_CHECK(isValidHttpSerializedOrigin("https://[0:0:1::1]"));
    RUVIA_CHECK(isValidHttpSerializedOrigin("custom+scheme://sub-domain.example:65535"));
    RUVIA_CHECK(isValidHttpSerializedOrigin("custom+scheme://sub-domain.example:0"));

    RUVIA_CHECK(!isValidHttpSerializedOrigin(""));
    RUVIA_CHECK(!isValidHttpSerializedOrigin("null"));
    RUVIA_CHECK(!isValidHttpSerializedOrigin("HTTPS://example.com"));
    RUVIA_CHECK(!isValidHttpSerializedOrigin("https://EXAMPLE.com"));
    RUVIA_CHECK(!isValidHttpSerializedOrigin("https://example.com/"));
    RUVIA_CHECK(!isValidHttpSerializedOrigin("https://exa%6dple.com"));
    RUVIA_CHECK(!isValidHttpSerializedOrigin("https://123"));
    RUVIA_CHECK(!isValidHttpSerializedOrigin("https://1.2.3"));
    RUVIA_CHECK(!isValidHttpSerializedOrigin("https://127.0.0.1."));
    RUVIA_CHECK(!isValidHttpSerializedOrigin("https://example.123"));
    RUVIA_CHECK(!isValidHttpSerializedOrigin("https://example.0x10"));
    RUVIA_CHECK(!isValidHttpSerializedOrigin("https://[v1.future]"));
    RUVIA_CHECK(!isValidHttpSerializedOrigin("https://[::A]"));
    RUVIA_CHECK(!isValidHttpSerializedOrigin("https://[::0001]"));
    RUVIA_CHECK(!isValidHttpSerializedOrigin("https://[1:2:3:4:5:6:7::]"));
    RUVIA_CHECK(!isValidHttpSerializedOrigin("https://[1::0:0:1]"));
    RUVIA_CHECK(!isValidHttpSerializedOrigin("https://[0:0:1::1:1:1]"));
    RUVIA_CHECK(!isValidHttpSerializedOrigin("https://example.com:"));
    RUVIA_CHECK(!isValidHttpSerializedOrigin("https://example.com:0443"));
    RUVIA_CHECK(!isValidHttpSerializedOrigin("http://example.com:80"));
    RUVIA_CHECK(!isValidHttpSerializedOrigin("https://example.com:443"));
    RUVIA_CHECK(!isValidHttpSerializedOrigin("ws://example.com:80"));
    RUVIA_CHECK(!isValidHttpSerializedOrigin("wss://example.com:443"));
    RUVIA_CHECK(!isValidHttpSerializedOrigin("ftp://example.com:21"));
    RUVIA_CHECK(!isValidHttpSerializedOrigin("https://example.com:65536"));
    RUVIA_CHECK(!isValidHttpSerializedOrigin("custom+scheme://sub-domain.example:00001"));
    RUVIA_CHECK(!isValidHttpSerializedOrigin("custom+scheme://sub-domain.example:99999"));
    RUVIA_CHECK(!isValidHttpSerializedOrigin("https://example.com:123456"));
}

RUVIA_TEST(http_origin_authority_brackets_ipv6_and_omits_default_port) {
    // The default port for the scheme is omitted: 80 for http, 443 for https.
    RUVIA_CHECK_EQ(
        authorityFor(HttpOriginView::http({.host = "example.com"})), std::string("example.com"));
    RUVIA_CHECK_EQ(
        authorityFor(HttpOriginView::https({.host = "example.com"})), std::string("example.com"));

    // A non-default port is appended.
    RUVIA_CHECK_EQ(authorityFor(originFor("example.com", 8080)), std::string("example.com:8080"));

    // The default depends on the typed scheme: 80 is not default for https, and
    // 443 is not default for http, so each is included.
    RUVIA_CHECK_EQ(authorityFor(originFor("example.com", 80, HttpScheme::kHttps)),
        std::string("example.com:80"));
    RUVIA_CHECK_EQ(authorityFor(originFor("example.com", 443)), std::string("example.com:443"));

    // uri-host already carries the brackets required for an IP-literal; the
    // serializer no longer guesses host kind by searching for a colon.
    RUVIA_CHECK_EQ(authorityFor(originFor("[::1]", 80)), std::string("[::1]"));
    RUVIA_CHECK_EQ(authorityFor(originFor("[::1]", 8080)), std::string("[::1]:8080"));
    RUVIA_CHECK_EQ(authorityFor(originFor("[2001:db8::1]", 443, HttpScheme::kHttps)),
        std::string("[2001:db8::1]"));
    RUVIA_CHECK_EQ(authorityFor(originFor("[v1.future]", 8080)), std::string("[v1.future]:8080"));
    RUVIA_CHECK_EQ(authorityFor(originFor("example.com", 0)), std::string("example.com:0"));
}
