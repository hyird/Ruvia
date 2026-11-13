#include <gtest/gtest.h>

#include <cstddef>
#include <new>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/http/cookies.h"
#include "ruvia/http/http_response.h"
#include "ruvia/http/http_set_cookie.h"

TEST(Ruvia, response_cookie_plan_and_raw_headers_share_replacement_semantics) {
    ruvia::http_response response;
    response.header("Set-Cookie", "first=one");
    const ruvia::cookie_options options;
    response.set_cookie(ruvia::set_cookie_plan("second", "two", options));
    EXPECT_EQ(response.headers().size(), std::size_t{2});

    response.header("Set-Cookie", "final=three");
    EXPECT_EQ(response.headers().size(), std::size_t{1});
    EXPECT_EQ(response.header("Set-Cookie").value_or(""), "final=three");
}

TEST(Ruvia, response_cookie_plan_replaces_itself_at_parser_limits) {
    ruvia::http_response response;
    const std::string path(1024, '/');
    const std::string value(4096 - 9 - 3, 'v');
    ruvia::cookie_options options;
    options.secure_ = ruvia::cookie_attribute_policy::emit;
    options.prefix_ = ruvia::cookie_prefix::secure;
    options.path_ = path;
    response.set_cookie(ruvia::set_cookie_plan("sid", value, options));
    response.set_cookie(ruvia::set_cookie_plan("sid", value, options));
    EXPECT_EQ(response.headers().size(), std::size_t{1});
}

TEST(Ruvia, response_cookie_partitioned_and_unpartitioned_storage_keys_are_distinct) {
    ruvia::http_response response;
    const ruvia::cookie_options partitioned{
        .secure_ = ruvia::cookie_attribute_policy::emit,
        .partitioned_ = ruvia::cookie_attribute_policy::emit};
    const ruvia::cookie_options ordinary{.secure_ = ruvia::cookie_attribute_policy::emit};
    response.set_cookie(ruvia::set_cookie_plan("session", "partitioned-first", partitioned));
    response.set_cookie(ruvia::set_cookie_plan("session", "ordinary-first", ordinary));
    response.set_cookie(ruvia::set_cookie_plan("session", "partitioned-middle", partitioned));
    EXPECT_EQ(response.headers().size(), std::size_t{2});
    response.header("Set-Cookie", "session=ordinary-final; Path=/; Secure",
        {.mode_ = ruvia::http_response_header_mode::append});
    response.header("Set-Cookie", "session=partitioned-final; Path=/; Secure; Partitioned",
        {.mode_ = ruvia::http_response_header_mode::append});

    std::size_t partitioned_count = 0;
    std::size_t ordinary_count = 0;
    for (const auto& header : response.headers()) {
        const auto cookie = ruvia::parse_set_cookie(header.value());
        EXPECT_TRUE(cookie.has_value());
        if (!cookie) {
            continue;
        }
        EXPECT_EQ(cookie->name(), "session");
        if (cookie->has(ruvia::http_set_cookie_attribute::partitioned)) {
            ++partitioned_count;
            EXPECT_EQ(cookie->value(), "partitioned-final");
        } else {
            ++ordinary_count;
            EXPECT_EQ(cookie->value(), "ordinary-final");
        }
    }
    EXPECT_EQ(partitioned_count, std::size_t{1});
    EXPECT_EQ(ordinary_count, std::size_t{1});
}

TEST(Ruvia, response_appended_values_can_borrow_existing_header_storage) {
    ruvia::http_response response;
    response.header("X-Source", "borrowed");
    const auto value = *response.header("X-Source");
    response.header("Link", std::string_view{}, {.mode_ = ruvia::http_response_header_mode::append});
    for (std::size_t i = 0; i < 64; ++i) {
        response.header("Link", value, {.mode_ = ruvia::http_response_header_mode::append});
    }
    EXPECT_EQ(response.headers().size(), std::size_t{66});
    std::size_t populated = 0;
    for (const auto& header : response.headers()) {
        if (header.name() == "Link" && !header.value().empty()) {
            ++populated;
            EXPECT_EQ(header.value(), "borrowed");
        }
    }
    EXPECT_EQ(populated, std::size_t{64});
}

// RFC 6265 section 5.2.4: an empty Path selects the default-path, the same
// storage key as an absent Path, so each later field replaces the earlier one.
