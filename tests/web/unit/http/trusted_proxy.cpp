#include <variant>

#include "context_services_fixture.h"
#include "test_harness.h"

// Who the client is behind a reverse proxy. The header is attacker-controlled,
// so the whole contract turns on the peer being one the deployment declared
// trustworthy; the default of trusting nobody must never read it.

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/core/memory/memory_pool.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_request.h"
#include "ruvia/web/conn_info.h"
#include "ruvia/web/context.h"

#include "context/context_access.h"
#include "context/context_services.h"
#include "server/trusted_proxies.h"

namespace {

using ruvia::http_header_view;
using ruvia::http_request;
using ruvia::request_memory;
using ruvia::worker_memory;
using ruvia::detail::context_access;
using ruvia::detail::context_services;
using ruvia::detail::trusted_proxy_set;

http_request make_request(request_memory& memory, std::initializer_list<http_header_view> headers) {
    const auto header_span = std::span<const http_header_view>(headers.begin(), headers.size());
    auto [request, error] = ruvia::make_parsed_http_request(
        "GET", "/", header_span, {}, memory.resource());
    if (error) {
        throw std::runtime_error("invalid test request");
    }
    return std::move(request);
}

trusted_proxy_set set_of(std::initializer_list<std::string_view> cidrs) {
    trusted_proxy_set set;
    set.trust_x_forwarded_proto(true);
    for (const auto cidr : cidrs) {
        if (const auto block = ruvia::detail::parse_trusted_proxy_block(cidr); block.index() == 0) {
            set.add(std::get<0>(block));
        }
    }
    return set;
}

}  // namespace

RUVIA_TEST(trusted_proxy_cidr_parsing_accepts_addresses_and_blocks) {
    RUVIA_CHECK(ruvia::detail::parse_trusted_proxy_block("10.0.0.0/8").index() == 0);
    RUVIA_CHECK(ruvia::detail::parse_trusted_proxy_block("127.0.0.1").index() == 0);
    RUVIA_CHECK(ruvia::detail::parse_trusted_proxy_block("2001:db8::/32").index() == 0);

    // A typo must fail configuration rather than silently trust nothing.
    RUVIA_CHECK((ruvia::detail::parse_trusted_proxy_block("10.0.0.0/33").index() != 0));
    RUVIA_CHECK((ruvia::detail::parse_trusted_proxy_block("2001:db8::/129").index() != 0));
    RUVIA_CHECK((ruvia::detail::parse_trusted_proxy_block("not-an-address").index() != 0));
    RUVIA_CHECK((ruvia::detail::parse_trusted_proxy_block("10.0.0.0/").index() != 0));
}

RUVIA_TEST(trusted_proxy_parsing_returns_typed_errors_and_complete_networks) {
    using ruvia::detail::parse_trusted_proxy_block;
    using ruvia::detail::trusted_proxy_parse_error;
    const auto invalid_address = parse_trusted_proxy_block("invalid/8");
    RUVIA_CHECK((invalid_address.index() != 0) && std::get<1>(invalid_address) == trusted_proxy_parse_error::invalid_address);
    for (const auto cidr : {"10.0.0.0/33", "10.0.0.0/-1", "10.0.0.0/", "2001:db8::/129"}) {
        const auto invalid_prefix = parse_trusted_proxy_block(cidr);
        RUVIA_CHECK((invalid_prefix.index() != 0) && std::get<1>(invalid_prefix) == trusted_proxy_parse_error::invalid_prefix);
    }
    const auto v4 = parse_trusted_proxy_block("10.1.2.3/8");
    RUVIA_CHECK((v4.index() == 0) && std::get<0>(v4).prefix_bits_ == 104 && std::get<0>(v4).network_[12] == 10);
    const auto v6 = parse_trusted_proxy_block("2001:db8::/32");
    RUVIA_CHECK((v6.index() == 0) && std::get<0>(v6).prefix_bits_ == 32 && std::get<0>(v6).network_[0] == 0x20);
}

