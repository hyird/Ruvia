#include <system_error>

#include "ruvia/web/BodyLimit.h"

#include "routing_fixture.h"

namespace {

class DispatchAuthorization final : public ruvia::Middleware<DispatchAuthorization> {
public:
    ruvia::Task<void> handle(ruvia::Context& context, ruvia::Next& next) {
        if (context.req().header("Authorization") != "Bearer allowed") {
            context.status(ruvia::http_status::kUnauthorized);
            context.respond(context.body("denied"));
            co_return;
        }
        co_await next();
    }
};

struct DispatchObservation {
    int calls{0};
    std::pmr::memory_resource* parentArena{nullptr};
    ruvia::testing::TestContext* checks{nullptr};
};

ruvia::Task<ruvia::HttpResponse> dispatchChild(void* opaque, ruvia::Context& context) {
    auto& observation = *static_cast<DispatchObservation*>(opaque);
    auto& ruvia_ctx = *observation.checks;
    ++observation.calls;
    RUVIA_CHECK(context.isSubrequest());
    RUVIA_CHECK(context.worker().isCurrent());
    RUVIA_CHECK(context.arena() != observation.parentArena);
    RUVIA_CHECK_EQ(context.req().param("id").value(), std::string_view("42"));
    RUVIA_CHECK_EQ(context.req().query("filter").value(), std::string_view("active"));
    const auto body = co_await context.req().json<ScopedValidationRequest>();
    context.header("X-Child", "complete");
    co_return context.body(body.get<"value">()->view());
}

ruvia::Task<ruvia::HttpResponse> dispatchParent(void* opaque, ruvia::Context& context) {
    auto& observation = *static_cast<DispatchObservation*>(opaque);
    auto& ruvia_ctx = *observation.checks;
    observation.parentArena = context.arena();
    RUVIA_CHECK(!context.isSubrequest());
    std::array headers{ruvia::HttpHeaderView{"Content-Type", "application/json"},
        ruvia::HttpHeaderView{"Authorization", "Bearer allowed"}};
    std::string input = R"({"value":"retained response"})";
    std::string target = "/child/42?filter=active";
    auto operation = context.dispatch({.method = "POST", .target = target, .headers = headers, .body = input});
    input.assign("overwritten");
    target.assign("/wrong");
    headers[1] = ruvia::HttpHeaderView{"Authorization", "denied"};
    const auto first = co_await std::move(operation);
    RUVIA_CHECK_EQ(first.status(), ruvia::http_status::kOk);
    RUVIA_CHECK_EQ(first.body(), std::string_view("retained response"));
    RUVIA_CHECK_EQ(first.header("x-child").value(), std::string_view("complete"));

    const auto denied = co_await context.dispatch({.method = "POST", .target = "/child/42?filter=active", .headers = headers, .body = "{}"});
    RUVIA_CHECK_EQ(denied.status(), ruvia::http_status::kUnauthorized);
    headers[1] = ruvia::HttpHeaderView{"Authorization", "Bearer allowed"};
    const auto invalid = co_await context.dispatch({.method = "POST", .target = "/child/42?filter=active", .headers = headers, .body = "{}"});
    RUVIA_CHECK_EQ(invalid.status(), ruvia::http_status::kBadRequest);
    const auto missing = co_await context.dispatch({.method = "GET", .target = "/missing"});
    RUVIA_CHECK_EQ(missing.status(), ruvia::http_status::kNotFound);

    const std::string oversized(128, 'x');
    const auto limited = co_await context.dispatch({.method = "POST", .target = "/child/42?filter=active", .headers = headers, .body = oversized});
    RUVIA_CHECK_EQ(limited.status(), ruvia::http_status::kContentTooLarge);
    for (const std::string_view badTarget : {"https://example.test/child", "//example.test/child", "/child\r\nInjected: true"}) {
        RUVIA_CHECK(ruvia::testing::throwsOn([&] {
            (void)context.dispatch({.method = "GET", .target = badTarget});
        }));
    }
    const std::array framingHeader{ruvia::HttpHeaderView{"Content-Length", "0"}};
    RUVIA_CHECK(ruvia::testing::throwsOn([&] {
        (void)context.dispatch({.method = "POST", .target = "/child/42", .headers = framingHeader});
    }));

    ruvia::StopSource canceled;
    canceled.requestStop();
    bool stopped = false;
    try {
        (void)co_await context.dispatch({.method = "POST", .target = "/child/42?filter=active", .headers = headers, .body = R"({"value":"should not run"})", .operation = {.stopToken = canceled.token()}});
    } catch (const std::system_error&) {
        stopped = true;
    }
    RUVIA_CHECK(stopped);
    RUVIA_CHECK_EQ(observation.calls, 1);
    RUVIA_CHECK_EQ(first.body(), std::string_view("retained response"));
    co_return context.body("passed");
}

}  // namespace

RUVIA_TEST(context_dispatch_owns_inputs_and_runs_authorized_validated_child_on_current_worker) {
    DispatchObservation observation{.checks = &ruvia_ctx};
    ruvia::detail::Router router;
    auto& impl = ruvia::detail::RouterImpl::from(router);
    const std::array middleware{
        ruvia::detail::makeMiddlewareDescriptor<ruvia::BodyLimit<64>>(),
        ruvia::detail::makeMiddlewareDescriptor<DispatchAuthorization>(),
        ruvia::detail::makeMiddlewareDescriptor<ScopedValidationValidator>()};
    impl.registerRoute(HttpKnownMethod::kPost, path("/child/:id"), RouteHandler(&observation, &dispatchChild),
        RequestBodyMode::kBuffered, std::span<const ControllerMiddlewareDescriptor>{}, std::span(middleware));
    impl.registerRoute(HttpKnownMethod::kGet, path("/parent"), RouteHandler(&observation, &dispatchParent),
        RequestBodyMode::kBuffered, std::span<const ControllerMiddlewareDescriptor>{}, std::span<const ControllerMiddlewareDescriptor>{});
    impl.finalize();
    ruvia::WorkerMemory workerMemory;
    ruvia::RequestMemory memory(workerMemory);
    auto request = ruvia::detail::HttpRequestAccess::make();
    ruvia::detail::HttpRequestAccess::reset(request);
    ruvia::detail::HttpRequestAccess::setMethod(request, "GET");
    ruvia::detail::HttpRequestAccess::setPath(request, "/parent");
    ruvia::detail::HttpRequestAccess::setResource(request, memory.resource());
    asio::io_context io;
    auto attachment = ruvia::attachEventLoop(io);
    const auto worker = attachment.loop().handle();
    const ruvia::StopToken stop;
    const auto services = ruvia::detail::ContextServices(worker, stop);
    std::exception_ptr failure;
    DispatchResult result;
    asio::co_spawn(io, ruvia::detail::taskAsAwaitable(impl.routeTable().dispatch(request, memory, services)),
        [&](std::exception_ptr error, ruvia::HttpResponse response) {
            failure = error;
            if (!error) {
                result = extractDispatchResult(response);
            }
            attachment.stop();
        });
    attachment.run();
    if (failure) {
        std::rethrow_exception(failure);
    }
    RUVIA_CHECK_EQ(result.status, 200);
    RUVIA_CHECK_EQ(result.body, std::string("passed"));
}
