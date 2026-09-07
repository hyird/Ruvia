#include "context_services_fixture.h"
#include "test_harness.h"

// Who the client is behind a reverse proxy. The header is attacker-controlled,
// so the whole contract turns on the peer being one the deployment declared
// trustworthy; the default of trusting nobody must never read it.

#include <string_view>

#include "ruvia/core/memory/MemoryPool.h"
#include "ruvia/http/HttpHeader.h"
#include "ruvia/http/detail/request/HttpRequestAccess.h"
#include "ruvia/web/ConnInfo.h"
#include "ruvia/web/Context.h"
#include "ruvia/web/detail/http/context/ContextAccess.h"
#include "ruvia/web/detail/http/context/ContextServices.h"
#include "ruvia/web/detail/server/TrustedProxies.h"

namespace {

using ruvia::HttpHeaderView;
using ruvia::HttpRequest;
using ruvia::RequestMemory;
using ruvia::WorkerMemory;
using ruvia::detail::ContextAccess;
using ruvia::detail::ContextServices;
using ruvia::detail::HttpRequestAccess;
using ruvia::detail::TrustedProxyBlock;
using ruvia::detail::TrustedProxySet;

TrustedProxySet setOf(std::initializer_list<std::string_view> cidrs) {
    TrustedProxySet set;
    for (const auto cidr : cidrs) {
        TrustedProxyBlock block;
        if (ruvia::detail::parseTrustedProxyBlock(cidr, block)) {
            set.add(block);
        }
    }
    return set;
}

}  // namespace

RUVIA_TEST(trusted_proxy_cidr_parsing_accepts_addresses_and_blocks) {
    TrustedProxyBlock block;
    RUVIA_CHECK(ruvia::detail::parseTrustedProxyBlock("10.0.0.0/8", block));
    RUVIA_CHECK(ruvia::detail::parseTrustedProxyBlock("127.0.0.1", block));
    RUVIA_CHECK(ruvia::detail::parseTrustedProxyBlock("2001:db8::/32", block));

    // A typo must fail configuration rather than silently trust nothing.
    RUVIA_CHECK(!ruvia::detail::parseTrustedProxyBlock("10.0.0.0/33", block));
    RUVIA_CHECK(!ruvia::detail::parseTrustedProxyBlock("2001:db8::/129", block));
    RUVIA_CHECK(!ruvia::detail::parseTrustedProxyBlock("not-an-address", block));
    RUVIA_CHECK(!ruvia::detail::parseTrustedProxyBlock("10.0.0.0/", block));
}

RUVIA_TEST(trusted_proxy_matching_spans_both_families) {
    const auto set = setOf({"10.0.0.0/8", "2001:db8::/32"});
    RUVIA_CHECK(set.trusts("10.1.2.3"));
    RUVIA_CHECK(!set.trusts("11.1.2.3"));
    RUVIA_CHECK(set.trusts("2001:db8::1"));
    RUVIA_CHECK(!set.trusts("2001:dba::1"));

    // The same host arriving as an IPv4-mapped IPv6 peer is still that host.
    RUVIA_CHECK(set.trusts("::ffff:10.1.2.3"));
    RUVIA_CHECK(!set.trusts("::ffff:11.1.2.3"));

    RUVIA_CHECK(!TrustedProxySet{}.trusts("10.1.2.3"));
}