RUVIA_TEST(trusted_proxy_matching_spans_both_families) {
    const auto set = set_of({"10.0.0.0/8", "2001:db8::/32"});
    RUVIA_CHECK(set.trusts("10.1.2.3"));
    RUVIA_CHECK(!set.trusts("11.1.2.3"));
    RUVIA_CHECK(set.trusts("2001:db8::1"));
    RUVIA_CHECK(!set.trusts("2001:dba::1"));

    // The same host arriving as an IPv4-mapped IPv6 peer is still that host.
    RUVIA_CHECK(set.trusts("::ffff:10.1.2.3"));
    RUVIA_CHECK(!set.trusts("::ffff:11.1.2.3"));

    RUVIA_CHECK(!trusted_proxy_set{}.trusts("10.1.2.3"));
}

RUVIA_TEST(trusted_proxy_set_matches_later_blocks_and_partial_prefixes) {
    const auto set = set_of({"192.0.2.0/24", "2001:db8:1::/48", "10.8.0.0/13", "2001:db8:8000::/33"});
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
    const auto set = set_of({"10.0.0.0/8", "2001:db8::/32"});
    const auto block = ruvia::detail::parse_trusted_proxy_block("10.0.0.0/8");
    RUVIA_CHECK((block.index() == 0));
    if ((block.index() != 0)) {
        return;
    }
    for (const auto prefix : {"10.1.2.3", "::ffff:10.1.2.3", "2001:db8::1"}) {
        std::string malformed(prefix);
        malformed.push_back('\0');
        RUVIA_CHECK(!set.trusts(malformed));
        RUVIA_CHECK(!ruvia::detail::trusted_proxy_block_contains(std::get<0>(block), malformed));
        malformed.append("unparsed");
        RUVIA_CHECK(!set.trusts(malformed));
        RUVIA_CHECK((ruvia::detail::parse_trusted_proxy_block(malformed).index() != 0));
        malformed.append("/8");
        RUVIA_CHECK((ruvia::detail::parse_trusted_proxy_block(malformed).index() != 0));
    }
    constexpr std::string_view storage = "10.1.2.3suffix";
    RUVIA_CHECK(set.trusts(storage.substr(0, 8)));
    RUVIA_CHECK(!set.trusts(storage));
}

RUVIA_TEST(trusted_proxy_matching_handles_long_and_scoped_address_text) {
    const auto set = set_of({"2001:db8::/32", "fe80::/10"});
    RUVIA_CHECK(set.trusts("2001:0db8:1234:5678:90ab:cdef:1234:5678"));
    RUVIA_CHECK(!set.trusts("2001:0db9:1234:5678:90ab:cdef:1234:5678"));
    RUVIA_CHECK(set.trusts("fe80::1%12"));
    for (const std::size_t size : {63u, 64u, 65u, 1024u}) {
        const std::string malformed(size, 'x');
        RUVIA_CHECK(!set.trusts(malformed));
        RUVIA_CHECK((ruvia::detail::parse_trusted_proxy_block(malformed).index() != 0));
    }
}

RUVIA_TEST(conn_info_ignores_forwarding_headers_from_an_untrusted_peer) {
    worker_memory worker;
    request_memory memory(worker);
    http_request request = make_request(memory, {http_header_view{"X-Forwarded-For", "203.0.113.9"},
                                                    http_header_view{"X-Forwarded-Proto", "https"}});

    // No trusted set at all: the default, and it must read nothing.
    const auto context_value = context_access::make(
        memory, request, ruvia::test::test_context_services().with_plain_transport("198.51.100.7", 53000));
    const auto info = context_value.conn();
    RUVIA_CHECK_EQ(info.client().address(), std::string_view("198.51.100.7"));
    RUVIA_CHECK_EQ(info.remote().port(), std::uint16_t{53000});
    RUVIA_CHECK_EQ(info.client().port(), std::uint16_t{53000});
    RUVIA_CHECK(info.scheme() == ruvia::http_scheme::http);
    RUVIA_CHECK(!info.via_trusted_proxy());

    // Configured, but this peer is not in it.
    const auto trusted = set_of({"10.0.0.0/8"});
    const auto guarded = context_access::make(memory, request,
        ruvia::test::test_context_services()
            .with_plain_transport("198.51.100.7")
            .with_trusted_proxies(trusted));
    const auto guarded_info = guarded.conn();
    RUVIA_CHECK_EQ(guarded_info.client().address(), std::string_view("198.51.100.7"));
    RUVIA_CHECK(guarded_info.scheme() == ruvia::http_scheme::http);
}

