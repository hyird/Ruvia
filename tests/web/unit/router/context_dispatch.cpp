#include <system_error>

#include "ruvia/web/body_limit.h"

#include "routing_fixture.h"

RUVIA_MODEL(dispatch_validation_request, RUVIA_REQUIRED_FIELD(value, ruvia::string));

namespace {

class dispatch_authorization final : public ruvia::middleware {
public:
    ruvia::task<void> handle(ruvia::context& context_value, ruvia::next& next_value) {
        if (context_value.req().header("Authorization") != "Bearer allowed") {
            context_value.status(ruvia::http_status::unauthorized);
            context_value.respond(context_value.body("denied"));
            co_return;
        }
        co_await next_value();
    }
};

struct dispatch_observation {
    int calls_{0};
    std::pmr::memory_resource* parent_arena_{nullptr};
    ruvia::testing::test_context* checks_{nullptr};
};

ruvia::task<ruvia::http_response> dispatch_child(void* opaque, ruvia::context& context_value) {
    auto& observation_value = *static_cast<dispatch_observation*>(opaque);
    auto& ruvia_ctx = *observation_value.checks_;
    ++observation_value.calls_;
    RUVIA_CHECK(context_value.is_subrequest());
    RUVIA_CHECK(context_value.worker().is_current());
    RUVIA_CHECK(context_value.arena() != observation_value.parent_arena_);
    RUVIA_CHECK_EQ(context_value.req().param("id").value(), std::string_view("42"));
    RUVIA_CHECK_EQ(context_value.req().query("filter").value(), std::string_view("active"));
    const auto body = context_value.req().validated_json<dispatch_validation_request>();
    RUVIA_CHECK(&body.value() == &context_value.req().validated<dispatch_validation_request>());
    RUVIA_CHECK_EQ(body.raw(), std::string_view(R"({"value":"retained response"})"));
    context_value.header("X-Child", "complete");
    co_return context_value.body(body.value().get<"value">().view());
}

ruvia::task<ruvia::http_response> dispatch_parent(void* opaque, ruvia::context& context_value) {
    auto& observation_value = *static_cast<dispatch_observation*>(opaque);
    auto& ruvia_ctx = *observation_value.checks_;
    observation_value.parent_arena_ = context_value.arena();
    RUVIA_CHECK(!context_value.is_subrequest());
    std::array headers{ruvia::http_header_view{"Content-Type", "application/json"},
        ruvia::http_header_view{"Authorization", "Bearer allowed"}};
    std::string input = R"({"value":"retained response"})";
    std::string target = "/child/42?filter=active";
    auto operation = context_value.dispatch({.method_ = "POST", .target_ = target, .headers_ = headers, .body_ = input});
    input.assign("overwritten");
    target.assign("/wrong");
    headers[1] = ruvia::http_header_view{"Authorization", "denied"};
    const auto first = co_await std::move(operation);
    RUVIA_CHECK_EQ(first.status(), ruvia::http_status::ok);
    RUVIA_CHECK_EQ(first.body(), std::string_view("retained response"));
    RUVIA_CHECK_EQ(first.header("x-child").value(), std::string_view("complete"));

    const auto denied = co_await context_value.dispatch({.method_ = "POST", .target_ = "/child/42?filter=active", .headers_ = headers, .body_ = "{}"});
    RUVIA_CHECK_EQ(denied.status(), ruvia::http_status::unauthorized);
    headers[1] = ruvia::http_header_view{"Authorization", "Bearer allowed"};
    const auto invalid = co_await context_value.dispatch({.method_ = "POST", .target_ = "/child/42?filter=active", .headers_ = headers, .body_ = "{}"});
    RUVIA_CHECK_EQ(invalid.status(), ruvia::http_status::bad_request);
    const auto missing = co_await context_value.dispatch({.method_ = "GET", .target_ = "/missing"});
    RUVIA_CHECK_EQ(missing.status(), ruvia::http_status::not_found);

    const std::string oversized(128, 'x');
    const auto limited = co_await context_value.dispatch({.method_ = "POST", .target_ = "/child/42?filter=active", .headers_ = headers, .body_ = oversized});
    RUVIA_CHECK_EQ(limited.status(), ruvia::http_status::content_too_large);
    for (const std::string_view bad_target : {"https://example.test/child", "//example.test/child", "/child\r\nInjected: true"}) {
        RUVIA_CHECK(ruvia::testing::throws_on([&] {
            (void)context_value.dispatch({.method_ = "GET", .target_ = bad_target});
        }));
    }
    const std::array framing_header{ruvia::http_header_view{"Content-Length", "0"}};
    RUVIA_CHECK(ruvia::testing::throws_on([&] {
        (void)context_value.dispatch({.method_ = "POST", .target_ = "/child/42", .headers_ = framing_header});
    }));

    ruvia::stop_source canceled;
    canceled.request_stop();
    bool stopped = false;
    try {
        (void)co_await context_value.dispatch({.method_ = "POST", .target_ = "/child/42?filter=active", .headers_ = headers, .body_ = R"({"value":"should not run"})", .operation_ = {.stop_token_ = canceled.token()}});
    } catch (const std::system_error&) {
        stopped = true;
    }
    RUVIA_CHECK(stopped);
    RUVIA_CHECK_EQ(observation_value.calls_, 1);
    RUVIA_CHECK_EQ(first.body(), std::string_view("retained response"));
    co_return context_value.body("passed");
}

}  // namespace

RUVIA_TEST(context_dispatch_owns_inputs_and_runs_authorized_validated_child_on_current_worker) {
    dispatch_observation observation_value{.checks_ = &ruvia_ctx};
    ruvia::detail::router router;
    auto& impl = ruvia::detail::router_impl::from(router);
    const std::array middleware_value{
        ruvia::detail::make_middleware_descriptor<ruvia::body_limit<64>>(),
        ruvia::detail::make_middleware_descriptor<dispatch_authorization>(),
        ruvia::detail::make_middleware_descriptor<ruvia::json_body<dispatch_validation_request>>()};
    impl.register_route(http_known_method::post, path("/child/:id"), route_handler_type(&observation_value, &dispatch_child),
        request_body_mode::buffered, std::span<const controller_middleware_descriptor>{}, std::span(middleware_value));
    impl.register_route(http_known_method::get, path("/parent"), route_handler_type(&observation_value, &dispatch_parent),
        request_body_mode::buffered, std::span<const controller_middleware_descriptor>{}, std::span<const controller_middleware_descriptor>{});
    impl.finalize();
    ruvia::worker_memory worker_memory;
    ruvia::request_memory memory(worker_memory);
    auto request = make_request(memory, "GET", "/parent");
    asio::io_context io;
    auto attachment = ruvia::attach_event_loop(io);
    const auto worker_value = attachment.loop().handle();
    const ruvia::stop_token stop;
    const auto services = ruvia::detail::context_services(worker_value, stop);
    std::exception_ptr failure;
    dispatch_result result;
    asio::co_spawn(io, ruvia::detail::task_as_awaitable(impl.route_table().dispatch(request, memory, services)),
        [&](std::exception_ptr error, ruvia::http_response response) {
            failure = error;
            if (!error) {
                result = extract_dispatch_result(response);
            }
            attachment.stop();
        });
    attachment.run();
    if (failure) {
        std::rethrow_exception(failure);
    }
    RUVIA_CHECK_EQ(result.status_, 200);
    RUVIA_CHECK_EQ(result.body_, std::string("passed"));
}
