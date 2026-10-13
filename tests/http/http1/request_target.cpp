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

RUVIA_TEST(http_ip_literal_validation_preserves_compression_and_tail_bounds) {
    for (const auto valid : {"::", "1::", "::1", "1:2:3:4:5:6:7:8", "1:2:3:4:5:6:7::",
             "1::2:3:4:5:6:7", "::ffff:192.0.2.1", "1:2:3:4:5:6:192.0.2.1"}) {
        RUVIA_CHECK(ruvia::is_valid_http_ipv6_literal(valid));
    }
    for (const auto invalid : {"", ":", ":::1", "1:::2", "1::2::3", "1:2:3:4:5:6:7",
             "1:2:3:4:5:6:7:8:9", "1:2:3:4:5:6:7:8::", "1:2:3:4:5:6::7:8",
             "1:2:3:4:5:6:7:192.0.2.1", "1:2:3:4:5::6:192.0.2.1",
             "::ffff:192.00.2.1", "::ffff:192.0.2.256", "::ffff:192.0.2.1:",
             "::ffff:192.0.2", "::ffff:192.0.2.1suffix", "12345::", "::1%zone", "[::1]"}) {
        RUVIA_CHECK(!ruvia::is_valid_http_ipv6_literal(invalid));
    }
    for (const auto invalid : {"", "1.2.3", "1.2.3.4.5", "01.2.3.4", "1.2.3.256", "1.2.3.4suffix"}) {
        RUVIA_CHECK(!ruvia::is_valid_http_ipv4_literal(invalid));
    }
    RUVIA_CHECK(ruvia::is_valid_http_ipv4_literal("0.0.0.0"));
    RUVIA_CHECK(ruvia::is_valid_http_ipv4_literal("255.255.255.255"));
}