RUVIA_TEST(conn_info_resolves_client_from_a_trusted_peer_x_forwarded_headers) {
    worker_memory worker;
    request_memory memory(worker);
    http_request request = make_request(memory, {http_header_view{"X-Forwarded-For", "203.0.113.9, 10.0.0.5"},
                                                    http_header_view{"X-Forwarded-Proto", "https, http"}});

    const auto trusted = set_of({"10.0.0.0/8"});
    const auto context_value = context_access::make(memory, request,
        ruvia::test::test_context_services()
            .with_plain_transport("10.0.0.5", 53001)
            .with_trusted_proxies(trusted));
    const auto info = context_value.conn();

    RUVIA_CHECK_EQ(info.client().address(), std::string_view("203.0.113.9"));
    RUVIA_CHECK_EQ(info.client().port(), std::uint16_t{0});
    // remote() still reports the socket peer, including its port.
    RUVIA_CHECK_EQ(info.remote().address(), std::string_view("10.0.0.5"));
    RUVIA_CHECK_EQ(info.remote().port(), std::uint16_t{53001});
    RUVIA_CHECK(info.scheme() == ruvia::http_scheme::https);
    RUVIA_CHECK(info.via_trusted_proxy());
    // The hop itself is plaintext: the end-to-end scheme is not a synonym for tls().
    RUVIA_CHECK(info.tls() == nullptr);
}

RUVIA_TEST(conn_info_reads_rfc7239_forwarded_when_x_headers_are_absent) {
    worker_memory worker;
    request_memory memory(worker);
    http_request request = make_request(memory, {http_header_view{"Forwarded", R"(for="[2001:db8::1]:4711";proto=https, for=10.0.0.5)"}});

    const auto trusted = set_of({"10.0.0.0/8"});
    const auto context_value = context_access::make(memory, request,
        ruvia::test::test_context_services()
            .with_plain_transport("10.0.0.5")
            .with_trusted_proxies(trusted));
    const auto info = context_value.conn();

    // Bracketed IPv6 with a port, unwrapped. No X- header to consult.
    RUVIA_CHECK_EQ(info.client().address(), std::string_view("2001:db8::1"));
    RUVIA_CHECK(info.scheme() == ruvia::http_scheme::https);
}

RUVIA_TEST(conn_info_prefers_proxy_written_x_headers_over_client_forwarded) {
    worker_memory worker;
    request_memory memory(worker);
    http_request request = make_request(memory, {http_header_view{"Forwarded", "for=198.51.100.1;proto=https"},
                                                    http_header_view{"X-Forwarded-For", "203.0.113.9"},
                                                    http_header_view{"X-Forwarded-Proto", "http"}});

    const auto trusted = set_of({"10.0.0.0/8"});
    const auto context_value = context_access::make(memory, request,
        ruvia::test::test_context_services()
            .with_plain_transport("10.0.0.5")
            .with_trusted_proxies(trusted));
    const auto info = context_value.conn();
    RUVIA_CHECK_EQ(info.client().address(), std::string_view("203.0.113.9"));
    RUVIA_CHECK(info.scheme() == ruvia::http_scheme::http);
}

RUVIA_TEST(conn_info_forwarded_quoted_comma_does_not_invent_a_hop) {
    worker_memory worker;
    request_memory memory(worker);
    http_request request = make_request(memory, {http_header_view{"Forwarded", R"(for=10.0.0.5, for="_x,203.0.113.9")"}});

    const auto trusted = set_of({"10.0.0.0/8"});
    const auto context_value = context_access::make(memory, request,
        ruvia::test::test_context_services()
            .with_plain_transport("10.0.0.5")
            .with_trusted_proxies(trusted));
    const auto info = context_value.conn();
    RUVIA_CHECK_EQ(info.client().address(), std::string_view("_x,203.0.113.9"));
}

RUVIA_TEST(conn_info_forwarded_quoted_semicolon_stays_one_parameter) {
    worker_memory worker;
    request_memory memory(worker);
    http_request request = make_request(memory, {http_header_view{"Forwarded", R"(for=10.0.0.5, for="_x;203.0.113.9")"}});

    const auto trusted = set_of({"10.0.0.0/8"});
    const auto context_value = context_access::make(memory, request,
        ruvia::test::test_context_services()
            .with_plain_transport("10.0.0.5")
            .with_trusted_proxies(trusted));
    const auto info = context_value.conn();
    RUVIA_CHECK_EQ(info.client().address(), std::string_view("_x;203.0.113.9"));
}

