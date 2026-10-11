// Request policies and local dispatch; no external services are required.
// Build: cmake --build build --config Release --target ruvia_example_request_policies -j$(nproc)
// Run the executable, then try:
//   curl -i http://127.0.0.1:8091/policies/slow
//   curl -i -d hello http://127.0.0.1:8091/policies/small
//   curl -i -H "X-Demo-Name: reader" http://127.0.0.1:8091/policies/dispatch
// Expected: a deadline response, an echo, and "reader (subrequest)" respectively.
// GET /policies/csrf issues XSRF-TOKEN. For POST, send that cookie AND copy its
// value into X-XSRF-TOKEN; a missing or mismatched value is rejected with 403.

#include <array>
#include <chrono>
#include <string>

#include "ruvia/core/timer.h"
#include "ruvia/web/app.h"
#include "ruvia/web/body_limit.h"
#include "ruvia/web/controller.h"
#include "ruvia/web/csrf.h"
#include "ruvia/web/deadline.h"
#include "ruvia/web/dispatch.h"

namespace {

struct request_label final {
    std::string_view name_;
};

class label_middleware final : public ruvia::middleware {
public:
    ruvia::task<void> handle(ruvia::context& c, ruvia::next& next_value) {
        // The binding borrows this local value until next() finishes. Neither
        // the label nor its request-backed string_view may escape the request.
        const request_label label{c.req().header("X-Demo-Name").value_or("guest")};
        auto binding = c.bind_request_state(label);
        co_await next_value();
    }
};

class policy_controller final : public ruvia::controller<policy_controller> {
public:
    RUVIA_CONTROLLER_GROUP("/policies", label_middleware)
    RUVIA_ROUTES_BEGIN
    RUVIA_POST("/small", small, ruvia::body_limit<16>);
    RUVIA_GET("/slow", slow, ruvia::deadline<100>);
    RUVIA_GET("/label", label);
    RUVIA_GET("/dispatch", dispatch);
    RUVIA_GET("/csrf", csrf, ruvia::csrf_protection);
    RUVIA_POST("/csrf", csrf, ruvia::csrf_protection);
    RUVIA_ROUTES_END

private:
    ruvia::task<ruvia::http_response> small(ruvia::context& c) {
        // body_limit is checked before buffering. More than 16 bytes produces
        // 413; a route may tighten, but cannot enlarge, the server-wide limit.
        const auto body = co_await c.req().text();
        co_return c.text(body);
    }

    ruvia::task<ruvia::http_response> slow(ruvia::context& c) {
        // Deadlines are cooperative: propagate the request token to custom
        // waits, then await their completion before releasing request state.
        // db(), redis(), get_http_client(), and run_blocking() already inherit it.
        const auto result_value = co_await ruvia::sleep_for(c.worker(), std::chrono::seconds(1), c.get_stop_token());
        if (result_value == ruvia::timer_sleep_result::stop_requested) {
            co_return c.error({.status_ = ruvia::http_status::gateway_timeout,
                .code_ = "deadline",
                .message_ = "the request stopped waiting"});
        }
        co_return c.text("completed\n");
    }

    ruvia::task<ruvia::http_response> label(ruvia::context& c) {
        const auto& value = c.request_state<request_label>();
        std::pmr::string text(value.name_, c.arena());
        text.append(c.is_subrequest() ? " (subrequest)\n" : " (network request)\n");
        co_return c.text(std::move(text));
    }

    ruvia::task<ruvia::http_response> dispatch(ruvia::context& c) {
        // Re-enter the production router on this worker. Child requests have
        // fresh state, so credentials and other headers must be passed explicitly.
        // Only buffered requests/responses are supported; streams, files, SSE,
        // and upgrades belong on their own network routes. Nesting is bounded.
        const std::array<ruvia::http_header_view, 1> headers{{
            {"X-Demo-Name", c.request_state<request_label>().name_},
        }};
        auto response = co_await c.dispatch({
            .method_ = "GET",
            .target_ = "/policies/label",
            .headers_ = headers,
        });
        c.status(response.status());
        // dispatch_response owns pooled bytes; text() copies them into this
        // response before the child result is destroyed on the same worker.
        co_return c.text(response.body());
    }

    ruvia::task<ruvia::http_response> csrf(ruvia::context& c) {
        co_return c.text("CSRF check passed\n");
    }
};

}  // namespace

int main() {
    ruvia::app()
        .server({.process_signal_handlers_ = ruvia::process_signal_handler_policy::install,
            .max_buffered_body_bytes_ = 1024})
        .listen({.address_ = "127.0.0.1", .http_ = 8091})
        .deadline({.handler_ = std::chrono::seconds(2)})
        .run();
}
