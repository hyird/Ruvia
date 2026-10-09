#include <cstddef>
#include <memory_resource>
#include <new>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/http/cookies.h"
#include "ruvia/http/detail/response/http_response_header_state.h"
#include "ruvia/http/http_response.h"
#include "ruvia/http/http_set_cookie.h"
#include "ruvia/http/http_set_cookie_plan.h"

#include "failing_memory_resource.h"
#include "response/response_header_utils.h"
#include "test_harness.h"

namespace {

constexpr std::string_view cookie_values[]{
    "session=first; Path=/app; Domain=.Example.test",
    "session = middle; Domain= example.TEST; Path= /app",
    "session=final; Path=/app; Domain=example.test"};

void populate_cookies(ruvia::http_response& response) {
    response.header("Content-Type", "text/plain");
    for (const auto value : cookie_values) {
        ruvia::detail::append_response_header_validated(response, "Set-Cookie", value,
            ruvia::detail::response_header_set_cookie);
    }
    const ruvia::http_response::header_options_type append{.mode_ = ruvia::http_response_header_mode::append};
    response.header("Set-Cookie", "session=other; Path=/other; Domain=example.test", append);
    response.header("Set-Cookie", "Session=upper; Path=/app; Domain=example.test", append);
    response.header("X-After", "after");
}

}  // namespace

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

RUVIA_TEST(response_cookie_append_accepts_values_borrowed_from_repeated_rows) {
    for (const auto selected : cookie_values) {
        failing_memory_resource resource;
        {
            ruvia::http_response response({.resource_ = &resource});
            populate_cookies(response);
            const auto live = resource.live_allocations();
            std::string_view borrowed;
            for (const auto& field : response.headers()) {
                if (field.value() == selected) {
                    borrowed = field.value();
                }
            }
            RUVIA_CHECK(!borrowed.empty());
            response.header("Set-Cookie", borrowed, {.mode_ = ruvia::http_response_header_mode::append});

            RUVIA_CHECK_EQ(response.headers().size(), std::size_t{5});
            RUVIA_CHECK_EQ(resource.live_allocations(), live - 2);
            std::size_t cookies = 0;
            bool has_selected = false;
            bool has_other_path = false;
            bool has_upper_name = false;
            for (const auto& field : response.headers()) {
                if (field.name() == "Set-Cookie") {
                    ++cookies;
                    has_selected = has_selected || field.value() == selected;
                    has_other_path = has_other_path || field.value() == "session=other; Path=/other; Domain=example.test";
                    has_upper_name = has_upper_name || field.value() == "Session=upper; Path=/app; Domain=example.test";
                }
            }
            RUVIA_CHECK_EQ(cookies, std::size_t{3});
            RUVIA_CHECK(has_selected);
            RUVIA_CHECK(has_other_path);
            RUVIA_CHECK(has_upper_name);
            RUVIA_CHECK_EQ(response.header("Content-Type").value_or(""), "text/plain");
            RUVIA_CHECK_EQ(response.header("X-After").value_or(""), "after");
        }
        RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
    }
}

RUVIA_TEST(response_cookie_append_preserves_empty_attribute_presence_with_borrowed_value) {
    failing_memory_resource resource;
    {
        ruvia::http_response response({.resource_ = &resource});
        response.header("Set-Cookie", "sid=first; Path=; Domain=");
        ruvia::detail::append_response_header_validated(response, "Set-Cookie", "sid = second; Path=",
            ruvia::detail::response_header_set_cookie);
        response.header("Set-Cookie", "sid=no-path", {.mode_ = ruvia::http_response_header_mode::append});
        const auto borrowed = response.headers().begin()[1].value();
        response.header("Set-Cookie", borrowed, {.mode_ = ruvia::http_response_header_mode::append});

        RUVIA_CHECK_EQ(response.headers().size(), std::size_t{2});
        RUVIA_CHECK_EQ(response.headers().begin()[0].value(), "sid = second; Path=");
        RUVIA_CHECK_EQ(response.headers().begin()[1].value(), "sid=no-path");
    }
    RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
}