RUVIA_TEST(conn_info_walks_same_name_forwarding_fields_as_one_chain) {
    worker_memory worker;
    request_memory memory(worker);
    http_request request = make_request(memory, {http_header_view{"X-Forwarded-For", "198.51.100.1"},
                                                    http_header_view{"X-Forwarded-For", "203.0.113.9"},
                                                    http_header_view{"X-Forwarded-Proto", "https"},
                                                    http_header_view{"X-Forwarded-Proto", "http"}});

    const auto trusted = set_of({"10.0.0.0/8"});
    const auto context_value = context_access::make(memory, request,
        ruvia::test::test_context_services()
            .with_plain_transport("10.0.0.5")
            .with_trusted_proxies(trusted));
    const auto info = context_value.conn();
    RUVIA_CHECK_EQ(info.client().address(), std::string_view("203.0.113.9"));
    RUVIA_CHECK(info.scheme() == ruvia::http_scheme::http);
}

RUVIA_TEST(conn_info_walks_same_name_rfc7239_fields_as_one_chain) {
    worker_memory worker;
    request_memory memory(worker);
    http_request request = make_request(memory, {http_header_view{"Forwarded", "for=198.51.100.1;proto=https"},
                                                    http_header_view{"Forwarded", "for=203.0.113.9;proto=http"}});

    const auto trusted = set_of({"10.0.0.0/8"});
    const auto context_value = context_access::make(memory, request,
        ruvia::test::test_context_services()
            .with_plain_transport("10.0.0.5")
            .with_trusted_proxies(trusted));
    const auto info = context_value.conn();
    RUVIA_CHECK_EQ(info.client().address(), std::string_view("203.0.113.9"));
    RUVIA_CHECK(info.scheme() == ruvia::http_scheme::http);
}

RUVIA_TEST(conn_info_forwarded_skips_client_prepended_hops) {
    worker_memory worker;
    request_memory memory(worker);
    http_request request = make_request(memory, {http_header_view{
                                                    "Forwarded", R"(for=198.51.100.1;proto=https, for=203.0.113.9;proto=http)"}});

    const auto trusted = set_of({"10.0.0.0/8"});
    const auto context_value = context_access::make(memory, request,
        ruvia::test::test_context_services()
            .with_plain_transport("10.0.0.5")
            .with_trusted_proxies(trusted));
    const auto info = context_value.conn();
    RUVIA_CHECK_EQ(info.client().address(), std::string_view("203.0.113.9"));
    RUVIA_CHECK(info.scheme() == ruvia::http_scheme::http);
}

RUVIA_TEST(conn_info_forwarded_ignores_proto_on_prepended_hops) {
    worker_memory worker;
    request_memory memory(worker);
    http_request request = make_request(memory, {http_header_view{"Forwarded", "for=198.51.100.1;proto=https, for=203.0.113.9"}});

    const auto trusted = set_of({"10.0.0.0/8"});
    const auto context_value = context_access::make(memory, request,
        ruvia::test::test_context_services()
            .with_plain_transport("10.0.0.5")
            .with_trusted_proxies(trusted));
    const auto info = context_value.conn();
    RUVIA_CHECK_EQ(info.client().address(), std::string_view("203.0.113.9"));
    RUVIA_CHECK(info.scheme() == ruvia::http_scheme::http);
    RUVIA_CHECK(info.via_trusted_proxy());
}

RUVIA_TEST(conn_info_forwarded_proto_is_case_insensitive) {
    // RFC 7239 §5.4 proto tokens are ABNF strings, so "HTTPS" is "https".
    worker_memory worker;
    request_memory memory(worker);
    http_request request = make_request(memory, {http_header_view{"Forwarded", "for=203.0.113.9;proto=HTTPS"}});

    const auto trusted = set_of({"10.0.0.0/8"});
    const auto context_value = context_access::make(memory, request,
        ruvia::test::test_context_services()
            .with_plain_transport("10.0.0.5")
            .with_trusted_proxies(trusted));
    const auto info = context_value.conn();
    RUVIA_CHECK(info.scheme() == ruvia::http_scheme::https);
    RUVIA_CHECK(info.via_trusted_proxy());
}

