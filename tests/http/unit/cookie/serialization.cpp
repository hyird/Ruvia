#include <chrono>
#include <cstdint>
#include <string>

#include "ruvia/http/cookies.h"
#include "ruvia/http/http_set_cookie.h"

#include "test_harness.h"

RUVIA_TEST(cookie_plan_serialization_emits_fields_without_attributes) {
    const ruvia::cookie_options options{.path_ = ""};
    const ruvia::set_cookie_plan plan("sid", "value", options);
    std::string wire(plan.size(), '\0');
    plan.write(wire.data());
    RUVIA_CHECK_EQ(wire, "sid=value");
    const auto parsed_value = ruvia::parse_set_cookie(wire);
    RUVIA_CHECK(parsed_value.has_value());
    if (parsed_value) {
        RUVIA_CHECK_EQ(parsed_value->name(), "sid");
        RUVIA_CHECK(parsed_value->name().data() == wire.data());
        RUVIA_CHECK(parsed_value->path().empty());
        RUVIA_CHECK(parsed_value->domain().empty());
    }
}

RUVIA_TEST(cookie_plan_serialization_formats_exact_max_age_widths_and_prefixed_fields) {
    constexpr std::int64_t ages[]{1, 9, 10, 99, 100, 999, 1000, ruvia::max_cookie_age_seconds};
    for (const auto age : ages) {
        const ruvia::cookie_options options{
            .path_ = "/", .max_age_ = std::chrono::seconds{age}, .prefix_ = ruvia::cookie_prefix::host, .secure_ = ruvia::cookie_attribute_policy::emit};
        const ruvia::set_cookie_plan plan("sid", "value", options);
        std::string wire(plan.size(), '\0');
        plan.write(wire.data());
        RUVIA_CHECK_EQ(wire, "__Host-sid=value; Path=/; Max-Age=" + std::to_string(age) + "; Secure");
        const auto parsed_value = ruvia::parse_set_cookie(wire);
        RUVIA_CHECK(parsed_value.has_value());
        if (parsed_value) {
            RUVIA_CHECK_EQ(parsed_value->name(), "__Host-sid");
            RUVIA_CHECK_EQ(parsed_value->path(), "/");
            RUVIA_CHECK(parsed_value->domain().empty());
            RUVIA_CHECK(parsed_value->name().data() == wire.data());
            RUVIA_CHECK(parsed_value->path().data() == wire.data() + wire.find('/'));
        }
    }
}

RUVIA_TEST(cookie_plan_serialization_writes_zero_max_age_as_deletion_expires) {
    // max-age-av = "Max-Age=" non-zero-digit *DIGIT (RFC 6265 section 4.1.1).
    const ruvia::cookie_options options{.path_ = "/",
        .expires_ = std::chrono::system_clock::time_point{std::chrono::hours{24 * 365 * 40}},
        .max_age_ = std::chrono::seconds{0}};
    const ruvia::set_cookie_plan plan("sid", "", options);
    std::string wire(plan.size(), '\0');
    plan.write(wire.data());
    RUVIA_CHECK_EQ(wire, "sid=; Path=/; Expires=Thu, 01 Jan 1970 00:00:00 GMT");
}
