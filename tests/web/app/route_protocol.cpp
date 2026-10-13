#include "test_harness.h"

// A CONNECT protocol route for a protocol no request can select would be dead,
// so startup must fail instead of serving without it. The controller registry
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
    RUVIA_CONNECT_PROTOCOL("foo", "/foo/:host/:port", tunnel);
    RUVIA_ROUTES_END

private:
    ruvia::task<void> tunnel(ruvia::context&) {
        co_return;
    }
};

}  // namespace

RUVIA_TEST(route_protocol_rejects_unsupported_connect_protocol_at_startup) {
    ruvia::test_app app;
    std::string message;
    try {
        (void)app.request(ruvia::test_request::get("/foo/example.test/53"));
    } catch (const std::invalid_argument& error) {
        message = error.what();
    }
    RUVIA_CHECK(message.find("unsupported CONNECT protocol") != std::string::npos);
}