RUVIA_TEST(conn_info_ignores_forwarding_headers_from_an_untrusted_peer) {
    WorkerMemory worker;
    HttpRequest request = HttpRequestAccess::make();
    HttpRequestAccess::reset(request);
    (void)HttpRequestAccess::addHeader(request, HttpHeaderView{"X-Forwarded-For", "203.0.113.9"});
    (void)HttpRequestAccess::addHeader(request, HttpHeaderView{"X-Forwarded-Proto", "https"});

    RequestMemory memory(worker);
    HttpRequestAccess::setResource(request, memory.resource());

    // No trusted set at all: the default, and it must read nothing.
    const auto context = ContextAccess::make(
        memory, request, ruvia::test::testContextServices().withPlainTransport("198.51.100.7"));
    const auto info = ruvia::getConnInfo(context);
    RUVIA_CHECK_EQ(info.client().address(), std::string_view("198.51.100.7"));
    RUVIA_CHECK(info.scheme() == ruvia::HttpScheme::kHttp);
    RUVIA_CHECK(!info.viaTrustedProxy());

    // Configured, but this peer is not in it.
    const auto trusted = setOf({"10.0.0.0/8"});
    const auto guarded = ContextAccess::make(memory, request,
        ruvia::test::testContextServices()
            .withPlainTransport("198.51.100.7")
            .withTrustedProxies(trusted));
    const auto guardedInfo = ruvia::getConnInfo(guarded);
    RUVIA_CHECK_EQ(guardedInfo.client().address(), std::string_view("198.51.100.7"));
    RUVIA_CHECK(guardedInfo.scheme() == ruvia::HttpScheme::kHttp);
}

RUVIA_TEST(conn_info_resolves_client_from_a_trusted_peer_x_forwarded_headers) {
    WorkerMemory worker;
    HttpRequest request = HttpRequestAccess::make();
    HttpRequestAccess::reset(request);
    // 10.0.0.5 is the trusted hop and is skipped; 203.0.113.9 is the caller.
    (void)HttpRequestAccess::addHeader(
        request, HttpHeaderView{"X-Forwarded-For", "203.0.113.9, 10.0.0.5"});
    (void)HttpRequestAccess::addHeader(request, HttpHeaderView{"X-Forwarded-Proto", "https"});

    RequestMemory memory(worker);
    HttpRequestAccess::setResource(request, memory.resource());

    const auto trusted = setOf({"10.0.0.0/8"});
    const auto context = ContextAccess::make(memory, request,
        ruvia::test::testContextServices()
            .withPlainTransport("10.0.0.5")
            .withTrustedProxies(trusted));
    const auto info = ruvia::getConnInfo(context);

    RUVIA_CHECK_EQ(info.client().address(), std::string_view("203.0.113.9"));
    // remote() still reports the hop, unchanged.
    RUVIA_CHECK_EQ(info.remote().address(), std::string_view("10.0.0.5"));
    RUVIA_CHECK(info.scheme() == ruvia::HttpScheme::kHttps);
    RUVIA_CHECK(info.viaTrustedProxy());
    // The hop itself is plaintext: the end-to-end scheme is not a synonym for tls().
    RUVIA_CHECK(info.tls() == nullptr);
}

RUVIA_TEST(conn_info_reads_rfc7239_forwarded_when_x_headers_are_absent) {
    WorkerMemory worker;
    HttpRequest request = HttpRequestAccess::make();
    HttpRequestAccess::reset(request);
    (void)HttpRequestAccess::addHeader(request,
        HttpHeaderView{"Forwarded", R"(for="[2001:db8::1]:4711";proto=https, for=10.0.0.5)"});

    RequestMemory memory(worker);
    HttpRequestAccess::setResource(request, memory.resource());

    const auto trusted = setOf({"10.0.0.0/8"});
    const auto context = ContextAccess::make(memory, request,
        ruvia::test::testContextServices()
            .withPlainTransport("10.0.0.5")
            .withTrustedProxies(trusted));
    const auto info = ruvia::getConnInfo(context);

    // Bracketed IPv6 with a port, unwrapped. No X- header to consult.
    RUVIA_CHECK_EQ(info.client().address(), std::string_view("2001:db8::1"));
    RUVIA_CHECK(info.scheme() == ruvia::HttpScheme::kHttps);
}

