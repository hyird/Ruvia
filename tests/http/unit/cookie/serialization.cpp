#include <chrono>
#include <cstdint>
#include <string>

#include "ruvia/http/Cookies.h"
#include "ruvia/http/HttpSetCookie.h"
#include "ruvia/http/HttpSetCookiePlan.h"

#include "test_harness.h"

RUVIA_TEST(cookie_plan_serialization_emits_fields_without_attributes) {
    const ruvia::CookieOptions options{.path = ""};
    const ruvia::SetCookiePlan plan("sid", "value", options);
    std::string wire(plan.size(), '\0');
    plan.write(wire.data());
    RUVIA_CHECK_EQ(wire, "sid=value");
    const auto parsed = ruvia::parseSetCookie(wire);
    RUVIA_CHECK(parsed.has_value());
    if (parsed) {
        RUVIA_CHECK_EQ(parsed->name(), "sid");
        RUVIA_CHECK(parsed->name().data() == wire.data());
        RUVIA_CHECK(parsed->path().empty());
        RUVIA_CHECK(parsed->domain().empty());
    }
}

RUVIA_TEST(cookie_plan_serialization_formats_exact_max_age_widths_and_prefixed_fields) {
    constexpr std::int64_t ages[]{0, 1, 9, 10, 99, 100, 999, 1000, ruvia::kMaxCookieAgeSeconds};
    for (const auto age : ages) {
        const ruvia::CookieOptions options{
            .path = "/", .maxAge = std::chrono::seconds{age}, .prefix = ruvia::CookiePrefix::kHost, .secure = ruvia::CookieAttributePolicy::kEmit};
        const ruvia::SetCookiePlan plan("sid", "value", options);
        std::string wire(plan.size(), '\0');
        plan.write(wire.data());
        RUVIA_CHECK_EQ(wire, "__Host-sid=value; Path=/; Max-Age=" + std::to_string(age) + "; Secure");
        const auto parsed = ruvia::parseSetCookie(wire);
        RUVIA_CHECK(parsed.has_value());
        if (parsed) {
            RUVIA_CHECK_EQ(parsed->name(), "__Host-sid");
            RUVIA_CHECK_EQ(parsed->path(), "/");
            RUVIA_CHECK(parsed->domain().empty());
            RUVIA_CHECK(parsed->name().data() == wire.data());
            RUVIA_CHECK(parsed->path().data() == wire.data() + wire.find('/'));
        }
    }
}
