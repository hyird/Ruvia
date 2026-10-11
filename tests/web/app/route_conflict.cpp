#include "test_harness.h"

// Two dynamic routes of one method that differ only in parameter names and a
// trailing slash occupy the same lookup slot: neither can take precedence, so
// startup must fail instead of letting registration order pick a handler. The
// controller registry is process-wide, so this conflict lives in its own test
// executable. Public API only.

#include <stdexcept>
#include <string>
#include <string_view>

#include "ruvia/web/app.h"
#include "ruvia/web/context.h"
#include "ruvia/web/controller.h"
#include "ruvia/web/testing.h"

namespace {

class route_conflict_controller final : public ruvia::controller<route_conflict_controller> {
public:
    RUVIA_ROUTES_BEGIN
    RUVIA_GET("/conflict/:id/files", first);
    RUVIA_GET("/conflict/:name/files/", second);
    RUVIA_ROUTES_END

private:
    ruvia::task<ruvia::http_response> first(ruvia::context& c) {
        co_return c.text("first");
    }

    ruvia::task<ruvia::http_response> second(ruvia::context& c) {
        co_return c.text("second");
    }
};

}  // namespace

RUVIA_TEST(route_conflict_rejects_equal_precedence_dynamic_routes_at_startup) {
    ruvia::test_app app;
    std::string message;
    try {
        (void)app.request(ruvia::test_request::get("/conflict/1/files"));
    } catch (const std::invalid_argument& error) {
        message = error.what();
    }
    RUVIA_CHECK(message.find("GET /conflict/:id/files") != std::string::npos);
    RUVIA_CHECK(message.find("GET /conflict/:name/files/") != std::string::npos);
}
