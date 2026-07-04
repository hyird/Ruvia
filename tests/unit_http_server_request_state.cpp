#include "test_harness.h"

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <string>

#include "http/HttpParserInternal.h"
#include "net/server/HttpServerAutoHttps.h"
#include "net/server/HttpServerRequestState.h"

namespace {

using ruvia::detail::appendHttpsPort;
using ruvia::detail::contentLengthExceedsLimit;
using ruvia::detail::hostWithoutExplicitPort;
using ruvia::detail::HttpServerParser;
using ruvia::detail::shouldKeepAlive;
using ruvia::detail::wantsContinue;

std::string withHttpsPort(std::string_view base, std::uint16_t port) {
    std::pmr::string location(std::pmr::get_default_resource());
    location.assign(base.data(), base.size());
    appendHttpsPort(location, port);
    return std::string(location.data(), location.size());
}

}  // namespace

RUVIA_TEST(request_state_content_length_exceeds_limit) {
    RUVIA_CHECK(contentLengthExceedsLimit(101, 100));
    RUVIA_CHECK(!contentLengthExceedsLimit(100, 100));       // exact fit is allowed
    RUVIA_CHECK(!contentLengthExceedsLimit(1'000'000, 0));   // a 0 limit means unlimited
}

RUVIA_TEST(request_state_keep_alive_by_connection_header) {
    HttpServerParser parser;
    RUVIA_CHECK(!shouldKeepAlive(
        parser.parse("GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")));
    RUVIA_CHECK(shouldKeepAlive(
        parser.parse("GET / HTTP/1.1\r\nHost: x\r\nConnection: keep-alive\r\n\r\n")));
}

RUVIA_TEST(request_state_keep_alive_default_by_version) {
    HttpServerParser parser;
    // HTTP/1.1 defaults to persistent; HTTP/1.0 defaults to close.
    RUVIA_CHECK(shouldKeepAlive(parser.parse("GET / HTTP/1.1\r\nHost: x\r\n\r\n")));
    RUVIA_CHECK(!shouldKeepAlive(parser.parse("GET / HTTP/1.0\r\n\r\n")));
    // HTTP/1.0 can still opt in with an explicit keep-alive.
    RUVIA_CHECK(shouldKeepAlive(parser.parse("GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\n")));
}

RUVIA_TEST(request_state_wants_continue) {
    HttpServerParser parser;
    RUVIA_CHECK(wantsContinue(parser.parse(
        "POST / HTTP/1.1\r\nHost: x\r\nExpect: 100-continue\r\nContent-Length: 0\r\n\r\n")));
    RUVIA_CHECK(!wantsContinue(parser.parse("GET / HTTP/1.1\r\nHost: x\r\n\r\n")));
}

RUVIA_TEST(auto_https_host_without_explicit_port_strips_port_bracket_aware) {
    // The HTTP->HTTPS redirect reuses the request Host but drops any explicit port
    // (the HTTPS port is appended separately). A reg-name loses its ":port".
    RUVIA_CHECK_EQ(hostWithoutExplicitPort("example.com:80"), std::string_view("example.com"));
    RUVIA_CHECK_EQ(hostWithoutExplicitPort("example.com:8080"), std::string_view("example.com"));
    RUVIA_CHECK_EQ(hostWithoutExplicitPort("example.com"), std::string_view("example.com"));

    // IPv6 must be handled bracket-aware: only the ":port" AFTER "]" is stripped, the
    // colons inside the literal are kept (a naive find(':') would corrupt it).
    RUVIA_CHECK_EQ(hostWithoutExplicitPort("[::1]:80"), std::string_view("[::1]"));
    RUVIA_CHECK_EQ(hostWithoutExplicitPort("[::1]"), std::string_view("[::1]"));
    RUVIA_CHECK_EQ(hostWithoutExplicitPort("[2001:db8::1]:443"), std::string_view("[2001:db8::1]"));
    // An unterminated bracket is returned unchanged rather than mangled.
    RUVIA_CHECK_EQ(hostWithoutExplicitPort("[::1"), std::string_view("[::1"));
    RUVIA_CHECK_EQ(hostWithoutExplicitPort(""), std::string_view(""));
}

RUVIA_TEST(auto_https_append_port_omits_default) {
    // 443 is the default HTTPS port: it must NOT appear in the redirect URL.
    RUVIA_CHECK_EQ(withHttpsPort("https://example.com", 443), std::string("https://example.com"));
    // Any other port is appended explicitly.
    RUVIA_CHECK_EQ(withHttpsPort("https://example.com", 8443), std::string("https://example.com:8443"));
    RUVIA_CHECK_EQ(withHttpsPort("https://example.com", 80), std::string("https://example.com:80"));
}
