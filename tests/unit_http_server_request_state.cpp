#include "test_harness.h"

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <string>

#include "ruvia/http/detail/HttpParserInternal.h"
#include "ruvia/web/detail/server/HttpServerAutoHttps.h"
#include "ruvia/web/detail/server/HttpServerRequestState.h"

namespace {

using ruvia::detail::appendHttpsPort;
using ruvia::detail::contentLengthExceedsLimit;
using ruvia::detail::hostWithoutExplicitPort;
using ruvia::detail::HttpServerParser;
using ruvia::detail::http1ShouldKeepAlive;
using ruvia::detail::http1WantsContinue;

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
    RUVIA_CHECK(!http1ShouldKeepAlive(
        parser.parse("GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")));
    RUVIA_CHECK(http1ShouldKeepAlive(
        parser.parse("GET / HTTP/1.1\r\nHost: x\r\nConnection: keep-alive\r\n\r\n")));
}

RUVIA_TEST(request_state_keep_alive_default_by_version) {
    HttpServerParser parser;
    // HTTP/1.1 defaults to persistent; HTTP/1.0 defaults to close.
    RUVIA_CHECK(http1ShouldKeepAlive(parser.parse("GET / HTTP/1.1\r\nHost: x\r\n\r\n")));
    RUVIA_CHECK(!http1ShouldKeepAlive(parser.parse("GET / HTTP/1.0\r\n\r\n")));
    // HTTP/1.0 can still opt in with an explicit keep-alive.
    RUVIA_CHECK(http1ShouldKeepAlive(parser.parse("GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\n")));
}

RUVIA_TEST(request_state_wants_continue) {
    HttpServerParser parser;
    RUVIA_CHECK(http1WantsContinue(parser.parse(
        "POST / HTTP/1.1\r\nHost: x\r\nExpect: 100-continue\r\nContent-Length: 0\r\n\r\n")));
    RUVIA_CHECK(!http1WantsContinue(parser.parse("GET / HTTP/1.1\r\nHost: x\r\n\r\n")));
    // A 100-continue expectation from an HTTP/1.0 client MUST be ignored: RFC 9110
    // §15.2 forbids sending any 1xx response to an HTTP/1.0 client, which would
    // misread the interim 100 as the final response.
    RUVIA_CHECK(!http1WantsContinue(parser.parse(
        "POST / HTTP/1.0\r\nHost: x\r\nExpect: 100-continue\r\nContent-Length: 0\r\n\r\n")));
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

RUVIA_TEST(auto_https_redirect_response_is_private_and_well_formed) {
    HttpServerParser parser;
    // Host carries the cleartext port, which must be dropped and replaced by the
    // (default, so omitted) HTTPS port; the path and query are preserved.
    const std::string request = "GET /a/b?x=1 HTTP/1.1\r\nHost: example.com:80\r\n\r\n";
    const auto parsed = parser.parse(request);
    ruvia::WorkerMemory worker;
    ruvia::RequestMemory memory(worker);
    const auto response = ruvia::detail::makeAutoHttpsRedirectResponse(parsed.request, memory, 443);

    RUVIA_CHECK_EQ(response.status(), std::uint16_t{308});
    RUVIA_CHECK_EQ(std::string(response.header("Location")), std::string("https://example.com/a/b?x=1"));
    // The Location is Host-derived, so the redirect must be private: a shared cache
    // must not store one Host's redirect and replay it for another.
    RUVIA_CHECK_EQ(std::string(response.header("Cache-Control")), std::string("private"));
    RUVIA_CHECK_EQ(std::string(response.header("Connection")), std::string("close"));
}
