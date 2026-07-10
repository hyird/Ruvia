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
using ruvia::detail::makeHttpOriginAuthority;
using ruvia::detail::validateHttpOrigin;

template <typename Fn>
bool throwsOn(Fn&& fn) {
    try {
        fn();
        return false;
    } catch (const std::exception&) {
        return true;
    }
}

HttpOrigin originFor(std::string_view host, std::uint16_t port, bool tls = false) {
    HttpOrigin origin;
    origin.host.assign(host.data(), host.size());
    origin.port = port;
    origin.tls = tls;
    return origin;
}

std::string authorityFor(const HttpOrigin& origin) {
    const auto authority = makeHttpOriginAuthority(origin, std::pmr::get_default_resource());
    return std::string(authority.data(), authority.size());
}

}  // namespace

RUVIA_TEST(http_origin_validation_rejects_invalid_authorities) {
    RUVIA_CHECK(!throwsOn([] { validateHttpOrigin(originFor("example.com", 80)); }));
    RUVIA_CHECK(!throwsOn([] { validateHttpOrigin(originFor("::1", 443, true)); }));
    RUVIA_CHECK(throwsOn([] { validateHttpOrigin(originFor("", 80)); }));
    RUVIA_CHECK(throwsOn([] { validateHttpOrigin(originFor("example.com", 0)); }));
    RUVIA_CHECK(throwsOn([] { validateHttpOrigin(originFor("bad host", 80)); }));
    RUVIA_CHECK(throwsOn([] { validateHttpOrigin(originFor("[::1]", 80)); }));
    RUVIA_CHECK(throwsOn([] {
        validateHttpOrigin(originFor(std::string_view("api\0.internal", 13), 80));
    }));
}

RUVIA_TEST(http_origin_authority_brackets_ipv6_and_omits_default_port) {
    // The default port for the scheme is omitted: 80 for http, 443 for https.
    RUVIA_CHECK_EQ(authorityFor(originFor("example.com", 80, false)), std::string("example.com"));
    RUVIA_CHECK_EQ(authorityFor(originFor("example.com", 443, true)), std::string("example.com"));

    // A non-default port is appended.
    RUVIA_CHECK_EQ(authorityFor(originFor("example.com", 8080, false)), std::string("example.com:8080"));

    // The default depends on the scheme: 80 is not default under TLS, and 443 is
    // not default without TLS, so each is included.
    RUVIA_CHECK_EQ(authorityFor(originFor("example.com", 80, true)), std::string("example.com:80"));
    RUVIA_CHECK_EQ(authorityFor(originFor("example.com", 443, false)), std::string("example.com:443"));

    // An IPv6 literal (the host contains ':') is wrapped in brackets, and the
    // default-port omission still applies inside the bracketed form.
    RUVIA_CHECK_EQ(authorityFor(originFor("::1", 80, false)), std::string("[::1]"));
    RUVIA_CHECK_EQ(authorityFor(originFor("::1", 8080, false)), std::string("[::1]:8080"));
    RUVIA_CHECK_EQ(authorityFor(originFor("2001:db8::1", 443, true)), std::string("[2001:db8::1]"));
}
