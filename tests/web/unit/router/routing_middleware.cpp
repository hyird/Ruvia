#include <string_view>

#include "ruvia/core/asio_task.h"

#include "routing_fixture.h"

// Routing: the middleware chain around a route.

RUVIA_TEST(validated_model_binding_spans_next_and_unwinds_before_upstream_resumes) {
    for (const bool handler_throws : {false, true}) {
        scoped_validation_handler_read = false;
        scoped_validation_raw_read = false;
        scoped_validation_handler_throws = handler_throws;
        validation_scope_probe::released_after_next = false;

        ruvia::detail::router router;
        auto& impl = ruvia::detail::router_impl::from(router);
        const std::array middlewares{
            ruvia::detail::make_middleware_descriptor<validation_scope_probe>(),
            ruvia::detail::make_middleware_descriptor<ruvia::json_body<scoped_validation_request>>()};
        impl.register_route(http_known_method::post, path("/validated-scope"),
            route_handler_type(nullptr, &scoped_validation_handler), request_body_mode::buffered,
            std::span<const controller_middleware_descriptor>{}, std::span(middlewares));
        impl.finalize();

        ruvia::worker_memory worker;
        ruvia::request_memory memory(worker);
        const std::array headers{ruvia::http_header_view{"Content-Type", "application/json"}};
        auto request = make_request(memory, "POST", "/validated-scope", headers, R"({"value":"ok"})");

        asio::io_context io_context(1);
        auto future = asio::co_spawn(io_context,
            ruvia::as_awaitable(
                impl.route_table().dispatch(request, memory, ruvia::test::test_context_services())),
            asio::use_future);
        io_context.run();
        const auto response = future.get();
        RUVIA_CHECK_EQ(response.status(),
            handler_throws ? ruvia::http_status::internal_server_error : ruvia::http_status::ok);
        RUVIA_CHECK(scoped_validation_handler_read);
        RUVIA_CHECK(scoped_validation_raw_read);
        RUVIA_CHECK(validation_scope_probe::released_after_next);
    }
}

RUVIA_TEST(head_only_stream_completion_unwinds_middleware_as_success) {
    g_chain_order.clear();
    g_head_only_handler_resumed_past_first_write = false;
    const controller_middleware_descriptor mws[] = {
        ruvia::detail::make_middleware_descriptor<chain_mw_a>(),
    };
    const auto observation_value =
        dispatch_head_only_stream(std::span<const controller_middleware_descriptor>(mws, 1));
    RUVIA_CHECK(observation_value.handled_);
    RUVIA_CHECK(!observation_value.threw_);
    RUVIA_CHECK(observation_value.ended_);
    // The middleware ran its pre-next() side, the handler started, and the
    // signal unwound the chain without converting into an error response
    // (which could no longer be sent past the committed head).
    const std::vector<int> expected{1, 0};
    RUVIA_CHECK(g_chain_order == expected);
}

RUVIA_TEST(middleware_chain_runs_in_onion_order) {
    g_chain_order.clear();
    const controller_middleware_descriptor mws[] = {
        ruvia::detail::make_middleware_descriptor<chain_mw_a>(),
        ruvia::detail::make_middleware_descriptor<chain_mw_b>(),
    };
    const auto body = dispatch_chain(std::span<const controller_middleware_descriptor>(mws, 2));
    RUVIA_CHECK_EQ(body, std::string("ok"));
    // Onion order: A pre, B pre, handler, B post, A post.
    const std::vector<int> expected{1, 2, 0, -2, -1};
    RUVIA_CHECK(g_chain_order == expected);
}

RUVIA_TEST(middleware_chain_short_circuits_without_next) {
    g_chain_order.clear();
    // A middleware that does not call next() stops the chain: the next middleware
    // and the handler never run, and the middleware's response is used.
    const controller_middleware_descriptor mws[] = {
        ruvia::detail::make_middleware_descriptor<chain_mw_stop>(),
        ruvia::detail::make_middleware_descriptor<chain_mw_a>(),
    };
    const auto body = dispatch_chain(std::span<const controller_middleware_descriptor>(mws, 2));
    RUVIA_CHECK_EQ(body, std::string("stopped"));
    const std::vector<int> expected{9};
    RUVIA_CHECK(g_chain_order == expected);
}

