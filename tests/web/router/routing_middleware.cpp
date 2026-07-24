#include "routing_fixture.h"

// Routing: the middleware chain around a route.

RUVIA_TEST(validated_model_binding_spans_next_and_unwinds_before_upstream_resumes) {
    for (const bool handlerThrows : {false, true}) {
        scopedValidationHandlerRead = false;
        scopedValidationRawRead = false;
        scopedValidationHandlerThrows = handlerThrows;
        ValidationScopeProbe::releasedAfterNext = false;

        ruvia::Router router;
        auto& impl = ruvia::detail::RouterImpl::from(router);
        const std::array middlewares{ruvia::detail::makeMiddlewareDescriptor<ValidationScopeProbe>(), ruvia::detail::makeMiddlewareDescriptor<ScopedValidationValidator>()};
        impl.registerRoute(HttpKnownMethod::kPost, path("/validated-scope"), RouteHandler(nullptr, &scopedValidationHandler), RequestBodyMode::kBuffered, std::span<const ControllerMiddlewareDescriptor>{}, std::span(middlewares));
        impl.finalize();

        ruvia::WorkerMemory worker;
        ruvia::RequestMemory memory(worker);
        auto request = ruvia::detail::HttpRequestAccess::make();
        ruvia::detail::HttpRequestAccess::reset(request);
        ruvia::detail::HttpRequestAccess::setMethod(request, "POST");
        ruvia::detail::HttpRequestAccess::setPath(request, "/validated-scope");
        ruvia::detail::HttpRequestAccess::setResource(request, memory.resource());
        const auto contentTypeSlot = ruvia::detail::HttpRequestAccess::knownHeaderSlot(ruvia::detail::RequestKnownHeader::kContentType);
        (void)ruvia::detail::HttpRequestAccess::addHeader(request, ruvia::HttpHeaderView{"Content-Type", "application/json"}, contentTypeSlot);
        ruvia::detail::HttpRequestAccess::setBody(request, R"({"value":"ok"})");

        asio::io_context ioContext(1);
        auto future = asio::co_spawn(ioContext, ruvia::detail::taskAsAwaitable(impl.routeTable().dispatch(request, memory, {})), asio::use_future);
        ioContext.run();
        const auto response = future.get();
        RUVIA_CHECK_EQ(response.status(), handlerThrows ? ruvia::http_status::kInternalServerError : ruvia::http_status::kOk);
        RUVIA_CHECK(scopedValidationHandlerRead);
        RUVIA_CHECK(scopedValidationRawRead);
        RUVIA_CHECK(ValidationScopeProbe::releasedAfterNext);
    }
}

RUVIA_TEST(head_only_stream_completion_unwinds_middleware_as_success) {
    g_chainOrder.clear();
    g_headOnlyHandlerResumedPastFirstWrite = false;
    const ControllerMiddlewareDescriptor mws[] = {
        ruvia::detail::makeMiddlewareDescriptor<ChainMwA>(),
    };
    const auto observation = dispatchHeadOnlyStream(std::span<const ControllerMiddlewareDescriptor>(mws, 1));
    RUVIA_CHECK(observation.handled);
    RUVIA_CHECK(!observation.threw);
    RUVIA_CHECK(observation.ended);
    // The middleware ran its pre-next() side, the handler started, and the
    // signal unwound the chain without converting into an error response
    // (which could no longer be sent past the committed head).
    const std::vector<int> expected{1, 0};
    RUVIA_CHECK(g_chainOrder == expected);
}

RUVIA_TEST(middleware_chain_runs_in_onion_order) {
    g_chainOrder.clear();
    const ControllerMiddlewareDescriptor mws[] = {
        ruvia::detail::makeMiddlewareDescriptor<ChainMwA>(),
        ruvia::detail::makeMiddlewareDescriptor<ChainMwB>(),
    };
    const auto body = dispatchChain(std::span<const ControllerMiddlewareDescriptor>(mws, 2));
    RUVIA_CHECK_EQ(body, std::string("ok"));
    // Onion order: A pre, B pre, handler, B post, A post.
    const std::vector<int> expected{1, 2, 0, -2, -1};
    RUVIA_CHECK(g_chainOrder == expected);
}

