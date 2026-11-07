#include <gtest/gtest.h>

#include <string>
#include <string_view>

#include "ruvia/web/redis/redis_types.h"

TEST(Ruvia, redis_error_uses_runtime_error_message_and_stable_code) {
    const ruvia::redis_error error(ruvia::redis_error::code_type::timeout, "redis timed out");
    EXPECT_TRUE(error.code() == ruvia::redis_error::code_type::timeout);
    EXPECT_EQ(std::string_view(error.what()), std::string_view("redis timed out"));
}
