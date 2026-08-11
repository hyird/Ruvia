#include "test_harness.h"

// Who the client is behind a reverse proxy. The header is attacker-controlled,
// so the whole contract turns on the peer being one the deployment declared
// trustworthy; the default of trusting nobody must never read it.

#include "ruvia/core/memory/MemoryPool.h"
#include "ruvia/http/HttpHeader.h"
#include "ruvia/http/detail/request/HttpRequestAccess.h"
#include "ruvia/web/ConnInfo.h"
#include "ruvia/web/Context.h"
#include "ruvia/web/detail/http/context/ContextAccess.h"
#include "ruvia/web/detail/http/context/ContextServices.h"
#include "ruvia/web/detail/server/TrustedProxies.h"

#include <string_view>

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
    const auto context = ContextAccess::make(memory, request, ContextServices{}.withPlainTransport("198.51.100.7"));
    const auto info = ruvia::getConnInfo(context);
    RUVIA_CHECK_EQ(info.client().address(), std::string_view("198.51.100.7"));
    RUVIA_CHECK_EQ(info.scheme(), std::string_view("http"));
    RUVIA_CHECK(!info.secure());
    RUVIA_CHECK(!info.viaTrustedProxy());

    // Configured, but this peer is not in it.
    const auto trusted = setOf({"10.0.0.0/8"});
    const auto guarded = ContextAccess::make(memory, request, ContextServices{}.withPlainTransport("198.51.100.7").withTrustedProxies(&trusted));
    const auto guardedInfo = ruvia::getConnInfo(guarded);
    RUVIA_CHECK_EQ(guardedInfo.client().address(), std::string_view("198.51.100.7"));
    RUVIA_CHECK(!guardedInfo.secure());
}

RUVIA_TEST(conn_info_resolves_client_from_a_trusted_peer_x_forwarded_headers) {
    WorkerMemory worker;
    HttpRequest request = HttpRequestAccess::make();
    HttpRequestAccess::reset(request);
    // Leftmost is the original client; the rest are intermediate hops.
    (void)HttpRequestAccess::addHeader(request, HttpHeaderView{"X-Forwarded-For", "203.0.113.9, 10.0.0.5"});
    (void)HttpRequestAccess::addHeader(request, HttpHeaderView{"X-Forwarded-Proto", "https"});

    RequestMemory memory(worker);
    HttpRequestAccess::setResource(request, memory.resource());

    const auto trusted = setOf({"10.0.0.0/8"});
    const auto context = ContextAccess::make(memory, request, ContextServices{}.withPlainTransport("10.0.0.5").withTrustedProxies(&trusted));
    const auto info = ruvia::getConnInfo(context);

    RUVIA_CHECK_EQ(info.client().address(), std::string_view("203.0.113.9"));
    // remote() still reports the hop, unchanged.
    RUVIA_CHECK_EQ(info.remote().address(), std::string_view("10.0.0.5"));
    RUVIA_CHECK_EQ(info.scheme(), std::string_view("https"));
    RUVIA_CHECK(info.secure());
    RUVIA_CHECK(info.viaTrustedProxy());
    // The hop itself is plaintext: secure() must not be a synonym for tls().
    RUVIA_CHECK(info.tls() == nullptr);
}

RUVIA_TEST(conn_info_prefers_rfc7239_forwarded_over_the_x_headers) {
    WorkerMemory worker;
    HttpRequest request = HttpRequestAccess::make();
    HttpRequestAccess::reset(request);
    (void)HttpRequestAccess::addHeader(request, HttpHeaderView{"Forwarded", R"(for="[2001:db8::1]:4711";proto=https, for=10.0.0.5)"});
    (void)HttpRequestAccess::addHeader(request, HttpHeaderView{"X-Forwarded-For", "198.51.100.99"});

    RequestMemory memory(worker);
    HttpRequestAccess::setResource(request, memory.resource());

    const auto trusted = setOf({"10.0.0.0/8"});
    const auto context = ContextAccess::make(memory, request, ContextServices{}.withPlainTransport("10.0.0.5").withTrustedProxies(&trusted));
    const auto info = ruvia::getConnInfo(context);

    // Bracketed IPv6 with a port, unwrapped, and the X- header not consulted.
    RUVIA_CHECK_EQ(info.client().address(), std::string_view("2001:db8::1"));
    RUVIA_CHECK(info.secure());
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
    const auto context = ContextAccess::make(memory, request, ContextServices{}.withTlsTransport("10.0.0.5", "CN=proxy").withTrustedProxies(&trusted));
    const auto info = ruvia::getConnInfo(context);

    RUVIA_CHECK_EQ(info.client().address(), std::string_view("203.0.113.9"));
    RUVIA_CHECK_EQ(info.scheme(), std::string_view("https"));
    RUVIA_CHECK(info.tls() != nullptr);
}
