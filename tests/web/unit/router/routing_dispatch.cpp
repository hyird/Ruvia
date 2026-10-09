#include <string_view>

#include "ruvia/core/asio_task.h"

#include "routing_fixture.h"

// Routing: dispatching into a route and turning failures into responses.

RUVIA_TEST(websocket_route_owns_validated_lifecycle_policy) {
    RUVIA_CHECK_EQ(ruvia::websocket_lifecycle_options{}.peer_transport_fin_timeout_,
        std::chrono::seconds(5));
    ruvia::detail::router invalid_router;
    auto& invalid = ruvia::detail::router_impl::from(invalid_router);
    ruvia::websocket_route_config invalid_options;
    invalid_options.lifecycle_.close_handshake_timeout_ = std::chrono::milliseconds(0);
    bool rejected = false;
    try {
        invalid.register_websocket_route(http_known_method::get, path("/invalid-ws"),
            ruvia::detail::route_stream_handler_type(nullptr, &dummy_stream_handler),
            std::span<const controller_middleware_descriptor>{},
            std::span<const controller_middleware_descriptor>{}, invalid_options);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    RUVIA_CHECK(rejected);
    invalid_options.lifecycle_.close_handshake_timeout_.reset();
    invalid_options.lifecycle_.peer_transport_fin_timeout_ = std::chrono::milliseconds(0);
    rejected = false;
    try {
        invalid.register_websocket_route(http_known_method::get, path("/invalid-ws-fin"),
            ruvia::detail::route_stream_handler_type(nullptr, &dummy_stream_handler),
            std::span<const controller_middleware_descriptor>{},
            std::span<const controller_middleware_descriptor>{}, invalid_options);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    RUVIA_CHECK(rejected);

    const auto rejects_subprotocols = [](std::vector<std::string> subprotocols) {
        ruvia::detail::router router;
        auto& impl = ruvia::detail::router_impl::from(router);
        ruvia::websocket_route_config options;
        options.subprotocols_ = std::move(subprotocols);
        try {
            impl.register_websocket_route(http_known_method::get, path("/invalid-ws-protocols"),
                ruvia::detail::route_stream_handler_type(nullptr, &dummy_stream_handler),
                std::span<const controller_middleware_descriptor>{},
                std::span<const controller_middleware_descriptor>{}, options);
        } catch (const std::invalid_argument& error) {
            return std::string_view(error.what()) ==
                   "websocket subprotocols must contain at most 64 unique HTTP tokens";
        }
        return false;
    };
    RUVIA_CHECK(rejects_subprotocols({"bad token"}));
    RUVIA_CHECK(rejects_subprotocols({"chat", "chat"}));
    RUVIA_CHECK(rejects_subprotocols({""}));
    std::vector<std::string> too_many_subprotocols;
    for (std::size_t i = 0; i <= ruvia::max_http_header_fields; ++i) {
        too_many_subprotocols.emplace_back("protocol-" + std::to_string(i));
    }
    RUVIA_CHECK(rejects_subprotocols(std::move(too_many_subprotocols)));

    ruvia::detail::router router;
    auto& impl = ruvia::detail::router_impl::from(router);
    ruvia::websocket_route_config options;
    options.lifecycle_.close_handshake_timeout_ = std::chrono::milliseconds(1234);
    options.lifecycle_.peer_transport_fin_timeout_ = std::chrono::milliseconds(2345);
    options.deflate_ = {.compression_level_ = 9, .context_takeover_ = true};
    impl.register_websocket_route(http_known_method::get, path("/ws"),
        ruvia::detail::route_stream_handler_type(nullptr, &dummy_stream_handler),
        std::span<const controller_middleware_descriptor>{},
        std::span<const controller_middleware_descriptor>{}, options);
    impl.finalize();
    const auto resolution = impl.route_table().resolve(http_known_method::get, "/ws");
    const auto* resolved = resolution.resolved();
    RUVIA_CHECK(resolved != nullptr);
    const auto* endpoint = resolved->route().endpoint().get_websocket();
    RUVIA_CHECK(endpoint != nullptr);
    RUVIA_CHECK_EQ(endpoint->lifecycle().close_handshake_timeout_->count(), std::int64_t{1234});
    RUVIA_CHECK_EQ(endpoint->lifecycle().peer_transport_fin_timeout_.count(), std::int64_t{2345});
    RUVIA_CHECK_EQ(endpoint->deflate().compression_level_, 9);
    RUVIA_CHECK(endpoint->deflate().context_takeover_);
    const auto cloned = resolved->route().endpoint().clone(std::pmr::get_default_resource());
    RUVIA_CHECK_EQ(cloned.get_websocket()->deflate().compression_level_, 9);
    RUVIA_CHECK(cloned.get_websocket()->deflate().context_takeover_);
    RUVIA_CHECK_EQ(cloned.get_websocket()->lifecycle().peer_transport_fin_timeout_.count(),
        std::int64_t{2345});
}

RUVIA_TEST(head_only_stream_completion_is_success_not_error) {
    g_chain_order.clear();
    g_head_only_handler_resumed_past_first_write = false;
    const auto observation_value = dispatch_head_only_stream({});
    // The signal ends the dispatch as a handled head-only stream: no error
    // response, no rethrow to the driver, and the stream is finished.
    RUVIA_CHECK(observation_value.handled_);
    RUVIA_CHECK(!observation_value.buffered_);
    RUVIA_CHECK(!observation_value.threw_);
    RUVIA_CHECK(observation_value.ended_);
    // The handler stopped deterministically at its first body write.
    RUVIA_CHECK(!g_head_only_handler_resumed_past_first_write);
}

RUVIA_TEST(streaming_get_routes_require_explicit_head_handlers) {
    ruvia::detail::router router;
    auto& impl = ruvia::detail::router_impl::from(router);
    impl.register_route(http_known_method::get, path("/page"), route_handler_type(nullptr, &dummy_handler),
        request_body_mode::buffered, {}, {});
    impl.register_response_stream_route(http_known_method::get, path("/events"),
        ruvia::detail::route_stream_handler_type(nullptr, &dummy_stream_handler), {}, {});
    impl.register_sse_route(http_known_method::get, path("/sse"),
        ruvia::detail::route_stream_handler_type(nullptr, &dummy_stream_handler), {}, {});
    impl.register_websocket_route(http_known_method::get, path("/ws"),
        ruvia::detail::route_stream_handler_type(nullptr, &dummy_stream_handler), {}, {});
    impl.finalize();
    const auto& table_value = impl.route_table();

    // Buffered GET routes still receive the ordinary implicit HEAD fallback.
    const auto page = table_value.resolve(http_known_method::head, "/page");
    RUVIA_CHECK(page.resolved() != nullptr);

    // Streaming/SSE routes have explicit stream lifecycles and must not be
    // entered by an implicit HEAD fallback. websocket remains GET-only too.
    const auto events_value = table_value.resolve(http_known_method::head, "/events");
    RUVIA_CHECK(events_value.resolved() == nullptr);
    RUVIA_CHECK(events_value.method_not_allowed() != nullptr);
    if (const auto* method_not_allowed = events_value.method_not_allowed()) {
        const auto bit = [](http_known_method method) { return 1U << static_cast<unsigned>(method); };
        RUVIA_CHECK((method_not_allowed->allowed_methods() & bit(http_known_method::get)) != 0);
        RUVIA_CHECK((method_not_allowed->allowed_methods() & bit(http_known_method::head)) == 0);
    }
    const auto sse = table_value.resolve(http_known_method::head, "/sse");
    RUVIA_CHECK(sse.resolved() == nullptr);
    const auto ws = table_value.resolve(http_known_method::head, "/ws");
    RUVIA_CHECK(ws.resolved() == nullptr);
}

RUVIA_TEST(dispatch_maps_handler_exceptions_to_error_responses) {
    // Application and protocol errors retain their status; unknown failures map
    // to 500. router never injects HTTP/1 Connection policy into any response.
    const auto application_value =
        dispatch_one(route_handler_type(nullptr, &throws_http_error_handler), http_known_method::get, "/x");
    RUVIA_CHECK_EQ(application_value.status_, std::uint16_t{403});
    RUVIA_CHECK(application_value.connection_.empty());
    const auto protocol = dispatch_one(
        route_handler_type(nullptr, &throws_protocol_error_handler), http_known_method::get, "/x");
    RUVIA_CHECK_EQ(protocol.status_, std::uint16_t{413});
    RUVIA_CHECK(protocol.connection_.empty());
    const auto generic =
        dispatch_one(route_handler_type(nullptr, &throws_generic_handler), http_known_method::get, "/x");
    RUVIA_CHECK_EQ(generic.status_, std::uint16_t{500});
    RUVIA_CHECK(generic.connection_.empty());
    // The unexpected exception's message must NOT leak into the response body: a
    // library error (SQL text, paths) could otherwise be disclosed to the client.
    RUVIA_CHECK(!(generic.body_.find("boom") != std::string_view::npos));
    RUVIA_CHECK((generic.body_.find("Internal Server Error") != std::string_view::npos));

    // std::invalid_argument is not a request-validation protocol. Application
    // code can throw it for programming errors (for example, a bad route
    // generation call), so it must not be downgraded to a client-visible 400.
    const auto invalid_argument = dispatch_one(
        route_handler_type(nullptr, &throws_invalid_argument_handler), http_known_method::get, "/x");
    RUVIA_CHECK_EQ(invalid_argument.status_, std::uint16_t{500});
    RUVIA_CHECK(!(invalid_argument.body_.find("application bug") != std::string_view::npos));
}

RUVIA_TEST(dispatch_rejects_unsupported_request_content_coding_with_advertisement) {
    const auto result_value = dispatch_one_token(
        route_handler_type(nullptr, &reads_request_body_handler), "GET", "/x", "compress", "encoded");
    RUVIA_CHECK_EQ(result_value.status_, std::uint16_t{415});
    RUVIA_CHECK_EQ(result_value.accept_encoding_, std::string("gzip, deflate, br, zstd"));
    RUVIA_CHECK((result_value.body_.find("unsupported_content_coding") != std::string_view::npos));
}

RUVIA_TEST(dispatch_defensively_rejects_malformed_request_content_coding) {
    const auto result_value = dispatch_one_token(
        route_handler_type(nullptr, &reads_request_body_handler), "GET", "/x", "gzip;level=9", "encoded");
    RUVIA_CHECK_EQ(result_value.status_, std::uint16_t{400});
    RUVIA_CHECK(result_value.accept_encoding_.empty());
}

RUVIA_TEST(dispatch_produces_404_and_405_for_unmatched_routes) {
    // A path with no route -> 404.
    RUVIA_CHECK_EQ(
        dispatch_one(route_handler_type(nullptr, &ok_handler), http_known_method::get, "/nope").status_,
        std::uint16_t{404});
    // The path exists but the method does not -> 405 with an Allow header listing GET.
    const auto not_allowed =
        dispatch_one(route_handler_type(nullptr, &ok_handler), http_known_method::post, "/x");
    RUVIA_CHECK_EQ(not_allowed.status_, std::uint16_t{405});
    RUVIA_CHECK((not_allowed.allow_.find("GET") != std::string_view::npos));
    // The registered method still works.
    RUVIA_CHECK_EQ(
        dispatch_one(route_handler_type(nullptr, &ok_handler), http_known_method::get, "/x").status_,
        std::uint16_t{200});
}

RUVIA_TEST(dispatch_produces_501_for_valid_unimplemented_method_token) {
    const auto extension = dispatch_one_token(route_handler_type(nullptr, &ok_handler), "PROPFIND", "/x");
    RUVIA_CHECK_EQ(extension.status_, std::uint16_t{501});
    RUVIA_CHECK(extension.allow_.empty());
    RUVIA_CHECK(extension.connection_.empty());

    // Method tokens are case-sensitive. A lowercase standard spelling is still
    // syntactically valid but is not the framework's GET semantic class.
    const auto lowercase = dispatch_one_token(route_handler_type(nullptr, &ok_handler), "get", "/x");
    RUVIA_CHECK_EQ(lowercase.status_, std::uint16_t{501});
}

RUVIA_TEST(dispatch_uses_custom_not_found_handler) {
    // A registered not-found handler replaces the default 404 response body.
    const auto result_value = dispatch_with_handlers(route_handler_type(nullptr, &ok_handler), nullptr,
        &custom_not_found, http_known_method::get, "/nope");
    RUVIA_CHECK_EQ(result_value.status_, std::uint16_t{404});
    RUVIA_CHECK_EQ(result_value.body_, std::string("custom-not-found"));
}

RUVIA_TEST(dispatch_uses_custom_error_handler_with_thrown_status) {
    // A registered error handler renders a thrown http_error, preserving its status.
    const auto result_value = dispatch_with_handlers(route_handler_type(nullptr, &throws_http_error_handler),
        &custom_error, nullptr, http_known_method::get, "/x");
    RUVIA_CHECK_EQ(result_value.status_, std::uint16_t{403});
    RUVIA_CHECK_EQ(result_value.body_, std::string("custom-error"));
}

RUVIA_TEST(dispatch_preserves_content_coding_advertisement_with_custom_error_handler) {
    const auto result_value = dispatch_with_handlers_token(route_handler_type(nullptr, &reads_request_body_handler),
        &custom_error, nullptr, "GET", "/x", "compress", "encoded");
    RUVIA_CHECK_EQ(result_value.status_, std::uint16_t{415});
    RUVIA_CHECK_EQ(result_value.body_, std::string("custom-error"));
    RUVIA_CHECK_EQ(result_value.accept_encoding_, std::string("gzip, deflate, br, zstd"));
}

RUVIA_TEST(dispatch_routes_unimplemented_method_through_custom_error_handler) {
    const auto result_value = dispatch_with_handlers_token(
        route_handler_type(nullptr, &ok_handler), &custom_error, nullptr, "PROPFIND", "/x");
    RUVIA_CHECK_EQ(result_value.status_, std::uint16_t{501});
    RUVIA_CHECK_EQ(result_value.body_, std::string("custom-error"));
}

RUVIA_TEST(request_json_form_map_media_type_mismatch_to_415) {
    const controller_middleware_descriptor json_body_value[] = {
        ruvia::detail::make_middleware_descriptor<ruvia::json_body<scoped_validation_request>>()};
    const controller_middleware_descriptor form_body_value[] = {
        ruvia::detail::make_middleware_descriptor<ruvia::form_body<scoped_validation_request>>()};

    // Sanity: the right media type with a parsable body reaches the handler.
    const auto ok = dispatch_body_request(route_handler_type(nullptr, &json_model_echo_handler),
        "application/json", R"({"value":"hi"})", json_body_value);
    RUVIA_CHECK_EQ(ok.status_, std::uint16_t{200});
    RUVIA_CHECK_EQ(ok.body_, std::string("hi"));

    // The wrong media type is the client's format mistake: 415, not 400.
    const auto wrong_type = dispatch_body_request(route_handler_type(nullptr, &json_model_echo_handler),
        "text/plain", R"({"value":"hi"})", json_body_value);
    RUVIA_CHECK_EQ(wrong_type.status_, std::uint16_t{415});
    const auto missing_type = dispatch_body_request(
        route_handler_type(nullptr, &json_model_echo_handler), "", R"({"value":"hi"})", json_body_value);
    RUVIA_CHECK_EQ(missing_type.status_, std::uint16_t{415});
    const auto form_wrong_type = dispatch_body_request(
        route_handler_type(nullptr, &form_model_echo_handler), "application/json", "value=hi", form_body_value);
    RUVIA_CHECK_EQ(form_wrong_type.status_, std::uint16_t{415});

    // A malformed body of the RIGHT type stays 400.
    const auto bad_body = dispatch_body_request(
        route_handler_type(nullptr, &json_model_echo_handler), "application/json", "{not-json", json_body_value);
    RUVIA_CHECK_EQ(bad_body.status_, std::uint16_t{400});

    const auto form_ok = dispatch_body_request(route_handler_type(nullptr, &form_model_echo_handler),
        "application/x-www-form-urlencoded", "value=hi", form_body_value);
    RUVIA_CHECK_EQ(form_ok.status_, std::uint16_t{200});
    RUVIA_CHECK_EQ(form_ok.body_, std::string("hi"));
}

RUVIA_TEST(request_json_if_and_form_if_only_fall_back_on_media_type_mismatch) {
    // A media-type mismatch yields nullopt so the handler can try another
    // representation...
    const auto wrong_type =
        dispatch_body_request(route_handler_type(nullptr, &json_if_echo_handler), "text/plain", "x");
    RUVIA_CHECK_EQ(wrong_type.status_, std::uint16_t{200});
    RUVIA_CHECK_EQ(wrong_type.body_, std::string("no-json"));
    const auto bad_body = dispatch_body_request(
        route_handler_type(nullptr, &json_if_echo_handler), "application/json", "{not-json");
    RUVIA_CHECK_EQ(bad_body.status_, std::uint16_t{400});
    const auto form_wrong_type =
        dispatch_body_request(route_handler_type(nullptr, &form_if_echo_handler), "text/plain", "value=hi");
    RUVIA_CHECK_EQ(form_wrong_type.status_, std::uint16_t{200});
    RUVIA_CHECK_EQ(form_wrong_type.body_, std::string("no-form"));
    const auto bad_form = dispatch_body_request(route_handler_type(nullptr, &form_if_echo_handler),
        "application/x-www-form-urlencoded", "value=%ZZ");
    RUVIA_CHECK_EQ(bad_form.status_, std::uint16_t{400});

    // A well-formed body of the right type still parses.
    const auto ok = dispatch_body_request(
        route_handler_type(nullptr, &json_if_echo_handler), "application/json", R"({"value":"hi"})");
    RUVIA_CHECK_EQ(ok.status_, std::uint16_t{200});
    RUVIA_CHECK_EQ(ok.body_, std::string("hi"));
    const auto form_ok = dispatch_body_request(
        route_handler_type(nullptr, &form_if_echo_handler), "application/x-www-form-urlencoded", "value=hi");
    RUVIA_CHECK_EQ(form_ok.status_, std::uint16_t{200});
    RUVIA_CHECK_EQ(form_ok.body_, std::string("hi"));
}

RUVIA_TEST(prefix_not_found_handler_scopes_by_longest_segment_prefix) {
    ruvia::detail::router router;
    auto& impl = ruvia::detail::router_impl::from(router);
    impl.register_route(http_known_method::get, path("/api/real"), route_handler_type(nullptr, &ok_handler),
        request_body_mode::buffered, std::span<const controller_middleware_descriptor>{},
        std::span<const controller_middleware_descriptor>{});
    ruvia::http_not_found_handler_type default_not_found(&custom_not_found);
    ruvia::http_not_found_handler_type api_not_found(&api_scoped_not_found);
    ruvia::http_not_found_handler_type v2_not_found(&v2_scoped_not_found);
    impl.set_not_found_handler(ruvia::detail::callback_access::ref(default_not_found));
    const ruvia::detail::http_prefix_not_found_handler scoped[] = {
        {"/api", ruvia::detail::callback_access::ref(api_not_found)},
        // Trailing slash normalizes away; this is the same scope as "/api/v2".
        {"/api/v2/", ruvia::detail::callback_access::ref(v2_not_found)},
    };
    impl.set_prefix_not_found_handlers(
        std::span<const ruvia::detail::http_prefix_not_found_handler>(scoped, 2));
    impl.finalize();
    const auto& table_value = impl.route_table();

    // Inside the scope: the prefix handler renders the miss.
    RUVIA_CHECK_EQ(dispatch_on(table_value, "GET", "/api/missing").body_, std::string("api-scope-404"));
    // The mount path itself belongs to the scope.
    RUVIA_CHECK_EQ(dispatch_on(table_value, "GET", "/api").body_, std::string("api-scope-404"));
    // The longest matching prefix wins over an enclosing one.
    RUVIA_CHECK_EQ(dispatch_on(table_value, "GET", "/api/v2/missing").body_, std::string("v2-scope-404"));
    // Segment boundary: "/apix" is not under "/api".
    RUVIA_CHECK_EQ(dispatch_on(table_value, "GET", "/apix").body_, std::string("custom-not-found"));
    // Outside every scope the app-wide handler still runs.
    RUVIA_CHECK_EQ(dispatch_on(table_value, "GET", "/other").body_, std::string("custom-not-found"));
}

RUVIA_TEST(prefix_error_handler_scopes_thrown_route_failures) {
    ruvia::detail::router router;
    auto& impl = ruvia::detail::router_impl::from(router);
    impl.register_route(http_known_method::get, path("/api/boom"),
        route_handler_type(nullptr, &throws_http_error_handler), request_body_mode::buffered,
        std::span<const controller_middleware_descriptor>{},
        std::span<const controller_middleware_descriptor>{});
    impl.register_route(http_known_method::get, path("/boom"),
        route_handler_type(nullptr, &throws_http_error_handler), request_body_mode::buffered,
        std::span<const controller_middleware_descriptor>{},
        std::span<const controller_middleware_descriptor>{});
    ruvia::http_error_handler_type default_error(&custom_error);
    ruvia::http_error_handler_type api_error(&api_scoped_error);
    impl.set_error_handler(ruvia::detail::callback_access::ref(default_error));
    const ruvia::detail::http_prefix_error_handler scoped[] = {
        {"/api", ruvia::detail::callback_access::ref(api_error)},
    };
    impl.set_prefix_error_handlers(std::span<const ruvia::detail::http_prefix_error_handler>(scoped, 1));
    impl.finalize();
    const auto& table_value = impl.route_table();

    const auto scoped_result = dispatch_on(table_value, "GET", "/api/boom");
    RUVIA_CHECK_EQ(scoped_result.status_, std::uint16_t{403});
    RUVIA_CHECK_EQ(scoped_result.body_, std::string("api-scope-error"));

    const auto global_result = dispatch_on(table_value, "GET", "/boom");
    RUVIA_CHECK_EQ(global_result.status_, std::uint16_t{403});
    RUVIA_CHECK_EQ(global_result.body_, std::string("custom-error"));
}

RUVIA_TEST(dispatch_options_asterisk_returns_server_wide_allow) {
    // A server-wide OPTIONS * request is answered with 204 and an Allow header
    // listing every method registered anywhere on the server, not per-route.
    ruvia::detail::router router;
    auto& impl = ruvia::detail::router_impl::from(router);
    impl.register_route(http_known_method::get, path("/a"), route_handler_type(nullptr, &ok_handler),
        request_body_mode::buffered, std::span<const controller_middleware_descriptor>{},
        std::span<const controller_middleware_descriptor>{});
    impl.register_route(http_known_method::post, path("/b"), route_handler_type(nullptr, &ok_handler),
        request_body_mode::buffered, std::span<const controller_middleware_descriptor>{},
        std::span<const controller_middleware_descriptor>{});
    impl.register_extension_method_route("PROPFIND", path("/c"), route_handler_type(nullptr, &ok_handler),
        request_body_mode::buffered, std::span<const controller_middleware_descriptor>{},
        std::span<const controller_middleware_descriptor>{});
    impl.finalize();
    const auto& table_value = impl.route_table();

    ruvia::worker_memory worker;
    ruvia::request_memory memory(worker);
    auto request = make_request(memory, "OPTIONS", "*");

    asio::io_context ctx(1);
    auto future = asio::co_spawn(ctx,
        ruvia::as_awaitable(
            table_value.dispatch(request, memory, ruvia::test::test_context_services())),
        asio::use_future);
    ctx.run();
    const auto response = future.get();

    RUVIA_CHECK_EQ(response.status(), ruvia::http_status::no_content);
    const auto allow = response.header("Allow").value_or(std::string_view{});
    RUVIA_CHECK((allow.find("GET") != std::string_view::npos));
    RUVIA_CHECK((allow.find("POST") != std::string_view::npos));
    RUVIA_CHECK((allow.find("PROPFIND") != std::string_view::npos));
}
