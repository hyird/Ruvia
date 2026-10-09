#include "ruvia/web/session.h"

#include <string_view>

#include "test_harness.h"

#ifdef RUVIA_ENABLE_REDIS

RUVIA_TEST(session_middleware_rejects_invalid_config_before_use) {
    const auto rejection = [](const ruvia::session_config& config) {
        try {
            const ruvia::session_middleware middleware(config);
        } catch (const std::invalid_argument& error) {
            return std::string(error.what());
        }
        return std::string{};
    };

    auto config = ruvia::session_config{};
    config.redis_alias_.clear();
    RUVIA_CHECK_EQ(rejection(config), std::string_view("session Redis alias must not be empty"));

    config = ruvia::session_config{};
    config.cookie_name_ = "bad cookie";
    RUVIA_CHECK_EQ(
        rejection(config), std::string_view("session cookie name must be a valid HTTP token"));

    config = ruvia::session_config{};
    config.key_prefix_.clear();
    RUVIA_CHECK_EQ(rejection(config), std::string_view("session key prefix must not be empty"));

    config = ruvia::session_config{};
    config.ttl_ = std::chrono::seconds::zero();
    RUVIA_CHECK_EQ(rejection(config), std::string_view("session TTL must be greater than zero"));
}

#endif
