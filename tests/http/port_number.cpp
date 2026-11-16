#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "borrowed_text.h"
#include "http_authority.h"
#include "http_request.h"
#include "http_uri.h"

TEST(Ruvia, port_number_rejects_wide_inputs_without_changing_an_existing_origin) {
    const auto rejects = [](auto input) {
        try {
            (void)ruvia::http_origin_view::https({.host_ = "example.test", .port_ = input});
        } catch (const std::invalid_argument&) {
            return true;
        }
        return false;
    };
    const std::array<std::int64_t, 7> invalid{std::numeric_limits<std::int64_t>::min(), -65093, -1, 65536, 65979, 131071, std::numeric_limits<std::int64_t>::max()};
    for (const auto input : invalid) {
        EXPECT_TRUE(rejects(input));
        EXPECT_TRUE(!ruvia::port_number::try_from_value(input).has_value());
        ruvia::http_origin_options options{.host_ = "example.test", .port_ = 8443};
        bool assignment_rejected = false;
        try {
            options.port_ = input;
        } catch (const std::invalid_argument&) {
            assignment_rejected = true;
        }
        EXPECT_TRUE(assignment_rejected);
        const auto retained = ruvia::http_origin_view::https(options);
        EXPECT_EQ(ruvia::make_http_origin_authority(retained), "example.test:8443");
    }
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    EXPECT_TRUE(rejects(maximum));
    EXPECT_TRUE(!ruvia::port_number::try_from_value(maximum).has_value());
}

TEST(Ruvia, port_number_preserves_zero_maximum_and_scheme_default_authorities) {
    struct port_case final {
        std::uint64_t input_;
        std::string_view authority_;
    };
    for (const auto entry : std::array{port_case{0, "[::1]:0"}, port_case{1, "[::1]:1"}, port_case{80, "[::1]:80"}, port_case{443, "[::1]"}, port_case{65535, "[::1]:65535"}}) {
        const auto origin = ruvia::http_origin_view::https({.host_ = "[::1]", .port_ = entry.input_});
        EXPECT_EQ(ruvia::make_http_origin_authority(origin), entry.authority_);
    }
    const auto absent = ruvia::http_origin_view::http({.host_ = "example.test"});
    const auto zero = ruvia::http_origin_view::http({.host_ = "example.test", .port_ = 0});
    EXPECT_EQ(ruvia::make_http_origin_authority(absent), "example.test");
    EXPECT_EQ(ruvia::make_http_origin_authority(zero), "example.test:0");
}

TEST(Ruvia, port_number_rejects_invalid_authority_defaults_before_origin_comparison) {
    const auto authority = ruvia::parse_http_authority("example.test");
    EXPECT_TRUE(authority.has_value());
    if (!authority) {
        return;
    }
    const auto rejects = [](auto&& operation) {
        try {
            operation();
        } catch (const std::invalid_argument&) {
            return true;
        }
        return false;
    };
    for (const auto source : std::array<std::int64_t, 7>{std::numeric_limits<std::int64_t>::min(), -65093, -1, 65536, 65979, 131071, std::numeric_limits<std::int64_t>::max()}) {
        EXPECT_TRUE(rejects([&] {
            (void)authority->effective_port(source);
        }));
        EXPECT_TRUE(rejects([&] {
            (void)ruvia::http_authorities_equal("example.test", "example.test:443", source);
        }));
    }
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    EXPECT_TRUE(rejects([&] {
        (void)authority->effective_port(maximum);
    }));
    EXPECT_TRUE(rejects([&] {
        (void)ruvia::http_authorities_equal("example.test", "example.test:443", maximum);
    }));
    EXPECT_TRUE(ruvia::http_authorities_equal("example.test", "EXAMPLE.test:443", 443));
    EXPECT_TRUE(!ruvia::http_authorities_equal("example.test", "example.test:443", 80));
}

TEST(Ruvia, port_number_preserves_absent_empty_and_explicit_authority_port_rules) {
    const auto absent = ruvia::parse_http_authority("[::1]");
    const auto empty = ruvia::parse_http_authority("[::1]:");
    const auto explicit_port = ruvia::parse_http_authority("[::1]:8443");
    EXPECT_TRUE(absent && empty && explicit_port);
    if (!absent || !empty || !explicit_port) {
        return;
    }
    for (const auto source : std::array<std::uint64_t, 5>{0, 1, 80, 443, 65535}) {
        EXPECT_TRUE(absent->effective_port(source) == source);
        EXPECT_TRUE(empty->effective_port(source) == source);
        EXPECT_TRUE(explicit_port->effective_port(source) == 8443);
        EXPECT_TRUE(ruvia::http_authorities_equal("[::1]", "[::1]:", source));
    }
    EXPECT_TRUE(!ruvia::http_authorities_equal("[::1]", "[::1]:0", std::nullopt));
    EXPECT_TRUE(ruvia::http_authorities_equal("[::1]", "[::1]:", std::nullopt));
    EXPECT_TRUE(ruvia::http_authorities_equal("[::1]:", "[::1]:65535", 65535));
    EXPECT_TRUE(!ruvia::http_authorities_equal("[::1]:8443", "[::1]:443", 443));
}