RUVIA_TEST(middleware_chain_short_circuits_without_next) {
    g_chainOrder.clear();
    // A middleware that does not call next() stops the chain: the next middleware
    // and the handler never run, and the middleware's response is used.
    const ControllerMiddlewareDescriptor mws[] = {
        ruvia::detail::makeMiddlewareDescriptor<ChainMwStop>(),
        ruvia::detail::makeMiddlewareDescriptor<ChainMwA>(),
    };
    const auto body = dispatchChain(std::span<const ControllerMiddlewareDescriptor>(mws, 2));
    RUVIA_CHECK_EQ(body, std::string("stopped"));
    const std::vector<int> expected{9};
    RUVIA_CHECK(g_chainOrder == expected);
}

RUVIA_TEST(middleware_chain_rejects_calling_next_twice) {
    g_chainOrder.clear();
    const ControllerMiddlewareDescriptor mws[] = {
        ruvia::detail::makeMiddlewareDescriptor<ChainMwDoubleNext>(),
    };
    const auto body = dispatchChain(std::span<const ControllerMiddlewareDescriptor>(mws, 1));
    // The handler (which records 0) must run EXACTLY once: the first next() runs
    // it, the second is detected and converted to an error instead of re-entering
    // the chain. A regression to the double-invoke guard would run it twice.
    std::size_t handlerRuns = 0;
    for (const int step : g_chainOrder) {
        if (step == 0) {
            ++handlerRuns;
        }
    }
    RUVIA_CHECK_EQ(handlerRuns, std::size_t{1});
    // The response is the "next called multiple times" error, not the handler body.
    RUVIA_CHECK(body != std::string("ok"));
}

RUVIA_TEST(global_middleware_prepends_to_every_route_chain) {
    // App-wide middleware (App::use) materializes once and runs before the
    // route's own middleware on EVERY matched route.
    ruvia::Router router;
    auto& impl = ruvia::detail::RouterImpl::from(router);
    const ControllerMiddlewareDescriptor routeMws[] = {
        ruvia::detail::makeMiddlewareDescriptor<ChainMwB>(),
    };
    impl.registerRoute(HttpKnownMethod::kGet, path("/with-route-mw"), RouteHandler(nullptr, &chainHandler), RequestBodyMode::kBuffered, {}, std::span<const ControllerMiddlewareDescriptor>(routeMws, 1));
    impl.registerRoute(HttpKnownMethod::kGet, path("/bare"), RouteHandler(nullptr, &chainHandler), RequestBodyMode::kBuffered, {}, {});
    const ControllerMiddlewareDescriptor globals[] = {
        ruvia::detail::makeMiddlewareDescriptor<ChainMwA>(),
    };
    impl.setGlobalMiddlewares(std::span<const ControllerMiddlewareDescriptor>(globals, 1));
    impl.finalize();
    const auto& table = impl.routeTable();

    const auto dispatchPath = [&table](std::string_view requestPath) {
        ruvia::WorkerMemory worker;
        ruvia::RequestMemory memory(worker);
        ruvia::HttpRequest request = ruvia::detail::HttpRequestAccess::make();
        ruvia::detail::HttpRequestAccess::reset(request);
        ruvia::detail::HttpRequestAccess::setMethod(request, "GET");
        ruvia::detail::HttpRequestAccess::setPath(request, requestPath);
        ruvia::detail::HttpRequestAccess::setResource(request, memory.resource());

        asio::io_context ctx(1);
        auto future = asio::co_spawn(ctx, ruvia::detail::taskAsAwaitable(table.dispatch(request, memory, {})), asio::use_future);
        ctx.run();
        auto response = future.get();
        const auto body = ruvia::detail::responseBody(response).bytes();
        return std::string(body.data(), body.size());
    };

    g_chainOrder.clear();
    RUVIA_CHECK_EQ(dispatchPath("/with-route-mw"), std::string("ok"));
    // Global A wraps route-level B: A pre, B pre, handler, B post, A post.
    const std::vector<int> withRouteMw{1, 2, 0, -2, -1};
    RUVIA_CHECK(g_chainOrder == withRouteMw);

    g_chainOrder.clear();
    RUVIA_CHECK_EQ(dispatchPath("/bare"), std::string("ok"));
    // A route with no middleware of its own still runs the global.
    const std::vector<int> bare{1, 0, -1};
    RUVIA_CHECK(g_chainOrder == bare);
}