RUVIA_TEST(conn_info_prefers_proxy_written_x_headers_over_client_forwarded) {
    WorkerMemory worker;
    HttpRequest request = HttpRequestAccess::make();
    HttpRequestAccess::reset(request);
    // nginx rewrites X-Forwarded-* and forwards a client Forwarded field
    // unchanged. Believing Forwarded here would let the caller spoof ConnInfo.
    (void)HttpRequestAccess::addHeader(
        request, HttpHeaderView{"Forwarded", "for=198.51.100.1;proto=https"});
    (void)HttpRequestAccess::addHeader(request, HttpHeaderView{"X-Forwarded-For", "203.0.113.9"});
    (void)HttpRequestAccess::addHeader(request, HttpHeaderView{"X-Forwarded-Proto", "http"});

    RequestMemory memory(worker);
    HttpRequestAccess::setResource(request, memory.resource());

    const auto trusted = setOf({"10.0.0.0/8"});
    const auto context = ContextAccess::make(memory, request,
        ruvia::test::testContextServices()
            .withPlainTransport("10.0.0.5")
            .withTrustedProxies(trusted));
    const auto info = ruvia::getConnInfo(context);
    RUVIA_CHECK_EQ(info.client().address(), std::string_view("203.0.113.9"));
    RUVIA_CHECK(info.scheme() == ruvia::HttpScheme::kHttp);
}

RUVIA_TEST(conn_info_forwarded_quoted_comma_does_not_invent_a_hop) {
    WorkerMemory worker;
    HttpRequest request = HttpRequestAccess::make();
    HttpRequestAccess::reset(request);
    // RFC 7239 quoted-string may contain commas. A quote-blind split of
    // for="_x,203.0.113.9" would invent a last hop and let it win as client.
    (void)HttpRequestAccess::addHeader(
        request, HttpHeaderView{"Forwarded", R"(for=10.0.0.5, for="_x,203.0.113.9")"});

    RequestMemory memory(worker);
    HttpRequestAccess::setResource(request, memory.resource());

    const auto trusted = setOf({"10.0.0.0/8"});
    const auto context = ContextAccess::make(memory, request,
        ruvia::test::testContextServices()
            .withPlainTransport("10.0.0.5")
            .withTrustedProxies(trusted));
    const auto info = ruvia::getConnInfo(context);
    RUVIA_CHECK_EQ(info.client().address(), std::string_view("_x,203.0.113.9"));
}

RUVIA_TEST(conn_info_forwarded_quoted_semicolon_stays_one_parameter) {
    WorkerMemory worker;
    HttpRequest request = HttpRequestAccess::make();
    HttpRequestAccess::reset(request);
    // A quote-blind ';' split of for="_x;203.0.113.9" would drop the identifier
    // and invent a second pair from inside the quotes.
    (void)HttpRequestAccess::addHeader(
        request, HttpHeaderView{"Forwarded", R"(for=10.0.0.5, for="_x;203.0.113.9")"});

    RequestMemory memory(worker);
    HttpRequestAccess::setResource(request, memory.resource());

    const auto trusted = setOf({"10.0.0.0/8"});
    const auto context = ContextAccess::make(memory, request,
        ruvia::test::testContextServices()
            .withPlainTransport("10.0.0.5")
            .withTrustedProxies(trusted));
    const auto info = ruvia::getConnInfo(context);
    RUVIA_CHECK_EQ(info.client().address(), std::string_view("_x;203.0.113.9"));
}

RUVIA_TEST(conn_info_walks_same_name_forwarding_fields_as_one_chain) {
    WorkerMemory worker;
    HttpRequest request = HttpRequestAccess::make();
    HttpRequestAccess::reset(request);
    // RFC 9110 §5.2: multiple list-field lines combine. A client-injected
    // first line must not win over the proxy-appended line that follows.
    (void)HttpRequestAccess::addHeader(request, HttpHeaderView{"X-Forwarded-For", "198.51.100.1"});
    (void)HttpRequestAccess::addHeader(request, HttpHeaderView{"X-Forwarded-For", "203.0.113.9"});
    (void)HttpRequestAccess::addHeader(request, HttpHeaderView{"X-Forwarded-Proto", "https"});
    (void)HttpRequestAccess::addHeader(request, HttpHeaderView{"X-Forwarded-Proto", "http"});

    RequestMemory memory(worker);
    HttpRequestAccess::setResource(request, memory.resource());

    const auto trusted = setOf({"10.0.0.0/8"});
    const auto context = ContextAccess::make(memory, request,
        ruvia::test::testContextServices()
            .withPlainTransport("10.0.0.5")
            .withTrustedProxies(trusted));
    const auto info = ruvia::getConnInfo(context);
    RUVIA_CHECK_EQ(info.client().address(), std::string_view("203.0.113.9"));
    RUVIA_CHECK(info.scheme() == ruvia::HttpScheme::kHttp);
}

