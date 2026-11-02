#include "ruvia/http/port_number.h"

#include <array>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string_view>

#include "ruvia/http/http_client.h"

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