RUVIA_TEST(conn_info_x_forwarded_proto_requires_aligned_hops) {
    worker_memory worker;
    request_memory memory(worker);
    http_request request = make_request(memory, {http_header_view{"X-Forwarded-For", "203.0.113.9"},
                                                    http_header_view{"X-Forwarded-Proto", "http, HTTPS"}});

    const auto trusted = set_of({"10.0.0.0/8"});
    const auto context_value = context_access::make(memory, request,
        ruvia::test::test_context_services()
            .with_plain_transport("10.0.0.5")
            .with_trusted_proxies(trusted));
    const auto info = context_value.conn();
    RUVIA_CHECK_EQ(info.client().address(), std::string_view("203.0.113.9"));
    RUVIA_CHECK(info.scheme() == ruvia::http_scheme::http);
}

RUVIA_TEST(conn_info_ignores_client_prepended_forwarding_hops) {
    worker_memory worker;
    request_memory memory(worker);
    http_request request = make_request(memory, {http_header_view{"X-Forwarded-For", "198.51.100.1, 203.0.113.9"},
                                                    http_header_view{"X-Forwarded-Proto", "https, http"}});

    const auto trusted = set_of({"10.0.0.0/8"});
    const auto context_value = context_access::make(memory, request,
        ruvia::test::test_context_services()
            .with_plain_transport("10.0.0.5")
            .with_trusted_proxies(trusted));
    const auto info = context_value.conn();
    RUVIA_CHECK_EQ(info.client().address(), std::string_view("203.0.113.9"));
    RUVIA_CHECK(info.scheme() == ruvia::http_scheme::http);
}

RUVIA_TEST(conn_info_keeps_transport_values_for_fields_the_proxy_omitted) {
    worker_memory worker;
    request_memory memory(worker);
    http_request request = make_request(memory, {http_header_view{"X-Forwarded-For", "203.0.113.9"}});

    const auto trusted = set_of({"10.0.0.0/8"});
    const auto context_value = context_access::make(memory, request,
        ruvia::test::test_context_services()
            .with_tls_transport("10.0.0.5", "CN=proxy")
            .with_trusted_proxies(trusted));
    const auto info = context_value.conn();

    RUVIA_CHECK_EQ(info.client().address(), std::string_view("203.0.113.9"));
    RUVIA_CHECK(info.scheme() == ruvia::http_scheme::https);
    RUVIA_CHECK(info.tls() != nullptr);
}

RUVIA_TEST(conn_info_x_forwarded_scheme_requires_explicit_sanitizing_proxy_policy) {
    worker_memory worker;
    request_memory memory(worker);
    auto request = make_request(memory, {http_header_view{"X-Forwarded-For", "203.0.113.9"},
                                            http_header_view{"X-Forwarded-Proto", "https"},
                                            http_header_view{"Forwarded", "for=198.51.100.2;proto=https"}});
    auto trusted = set_of({"10.0.0.0/8"});
    trusted.trust_x_forwarded_proto(false);
    auto context_value = context_access::make(memory, request,
        ruvia::test::test_context_services().with_plain_transport("10.0.0.5").with_trusted_proxies(trusted));
    RUVIA_CHECK_EQ(context_value.conn().client().address(), std::string_view("203.0.113.9"));
    RUVIA_CHECK(context_value.conn().scheme() == ruvia::http_scheme::http);
}

RUVIA_TEST(conn_info_x_forwarded_scheme_keeps_the_selected_client_hop) {
    worker_memory worker;
    request_memory memory(worker);
    auto trusted = set_of({"10.0.0.0/8"});
    for (const auto proto : {"https, http", "https", "https, invalid, https"}) {
        auto request = make_request(memory, {http_header_view{"X-Forwarded-For", "203.0.113.9, 10.0.0.6"},
                                                http_header_view{"X-Forwarded-Proto", proto}});
        auto context_value = context_access::make(memory, request,
            ruvia::test::test_context_services().with_plain_transport("10.0.0.5").with_trusted_proxies(trusted));
        RUVIA_CHECK(context_value.conn().scheme() == (std::string_view(proto) == "https, http"
                                                             ? ruvia::http_scheme::https
                                                             : ruvia::http_scheme::http));
    }
}
