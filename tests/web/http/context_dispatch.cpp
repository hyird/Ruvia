#include <string_view>

#include "ruvia/web/app.h"
#include "ruvia/web/context.h"
#include "ruvia/web/controller.h"
#include "ruvia/web/testing.h"

#include "test_harness.h"

namespace {

class context_dispatch_controller final : public ruvia::controller<context_dispatch_controller> {
public:
    RUVIA_CONTROLLER_GROUP("/context-dispatch")
    RUVIA_ROUTES_BEGIN
    RUVIA_GET("/parent", parent);
    RUVIA_GET("/child", child);
    RUVIA_ROUTES_END

private:
    ruvia::task<ruvia::http_response> parent(ruvia::context& c) {
        // The child request carries no Early-Data field of its own.
        auto response = co_await c.dispatch({.method_ = "GET", .target_ = "/context-dispatch/child"});
        c.status(response.status());
        co_return c.text(response.body());
    }

    ruvia::task<ruvia::http_response> child(ruvia::context& c) {
        const auto early_data = c.early_data_info();
        if (!c.is_subrequest() || early_data.received_from_early_data()) {
            co_return c.text("unexpected");
        }
        co_return c.text(std::string_view(early_data.upstream_declared_early_data() ? "declared" : "none"));
    }
};

}  // namespace

RUVIA_TEST(context_dispatch_subrequest_inherits_parent_early_data_snapshot) {
    ruvia::test_app app;

    const auto declared = app.request(ruvia::test_request::get("/context-dispatch/parent").header("Early-Data", "1"));
    RUVIA_CHECK_EQ(declared.status(), ruvia::http_status::ok);
    RUVIA_CHECK_EQ(declared.body(), std::string_view("declared"));

    const auto plain = app.request(ruvia::test_request::get("/context-dispatch/parent"));
    RUVIA_CHECK_EQ(plain.status(), ruvia::http_status::ok);
    RUVIA_CHECK_EQ(plain.body(), std::string_view("none"));
}
