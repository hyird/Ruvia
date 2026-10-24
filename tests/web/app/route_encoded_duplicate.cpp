#include "test_harness.h"

// Two spellings of one path ("/dup/admin" and "/dup/%61dmin") name one route:
// the router matches requests in that canonical form, so registering both
// must fail startup instead of leaving one handler unreachable. The controller
// registry is process-wide, so this conflict lives in its own test executable.
// Public API only.

#include <stdexcept>

#include "ruvia/web/app.h"
#include "ruvia/web/context.h"
#include "ruvia/web/controller.h"
#include "ruvia/web/testing.h"

namespace {

class route_encoded_duplicate_controller final {
public:
    RUVIA_ROUTES_BEGIN(route_encoded_duplicate_controller)
    RUVIA_GET("/dup/admin", first);
    RUVIA_GET("/dup/%61dmin", second);
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

RUVIA_TEST(route_encoded_duplicate_rejects_equivalent_spellings_at_startup) {
    ruvia::test_app app;
    bool rejected = false;
    try {
        (void)app.request(ruvia::test_request::get("/dup/admin"));
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    RUVIA_CHECK(rejected);
}
