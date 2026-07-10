#include "test_harness.h"

#include <cstdint>
#include <memory_resource>
#include <string>
#include <string_view>

#include "ruvia/http/detail/client/HttpClientRedirect.h"
#include "ruvia/http/HttpClient.h"

namespace {

using ruvia::HttpFetchOptions;
using ruvia::HttpOrigin;
using ruvia::detail::applyHttpClientRedirectMethod;
using ruvia::detail::httpClientAuthorityMatchesOrigin;
using ruvia::detail::isHttpClientRedirectStatus;
using ruvia::detail::resolveHttpClientSameOriginRedirect;

HttpOrigin originFor(std::string_view host, std::uint16_t port, bool tls = false) {
    HttpOrigin origin;
    origin.host.assign(host.data(), host.size());
    origin.port = port;
    origin.tls = tls;
    return origin;
}

std::string resolvePath(const HttpOrigin& origin, std::string_view location, bool& ok) {
    std::pmr::string outPath(std::pmr::get_default_resource());
    ok = resolveHttpClientSameOriginRedirect(origin, location, outPath);
    return std::string(outPath.data(), outPath.size());
}

}  // namespace

RUVIA_TEST(http_client_redirect_status_set) {
    for (const std::uint16_t s : {std::uint16_t{301}, std::uint16_t{302}, std::uint16_t{303},
                                  std::uint16_t{307}, std::uint16_t{308}}) {
        RUVIA_CHECK(isHttpClientRedirectStatus(s));
    }
    for (const std::uint16_t s : {std::uint16_t{200}, std::uint16_t{204}, std::uint16_t{300},
                                  std::uint16_t{304}, std::uint16_t{305}, std::uint16_t{306},
                                  std::uint16_t{399}, std::uint16_t{404}}) {
        RUVIA_CHECK(!isHttpClientRedirectStatus(s));
    }
}

RUVIA_TEST(http_client_redirect_method_rewrite_follows_rfc) {
    // 303 See Other: becomes GET (HEAD stays HEAD) and always drops the body.
    {
        HttpFetchOptions o;
        o.method = "POST";
        o.body = "payload";
        applyHttpClientRedirectMethod(o, 303);
        RUVIA_CHECK_EQ(o.method, std::string_view("GET"));
        RUVIA_CHECK(o.body.empty());
    }
    {
        HttpFetchOptions o;
        o.method = "HEAD";
        applyHttpClientRedirectMethod(o, 303);
        RUVIA_CHECK_EQ(o.method, std::string_view("HEAD"));  // HEAD is preserved
    }
    // 301/302: a non-GET/HEAD (e.g. POST) is rewritten to GET and the body dropped;
    // an existing GET is left untouched.
    {
        HttpFetchOptions o;
        o.method = "POST";
        o.body = "payload";
        applyHttpClientRedirectMethod(o, 302);
        RUVIA_CHECK_EQ(o.method, std::string_view("GET"));
        RUVIA_CHECK(o.body.empty());
    }
    {
        HttpFetchOptions o;
        o.method = "GET";
        applyHttpClientRedirectMethod(o, 301);
        RUVIA_CHECK_EQ(o.method, std::string_view("GET"));
    }
    // 307/308: method AND body are preserved -- the entire reason these codes exist.
    {
        HttpFetchOptions o;
        o.method = "POST";
        o.body = "payload";
        applyHttpClientRedirectMethod(o, 307);
        RUVIA_CHECK_EQ(o.method, std::string_view("POST"));
        RUVIA_CHECK_EQ(o.body, std::string_view("payload"));
    }
    {
        HttpFetchOptions o;
        o.method = "PUT";
        o.body = "payload";
        applyHttpClientRedirectMethod(o, 308);
        RUVIA_CHECK_EQ(o.method, std::string_view("PUT"));
        RUVIA_CHECK_EQ(o.body, std::string_view("payload"));
    }
}

RUVIA_TEST(http_client_authority_matches_origin) {
    const auto origin = originFor("example.com", 8080);
    RUVIA_CHECK(httpClientAuthorityMatchesOrigin(origin, "example.com:8080", 80));
    RUVIA_CHECK(httpClientAuthorityMatchesOrigin(origin, "example.com", 8080));      // port from default
    RUVIA_CHECK(!httpClientAuthorityMatchesOrigin(origin, "example.com:9090", 80));  // port mismatch
    RUVIA_CHECK(!httpClientAuthorityMatchesOrigin(origin, "other.com:8080", 80));    // host mismatch
    RUVIA_CHECK(!httpClientAuthorityMatchesOrigin(origin, "user@example.com:8080", 80));  // userinfo rejected
    RUVIA_CHECK(!httpClientAuthorityMatchesOrigin(origin, "example.com:0", 80));     // port 0 invalid

    const auto v6 = originFor("::1", 8080);
    RUVIA_CHECK(httpClientAuthorityMatchesOrigin(v6, "[::1]:8080", 80));
    RUVIA_CHECK(!httpClientAuthorityMatchesOrigin(v6, "[::2]:8080", 80));
}

RUVIA_TEST(http_client_same_origin_redirect_resolution) {
    const auto origin = originFor("example.com", 80, /*tls=*/false);
    bool ok = false;

    // An origin-form path is same-origin by definition.
    RUVIA_CHECK_EQ(resolvePath(origin, "/new/path", ok), std::string("/new/path"));
    RUVIA_CHECK(ok);
    // An absolute URL to the same origin resolves to just its path.
    RUVIA_CHECK_EQ(resolvePath(origin, "http://example.com/next", ok), std::string("/next"));
    RUVIA_CHECK(ok);
    // A protocol-relative reference to the same origin resolves too.
    RUVIA_CHECK_EQ(resolvePath(origin, "//example.com/rel", ok), std::string("/rel"));
    RUVIA_CHECK(ok);
    // A fragment is stripped from the resolved path.
    RUVIA_CHECK_EQ(resolvePath(origin, "http://example.com/frag#section", ok), std::string("/frag"));
    RUVIA_CHECK(ok);

    // Cross-origin, scheme-mismatch, userinfo, and empty are all refused: the client
    // follows only same-origin redirects, so credentials never leak to another origin.
    (void)resolvePath(origin, "http://evil.com/next", ok);
    RUVIA_CHECK(!ok);
    (void)resolvePath(origin, "https://example.com/next", ok);  // scheme disagrees with origin.tls
    RUVIA_CHECK(!ok);
    (void)resolvePath(origin, "http://user@example.com/next", ok);
    RUVIA_CHECK(!ok);
    (void)resolvePath(origin, "", ok);
    RUVIA_CHECK(!ok);
}
