#include <cstddef>
#include <memory_resource>
#include <new>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/http/cookies.h"
#include "ruvia/http/http_response.h"
#include "ruvia/http/http_set_cookie.h"
#include "ruvia/http/http_set_cookie_plan.h"

#include "failing_memory_resource.h"
#include "test_harness.h"

RUVIA_TEST(response_cookie_plan_and_raw_headers_share_replacement_semantics) {
    ruvia::http_response response;
    response.header("Set-Cookie", "first=one");
    const ruvia::cookie_options options;
    response.set_cookie(ruvia::set_cookie_plan("second", "two", options));
    RUVIA_CHECK_EQ(response.headers().size(), std::size_t{2});

    response.header("Set-Cookie", "final=three");
    RUVIA_CHECK_EQ(response.headers().size(), std::size_t{1});
    RUVIA_CHECK_EQ(response.header("Set-Cookie").value_or(""), "final=three");
}

RUVIA_TEST(response_cookie_partitioned_and_unpartitioned_storage_keys_are_distinct) {
    ruvia::http_response response;
    const ruvia::cookie_options partitioned{
        .secure_ = ruvia::cookie_attribute_policy::emit,
        .partitioned_ = ruvia::cookie_attribute_policy::emit};
    const ruvia::cookie_options ordinary{.secure_ = ruvia::cookie_attribute_policy::emit};
    response.set_cookie(ruvia::set_cookie_plan("session", "partitioned-first", partitioned));
    response.set_cookie(ruvia::set_cookie_plan("session", "ordinary-first", ordinary));
    response.set_cookie(ruvia::set_cookie_plan("session", "partitioned-middle", partitioned));
    RUVIA_CHECK_EQ(response.headers().size(), std::size_t{2});
    response.header("Set-Cookie", "session=ordinary-final; Path=/; Secure",
        {.mode_ = ruvia::http_response_header_mode::append});
    response.header("Set-Cookie", "session=partitioned-final; Path=/; Secure; Partitioned",
        {.mode_ = ruvia::http_response_header_mode::append});

    std::size_t partitioned_count = 0;
    std::size_t ordinary_count = 0;
    for (const auto& header : response.headers()) {
        const auto cookie = ruvia::parse_set_cookie(header.value());
        RUVIA_CHECK(cookie.has_value());
        if (!cookie) {
            continue;
        }
        RUVIA_CHECK_EQ(cookie->name(), "session");
        if (cookie->has(ruvia::http_set_cookie_attribute::partitioned)) {
            ++partitioned_count;
            RUVIA_CHECK_EQ(cookie->value(), "partitioned-final");
        } else {
            ++ordinary_count;
            RUVIA_CHECK_EQ(cookie->value(), "ordinary-final");
        }
    }
    RUVIA_CHECK_EQ(partitioned_count, std::size_t{1});
    RUVIA_CHECK_EQ(ordinary_count, std::size_t{1});
}

RUVIA_TEST(response_cookie_plan_append_can_retry_after_allocation_failure) {
    failing_memory_resource resource;
    {
        ruvia::http_response response({.resource_ = &resource});
        response.header("Set-Cookie", "first=one");
        const ruvia::cookie_options options;
        const ruvia::set_cookie_plan plan("second", "two", options);
        const auto live = resource.live_allocations();
        resource.fail_after(0);
        bool failed = false;
        try {
            response.set_cookie(plan);
        } catch (const std::bad_alloc&) {
            failed = true;
        }
        RUVIA_CHECK(failed);
        RUVIA_CHECK_EQ(response.headers().size(), std::size_t{1});
        RUVIA_CHECK_EQ(response.header("Set-Cookie").value_or(""), "first=one");
        RUVIA_CHECK_EQ(resource.live_allocations(), live);

        resource.allow_allocations();
        response.set_cookie(plan);
        response.header("Set-Cookie", "final=three");
        RUVIA_CHECK_EQ(response.headers().size(), std::size_t{1});
        RUVIA_CHECK_EQ(response.header("Set-Cookie").value_or(""), "final=three");
    }
    RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
}

