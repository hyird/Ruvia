#include "context_services_fixture.h"
#include "test_harness.h"

// Who the client is behind a reverse proxy. The header is attacker-controlled,
// so the whole contract turns on the peer being one the deployment declared
// trustworthy; the default of trusting nobody must never read it.

#include <cstddef>
#include <initializer_list>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/core/memory/MemoryPool.h"
#include "ruvia/http/HttpHeader.h"
#include "ruvia/http/HttpRequest.h"
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
using ruvia::detail::TrustedProxySet;

HttpRequest makeRequest(RequestMemory& memory, std::initializer_list<HttpHeaderView> headers) {
    const auto headerSpan = std::span<const HttpHeaderView>(headers.begin(), headers.size());
    auto [request, error] = ruvia::makeParsedHttpRequest(
        "GET", "/", headerSpan, {}, memory.resource());
    if (error) {
        throw std::runtime_error("invalid test request");
    }
    return std::move(request);
}

TrustedProxySet setOf(std::initializer_list<std::string_view> cidrs) {
    TrustedProxySet set;
    for (const auto cidr : cidrs) {
        if (const auto block = ruvia::detail::parseTrustedProxyBlock(cidr)) {
            set.add(*block);
        }
    }
    return set;
}

}  // namespace

RUVIA_TEST(trusted_proxy_cidr_parsing_accepts_addresses_and_blocks) {
    RUVIA_CHECK(ruvia::detail::parseTrustedProxyBlock("10.0.0.0/8"));
    RUVIA_CHECK(ruvia::detail::parseTrustedProxyBlock("127.0.0.1"));
    RUVIA_CHECK(ruvia::detail::parseTrustedProxyBlock("2001:db8::/32"));

    // A typo must fail configuration rather than silently trust nothing.
    RUVIA_CHECK(!ruvia::detail::parseTrustedProxyBlock("10.0.0.0/33"));
    RUVIA_CHECK(!ruvia::detail::parseTrustedProxyBlock("2001:db8::/129"));
    RUVIA_CHECK(!ruvia::detail::parseTrustedProxyBlock("not-an-address"));
    RUVIA_CHECK(!ruvia::detail::parseTrustedProxyBlock("10.0.0.0/"));
}