namespace {

template <typename text_type>
concept http_url_input = requires(text_type&& text) {
    ruvia::parse_http_url(std::forward<text_type>(text));
};

template <typename text_type>
concept uri_host_port_input = requires(text_type&& text, std::string_view& host, std::string_view& port, bool& has_port) {
    ruvia::split_uri_host_port(std::forward<text_type>(text), host, port, has_port);
};

static_assert(http_url_input<std::string&>);
static_assert(http_url_input<std::string_view>);
static_assert(!http_url_input<std::string>);
static_assert(uri_host_port_input<std::string&>);
static_assert(!uri_host_port_input<std::string>);

}  // namespace

TEST(RuviaHttp, public_dns_primitives_enforce_labels_and_proper_subdomains) {
    for (const std::string_view name : {"localhost", "example.test", "XN--bcher-kva.example", "0.example", "a-b.example"}) {
        EXPECT_TRUE(ruvia::is_valid_dns_domain_name(name)) << name;
    }
    for (const std::string_view name : {"", ".", ".example", "example.", "a..example", "-a.example", "a-.example", "a_b.example", "%61.example", "bücher.example"}) {
        EXPECT_FALSE(ruvia::is_valid_dns_domain_name(name)) << name;
    }
    EXPECT_TRUE(ruvia::is_valid_dns_domain_name(std::string(63, 'a') + ".test"));
    EXPECT_FALSE(ruvia::is_valid_dns_domain_name(std::string(64, 'a') + ".test"));
    const std::string maximum = std::string(63, 'a') + "." + std::string(63, 'b') + "." + std::string(63, 'c') + "." + std::string(61, 'd');
    EXPECT_TRUE(ruvia::is_valid_dns_domain_name(maximum));
    EXPECT_FALSE(ruvia::is_valid_dns_domain_name(maximum + "e"));
    EXPECT_TRUE(ruvia::is_dns_subdomain_of("a.EXAMPLE.test", "example.TEST"));
    EXPECT_FALSE(ruvia::is_dns_subdomain_of("example.test", "example.test"));
    EXPECT_FALSE(ruvia::is_dns_subdomain_of("notexample.test", "example.test"));
    EXPECT_FALSE(ruvia::is_dns_subdomain_of("example.test.attacker", "example.test"));
}

TEST(RuviaHttp, public_host_port_split_preserves_text_without_validating_components) {
    std::string_view host;
    std::string_view port;
    bool has_port = false;
    constexpr std::string_view input = "[2001:db8::1]:00443";
    ASSERT_TRUE(ruvia::split_uri_host_port(input, host, port, has_port));
    EXPECT_EQ(host, "[2001:db8::1]");
    EXPECT_EQ(host.data(), input.data());
    EXPECT_EQ(port, "00443");
    EXPECT_TRUE(has_port);
    ASSERT_TRUE(ruvia::split_uri_host_port("host:", host, port, has_port));
    EXPECT_EQ(host, "host");
    EXPECT_TRUE(port.empty());
    EXPECT_TRUE(has_port);
    ASSERT_TRUE(ruvia::split_uri_host_port("host", host, port, has_port));
    EXPECT_FALSE(has_port);
    ASSERT_TRUE(ruvia::split_uri_host_port(":nonnumeric", host, port, has_port));
    EXPECT_TRUE(host.empty());
    EXPECT_EQ(port, "nonnumeric");
    for (const std::string_view invalid : {"[::1", "[::1]suffix", "[::1]:80:90", "host:80:90", "[[::1]", "host[", "host]", "[::1]:8]"}) {
        EXPECT_FALSE(ruvia::split_uri_host_port(invalid, host, port, has_port)) << invalid;
    }
}