RUVIA_TEST(global_middleware_registration_rejected_after_finalize) {
    ruvia::Router router;
    auto& impl = ruvia::detail::RouterImpl::from(router);
    impl.registerRoute(HttpKnownMethod::kGet, path("/sealed"), RouteHandler(nullptr, &chainHandler), RequestBodyMode::kBuffered, {}, {});
    const ControllerMiddlewareDescriptor globals[] = {
        ruvia::detail::makeMiddlewareDescriptor<ChainMwA>(),
    };
    impl.setGlobalMiddlewares(std::span<const ControllerMiddlewareDescriptor>(globals, 1));
    impl.finalize();

    // Re-applying the identical set is the app stop()/run() restart path.
    impl.setGlobalMiddlewares(std::span<const ControllerMiddlewareDescriptor>(globals, 1));

    // Changing the set after finalize must fail loudly.
    const ControllerMiddlewareDescriptor changed[] = {
        ruvia::detail::makeMiddlewareDescriptor<ChainMwB>(),
    };
    bool threw = false;
    try {
        impl.setGlobalMiddlewares(std::span<const ControllerMiddlewareDescriptor>(changed, 1));
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
    // complete one. dispatchResponseStream must therefore rethrow so the transport
    // aborts (connection close / RST_STREAM).
    ruvia::Router router;
    auto& impl = ruvia::detail::RouterImpl::from(router);
    const ControllerMiddlewareDescriptor mws[] = {
        ruvia::detail::makeMiddlewareDescriptor<ChainMwA>(),
    };
    impl.registerResponseStreamRoute(HttpKnownMethod::kGet, path("/s"), ruvia::detail::RouteStreamHandler(nullptr, &streamCommitThenThrow), std::span<const ControllerMiddlewareDescriptor>{}, std::span<const ControllerMiddlewareDescriptor>(mws, 1));
    impl.finalize();
    const auto& table = impl.routeTable();

    ruvia::WorkerMemory worker;
    ruvia::RequestMemory memory(worker);
    ruvia::HttpRequest request = ruvia::detail::HttpRequestAccess::make();
    ruvia::detail::HttpRequestAccess::reset(request);
    ruvia::detail::HttpRequestAccess::setMethod(request, "GET");
    ruvia::detail::HttpRequestAccess::setPath(request, "/s");
    ruvia::detail::HttpRequestAccess::setResource(request, memory.resource());

    const auto resolution = table.resolve(HttpKnownMethod::kGet, "/s");
    const auto* resolved = resolution.resolved();
    RUVIA_CHECK(resolved != nullptr);

    StreamCaptureSink sink;
    auto writer = scMakeWriter(sink);

    // Co_await inside a detached coroutine so the test can capture the transport
    // state and whether dispatch surfaced the committed-stream failure.
    bool threw = false;
    asio::io_context ctx(1);
    asio::co_spawn(
        ctx,
        [&]() -> asio::awaitable<void> {
            try {
                (void)co_await ruvia::detail::taskAsAwaitable(table.dispatchResponseStream(request, *resolved, memory, writer, {}));
            } catch (const std::exception&) {
                threw = true;
            }
        },
        asio::detached);
    ctx.run();
    // The handler committed (wrote a chunk) before failing, and the failure was
    // surfaced (not masked as a clean complete stream).
    RUVIA_CHECK(sink.committedFlag);
    RUVIA_CHECK(!sink.endedFlag);
    RUVIA_CHECK(threw);
}

RUVIA_TEST(stream_route_middleware_propagates_empty_handler_completion) {
    g_chainOrder.clear();
    const auto middleware = ruvia::detail::makeMiddlewareDescriptor<ChainMwA>();
    const auto observation = dispatchEmptyStreamWith(middleware);
    RUVIA_CHECK(!observation.threw);
    RUVIA_CHECK(observation.handled);
    RUVIA_CHECK(!observation.buffered);
    RUVIA_CHECK(observation.ended);
    RUVIA_CHECK(observation.committed);
    RUVIA_CHECK(observation.contextReleased);
    const std::vector<int> expected{1, -1};
    RUVIA_CHECK(g_chainOrder == expected);
}

RUVIA_TEST(stream_route_uncommitted_handler_allows_middleware_override) {
    const auto middleware = ruvia::detail::makeMiddlewareDescriptor<ChainMwOverrideAfterNext>();
    const auto observation = dispatchEmptyStreamWith(middleware);
    RUVIA_CHECK(!observation.threw);
    RUVIA_CHECK(!observation.handled);
    RUVIA_CHECK(observation.buffered);
    RUVIA_CHECK(!observation.ended);
    RUVIA_CHECK(!observation.committed);
    RUVIA_CHECK(observation.contextReleased);
    RUVIA_CHECK_EQ(observation.bufferedBody, std::string("override"));
}

RUVIA_TEST(websocket_middleware_short_circuits_before_upgrade_terminal) {
    g_chainOrder.clear();
    const auto middleware = ruvia::detail::makeMiddlewareDescriptor<ChainMwStop>();
    const auto observation = dispatchWebSocketWith(middleware);
    RUVIA_CHECK(!observation.terminalInvoked);
    RUVIA_CHECK(observation.buffered);
    RUVIA_CHECK_EQ(observation.bufferedBody, std::string("stopped"));
    const std::vector<int> expected{9};
    RUVIA_CHECK(g_chainOrder == expected);
}

RUVIA_TEST(websocket_middleware_pre_upgrade_failure_stays_http_buffered) {
    const auto middleware = ruvia::detail::makeMiddlewareDescriptor<ChainMwThrows>();
    const auto observation = dispatchWebSocketWith(middleware);
    RUVIA_CHECK(!observation.terminalInvoked);
    RUVIA_CHECK(observation.buffered);
    RUVIA_CHECK(observation.bufferedBody.find("\"code\":\"mw_rejected\"") != std::string_view::npos);
}

RUVIA_TEST(websocket_middleware_wraps_upgrade_and_session_terminal) {
    g_chainOrder.clear();
    const auto middleware = ruvia::detail::makeMiddlewareDescriptor<ChainMwA>();
    const auto observation = dispatchWebSocketWith(middleware);
    RUVIA_CHECK(observation.terminalInvoked);
    RUVIA_CHECK(observation.capabilityAvailableInTerminal);
    RUVIA_CHECK(!observation.buffered);
    const std::vector<int> expected{1, 0, -1};
    RUVIA_CHECK(g_chainOrder == expected);
}

RUVIA_TEST(websocket_capability_expires_before_middleware_post_processing) {
    g_chainOrder.clear();
    g_webSocketUnavailableAfterNext = false;
    const auto middleware = ruvia::detail::makeMiddlewareDescriptor<ChainMwProbeWebSocketAfterNext>();
    const auto observation = dispatchWebSocketWith(middleware);
    RUVIA_CHECK(observation.terminalInvoked);
    RUVIA_CHECK(observation.capabilityAvailableInTerminal);
    RUVIA_CHECK(g_webSocketUnavailableAfterNext);
    const std::vector<int> expected{0};
    RUVIA_CHECK(g_chainOrder == expected);
}

RUVIA_TEST(websocket_middleware_post_failure_escapes_for_session_close) {
    const auto middleware = ruvia::detail::makeMiddlewareDescriptor<ChainMwThrowsAfterNext>();
    bool threw = false;
    try {
        (void)dispatchWebSocketWith(middleware);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    RUVIA_CHECK(threw);
}

RUVIA_TEST(middleware_chain_maps_middleware_exception_to_error_response) {
    g_chainOrder.clear();
    const ControllerMiddlewareDescriptor mws[] = {
        ruvia::detail::makeMiddlewareDescriptor<ChainMwThrows>(),
    };
    const auto body = dispatchChain(std::span<const ControllerMiddlewareDescriptor>(mws, 1));
    // A middleware that throws before next() short-circuits the chain: the handler
    // (which records 0) never runs, and the HttpError is mapped to an error
    // response through the same handleException path as a handler exception -- its
    // "code" survives, so it is not swallowed into a generic 500.
    RUVIA_CHECK(g_chainOrder.empty());
    RUVIA_CHECK(body.find("\"code\":\"mw_rejected\"") != std::string_view::npos);
}

RUVIA_TEST(middleware_chain_controller_middleware_wraps_route_middleware) {
    g_chainOrder.clear();
    // Controller-level middleware and route-level middleware are combined into one
    // chain; controller middleware must be OUTERMOST (it wraps the route's), so a
    // controller-level auth/logging guard always brackets route-specific logic.
    const ControllerMiddlewareDescriptor controllerMws[] = {
        ruvia::detail::makeMiddlewareDescriptor<ChainMwA>(),
    };
    const ControllerMiddlewareDescriptor routeMws[] = {
        ruvia::detail::makeMiddlewareDescriptor<ChainMwB>(),
    };
    const auto body = dispatchChain(std::span<const ControllerMiddlewareDescriptor>(controllerMws, 1), std::span<const ControllerMiddlewareDescriptor>(routeMws, 1));
    RUVIA_CHECK_EQ(body, std::string("ok"));
    // A(controller) pre, B(route) pre, handler, B post, A post. A regression that
    // swapped the two spans would run route middleware outside controller middleware.
    const std::vector<int> expected{1, 2, 0, -2, -1};
    RUVIA_CHECK(g_chainOrder == expected);
}
