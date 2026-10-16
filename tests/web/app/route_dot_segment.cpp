#include "test_harness.h"

// A route pattern containing a dot segment can never be matched: resolve()
// rejects every request carrying one before lookup (RFC 3986 6.2.2.2). Such a
// registration must fail at startup instead of installing an unreachable
// handler. The controller registry is process-wide, so this lives in its own
// test executable. Public API only.

#include <stdexcept>
#include <string>

#include "ruvia/web/app.h"
#include "ruvia/web/context.h"
#include "ruvia/web/controller.h"
#include "ruvia/web/testing.h"

namespace {

class route_dot_segment_controller final {
public:
    RUVIA_ROUTES_BEGIN(route_dot_segment_controller)
    RUVIA_GET("/a/../b", unreachable);
    RUVIA_ROUTES_END

private:
    ruvia::task<ruvia::http_response> unreachable(ruvia::context& c) {
        co_return c.text("unreachable");
    }
};

}  // namespace

RUVIA_TEST(route_dot_segment_pattern_is_rejected_at_startup) {
    ruvia::test_app app;
    std::string message;
    try {
        (void)app.request(ruvia::test_request::get("/b"));
    } catch (const std::invalid_argument& error) {
        message = error.what();
    }
    RUVIA_CHECK(message.find("dot segment") != std::string::npos);
}