RUVIA_TEST(trusted_proxy_parsing_returns_typed_errors_and_complete_networks) {
    using ruvia::detail::parseTrustedProxyBlock;
    using ruvia::detail::TrustedProxyParseError;
    const auto invalidAddress = parseTrustedProxyBlock("invalid/8");
    RUVIA_CHECK(!invalidAddress && invalidAddress.error() == TrustedProxyParseError::kInvalidAddress);
    for (const auto cidr : {"10.0.0.0/33", "10.0.0.0/-1", "10.0.0.0/", "2001:db8::/129"}) {
        const auto invalidPrefix = parseTrustedProxyBlock(cidr);
        RUVIA_CHECK(!invalidPrefix && invalidPrefix.error() == TrustedProxyParseError::kInvalidPrefix);
    }
    const auto v4 = parseTrustedProxyBlock("10.1.2.3/8");
    RUVIA_CHECK(v4 && v4->prefixBits == 104 && v4->network[12] == 10);
    const auto v6 = parseTrustedProxyBlock("2001:db8::/32");
    RUVIA_CHECK(v6 && v6->prefixBits == 32 && v6->network[0] == 0x20);
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

RUVIA_TEST(trusted_proxy_set_matches_later_blocks_and_partial_prefixes) {
    const auto set = setOf({"192.0.2.0/24", "2001:db8:1::/48", "10.8.0.0/13", "2001:db8:8000::/33"});
    RUVIA_CHECK(set.trusts("10.15.255.255"));
    RUVIA_CHECK(set.trusts("::ffff:10.8.0.1"));
    RUVIA_CHECK(!set.trusts("10.16.0.1"));
    RUVIA_CHECK(set.trusts("2001:db8:ffff::1"));
    RUVIA_CHECK(!set.trusts("2001:db8:7fff::1"));
    RUVIA_CHECK(!set.trusts("not-an-address"));
    RUVIA_CHECK(!set.trusts(""));
    // A match must not be retained across calls on the same immutable set.
    RUVIA_CHECK(set.trusts("192.0.2.1"));
    RUVIA_CHECK(!set.trusts("192.0.3.1"));
}

RUVIA_TEST(trusted_proxy_address_parsing_consumes_the_complete_view) {
    const auto set = setOf({"10.0.0.0/8", "2001:db8::/32"});
    const auto block = ruvia::detail::parseTrustedProxyBlock("10.0.0.0/8");
    RUVIA_CHECK(block.has_value());
    if (!block) {
        return;
    }
    for (const auto prefix : {"10.1.2.3", "::ffff:10.1.2.3", "2001:db8::1"}) {
        std::string malformed(prefix);
        malformed.push_back('\0');
        RUVIA_CHECK(!set.trusts(malformed));
        RUVIA_CHECK(!ruvia::detail::trustedProxyBlockContains(*block, malformed));
        malformed.append("unparsed");
        RUVIA_CHECK(!set.trusts(malformed));
        RUVIA_CHECK(!ruvia::detail::parseTrustedProxyBlock(malformed));
        malformed.append("/8");
        RUVIA_CHECK(!ruvia::detail::parseTrustedProxyBlock(malformed));
    }
    constexpr std::string_view storage = "10.1.2.3suffix";
    RUVIA_CHECK(set.trusts(storage.substr(0, 8)));
    RUVIA_CHECK(!set.trusts(storage));
}

RUVIA_TEST(trusted_proxy_matching_handles_long_and_scoped_address_text) {
    const auto set = setOf({"2001:db8::/32", "fe80::/10"});
    RUVIA_CHECK(set.trusts("2001:0db8:1234:5678:90ab:cdef:1234:5678"));
    RUVIA_CHECK(!set.trusts("2001:0db9:1234:5678:90ab:cdef:1234:5678"));
    RUVIA_CHECK(set.trusts("fe80::1%12"));
    for (const std::size_t size : {63u, 64u, 65u, 1024u}) {
        const std::string malformed(size, 'x');
        RUVIA_CHECK(!set.trusts(malformed));
        RUVIA_CHECK(!ruvia::detail::parseTrustedProxyBlock(malformed));
    }
}

RUVIA_TEST(conn_info_ignores_forwarding_headers_from_an_untrusted_peer) {
    WorkerMemory worker;
    RequestMemory memory(worker);
    HttpRequest request = makeRequest(memory, {HttpHeaderView{"X-Forwarded-For", "203.0.113.9"},
                                                  HttpHeaderView{"X-Forwarded-Proto", "https"}});

    // No trusted set at all: the default, and it must read nothing.
    const auto context = ContextAccess::make(
        memory, request, ruvia::test::testContextServices().withPlainTransport("198.51.100.7"));
    const auto info = context.conn();
    RUVIA_CHECK_EQ(info.client().address(), std::string_view("198.51.100.7"));
    RUVIA_CHECK(info.scheme() == ruvia::HttpScheme::kHttp);
    RUVIA_CHECK(!info.viaTrustedProxy());

    // Configured, but this peer is not in it.
    const auto trusted = setOf({"10.0.0.0/8"});
    const auto guarded = ContextAccess::make(memory, request,
        ruvia::test::testContextServices()
            .withPlainTransport("198.51.100.7")
            .withTrustedProxies(trusted));
    const auto guardedInfo = guarded.conn();
    RUVIA_CHECK_EQ(guardedInfo.client().address(), std::string_view("198.51.100.7"));
    RUVIA_CHECK(guardedInfo.scheme() == ruvia::HttpScheme::kHttp);
}

RUVIA_TEST(conn_info_resolves_client_from_a_trusted_peer_x_forwarded_headers) {
    WorkerMemory worker;
    RequestMemory memory(worker);
    HttpRequest request = makeRequest(memory, {HttpHeaderView{"X-Forwarded-For", "203.0.113.9, 10.0.0.5"},
                                                  HttpHeaderView{"X-Forwarded-Proto", "https"}});

    const auto trusted = setOf({"10.0.0.0/8"});
    const auto context = ContextAccess::make(memory, request,
        ruvia::test::testContextServices()
            .withPlainTransport("10.0.0.5")
            .withTrustedProxies(trusted));
    const auto info = context.conn();

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
    RequestMemory memory(worker);
    HttpRequest request = makeRequest(memory, {HttpHeaderView{"Forwarded", R"(for="[2001:db8::1]:4711";proto=https, for=10.0.0.5)"}});

    const auto trusted = setOf({"10.0.0.0/8"});
    const auto context = ContextAccess::make(memory, request,
        ruvia::test::testContextServices()
            .withPlainTransport("10.0.0.5")
            .withTrustedProxies(trusted));
    const auto info = context.conn();

    // Bracketed IPv6 with a port, unwrapped. No X- header to consult.
    RUVIA_CHECK_EQ(info.client().address(), std::string_view("2001:db8::1"));
    RUVIA_CHECK(info.scheme() == ruvia::HttpScheme::kHttps);
}

RUVIA_TEST(conn_info_prefers_proxy_written_x_headers_over_client_forwarded) {
    WorkerMemory worker;
    RequestMemory memory(worker);
    HttpRequest request = makeRequest(memory, {HttpHeaderView{"Forwarded", "for=198.51.100.1;proto=https"},
                                                  HttpHeaderView{"X-Forwarded-For", "203.0.113.9"},
                                                  HttpHeaderView{"X-Forwarded-Proto", "http"}});

    const auto trusted = setOf({"10.0.0.0/8"});
    const auto context = ContextAccess::make(memory, request,
        ruvia::test::testContextServices()
            .withPlainTransport("10.0.0.5")
            .withTrustedProxies(trusted));
    const auto info = context.conn();
    RUVIA_CHECK_EQ(info.client().address(), std::string_view("203.0.113.9"));
    RUVIA_CHECK(info.scheme() == ruvia::HttpScheme::kHttp);
}

RUVIA_TEST(conn_info_forwarded_quoted_comma_does_not_invent_a_hop) {
    WorkerMemory worker;
    RequestMemory memory(worker);
    HttpRequest request = makeRequest(memory, {HttpHeaderView{"Forwarded", R"(for=10.0.0.5, for="_x,203.0.113.9")"}});

    const auto trusted = setOf({"10.0.0.0/8"});
    const auto context = ContextAccess::make(memory, request,
        ruvia::test::testContextServices()
            .withPlainTransport("10.0.0.5")
            .withTrustedProxies(trusted));
    const auto info = context.conn();
    RUVIA_CHECK_EQ(info.client().address(), std::string_view("_x,203.0.113.9"));
}

RUVIA_TEST(conn_info_forwarded_quoted_semicolon_stays_one_parameter) {
    WorkerMemory worker;
    RequestMemory memory(worker);
    HttpRequest request = makeRequest(memory, {HttpHeaderView{"Forwarded", R"(for=10.0.0.5, for="_x;203.0.113.9")"}});

    const auto trusted = setOf({"10.0.0.0/8"});
    const auto context = ContextAccess::make(memory, request,
        ruvia::test::testContextServices()
            .withPlainTransport("10.0.0.5")
            .withTrustedProxies(trusted));
    const auto info = context.conn();
    RUVIA_CHECK_EQ(info.client().address(), std::string_view("_x;203.0.113.9"));
}

RUVIA_TEST(conn_info_walks_same_name_forwarding_fields_as_one_chain) {
    WorkerMemory worker;
    RequestMemory memory(worker);
    HttpRequest request = makeRequest(memory, {HttpHeaderView{"X-Forwarded-For", "198.51.100.1"},
                                                  HttpHeaderView{"X-Forwarded-For", "203.0.113.9"},
                                                  HttpHeaderView{"X-Forwarded-Proto", "https"},
                                                  HttpHeaderView{"X-Forwarded-Proto", "http"}});

    const auto trusted = setOf({"10.0.0.0/8"});
    const auto context = ContextAccess::make(memory, request,
        ruvia::test::testContextServices()
            .withPlainTransport("10.0.0.5")
            .withTrustedProxies(trusted));
    const auto info = context.conn();
    RUVIA_CHECK_EQ(info.client().address(), std::string_view("203.0.113.9"));
    RUVIA_CHECK(info.scheme() == ruvia::HttpScheme::kHttp);
}

RUVIA_TEST(conn_info_walks_same_name_rfc7239_fields_as_one_chain) {
    WorkerMemory worker;
    RequestMemory memory(worker);
    HttpRequest request = makeRequest(memory, {HttpHeaderView{"Forwarded", "for=198.51.100.1;proto=https"},
                                                  HttpHeaderView{"Forwarded", "for=203.0.113.9;proto=http"}});

    const auto trusted = setOf({"10.0.0.0/8"});
    const auto context = ContextAccess::make(memory, request,
        ruvia::test::testContextServices()
            .withPlainTransport("10.0.0.5")
            .withTrustedProxies(trusted));
    const auto info = context.conn();
    RUVIA_CHECK_EQ(info.client().address(), std::string_view("203.0.113.9"));
    RUVIA_CHECK(info.scheme() == ruvia::HttpScheme::kHttp);
}

RUVIA_TEST(conn_info_forwarded_skips_client_prepended_hops) {
    WorkerMemory worker;
    RequestMemory memory(worker);
    HttpRequest request = makeRequest(memory, {HttpHeaderView{
                                                  "Forwarded", R"(for=198.51.100.1;proto=https, for=203.0.113.9;proto=http)"}});

    const auto trusted = setOf({"10.0.0.0/8"});
    const auto context = ContextAccess::make(memory, request,
        ruvia::test::testContextServices()
            .withPlainTransport("10.0.0.5")
            .withTrustedProxies(trusted));
    const auto info = context.conn();
    RUVIA_CHECK_EQ(info.client().address(), std::string_view("203.0.113.9"));
    RUVIA_CHECK(info.scheme() == ruvia::HttpScheme::kHttp);
}

RUVIA_TEST(conn_info_forwarded_ignores_proto_on_prepended_hops) {
    WorkerMemory worker;
    RequestMemory memory(worker);
    HttpRequest request = makeRequest(memory, {HttpHeaderView{"Forwarded", "for=198.51.100.1;proto=https, for=203.0.113.9"}});

    const auto trusted = setOf({"10.0.0.0/8"});
    const auto context = ContextAccess::make(memory, request,
        ruvia::test::testContextServices()
            .withPlainTransport("10.0.0.5")
            .withTrustedProxies(trusted));
    const auto info = context.conn();
    RUVIA_CHECK_EQ(info.client().address(), std::string_view("203.0.113.9"));
    RUVIA_CHECK(info.scheme() == ruvia::HttpScheme::kHttp);
    RUVIA_CHECK(info.viaTrustedProxy());
}

RUVIA_TEST(conn_info_forwarded_proto_is_case_insensitive) {
    // RFC 7239 §5.4 proto tokens are ABNF strings, so "HTTPS" is "https".
    WorkerMemory worker;
    RequestMemory memory(worker);
    HttpRequest request = makeRequest(memory, {HttpHeaderView{"Forwarded", "for=203.0.113.9;proto=HTTPS"}});

    const auto trusted = setOf({"10.0.0.0/8"});
    const auto context = ContextAccess::make(memory, request,
        ruvia::test::testContextServices()
            .withPlainTransport("10.0.0.5")
            .withTrustedProxies(trusted));
    const auto info = context.conn();
    RUVIA_CHECK(info.scheme() == ruvia::HttpScheme::kHttps);
    RUVIA_CHECK(info.viaTrustedProxy());
}

RUVIA_TEST(conn_info_x_forwarded_proto_is_case_insensitive_and_uses_the_last_hop) {
    WorkerMemory worker;
    RequestMemory memory(worker);
    HttpRequest request = makeRequest(memory, {HttpHeaderView{"X-Forwarded-For", "203.0.113.9"},
                                                  HttpHeaderView{"X-Forwarded-Proto", "http, HTTPS"}});

    const auto trusted = setOf({"10.0.0.0/8"});
    const auto context = ContextAccess::make(memory, request,
        ruvia::test::testContextServices()
            .withPlainTransport("10.0.0.5")
            .withTrustedProxies(trusted));
    const auto info = context.conn();
    RUVIA_CHECK_EQ(info.client().address(), std::string_view("203.0.113.9"));
    RUVIA_CHECK(info.scheme() == ruvia::HttpScheme::kHttps);
}

RUVIA_TEST(conn_info_ignores_client_prepended_forwarding_hops) {
    WorkerMemory worker;
    RequestMemory memory(worker);
    HttpRequest request = makeRequest(memory, {HttpHeaderView{"X-Forwarded-For", "198.51.100.1, 203.0.113.9"},
                                                  HttpHeaderView{"X-Forwarded-Proto", "https, http"}});

    const auto trusted = setOf({"10.0.0.0/8"});
    const auto context = ContextAccess::make(memory, request,
        ruvia::test::testContextServices()
            .withPlainTransport("10.0.0.5")
            .withTrustedProxies(trusted));
    const auto info = context.conn();
    RUVIA_CHECK_EQ(info.client().address(), std::string_view("203.0.113.9"));
    RUVIA_CHECK(info.scheme() == ruvia::HttpScheme::kHttp);
}

RUVIA_TEST(conn_info_keeps_transport_values_for_fields_the_proxy_omitted) {
    WorkerMemory worker;
    RequestMemory memory(worker);
    HttpRequest request = makeRequest(memory, {HttpHeaderView{"X-Forwarded-For", "203.0.113.9"}});

    const auto trusted = setOf({"10.0.0.0/8"});
    const auto context = ContextAccess::make(memory, request,
        ruvia::test::testContextServices()
            .withTlsTransport("10.0.0.5", "CN=proxy")
            .withTrustedProxies(trusted));
    const auto info = context.conn();

    RUVIA_CHECK_EQ(info.client().address(), std::string_view("203.0.113.9"));
    RUVIA_CHECK(info.scheme() == ruvia::HttpScheme::kHttps);
    RUVIA_CHECK(info.tls() != nullptr);
}
