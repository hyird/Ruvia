#include "ruvia/http/port_number.h"

#include <array>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string_view>

#include "ruvia/http/http_client.h"
#include "ruvia/http/http_request_target.h"

#include "test_harness.h"

RUVIA_TEST(port_number_rejects_wide_inputs_without_changing_an_existing_origin) {
    const auto rejects = [](auto input) {
        try {
            (void)ruvia::http_origin_view::https({.host_ = "example.test", .port_ = input});
        } catch (const std::invalid_argument&) {
            return true;
        }
        return false;
    };
    const std::array<std::int64_t, 7> invalid{
        std::numeric_limits<std::int64_t>::min(), -65093, -1,
        65536, 65979, 131071, std::numeric_limits<std::int64_t>::max()};
    for (const auto input : invalid) {
        RUVIA_CHECK(rejects(input));
        RUVIA_CHECK(!ruvia::port_number::try_from_value(input).has_value());
        ruvia::http_origin_options options{.host_ = "example.test", .port_ = 8443};
        bool assignment_rejected = false;
        try {
            options.port_ = input;
        } catch (const std::invalid_argument&) {
            assignment_rejected = true;
        }
        RUVIA_CHECK(assignment_rejected);
        const auto retained = ruvia::http_origin_view::https(options);
        RUVIA_CHECK_EQ(ruvia::make_http_origin_authority(retained), "example.test:8443");
    }
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    RUVIA_CHECK(rejects(maximum));
    RUVIA_CHECK(!ruvia::port_number::try_from_value(maximum).has_value());
}

RUVIA_TEST(port_number_preserves_zero_maximum_and_scheme_default_authorities) {
    struct port_case final {
        std::uint64_t input_;
        std::string_view authority_;
    };
    for (const auto entry : std::array{
             port_case{0, "[::1]:0"},
             port_case{1, "[::1]:1"},
             port_case{80, "[::1]:80"},
             port_case{443, "[::1]"},
             port_case{65535, "[::1]:65535"}}) {
        const auto origin = ruvia::http_origin_view::https({.host_ = "[::1]", .port_ = entry.input_});
        RUVIA_CHECK_EQ(ruvia::make_http_origin_authority(origin), entry.authority_);
    }
    const auto absent = ruvia::http_origin_view::http({.host_ = "example.test"});
    const auto zero = ruvia::http_origin_view::http({.host_ = "example.test", .port_ = 0});
    RUVIA_CHECK_EQ(ruvia::make_http_origin_authority(absent), "example.test");
    RUVIA_CHECK_EQ(ruvia::make_http_origin_authority(zero), "example.test:0");
}

RUVIA_TEST(port_number_rejects_invalid_authority_defaults_before_origin_comparison) {
    const auto authority = ruvia::parse_http_authority("example.test");
    RUVIA_CHECK(authority.has_value());
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
    for (const auto source : std::array<std::int64_t, 7>{
             std::numeric_limits<std::int64_t>::min(), -65093, -1,
             65536, 65979, 131071, std::numeric_limits<std::int64_t>::max()}) {
        RUVIA_CHECK(rejects([&] { (void)authority->effective_port(source); }));
        RUVIA_CHECK(rejects([&] {
            (void)ruvia::http_authorities_equal("example.test", "example.test:443", source);
        }));
    }
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    RUVIA_CHECK(rejects([&] { (void)authority->effective_port(maximum); }));
    RUVIA_CHECK(rejects([&] {
        (void)ruvia::http_authorities_equal("example.test", "example.test:443", maximum);
    }));
    RUVIA_CHECK(ruvia::http_authorities_equal("example.test", "EXAMPLE.test:443", 443));
    RUVIA_CHECK(!ruvia::http_authorities_equal("example.test", "example.test:443", 80));
}

RUVIA_TEST(port_number_preserves_absent_empty_and_explicit_authority_port_rules) {
    const auto absent = ruvia::parse_http_authority("[::1]");
    const auto empty = ruvia::parse_http_authority("[::1]:");
    const auto explicit_port = ruvia::parse_http_authority("[::1]:8443");
    RUVIA_CHECK(absent && empty && explicit_port);
    if (!absent || !empty || !explicit_port) {
        return;
    }
    for (const auto source : std::array<std::uint64_t, 5>{0, 1, 80, 443, 65535}) {
        RUVIA_CHECK(absent->effective_port(source) == source);
        RUVIA_CHECK(empty->effective_port(source) == source);
        RUVIA_CHECK(explicit_port->effective_port(source) == 8443);
        RUVIA_CHECK(ruvia::http_authorities_equal("[::1]", "[::1]:", source));
    }
    RUVIA_CHECK(!ruvia::http_authorities_equal("[::1]", "[::1]:0", 0));
    RUVIA_CHECK(ruvia::http_authorities_equal("[::1]:", "[::1]:65535", 65535));
    RUVIA_CHECK(!ruvia::http_authorities_equal("[::1]:8443", "[::1]:443", 443));
}