RUVIA_TEST(conn_info_walks_same_name_rfc7239_fields_as_one_chain) {
    WorkerMemory worker;
    HttpRequest request = HttpRequestAccess::make();
    HttpRequestAccess::reset(request);
    (void)HttpRequestAccess::addHeader(
        request, HttpHeaderView{"Forwarded", "for=198.51.100.1;proto=https"});
    (void)HttpRequestAccess::addHeader(
        request, HttpHeaderView{"Forwarded", "for=203.0.113.9;proto=http"});

    RequestMemory memory(worker);
    HttpRequestAccess::setResource(request, memory.resource());

    const auto trusted = setOf({"10.0.0.0/8"});
    const auto context = ContextAccess::make(memory, request,
        ruvia::test::testContextServices()
            .withPlainTransport("10.0.0.5")
            .withTrustedProxies(trusted));
    const auto info = ruvia::getConnInfo(context);
    RUVIA_CHECK_EQ(info.client().address(), std::string_view("203.0.113.9"));
    RUVIA_CHECK(info.scheme() == ruvia::HttpScheme::kHttp);
}

RUVIA_TEST(conn_info_forwarded_skips_client_prepended_hops) {
    WorkerMemory worker;
    HttpRequest request = HttpRequestAccess::make();
    HttpRequestAccess::reset(request);
    (void)HttpRequestAccess::addHeader(request,
        HttpHeaderView{
            "Forwarded", R"(for=198.51.100.1;proto=https, for=203.0.113.9;proto=http)"});

    RequestMemory memory(worker);
    HttpRequestAccess::setResource(request, memory.resource());

    const auto trusted = setOf({"10.0.0.0/8"});
    const auto context = ContextAccess::make(memory, request,
        ruvia::test::testContextServices()
            .withPlainTransport("10.0.0.5")
            .withTrustedProxies(trusted));
    const auto info = ruvia::getConnInfo(context);
    RUVIA_CHECK_EQ(info.client().address(), std::string_view("203.0.113.9"));
    RUVIA_CHECK(info.scheme() == ruvia::HttpScheme::kHttp);
}

RUVIA_TEST(conn_info_forwarded_ignores_proto_on_prepended_hops) {
    WorkerMemory worker;
    HttpRequest request = HttpRequestAccess::make();
    HttpRequestAccess::reset(request);
    // The last untrusted hop omitted proto. A prepended proto=https must not
    // become the client scheme -- that is how a caller claims TLS.
    (void)HttpRequestAccess::addHeader(
        request, HttpHeaderView{"Forwarded", "for=198.51.100.1;proto=https, for=203.0.113.9"});

    RequestMemory memory(worker);
    HttpRequestAccess::setResource(request, memory.resource());

    const auto trusted = setOf({"10.0.0.0/8"});
    const auto context = ContextAccess::make(memory, request,
        ruvia::test::testContextServices()
            .withPlainTransport("10.0.0.5")
            .withTrustedProxies(trusted));
    const auto info = ruvia::getConnInfo(context);
    RUVIA_CHECK_EQ(info.client().address(), std::string_view("203.0.113.9"));
    RUVIA_CHECK(info.scheme() == ruvia::HttpScheme::kHttp);
    RUVIA_CHECK(info.viaTrustedProxy());
}

