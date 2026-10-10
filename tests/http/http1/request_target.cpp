#include <cstddef>
#include <string_view>

#include "ruvia/http/http1_request_parser.h"
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

RUVIA_TEST(http1_empty_host_authority_round_trips_through_public_target_parser) {
    const ruvia::http1_request_parser parser;
    for (const std::string_view message : {
             "GET urn:example:animal HTTP/1.1\r\nHost:\r\n\r\n",
             "GET file:///path HTTP/1.1\r\nHost:\r\n\r\n"}) {
        for (std::size_t length = 0; length < message.size(); ++length) {
            const auto partial = parser.parse(message.substr(0, length));
            RUVIA_CHECK(partial.need_more() != nullptr);
        }

        const auto parsed_value = parser.parse(message);
        RUVIA_CHECK(parsed_value.parsed() != nullptr);
        if (parsed_value.parsed() == nullptr) {
            continue;
        }
        RUVIA_CHECK_EQ(parsed_value.parsed()->consumed_bytes(), message.size());
        const auto host = parsed_value.parsed()->request().header("Host");
        RUVIA_CHECK(host.has_value());
        if (!host) {
            continue;
        }
        RUVIA_CHECK(host->empty());
        const auto parsed_host = ruvia::parse_http_authority_host(*host);
        RUVIA_CHECK(parsed_host.has_value());
        if (parsed_host) {
            RUVIA_CHECK(parsed_host->empty());
        }
    }
}