RUVIA_TEST(response_appended_values_can_borrow_existing_header_storage) {
    ruvia::http_response response;
    response.header("X-Source", "borrowed");
    const auto value = *response.header("X-Source");
    response.header("Link", std::string_view{}, {.mode_ = ruvia::http_response_header_mode::append});
    for (std::size_t i = 0; i < 64; ++i) {
        response.header("Link", value, {.mode_ = ruvia::http_response_header_mode::append});
    }
    RUVIA_CHECK_EQ(response.headers().size(), std::size_t{66});
    std::size_t populated = 0;
    for (const auto& header : response.headers()) {
        if (header.name() == "Link" && !header.value().empty()) {
            ++populated;
            RUVIA_CHECK_EQ(header.value(), "borrowed");
        }
    }
    RUVIA_CHECK_EQ(populated, std::size_t{64});
}

RUVIA_TEST(response_cookie_append_preserves_empty_attribute_presence_with_borrowed_value) {
    failing_memory_resource resource;
    {
        ruvia::http_response response({.resource_ = &resource});
        response.header("Set-Cookie", "sid=first; Path=; Domain=");
        response.header("Set-Cookie", "sid = second; Path=",
            {.mode_ = ruvia::http_response_header_mode::append});
        response.header("Set-Cookie", "sid=no-path", {.mode_ = ruvia::http_response_header_mode::append});
        const auto borrowed = response.headers().begin()[0].value();
        response.header("Set-Cookie", borrowed, {.mode_ = ruvia::http_response_header_mode::append});

        RUVIA_CHECK_EQ(response.headers().size(), std::size_t{2});
        RUVIA_CHECK_EQ(response.headers().begin()[0].value(), "sid = second; Path=");
        RUVIA_CHECK_EQ(response.headers().begin()[1].value(), "sid=no-path");
    }
    RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
}

RUVIA_TEST(response_cookie_plan_alias_allocation_failure_preserves_each_input_for_retry) {
    for (std::size_t alias = 0; alias < 4; ++alias) {
        failing_memory_resource resource;
        {
            ruvia::http_response response({.resource_ = &resource});
            response.header("Set-Cookie", "sid=old; Path=/; Domain=example.test");
            const auto parsed_value = ruvia::parse_set_cookie(response.header("Set-Cookie").value());
            RUVIA_CHECK(parsed_value.has_value());
            if (!parsed_value) {
                continue;
            }
            const auto name = alias == 0 ? parsed_value->name() : std::string_view("sid");
            const auto value = alias == 1 ? parsed_value->value() : std::string_view("new");
            const ruvia::cookie_options options{
                .path_ = alias == 2 ? parsed_value->path() : std::string_view("/"),
                .domain_ = alias == 3 ? parsed_value->domain() : std::string_view("example.test")};
            const ruvia::set_cookie_plan plan(name, value, options);
            const auto expected = std::string("sid=") + std::string(value) + "; Path=/; Domain=example.test";
            const auto live = resource.live_allocations();
            resource.fail_after(0);
            bool failed = false;
            try {
                response.set_cookie(plan);
            } catch (const std::bad_alloc&) {
                failed = true;
            }
            RUVIA_CHECK(failed);
            RUVIA_CHECK_EQ(response.header("Set-Cookie").value_or(""), "sid=old; Path=/; Domain=example.test");
            RUVIA_CHECK_EQ(resource.live_allocations(), live);
            RUVIA_CHECK_EQ(parsed_value->name(), "sid");
            RUVIA_CHECK_EQ(parsed_value->value(), "old");
            RUVIA_CHECK_EQ(parsed_value->path(), "/");
            RUVIA_CHECK_EQ(parsed_value->domain(), "example.test");
            resource.allow_allocations();
            response.set_cookie(plan);
            RUVIA_CHECK_EQ(response.header("Set-Cookie").value_or(""), expected);
        }
        RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
    }
}

RUVIA_TEST(response_cookie_plan_keeps_prefixed_keys_with_large_values) {
    failing_memory_resource resource;
    {
        ruvia::http_response response({.resource_ = &resource});
        response.header("Set-Cookie", "__Host-sid=first; Path=/; Secure");
        const std::string value(5000, 'v');
        const ruvia::cookie_options options{
            .path_ = "/", .prefix_ = ruvia::cookie_prefix::host, .secure_ = ruvia::cookie_attribute_policy::emit};
        const ruvia::set_cookie_plan plan("sid", value, options);
        response.set_cookie(plan);
        RUVIA_CHECK_EQ(response.headers().size(), std::size_t{1});
        RUVIA_CHECK_EQ(response.header("Set-Cookie").value_or(""),
            std::string("__Host-sid=") + value + "; Path=/; Secure");
    }
    RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
}