RUVIA_TEST(conn_info_forwarded_proto_is_case_insensitive) {
    // RFC 7239 §5.4 proto tokens are ABNF strings, so "HTTPS" is "https".
    WorkerMemory worker;
    HttpRequest request = HttpRequestAccess::make();
    HttpRequestAccess::reset(request);
    (void)HttpRequestAccess::addHeader(
        request, HttpHeaderView{"Forwarded", "for=203.0.113.9;proto=HTTPS"});

    RequestMemory memory(worker);
    HttpRequestAccess::setResource(request, memory.resource());

    const auto trusted = setOf({"10.0.0.0/8"});
    const auto context = ContextAccess::make(memory, request,
        ruvia::test::testContextServices()
            .withPlainTransport("10.0.0.5")
            .withTrustedProxies(trusted));
    const auto info = ruvia::getConnInfo(context);
    RUVIA_CHECK(info.scheme() == ruvia::HttpScheme::kHttps);
    RUVIA_CHECK(info.viaTrustedProxy());
}

RUVIA_TEST(conn_info_x_forwarded_proto_is_case_insensitive_and_uses_the_last_hop) {
    WorkerMemory worker;
    HttpRequest request = HttpRequestAccess::make();
    HttpRequestAccess::reset(request);
    (void)HttpRequestAccess::addHeader(request, HttpHeaderView{"X-Forwarded-For", "203.0.113.9"});
    // Some TLS-terminating proxies emit uppercase HTTPS. The last token is the
    // hop that delivered the request; a client-prepended https must not win.
    (void)HttpRequestAccess::addHeader(
        request, HttpHeaderView{"X-Forwarded-Proto", "http, HTTPS"});

    RequestMemory memory(worker);
    HttpRequestAccess::setResource(request, memory.resource());

    const auto trusted = setOf({"10.0.0.0/8"});
    const auto context = ContextAccess::make(memory, request,
        ruvia::test::testContextServices()
            .withPlainTransport("10.0.0.5")
            .withTrustedProxies(trusted));
    const auto info = ruvia::getConnInfo(context);
    RUVIA_CHECK_EQ(info.client().address(), std::string_view("203.0.113.9"));
    RUVIA_CHECK(info.scheme() == ruvia::HttpScheme::kHttps);
}

RUVIA_TEST(conn_info_ignores_client_prepended_forwarding_hops) {
    WorkerMemory worker;
    HttpRequest request = HttpRequestAccess::make();
    HttpRequestAccess::reset(request);
    (void)HttpRequestAccess::addHeader(
        request, HttpHeaderView{"X-Forwarded-For", "198.51.100.1, 203.0.113.9"});
    (void)HttpRequestAccess::addHeader(
        request, HttpHeaderView{"X-Forwarded-Proto", "https, http"});

    RequestMemory memory(worker);
    HttpRequestAccess::setResource(request, memory.resource());

    const auto trusted = setOf({"10.0.0.0/8"});
    const auto context = ContextAccess::make(memory, request,
        ruvia::test::testContextServices()
            .withPlainTransport("10.0.0.5")
            .withTrustedProxies(trusted));
    const auto info = ruvia::getConnInfo(context);
    RUVIA_CHECK_EQ(info.client().address(), std::string_view("203.0.113.9"));
    RUVIA_CHECK(info.scheme() == ruvia::HttpScheme::kHttp);
}

RUVIA_TEST(conn_info_keeps_transport_values_for_fields_the_proxy_omitted) {
    WorkerMemory worker;
    HttpRequest request = HttpRequestAccess::make();
    HttpRequestAccess::reset(request);
    // Address only: the scheme must stay whatever the transport says.
    (void)HttpRequestAccess::addHeader(request, HttpHeaderView{"X-Forwarded-For", "203.0.113.9"});

    RequestMemory memory(worker);
    HttpRequestAccess::setResource(request, memory.resource());

    const auto trusted = setOf({"10.0.0.0/8"});
    const auto context = ContextAccess::make(memory, request,
        ruvia::test::testContextServices()
            .withTlsTransport("10.0.0.5", "CN=proxy")
            .withTrustedProxies(trusted));
    const auto info = ruvia::getConnInfo(context);

    RUVIA_CHECK_EQ(info.client().address(), std::string_view("203.0.113.9"));
    RUVIA_CHECK(info.scheme() == ruvia::HttpScheme::kHttps);
    RUVIA_CHECK(info.tls() != nullptr);
}
