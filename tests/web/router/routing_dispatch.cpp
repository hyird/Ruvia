#include "routing_fixture.h"

// Routing: dispatching into a route and turning failures into responses.

RUVIA_TEST(websocket_route_owns_validated_lifecycle_policy) {
    ruvia::Router invalidRouter;
    auto& invalid = ruvia::detail::RouterImpl::from(invalidRouter);
    ruvia::WebSocketRouteOptions invalidOptions;
    invalidOptions.lifecycle.closeHandshakeTimeout = std::chrono::milliseconds(0);
    bool rejected = false;
    try {
        invalid.registerWebSocketRoute(HttpKnownMethod::kGet, path("/invalid-ws"), ruvia::detail::RouteStreamHandler(nullptr, &dummyStreamHandler), std::span<const ControllerMiddlewareDescriptor>{}, std::span<const ControllerMiddlewareDescriptor>{}, invalidOptions);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    RUVIA_CHECK(rejected);

    const auto rejectsSubprotocols = [](std::string_view subprotocols) {
        ruvia::Router router;
        auto& impl = ruvia::detail::RouterImpl::from(router);
        ruvia::WebSocketRouteOptions options;
        options.subprotocols = subprotocols;
        try {
            impl.registerWebSocketRoute(HttpKnownMethod::kGet, path("/invalid-ws-protocols"), ruvia::detail::RouteStreamHandler(nullptr, &dummyStreamHandler), std::span<const ControllerMiddlewareDescriptor>{}, std::span<const ControllerMiddlewareDescriptor>{}, options);
        } catch (const std::invalid_argument& error) {
            return std::string_view(error.what()) == "websocket subprotocols must be a list of at most 64 unique HTTP tokens";
        }
        return false;
    };
    RUVIA_CHECK(rejectsSubprotocols("chat, bad token"));
    RUVIA_CHECK(rejectsSubprotocols("chat, chat"));
    RUVIA_CHECK(rejectsSubprotocols(", ,"));
    std::string tooManySubprotocols;
    for (std::size_t i = 0; i <= ruvia::kMaxHttpHeaderFields; ++i) {
        if (i != 0) {
            tooManySubprotocols.append(", ");
        }
        tooManySubprotocols.append("protocol-");
        tooManySubprotocols.append(std::to_string(i));
    }
    RUVIA_CHECK(rejectsSubprotocols(tooManySubprotocols));

    ruvia::Router router;
    auto& impl = ruvia::detail::RouterImpl::from(router);
    ruvia::WebSocketRouteOptions options;
    options.lifecycle.closeHandshakeTimeout = std::chrono::milliseconds(1234);
    impl.registerWebSocketRoute(HttpKnownMethod::kGet, path("/ws"), ruvia::detail::RouteStreamHandler(nullptr, &dummyStreamHandler), std::span<const ControllerMiddlewareDescriptor>{}, std::span<const ControllerMiddlewareDescriptor>{}, options);
    impl.finalize();
    const auto resolution = impl.routeTable().resolve(HttpKnownMethod::kGet, "/ws");
    const auto* resolved = resolution.resolved();
    RUVIA_CHECK(resolved != nullptr);
    const auto* endpoint = resolved->route().endpoint().webSocket();
    RUVIA_CHECK(endpoint != nullptr);
    RUVIA_CHECK_EQ(endpoint->lifecycle().closeHandshakeTimeout->count(), std::int64_t{1234});
}

RUVIA_TEST(head_only_stream_completion_is_success_not_error) {
    g_chainOrder.clear();
    g_headOnlyHandlerResumedPastFirstWrite = false;
    const auto observation = dispatchHeadOnlyStream({});
    // The signal ends the dispatch as a handled head-only stream: no error
    // response, no rethrow to the driver, and the stream is finished.
    RUVIA_CHECK(observation.handled);
    RUVIA_CHECK(!observation.buffered);
    RUVIA_CHECK(!observation.threw);
    RUVIA_CHECK(observation.ended);
    // The handler stopped deterministically at its first body write.
    RUVIA_CHECK(!g_headOnlyHandlerResumedPastFirstWrite);
}

RUVIA_TEST(streaming_get_routes_gain_head_shadow) {
    ruvia::Router router;
    auto& impl = ruvia::detail::RouterImpl::from(router);
    impl.registerResponseStreamRoute(HttpKnownMethod::kGet, path("/events"), ruvia::detail::RouteStreamHandler(nullptr, &dummyStreamHandler), {}, {});
    impl.registerSseRoute(HttpKnownMethod::kGet, path("/sse"), ruvia::detail::RouteStreamHandler(nullptr, &dummyStreamHandler), {}, {});
    impl.registerWebSocketRoute(HttpKnownMethod::kGet, path("/ws"), ruvia::detail::RouteStreamHandler(nullptr, &dummyStreamHandler), {}, {});
    impl.finalize();
    const auto& table = impl.routeTable();

    // Streaming and SSE GET routes answer HEAD via an auto shadow, like
    // buffered GET routes do.
    const auto events = table.resolve(HttpKnownMethod::kHead, "/events");
    RUVIA_CHECK(events.resolved() != nullptr);
    if (const auto* resolved = events.resolved()) {
        RUVIA_CHECK(resolved->route().endpoint().responseStream() != nullptr);
    }
    const auto sse = table.resolve(HttpKnownMethod::kHead, "/sse");
    RUVIA_CHECK(sse.resolved() != nullptr);
    // A WebSocket handshake is GET-only; no HEAD shadow may reach it.
    const auto ws = table.resolve(HttpKnownMethod::kHead, "/ws");
    RUVIA_CHECK(ws.resolved() == nullptr);
}

RUVIA_TEST(dispatch_maps_handler_exceptions_to_error_responses) {
    // Application and protocol errors retain their status; unknown failures map
    // to 500. Router never injects HTTP/1 Connection policy into any response.
    const auto application = dispatchOne(RouteHandler(nullptr, &throwsHttpErrorHandler), HttpKnownMethod::kGet, "/x");
    RUVIA_CHECK_EQ(application.status, std::uint16_t{403});
    RUVIA_CHECK(application.connection.empty());
    const auto protocol = dispatchOne(RouteHandler(nullptr, &throwsProtocolErrorHandler), HttpKnownMethod::kGet, "/x");
    RUVIA_CHECK_EQ(protocol.status, std::uint16_t{413});
    RUVIA_CHECK(protocol.connection.empty());
    const auto generic = dispatchOne(RouteHandler(nullptr, &throwsGenericHandler), HttpKnownMethod::kGet, "/x");
    RUVIA_CHECK_EQ(generic.status, std::uint16_t{500});
    RUVIA_CHECK(generic.connection.empty());
    // The unexpected exception's message must NOT leak into the response body: a
    // library error (SQL text, paths) could otherwise be disclosed to the client.
    RUVIA_CHECK(generic.body.find("boom") == std::string_view::npos);
    RUVIA_CHECK(generic.body.find("Internal Server Error") != std::string_view::npos);
}

RUVIA_TEST(dispatch_rejects_unsupported_request_content_coding_with_advertisement) {
    const auto result = dispatchOneToken(RouteHandler(nullptr, &readsRequestBodyHandler), "GET", "/x", "deflate", "encoded");
    RUVIA_CHECK_EQ(result.status, std::uint16_t{415});
    RUVIA_CHECK_EQ(result.acceptEncoding, std::string("gzip, br, zstd"));
    RUVIA_CHECK(result.body.find("unsupported_content_coding") != std::string_view::npos);
}

RUVIA_TEST(dispatch_defensively_rejects_malformed_request_content_coding) {
    const auto result = dispatchOneToken(RouteHandler(nullptr, &readsRequestBodyHandler), "GET", "/x", "gzip;level=9", "encoded");
    RUVIA_CHECK_EQ(result.status, std::uint16_t{400});
    RUVIA_CHECK(result.acceptEncoding.empty());
}

RUVIA_TEST(dispatch_produces_404_and_405_for_unmatched_routes) {
    // A path with no route -> 404.
    RUVIA_CHECK_EQ(dispatchOne(RouteHandler(nullptr, &okHandler), HttpKnownMethod::kGet, "/nope").status, std::uint16_t{404});
    // The path exists but the method does not -> 405 with an Allow header listing GET.
    const auto notAllowed = dispatchOne(RouteHandler(nullptr, &okHandler), HttpKnownMethod::kPost, "/x");
    RUVIA_CHECK_EQ(notAllowed.status, std::uint16_t{405});
    RUVIA_CHECK(notAllowed.allow.find("GET") != std::string_view::npos);
    // The registered method still works.
    RUVIA_CHECK_EQ(dispatchOne(RouteHandler(nullptr, &okHandler), HttpKnownMethod::kGet, "/x").status, std::uint16_t{200});
}

RUVIA_TEST(dispatch_produces_501_for_valid_unimplemented_method_token) {
    const auto extension = dispatchOneToken(RouteHandler(nullptr, &okHandler), "PROPFIND", "/x");
    RUVIA_CHECK_EQ(extension.status, std::uint16_t{501});
    RUVIA_CHECK(extension.allow.empty());
    RUVIA_CHECK(extension.connection.empty());

    // Method tokens are case-sensitive. A lowercase standard spelling is still
    // syntactically valid but is not the framework's GET semantic class.
    const auto lowercase = dispatchOneToken(RouteHandler(nullptr, &okHandler), "get", "/x");
    RUVIA_CHECK_EQ(lowercase.status, std::uint16_t{501});
}

RUVIA_TEST(dispatch_uses_custom_not_found_handler) {
    // A registered not-found handler replaces the default 404 response body.
    const auto result = dispatchWithHandlers(RouteHandler(nullptr, &okHandler), nullptr, &customNotFound, HttpKnownMethod::kGet, "/nope");
    RUVIA_CHECK_EQ(result.status, std::uint16_t{404});
    RUVIA_CHECK_EQ(result.body, std::string("custom-not-found"));
}

RUVIA_TEST(dispatch_uses_custom_error_handler_with_thrown_status) {
    // A registered error handler renders a thrown HttpError, preserving its status.
    const auto result = dispatchWithHandlers(RouteHandler(nullptr, &throwsHttpErrorHandler), &customError, nullptr, HttpKnownMethod::kGet, "/x");
    RUVIA_CHECK_EQ(result.status, std::uint16_t{403});
    RUVIA_CHECK_EQ(result.body, std::string("custom-error"));
}

RUVIA_TEST(dispatch_preserves_content_coding_advertisement_with_custom_error_handler) {
    const auto result = dispatchWithHandlersToken(RouteHandler(nullptr, &readsRequestBodyHandler), &customError, nullptr, "GET", "/x", "compress", "encoded");
    RUVIA_CHECK_EQ(result.status, std::uint16_t{415});
    RUVIA_CHECK_EQ(result.body, std::string("custom-error"));
    RUVIA_CHECK_EQ(result.acceptEncoding, std::string("gzip, br, zstd"));
}

RUVIA_TEST(dispatch_routes_unimplemented_method_through_custom_error_handler) {
    const auto result = dispatchWithHandlersToken(RouteHandler(nullptr, &okHandler), &customError, nullptr, "PROPFIND", "/x");
    RUVIA_CHECK_EQ(result.status, std::uint16_t{501});
    RUVIA_CHECK_EQ(result.body, std::string("custom-error"));
}

RUVIA_TEST(request_json_form_map_media_type_mismatch_to_415) {
    // Sanity: the right media type with a parsable body reaches the handler.
    const auto ok = dispatchBodyRequest(RouteHandler(nullptr, &jsonModelEchoHandler), "application/json", R"({"value":"hi"})");
    RUVIA_CHECK_EQ(ok.status, std::uint16_t{200});
    RUVIA_CHECK_EQ(ok.body, std::string("hi"));

    // The wrong media type is the client's format mistake: 415, not 400.
    const auto wrongType = dispatchBodyRequest(RouteHandler(nullptr, &jsonModelEchoHandler), "text/plain", R"({"value":"hi"})");
    RUVIA_CHECK_EQ(wrongType.status, std::uint16_t{415});
    const auto missingType = dispatchBodyRequest(RouteHandler(nullptr, &jsonModelEchoHandler), "", R"({"value":"hi"})");
    RUVIA_CHECK_EQ(missingType.status, std::uint16_t{415});
    const auto formWrongType = dispatchBodyRequest(RouteHandler(nullptr, &formModelEchoHandler), "application/json", "value=hi");
    RUVIA_CHECK_EQ(formWrongType.status, std::uint16_t{415});

    // A malformed body of the RIGHT type stays 400.
    const auto badBody = dispatchBodyRequest(RouteHandler(nullptr, &jsonModelEchoHandler), "application/json", "{not-json");
    RUVIA_CHECK_EQ(badBody.status, std::uint16_t{400});

    const auto formOk = dispatchBodyRequest(RouteHandler(nullptr, &formModelEchoHandler), "application/x-www-form-urlencoded", "value=hi");
    RUVIA_CHECK_EQ(formOk.status, std::uint16_t{200});
    RUVIA_CHECK_EQ(formOk.body, std::string("hi"));
}

RUVIA_TEST(request_json_if_and_form_if_fall_back_instead_of_failing) {
    // Format problems yield nullopt so the handler can fall back...
    const auto wrongType = dispatchBodyRequest(RouteHandler(nullptr, &jsonIfEchoHandler), "text/plain", "x");
    RUVIA_CHECK_EQ(wrongType.status, std::uint16_t{200});
    RUVIA_CHECK_EQ(wrongType.body, std::string("no-json"));
    const auto badBody = dispatchBodyRequest(RouteHandler(nullptr, &jsonIfEchoHandler), "application/json", "{not-json");
    RUVIA_CHECK_EQ(badBody.status, std::uint16_t{200});
    RUVIA_CHECK_EQ(badBody.body, std::string("no-json"));
    const auto formWrongType = dispatchBodyRequest(RouteHandler(nullptr, &formIfEchoHandler), "text/plain", "value=hi");
    RUVIA_CHECK_EQ(formWrongType.status, std::uint16_t{200});
    RUVIA_CHECK_EQ(formWrongType.body, std::string("no-form"));
    const auto jsonValueWrongType = dispatchBodyRequest(RouteHandler(nullptr, &jsonValueIfEchoHandler), "text/plain", "{}");
    RUVIA_CHECK_EQ(jsonValueWrongType.status, std::uint16_t{200});
    RUVIA_CHECK_EQ(jsonValueWrongType.body, std::string("no-json"));

    // ...and a well-formed body of the right type still parses.
    const auto ok = dispatchBodyRequest(RouteHandler(nullptr, &jsonIfEchoHandler), "application/json", R"({"value":"hi"})");
    RUVIA_CHECK_EQ(ok.status, std::uint16_t{200});
    RUVIA_CHECK_EQ(ok.body, std::string("hi"));
    const auto formOk = dispatchBodyRequest(RouteHandler(nullptr, &formIfEchoHandler), "application/x-www-form-urlencoded", "value=hi");
    RUVIA_CHECK_EQ(formOk.status, std::uint16_t{200});
    RUVIA_CHECK_EQ(formOk.body, std::string("hi"));
}

RUVIA_TEST(prefix_not_found_handler_scopes_by_longest_segment_prefix) {
    ruvia::Router router;
    auto& impl = ruvia::detail::RouterImpl::from(router);
    impl.registerRoute(HttpKnownMethod::kGet, path("/api/real"), RouteHandler(nullptr, &okHandler), RequestBodyMode::kBuffered, std::span<const ControllerMiddlewareDescriptor>{}, std::span<const ControllerMiddlewareDescriptor>{});
    impl.setNotFoundHandler(&customNotFound);
    const ruvia::detail::HttpPrefixNotFoundHandler scoped[] = {
        {"/api", &apiScopedNotFound},
        // Trailing slash normalizes away; this is the same scope as "/api/v2".
        {"/api/v2/", &v2ScopedNotFound},
    };
    impl.setPrefixNotFoundHandlers(std::span<const ruvia::detail::HttpPrefixNotFoundHandler>(scoped, 2));
    impl.finalize();
    const auto& table = impl.routeTable();

    // Inside the scope: the prefix handler renders the miss.
    RUVIA_CHECK_EQ(dispatchOn(table, "GET", "/api/missing").body, std::string("api-scope-404"));
    // The mount path itself belongs to the scope.
    RUVIA_CHECK_EQ(dispatchOn(table, "GET", "/api").body, std::string("api-scope-404"));
    // The longest matching prefix wins over an enclosing one.
    RUVIA_CHECK_EQ(dispatchOn(table, "GET", "/api/v2/missing").body, std::string("v2-scope-404"));
    // Segment boundary: "/apix" is not under "/api".
    RUVIA_CHECK_EQ(dispatchOn(table, "GET", "/apix").body, std::string("custom-not-found"));
    // Outside every scope the app-wide handler still runs.
    RUVIA_CHECK_EQ(dispatchOn(table, "GET", "/other").body, std::string("custom-not-found"));
}

RUVIA_TEST(prefix_error_handler_scopes_thrown_route_failures) {
    ruvia::Router router;
    auto& impl = ruvia::detail::RouterImpl::from(router);
    impl.registerRoute(HttpKnownMethod::kGet, path("/api/boom"), RouteHandler(nullptr, &throwsHttpErrorHandler), RequestBodyMode::kBuffered, std::span<const ControllerMiddlewareDescriptor>{}, std::span<const ControllerMiddlewareDescriptor>{});
    impl.registerRoute(HttpKnownMethod::kGet, path("/boom"), RouteHandler(nullptr, &throwsHttpErrorHandler), RequestBodyMode::kBuffered, std::span<const ControllerMiddlewareDescriptor>{}, std::span<const ControllerMiddlewareDescriptor>{});
    impl.setErrorHandler(&customError);
    const ruvia::detail::HttpPrefixErrorHandler scoped[] = {
        {"/api", &apiScopedError},
    };
    impl.setPrefixErrorHandlers(std::span<const ruvia::detail::HttpPrefixErrorHandler>(scoped, 1));
    impl.finalize();
    const auto& table = impl.routeTable();

    const auto scopedResult = dispatchOn(table, "GET", "/api/boom");
    RUVIA_CHECK_EQ(scopedResult.status, std::uint16_t{403});
    RUVIA_CHECK_EQ(scopedResult.body, std::string("api-scope-error"));

    const auto globalResult = dispatchOn(table, "GET", "/boom");
    RUVIA_CHECK_EQ(globalResult.status, std::uint16_t{403});
    RUVIA_CHECK_EQ(globalResult.body, std::string("custom-error"));
}

RUVIA_TEST(dispatch_options_asterisk_returns_server_wide_allow) {
    // A server-wide OPTIONS * request is answered with 204 and an Allow header
    // listing every method registered anywhere on the server, not per-route.
    ruvia::Router router;
    auto& impl = ruvia::detail::RouterImpl::from(router);
    impl.registerRoute(HttpKnownMethod::kGet, path("/a"), RouteHandler(nullptr, &okHandler), RequestBodyMode::kBuffered, std::span<const ControllerMiddlewareDescriptor>{}, std::span<const ControllerMiddlewareDescriptor>{});
    impl.registerRoute(HttpKnownMethod::kPost, path("/b"), RouteHandler(nullptr, &okHandler), RequestBodyMode::kBuffered, std::span<const ControllerMiddlewareDescriptor>{}, std::span<const ControllerMiddlewareDescriptor>{});
    impl.finalize();
    const auto& table = impl.routeTable();

    ruvia::WorkerMemory worker;
    ruvia::RequestMemory memory(worker);
    ruvia::HttpRequest request = ruvia::detail::HttpRequestAccess::make();
    ruvia::detail::HttpRequestAccess::reset(request);
    ruvia::detail::HttpRequestAccess::setMethod(request, "OPTIONS");
    ruvia::detail::HttpRequestAccess::setPath(request, "*");
    ruvia::detail::HttpRequestAccess::setResource(request, memory.resource());

    asio::io_context ctx(1);
    auto future = asio::co_spawn(ctx, ruvia::detail::taskAsAwaitable(table.dispatch(request, memory, {})), asio::use_future);
    ctx.run();
    const auto response = future.get();

    RUVIA_CHECK_EQ(response.status(), ruvia::http_status::kNoContent);
    const auto allow = response.header("Allow").value_or(std::string_view{});
    RUVIA_CHECK(allow.find("GET") != std::string_view::npos);
    RUVIA_CHECK(allow.find("POST") != std::string_view::npos);
}
