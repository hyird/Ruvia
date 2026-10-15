#include "test_harness.h"

// Extended CONNECT protocol identifiers must be HTTP tokens; an invalid route
// fails at startup before any request can be served. The controller registry
// is process-wide, so this route lives in its own test executable. Public API
// only.

#include <stdexcept>
#include <string>
#include <string_view>

#include "ruvia/web/app.h"
#include "ruvia/web/context.h"
#include "ruvia/web/controller.h"
#include "ruvia/web/testing.h"

namespace {

class route_protocol_controller final {
public:
    RUVIA_ROUTES_BEGIN(route_protocol_controller)
    RUVIA_CONNECT_PROTOCOL("bad protocol", "/foo/:host/:port", tunnel);
    RUVIA_ROUTES_END

private:
    ruvia::task<void> tunnel(ruvia::context&) {
        co_return;
    }
};

}  // namespace

RUVIA_TEST(route_protocol_rejects_invalid_connect_protocol_at_startup) {
    ruvia::test_app app;
    std::string message;
    try {
        (void)app.request(ruvia::test_request::get("/foo/example.test/53"));
    } catch (const std::invalid_argument& error) {
        message = error.what();
    }
    RUVIA_CHECK(message.find("CONNECT protocol must be an HTTP token") != std::string::npos);
}