RUVIA_TEST(middleware_chain_rejects_next_after_response) {
    g_chain_order.clear();
    const controller_middleware_descriptor mws[] = {
        ruvia::detail::make_middleware_descriptor<chain_mw_respond_then_next>(),
    };
    const auto body = dispatch_chain(std::span<const controller_middleware_descriptor>(mws, 1));

    // respond() is terminal for the pre-next branch. The downstream handler
    // must not run, and the misuse must not silently return the early body.
    std::size_t handler_runs = 0;
    for (const int step : g_chain_order) {
        if (step == 0) {
            ++handler_runs;
        }
    }
    RUVIA_CHECK_EQ(handler_runs, std::size_t{0});
    RUVIA_CHECK((body.find("next_called_after_response") != std::string_view::npos));
}

RUVIA_TEST(middleware_chain_rejects_calling_next_twice) {
    g_chain_order.clear();
    const controller_middleware_descriptor mws[] = {
        ruvia::detail::make_middleware_descriptor<chain_mw_double_next>(),
    };
    const auto body = dispatch_chain(std::span<const controller_middleware_descriptor>(mws, 1));
    // The handler (which records 0) must run EXACTLY once: the first next() runs
    // it, the second is detected and converted to an error instead of re-entering
    // the chain. A regression to the double-invoke guard would run it twice.
    std::size_t handler_runs = 0;
    for (const int step : g_chain_order) {
        if (step == 0) {
            ++handler_runs;
        }
    }
    RUVIA_CHECK_EQ(handler_runs, std::size_t{1});
    // The response is the "next called multiple times" error, not the handler body.
    RUVIA_CHECK(body != std::string("ok"));
}

RUVIA_TEST(global_middleware_prepends_to_every_route_chain) {
    // application-wide middleware (application::use) materializes once and runs before the
    // route's own middleware on EVERY matched route.
    ruvia::detail::router router;
    auto& impl = ruvia::detail::router_impl::from(router);
    const controller_middleware_descriptor route_mws[] = {
        ruvia::detail::make_middleware_descriptor<chain_mw_b>(),
    };
    impl.register_route(http_known_method::get, path("/with-route-mw"),
        route_handler_type(nullptr, &chain_handler), request_body_mode::buffered, {},
        std::span<const controller_middleware_descriptor>(route_mws, 1));
    impl.register_route(http_known_method::get, path("/bare"), route_handler_type(nullptr, &chain_handler),
        request_body_mode::buffered, {}, {});
    const controller_middleware_descriptor globals[] = {
        ruvia::detail::make_middleware_descriptor<chain_mw_a>(),
    };
    impl.set_global_middlewares(std::span<const controller_middleware_descriptor>(globals, 1));
    impl.finalize();
    const auto& table_value = impl.route_table();

    const auto dispatch_path = [&table_value](std::string_view request_path) {
        ruvia::worker_memory worker;
        ruvia::request_memory memory(worker);
        auto request = make_request(memory, "GET", request_path);

        asio::io_context ctx(1);
        auto future = asio::co_spawn(ctx,
            ruvia::as_awaitable(
                table_value.dispatch(request, memory, ruvia::test::test_context_services())),
            asio::use_future);
        ctx.run();
        auto response = future.get();
        const auto body = ruvia::detail::response_body(response).bytes();
        return std::string(body.data(), body.size());
    };

    g_chain_order.clear();
    RUVIA_CHECK_EQ(dispatch_path("/with-route-mw"), std::string("ok"));
    // Global A wraps route-level B: A pre, B pre, handler, B post, A post.
    const std::vector<int> with_route_mw{1, 2, 0, -2, -1};
    RUVIA_CHECK(g_chain_order == with_route_mw);

    g_chain_order.clear();
    RUVIA_CHECK_EQ(dispatch_path("/bare"), std::string("ok"));
    // A route with no middleware of its own still runs the global.
    const std::vector<int> bare{1, 0, -1};
    RUVIA_CHECK(g_chain_order == bare);
}

RUVIA_TEST(global_middleware_registration_rejected_after_finalize) {
    ruvia::detail::router router;
    auto& impl = ruvia::detail::router_impl::from(router);
    impl.register_route(http_known_method::get, path("/sealed"), route_handler_type(nullptr, &chain_handler),
        request_body_mode::buffered, {}, {});
    const controller_middleware_descriptor globals[] = {
        ruvia::detail::make_middleware_descriptor<chain_mw_a>(),
    };
    impl.set_global_middlewares(std::span<const controller_middleware_descriptor>(globals, 1));
    impl.finalize();

    // Re-applying the identical set is the app stop()/run() restart path.
    impl.set_global_middlewares(std::span<const controller_middleware_descriptor>(globals, 1));

    // Changing the set after finalize must fail loudly.
    const controller_middleware_descriptor changed[] = {
        ruvia::detail::make_middleware_descriptor<chain_mw_b>(),
    };
    bool threw = false;
    try {
        impl.set_global_middlewares(std::span<const controller_middleware_descriptor>(changed, 1));
    } catch (const std::logic_error&) {
        threw = true;
    }
    RUVIA_CHECK(threw);
}

