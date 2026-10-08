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

#include "ruvia/core/Timer.h"
#include "ruvia/web/App.h"
#include "ruvia/web/BodyLimit.h"
#include "ruvia/web/Controller.h"
#include "ruvia/web/Csrf.h"
#include "ruvia/web/Deadline.h"
#include "ruvia/web/Dispatch.h"

namespace {

struct request_label final {
    std::string_view name;
};

class label_middleware final : public ruvia::Middleware {
public:
    ruvia::Task<void> handle(ruvia::Context& c, ruvia::Next& next) {
        // The binding borrows this local value until next() finishes. Neither
        // the label nor its request-backed string_view may escape the request.
        const request_label label{c.req().header("X-Demo-Name").value_or("guest")};
        auto binding = c.bindRequestState(label);
        co_await next();
    }
};

class policy_controller final : public ruvia::Controller<policy_controller> {
public:
    RUVIA_CONTROLLER_GROUP("/policies", label_middleware)
    RUVIA_ROUTES_BEGIN
    RUVIA_POST("/small", small, ruvia::BodyLimit<16>);
    RUVIA_GET("/slow", slow, ruvia::Deadline<100>);
    RUVIA_GET("/label", label);
    RUVIA_GET("/dispatch", dispatch);
    RUVIA_GET("/csrf", csrf, ruvia::CsrfProtection);
    RUVIA_POST("/csrf", csrf, ruvia::CsrfProtection);
    RUVIA_ROUTES_END

private:
    ruvia::Task<ruvia::HttpResponse> small(ruvia::Context& c) {
        // BodyLimit is checked before buffering. More than 16 bytes produces
        // 413; a route may tighten, but cannot enlarge, the server-wide limit.
        const auto body = co_await c.req().text();
        co_return c.text(body);
    }

    ruvia::Task<ruvia::HttpResponse> slow(ruvia::Context& c) {
        // Deadlines are cooperative: propagate the request token to custom
        // waits, then await their completion before releasing request state.
        // db(), redis(), httpClient(), and runBlocking() already inherit it.
        const auto result = co_await ruvia::sleepFor(c.worker(), std::chrono::seconds(1), c.stopToken());
        if (result == ruvia::TimerSleepResult::kStopRequested) {
            co_return c.error({.status = ruvia::http_status::kGatewayTimeout,
                .code = "deadline",
                .message = "the request stopped waiting"});
        }
        co_return c.text("completed\n");
    }

    ruvia::Task<ruvia::HttpResponse> label(ruvia::Context& c) {
        const auto& value = c.requestState<request_label>();
        std::pmr::string text(value.name, c.arena());
        text.append(c.isSubrequest() ? " (subrequest)\n" : " (network request)\n");
        co_return c.text(std::move(text));
    }

    ruvia::Task<ruvia::HttpResponse> dispatch(ruvia::Context& c) {
        // Re-enter the production router on this worker. Child requests have
        // fresh state, so credentials and other headers must be passed explicitly.
        // Only buffered requests/responses are supported; streams, files, SSE,
        // and upgrades belong on their own network routes. Nesting is bounded.
        const std::array<ruvia::HttpHeaderView, 1> headers{{
            {"X-Demo-Name", c.requestState<request_label>().name},
        }};
        auto response = co_await c.dispatch({
            .method = "GET",
            .target = "/policies/label",
            .headers = headers,
        });
        c.status(response.status());
        // DispatchResponse owns pooled bytes; text() copies them into this
        // response before the child result is destroyed on the same worker.
        co_return c.text(response.body());
    }

    ruvia::Task<ruvia::HttpResponse> csrf(ruvia::Context& c) {
        co_return c.text("CSRF check passed\n");
    }
};

}  // namespace

int main() {
    ruvia::app()
        .server({.process_signal_handlers = ruvia::process_signal_handler_policy::install,
            .max_buffered_body_bytes = 1024})
        .listen({.address = "127.0.0.1", .http = 8091})
        .deadline({.handler = std::chrono::seconds(2)})
        .run();
}
