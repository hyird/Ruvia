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

RUVIA_TEST(http_authority_host_public_parse_distinguishes_empty_host_from_invalid_authority) {
    const auto empty_host = ruvia::parse_http_authority_host("");
    RUVIA_CHECK(empty_host.has_value());
    if (empty_host) {
        RUVIA_CHECK(empty_host->empty());
    }

    RUVIA_CHECK(!ruvia::parse_http_authority(""));
    RUVIA_CHECK(!ruvia::is_valid_http_connect_authority(""));
    RUVIA_CHECK(!ruvia::is_valid_http_connect_authority(":443"));
    for (const std::string_view invalid : {":", ":80", "user@example.test", " ", "[::1", "example.test:65536", "example.test:-1"}) {
        RUVIA_CHECK(!ruvia::parse_http_authority_host(invalid));
    }
}