RUVIA_TEST(stream_route_middleware_mid_stream_failure_propagates_like_no_middleware) {
    // A stream handler on a route WITH middleware that commits the stream (writes a
    // chunk) then throws must surface the failure, exactly as the no-middleware path
    // does. Otherwise the middleware chain converts the throw into a buffered error
    // response, dispatch returns it for an already-committed stream, and the driver
    // finalizes the stream with a clean terminator -- framing a truncated body as a
    // complete one. dispatch_response_stream must therefore rethrow so the transport
    // aborts (connection close / RST_STREAM).
    ruvia::detail::router router;
    auto& impl = ruvia::detail::router_impl::from(router);
    const controller_middleware_descriptor mws[] = {
        ruvia::detail::make_middleware_descriptor<chain_mw_a>(),
    };
    impl.register_response_stream_route(http_known_method::get, path("/s"),
        ruvia::detail::route_stream_handler_type(nullptr, &stream_commit_then_throw),
        std::span<const controller_middleware_descriptor>{},
        std::span<const controller_middleware_descriptor>(mws, 1));
    impl.finalize();
    const auto& table_value = impl.route_table();

    ruvia::worker_memory worker;
    ruvia::request_memory memory(worker);
    auto request = make_request(memory, "GET", "/s");

    const auto resolution = table_value.resolve(http_known_method::get, "/s");
    const auto* resolved = resolution.resolved();
    RUVIA_CHECK(resolved != nullptr);

    stream_capture_sink sink;
    auto writer = sc_make_writer(sink);

    // Co_await inside a detached coroutine so the test can capture the transport
    // state and whether dispatch surfaced the committed-stream failure.
    bool threw = false;
    asio::io_context ctx(1);
    asio::co_spawn(
        ctx,
        [&]() -> asio::awaitable<void> {
            try {
                (void)co_await ruvia::as_awaitable(table_value.dispatch_response_stream(
                    request, *resolved, memory, writer, ruvia::test::test_context_services()));
            } catch (const std::exception&) {
                threw = true;
            }
        },
        asio::detached);
    ctx.run();
    // The handler committed (wrote a chunk) before failing, and the failure was
    // surfaced (not masked as a clean complete stream).
    RUVIA_CHECK(sink.committed_flag_);
    RUVIA_CHECK(!sink.ended_flag_);
    RUVIA_CHECK(threw);
}

RUVIA_TEST(stream_route_middleware_propagates_empty_handler_completion) {
    g_chain_order.clear();
    const auto middleware_value = ruvia::detail::make_middleware_descriptor<chain_mw_a>();
    const auto observation_value = dispatch_empty_stream_with(middleware_value);
    RUVIA_CHECK(!observation_value.threw_);
    RUVIA_CHECK(observation_value.handled_);
    RUVIA_CHECK(!observation_value.buffered_);
    RUVIA_CHECK(observation_value.ended_);
    RUVIA_CHECK(observation_value.committed_);
    RUVIA_CHECK(observation_value.context_released_);
    const std::vector<int> expected{1, -1};
    RUVIA_CHECK(g_chain_order == expected);
}

RUVIA_TEST(stream_route_uncommitted_handler_allows_middleware_override) {
    const auto middleware_value = ruvia::detail::make_middleware_descriptor<chain_mw_override_after_next>();
    const auto observation_value = dispatch_empty_stream_with(middleware_value);
    RUVIA_CHECK(!observation_value.threw_);
    RUVIA_CHECK(!observation_value.handled_);
    RUVIA_CHECK(observation_value.buffered_);
    RUVIA_CHECK(!observation_value.ended_);
    RUVIA_CHECK(!observation_value.committed_);
    RUVIA_CHECK(observation_value.context_released_);
    RUVIA_CHECK_EQ(observation_value.buffered_body_, std::string("override"));
}

