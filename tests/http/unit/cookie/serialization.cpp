#include <chrono>
#include <cstdint>
#include <string>

#include "ruvia/http/cookies.h"
#include "ruvia/http/http_set_cookie.h"
#include "ruvia/http/http_set_cookie_plan.h"

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
    constexpr std::int64_t ages[]{0, 1, 9, 10, 99, 100, 999, 1000, ruvia::max_cookie_age_seconds};
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
