#include <string>
#include <string_view>

#include "ruvia/web/redis/redis_types.h"

#include "test_harness.h"

RUVIA_TEST(redis_error_uses_runtime_error_message_and_stable_code) {
    const ruvia::redis_error error(ruvia::redis_error::code_type::timeout, "redis timed out");
    RUVIA_CHECK(error.code() == ruvia::redis_error::code_type::timeout);
    RUVIA_CHECK_EQ(std::string_view(error.what()), std::string_view("redis timed out"));
}
