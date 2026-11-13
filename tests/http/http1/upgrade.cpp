#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/http/http1_server_semantics.h"
#include "ruvia/http/http_ascii.h"
#include "ruvia/http/http_request.h"
#include "ruvia/http/http_response.h"
#include "ruvia/http/http_response_server.h"

#include "http1_request_message_fixture.h"

namespace {

using ruvia::test::parse_http1_request_message;

[[nodiscard]] bool offers_upgrade(std::string_view wire) {
    const auto parsed = parse_http1_request_message(wire);
    EXPECT_TRUE(parsed.complete()) << wire;
    return parsed.complete() && ruvia::http1_request_offers_upgrade(parsed.state_.request());
}

[[nodiscard]] std::size_t field_count(const ruvia::http_response& response, std::string_view name) {
    std::size_t count = 0;
    for (const auto& header : response.headers()) {
        count += ruvia::http_ascii_equals_ignore_case(header.name(), name) ? 1 : 0;
    }
    return count;
}

// The 101 head `response` becomes for `request_wire`, or nothing when
// preparing it throws http1_response_head_error.
[[nodiscard]] std::string upgrade_head(ruvia::http_response& response, std::string_view request_wire) {
    const auto parsed = parse_http1_request_message(request_wire);
    EXPECT_TRUE(parsed.complete()) << request_wire;
    try {
        const auto plan = ruvia::prepare_http1_upgrade_response_head(response, parsed.state_.request());
        EXPECT_TRUE(plan.close_delimited_stream() != nullptr);
        ruvia::http_response_head_buffer head;
        ruvia::append_http1_response_head(response, head, plan);
        return std::string(head.view());
    } catch (const ruvia::http1_response_head_error&) {
        return {};
    }
}

constexpr std::string_view offering_request =
    "GET /chat HTTP/1.1\r\nHost: example.test\r\nConnection: keep-alive, Upgrade\r\nUpgrade: echo/1, other\r\n\r\n";

}  // namespace

// RFC 9110 §7.8: only an HTTP/1.1 request that lists the Upgrade connection
// option beside an Upgrade field naming a protocol asks to switch; CONNECT
// establishes a tunnel through its 2xx instead.
TEST(Ruvia, http1_upgrade_offer_needs_http11_the_upgrade_option_and_a_protocol) {
    EXPECT_TRUE(offers_upgrade(offering_request));
    EXPECT_TRUE(offers_upgrade("GET / HTTP/1.1\r\nHost: x\r\nconnection: UPGRADE\r\nupgrade: Echo\r\n\r\n"));
    EXPECT_TRUE(offers_upgrade(
        "GET / HTTP/1.1\r\nHost: x\r\nConnection: keep-alive\r\nConnection: upgrade\r\n"
        "Upgrade: , echo\r\n\r\n"));
    EXPECT_FALSE(offers_upgrade("GET / HTTP/1.1\r\nHost: x\r\nUpgrade: echo\r\n\r\n"));
    EXPECT_FALSE(offers_upgrade("GET / HTTP/1.1\r\nHost: x\r\nConnection: upgrade\r\n\r\n"));
    EXPECT_FALSE(offers_upgrade("GET / HTTP/1.1\r\nHost: x\r\nConnection: upgrade\r\nUpgrade: ,\r\n\r\n"));
    EXPECT_FALSE(offers_upgrade("GET / HTTP/1.0\r\nHost: x\r\nConnection: upgrade\r\nUpgrade: echo\r\n\r\n"));
    EXPECT_FALSE(offers_upgrade(
        "CONNECT example.test:443 HTTP/1.1\r\nHost: example.test:443\r\n"
        "Connection: upgrade\r\nUpgrade: echo\r\n\r\n"));
}

// Without an application Upgrade field the 101 names the first offered
// protocol; the head carries no framing and the connection option is added
// once, beside the application's own options.
TEST(Ruvia, http1_upgrade_head_switches_to_an_offered_protocol_without_framing) {
    {
        ruvia::http_response response;
        response.header("x-tunnel", "echo");
        const auto head = upgrade_head(response, offering_request);
        EXPECT_EQ(response.status().value(), 101);
        EXPECT_TRUE(head.starts_with("HTTP/1.1 101 Switching Protocols\r\n")) << head;
        EXPECT_NE(head.find("\r\nUpgrade: echo/1\r\n"), std::string::npos) << head;
        EXPECT_NE(head.find("\r\nConnection: Upgrade\r\n"), std::string::npos) << head;
        EXPECT_NE(head.find("\r\nx-tunnel: echo\r\n"), std::string::npos) << head;
        EXPECT_EQ(head.find("Content-Length:"), std::string::npos) << head;
        EXPECT_EQ(head.find("Transfer-Encoding:"), std::string::npos) << head;
        EXPECT_TRUE(head.ends_with("\r\n\r\n")) << head;
    }
    {
        ruvia::http_response response;
        response.header("Upgrade", "OTHER");
        response.header("Connection", "upgrade");
        const auto head = upgrade_head(response, offering_request);
        EXPECT_NE(head.find("\r\nUpgrade: OTHER\r\n"), std::string::npos) << head;
        EXPECT_EQ(head.find("echo/1"), std::string::npos) << head;
        EXPECT_EQ(field_count(response, "Upgrade"), 1U);
        EXPECT_EQ(field_count(response, "Connection"), 1U);
    }
    {
        ruvia::http_response response;
        response.header("Connection", "x-hop");
        response.header("x-hop", "1");
        const auto head = upgrade_head(response, offering_request);
        EXPECT_TRUE(head.starts_with("HTTP/1.1 101 Switching Protocols\r\n")) << head;
        EXPECT_NE(head.find("\r\nConnection: x-hop\r\n"), std::string::npos) << head;
        EXPECT_NE(head.find("\r\nConnection: Upgrade\r\n"), std::string::npos) << head;
    }
}

// A request without an offer, a protocol the client did not offer, content
// and framing fields are refused before the response changes.
TEST(Ruvia, http1_upgrade_head_rejects_unoffered_switches_content_and_framing_unchanged) {
    const auto refused_unchanged = [&](ruvia::http_response& response, std::string_view request_wire) {
        const auto fields = response.headers().size();
        const auto refused = upgrade_head(response, request_wire).empty();
        return refused && response.status().value() == 200 && response.headers().size() == fields;
    };
    {
        ruvia::http_response response;
        EXPECT_TRUE(refused_unchanged(response, "GET / HTTP/1.1\r\nHost: x\r\nUpgrade: echo\r\n\r\n"));
    }
    {
        ruvia::http_response response;
        response.header("Upgrade", "echo/2");
        EXPECT_TRUE(refused_unchanged(response, offering_request));
    }
    {
        ruvia::http_response response;
        response.header("Upgrade", "other, unoffered");
        EXPECT_TRUE(refused_unchanged(response, offering_request));
    }
    {
        ruvia::http_response response;
        response.body("payload");
        EXPECT_TRUE(refused_unchanged(response, offering_request));
    }
    for (const auto& [name, value] : {std::pair<std::string_view, std::string_view>{"Content-Length", "0"},
             std::pair<std::string_view, std::string_view>{"Transfer-Encoding", "chunked"}}) {
        ruvia::http_response response;
        response.header(name, value);
        EXPECT_TRUE(refused_unchanged(response, offering_request)) << name;
    }
}