RUVIA_TEST(response_cookie_append_failure_preserves_borrowed_value_for_retry) {
    failing_memory_resource resource;
    {
        ruvia::http_response response({.resource_ = &resource});
        populate_cookies(response);
        const auto borrowed = response.header("Set-Cookie").value();
        const std::string expected(borrowed);
        const auto live = resource.live_allocations();
        resource.fail_after(0);
        bool failed = false;
        try {
            response.header("Set-Cookie", borrowed, {.mode_ = ruvia::http_response_header_mode::append});
        } catch (const std::bad_alloc&) {
            failed = true;
        }
        RUVIA_CHECK(failed);
        RUVIA_CHECK_EQ(borrowed, expected);
        RUVIA_CHECK_EQ(response.headers().size(), std::size_t{7});
        RUVIA_CHECK_EQ(resource.live_allocations(), live);
        resource.allow_allocations();
        response.header("Set-Cookie", borrowed, {.mode_ = ruvia::http_response_header_mode::append});
        RUVIA_CHECK_EQ(response.headers().size(), std::size_t{5});
        RUVIA_CHECK_EQ(response.header("Set-Cookie").value_or(""), expected);
    }
    RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
}

RUVIA_TEST(response_cookie_plan_consumes_fields_before_retiring_repeated_rows) {
    for (const auto selected : cookie_values) {
        failing_memory_resource resource;
        {
            ruvia::http_response response({.resource_ = &resource});
            populate_cookies(response);
            std::string_view borrowed;
            for (const auto& field : response.headers()) {
                if (field.value() == selected) {
                    borrowed = field.value();
                }
            }
            const auto parsed_value = ruvia::parse_set_cookie(borrowed);
            RUVIA_CHECK(parsed_value.has_value());
            if (!parsed_value) {
                continue;
            }
            const ruvia::cookie_options options{.path_ = parsed_value->path(), .domain_ = parsed_value->domain()};
            const ruvia::set_cookie_plan plan(parsed_value->name(), parsed_value->value(), options);
            const auto expected = std::string("session=") + std::string(parsed_value->value()) +
                                  "; Path=/app; Domain=" + std::string(parsed_value->domain());
            response.set_cookie(plan);
            RUVIA_CHECK_EQ(response.headers().size(), std::size_t{5});
            RUVIA_CHECK_EQ(response.header("Set-Cookie").value_or(""), expected);
            RUVIA_CHECK_EQ(response.header("Content-Type").value_or(""), "text/plain");
            RUVIA_CHECK_EQ(response.header("X-After").value_or(""), "after");
            std::size_t other_scopes = 0;
            for (const auto& field : response.headers()) {
                other_scopes += field.value() == "session=other; Path=/other; Domain=example.test" ||
                                field.value() == "Session=upper; Path=/app; Domain=example.test";
            }
            RUVIA_CHECK_EQ(other_scopes, std::size_t{2});
        }
        RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
    }
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

RUVIA_TEST(response_cookie_plan_nonalias_equal_size_update_reuses_storage) {
    failing_memory_resource resource;
    {
        ruvia::http_response response({.resource_ = &resource});
        const ruvia::cookie_options options{.path_ = "/", .domain_ = "example.test"};
        const ruvia::set_cookie_plan first("sid", "old", options);
        response.set_cookie(first);
        const auto* storage = response.header("Set-Cookie")->data();
        const auto live = resource.live_allocations();
        const ruvia::set_cookie_plan second("sid", "new", options);
        resource.fail_after(0);
        response.set_cookie(second);
        RUVIA_CHECK(response.header("Set-Cookie")->data() == storage);
        RUVIA_CHECK_EQ(response.header("Set-Cookie").value_or(""), "sid=new; Path=/; Domain=example.test");
        RUVIA_CHECK_EQ(resource.live_allocations(), live);
    }
    RUVIA_CHECK_EQ(resource.live_allocations(), std::size_t{0});
}

RUVIA_TEST(response_cookie_plan_keeps_prefixed_keys_with_large_values) {
    failing_memory_resource resource;
    {
        ruvia::http_response response({.resource_ = &resource});
        response.header("Set-Cookie", "__Host-sid=first; Path=/; Secure");
        ruvia::detail::append_response_header_validated(response, "Set-Cookie", "__Host-sid=second; Path=/; Secure",
            ruvia::detail::response_header_set_cookie);
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
