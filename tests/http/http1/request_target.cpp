#include <string_view>

#include "ruvia/http/http_request_target.h"

#include "test_harness.h"

RUVIA_TEST(http_authority_host_public_parse_preserves_ip_literal_brackets) {
    RUVIA_CHECK_EQ(ruvia::parse_http_authority_host("[::1]:8080").value(), std::string_view("[::1]"));
    RUVIA_CHECK_EQ(ruvia::parse_http_authority_host("example.test:443").value(),
        std::string_view("example.test"));
    RUVIA_CHECK(!ruvia::parse_http_authority_host("user@example.test"));
    RUVIA_CHECK(ruvia::is_valid_http_ipv4_literal("127.0.0.1"));
    RUVIA_CHECK(!ruvia::is_valid_http_ipv4_literal("999.0.0.1"));
    RUVIA_CHECK(ruvia::is_valid_http_ipv6_literal("::1"));
    RUVIA_CHECK(!ruvia::is_valid_http_ipv6_literal("not-an-ip"));
}