RUVIA_TEST(websocket_middleware_short_circuits_before_upgrade_terminal) {
    g_chain_order.clear();
    const auto middleware_value = ruvia::detail::make_middleware_descriptor<chain_mw_stop>();
    const auto observation_value = dispatch_websocket_with(middleware_value);
    RUVIA_CHECK(!observation_value.terminal_invoked_);
    RUVIA_CHECK(observation_value.buffered_);
    RUVIA_CHECK_EQ(observation_value.buffered_body_, std::string("stopped"));
    const std::vector<int> expected{9};
    RUVIA_CHECK(g_chain_order == expected);
}

RUVIA_TEST(websocket_middleware_pre_upgrade_failure_stays_http_buffered) {
    const auto middleware_value = ruvia::detail::make_middleware_descriptor<chain_mw_throws>();
    const auto observation_value = dispatch_websocket_with(middleware_value);
    RUVIA_CHECK(!observation_value.terminal_invoked_);
    RUVIA_CHECK(observation_value.buffered_);
    RUVIA_CHECK(
        (observation_value.buffered_body_.find("\"code\":\"mw_rejected\"") != std::string_view::npos));
}

RUVIA_TEST(websocket_middleware_wraps_upgrade_and_session_terminal) {
    g_chain_order.clear();
    const auto middleware_value = ruvia::detail::make_middleware_descriptor<chain_mw_a>();
    const auto observation_value = dispatch_websocket_with(middleware_value);
    RUVIA_CHECK(observation_value.terminal_invoked_);
    RUVIA_CHECK(observation_value.capability_available_in_terminal_);
    RUVIA_CHECK(!observation_value.buffered_);
    const std::vector<int> expected{1, 0, -1};
    RUVIA_CHECK(g_chain_order == expected);
}

RUVIA_TEST(websocket_capability_expires_before_middleware_post_processing) {
    g_chain_order.clear();
    g_websocket_unavailable_after_next = false;
    const auto middleware_value =
        ruvia::detail::make_middleware_descriptor<chain_mw_probe_websocket_after_next>();
    const auto observation_value = dispatch_websocket_with(middleware_value);
    RUVIA_CHECK(observation_value.terminal_invoked_);
    RUVIA_CHECK(observation_value.capability_available_in_terminal_);
    RUVIA_CHECK(g_websocket_unavailable_after_next);
    const std::vector<int> expected{0};
    RUVIA_CHECK(g_chain_order == expected);
}

RUVIA_TEST(websocket_middleware_post_failure_escapes_for_session_close) {
    const auto middleware_value = ruvia::detail::make_middleware_descriptor<chain_mw_throws_after_next>();
    bool threw = false;
    try {
        (void)dispatch_websocket_with(middleware_value);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    RUVIA_CHECK(threw);
}

RUVIA_TEST(middleware_chain_maps_middleware_exception_to_error_response) {
    g_chain_order.clear();
    const controller_middleware_descriptor mws[] = {
        ruvia::detail::make_middleware_descriptor<chain_mw_throws>(),
    };
    const auto body = dispatch_chain(std::span<const controller_middleware_descriptor>(mws, 1));
    // A middleware that throws before next() short-circuits the chain: the handler
    // (which records 0) never runs, and the http_error is mapped to an error
    // response through the same handle_exception path as a handler exception -- its
    // "code" survives, so it is not swallowed into a generic 500.
    RUVIA_CHECK(g_chain_order.empty());
    RUVIA_CHECK((body.find("\"code\":\"mw_rejected\"") != std::string_view::npos));
}

RUVIA_TEST(middleware_chain_controller_middleware_wraps_route_middleware) {
    g_chain_order.clear();
    // controller-level middleware and route-level middleware are combined into one
    // chain; controller middleware must be OUTERMOST (it wraps the route's), so a
    // controller-level auth/logging guard always brackets route-specific logic.
    const controller_middleware_descriptor controller_mws[] = {
        ruvia::detail::make_middleware_descriptor<chain_mw_a>(),
    };
    const controller_middleware_descriptor route_mws[] = {
        ruvia::detail::make_middleware_descriptor<chain_mw_b>(),
    };
    const auto body =
        dispatch_chain(std::span<const controller_middleware_descriptor>(controller_mws, 1),
            std::span<const controller_middleware_descriptor>(route_mws, 1));
    RUVIA_CHECK_EQ(body, std::string("ok"));
    // A(controller) pre, B(route) pre, handler, B post, A post. A regression that
    // swapped the two spans would run route middleware outside controller middleware.
    const std::vector<int> expected{1, 2, 0, -2, -1};
    RUVIA_CHECK(g_chain_order == expected);
}