TEST(RuviaHttp, authority_host_kinds_and_raw_ports_preserve_rfc_receiver_syntax) {
    struct authority_case final {
        std::string_view text;
        ruvia::http_host_kind kind;
    };
    for (const auto& entry : std::array{
             authority_case{"example.test", ruvia::http_host_kind::reg_name},
             authority_case{"exa%6dple.test", ruvia::http_host_kind::reg_name},
             authority_case{"192.0.2.1", ruvia::http_host_kind::ipv4},
             authority_case{"[2001:db8::1]", ruvia::http_host_kind::ipv6},
             authority_case{"[v1.a:b]", ruvia::http_host_kind::ipv_future}
         }) {
        const auto parsed = ruvia::parse_http_authority(entry.text);
        ASSERT_TRUE(parsed) << entry.text;
        EXPECT_EQ(parsed->host_kind(), entry.kind);
    }
    constexpr std::string_view input = "[::1]:00443";
    const auto authority = ruvia::parse_http_authority(input);
    ASSERT_TRUE(authority);
    EXPECT_EQ(authority->port_text(), "00443");
    EXPECT_EQ(authority->port_text().data(), input.data() + 6);
    EXPECT_EQ(authority->port(), 443);
    for (const std::string_view text : {"example.test:0", "example.test:00080", "example.test:", "[v1.a:b]:80"}) {
        EXPECT_TRUE(ruvia::parse_http_authority(text)) << text;
        EXPECT_FALSE(ruvia::parse_http_authority(text, ruvia::http_authority_policy::endpoint)) << text;
    }
    for (const std::string_view text : {"example.test", "example.test:1", "example.test:65535", "[::1]:443"}) {
        EXPECT_TRUE(ruvia::parse_http_authority(text, ruvia::http_authority_policy::endpoint)) << text;
    }
    for (const std::string_view text : {"example.test:65536", "example.test:+80", "[::1%2]:80", "[::1%252]:80", "user@example.test", "[]", "[v.a]", "[::1]:80:90"}) {
        EXPECT_FALSE(ruvia::parse_http_authority(text)) << text;
    }
}

TEST(RuviaHttp, full_url_decomposition_is_borrowed_and_preserves_origin_form) {
    constexpr std::string_view input = "HtTpS://[2001:db8::1]:00443/a%20b?x=%2f#section";
    const auto url = ruvia::parse_http_url(input);
    ASSERT_TRUE(url);
    EXPECT_EQ(url->scheme(), "HtTpS");
    EXPECT_EQ(url->scheme().data(), input.data());
    EXPECT_EQ(url->authority_text(), "[2001:db8::1]:00443");
    EXPECT_EQ(url->authority_text().data(), input.data() + 8);
    EXPECT_EQ(url->authority().host_kind(), ruvia::http_host_kind::ipv6);
    EXPECT_EQ(url->authority().port_text(), "00443");
    EXPECT_EQ(url->path(), "/a%20b");
    EXPECT_EQ(url->query(), "x=%2f");
    EXPECT_EQ(url->fragment(), "section");
    EXPECT_EQ(url->target()[0], "/a%20b?x=%2f");
    EXPECT_TRUE(url->target()[1].empty());
    EXPECT_EQ(url->target()[0].data(), url->path().data());
    EXPECT_FALSE(ruvia::parse_http_url(input, ruvia::http_authority_policy::endpoint));

    for (const std::string_view scheme : {"http", "https", "ws", "wss", "WS", "WSS"}) {
        const std::string text = std::string(scheme) + "://example.test";
        const auto bare = ruvia::parse_http_url(text);
        ASSERT_TRUE(bare);
        EXPECT_FALSE(bare->has_path());
        EXPECT_EQ(bare->path(), "/");
        EXPECT_FALSE(bare->query());
        EXPECT_FALSE(bare->fragment());
        EXPECT_EQ(bare->target()[0], "/");
        EXPECT_TRUE(bare->target()[1].empty());
    }
    const auto query_only = ruvia::parse_http_url("wss://example.test?x=1#");
    ASSERT_TRUE(query_only);
    EXPECT_FALSE(query_only->has_path());
    EXPECT_EQ(query_only->path(), "/");
    EXPECT_EQ(query_only->query(), "x=1");
    ASSERT_TRUE(query_only->fragment());
    EXPECT_TRUE(query_only->fragment()->empty());
    EXPECT_EQ(query_only->target()[0], "/");
    EXPECT_EQ(query_only->target()[1], "?x=1");
    const auto empty_query = ruvia::parse_http_url("http://example.test/?");
    ASSERT_TRUE(empty_query);
    ASSERT_TRUE(empty_query->query());
    EXPECT_TRUE(empty_query->query()->empty());
    EXPECT_EQ(empty_query->target()[0], "/?");
    EXPECT_TRUE(ruvia::parse_http_url("http://[v1.a]:0/?q=%00"));
    EXPECT_FALSE(ruvia::parse_http_url("http://[v1.a]:0/", ruvia::http_authority_policy::endpoint));
}

TEST(RuviaHttp, full_url_decomposition_rejects_invalid_components) {
    for (const std::string_view text : {"", "ftp://example.test/", "//example.test/", "http:/example.test/", "http:///path", "http://user@example.test/", "http://example.test:65536/", "http://[::1%2]/", "http://example.test/a b", "http://example.test/a\\b", "http://example.test/%", "http://example.test/%00", "http://example.test/?q=%gg", "http://example.test/a[", "http://example.test/?q=[", "http://example.test/#bad%", "http://example.test/#a#b"}) {
        EXPECT_FALSE(ruvia::parse_http_url(text)) << text;
    }
    EXPECT_FALSE(ruvia::parse_http_url(std::string_view("http://example.test/a\0b", 23)));
}
