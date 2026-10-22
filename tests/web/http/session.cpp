#include "ruvia/web/session.h"

#include <cstddef>
#include <string_view>

#include "ruvia/web/context.h"
#include "ruvia/web/controller.h"
#include "ruvia/web/testing.h"

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
    config.redis_alias_ = "";
    RUVIA_CHECK_EQ(rejection(config), std::string_view("session Redis alias must not be empty"));

    config = ruvia::session_config{};
    config.cookie_name_ = "bad cookie";
    RUVIA_CHECK_EQ(
        rejection(config), std::string_view("session cookie name must be a valid HTTP token"));

    config = ruvia::session_config{};
    config.key_prefix_ = "";
    RUVIA_CHECK_EQ(rejection(config), std::string_view("session key prefix must not be empty"));

    config = ruvia::session_config{};
    config.ttl_ = std::chrono::seconds::zero();
    RUVIA_CHECK_EQ(rejection(config), std::string_view("session TTL must be greater than zero"));

    // A new session is written with a millisecond expiry.
    constexpr auto max_ttl = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::milliseconds::max());
    config.ttl_ = max_ttl + std::chrono::seconds(1);
    RUVIA_CHECK_EQ(rejection(config), std::string_view("session TTL must be representable in milliseconds"));
    config.ttl_ = std::chrono::seconds::max();
    RUVIA_CHECK_EQ(rejection(config), std::string_view("session TTL must be representable in milliseconds"));
    config.ttl_ = max_ttl;
    RUVIA_CHECK_EQ(rejection(config), std::string_view{});
}

namespace session_test {

// Writes a cookie under the session's name before clearing the session, so the
// commit stage finds a same-name Set-Cookie already in the response. The
// session middleware only runs on matched requests, so this needs a route.
class same_name_cookie_controller final {
public:
    RUVIA_CONTROLLER_GROUP("/session-test")
    RUVIA_ROUTES_BEGIN(same_name_cookie_controller)
    RUVIA_GET("/same-name-cookie", same_name_cookie);
    RUVIA_ROUTES_END

private:
    ruvia::task<ruvia::http_response> same_name_cookie(ruvia::context& c) {
        c.set_cookie({.name_ = "ruvia_session", .value_ = "handler"});
        c.session().clear();
        co_return c.text("ok");
    }
};

}  // namespace session_test

RUVIA_TEST(session_cookie_replaces_a_same_name_cookie_set_by_the_handler) {
    ruvia::test_app app;
    {
        ruvia::session_config config;
        config.cookie_name_ = "ruvia_session";
        app.use<ruvia::session_middleware>(config);
    }

    const auto response = app.request(ruvia::test_request::get("/session-test/same-name-cookie"));
    RUVIA_CHECK_EQ(response.status(), ruvia::http_status::ok);
    std::size_t session_cookies = 0;
    bool expired = false;
    for (const auto& header : response.headers()) {
        if (header.name() != std::string_view("Set-Cookie") || !header.value().starts_with("ruvia_session=")) {
            continue;
        }
        ++session_cookies;
        expired = expired || header.value().starts_with("ruvia_session=;");
    }
    RUVIA_CHECK_EQ(session_cookies, std::size_t{1});
    RUVIA_CHECK(expired);
}

#endif
