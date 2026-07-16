#include "test_harness.h"

#include <asio/awaitable.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/use_future.hpp>

#include <array>
#include <chrono>
#include <exception>
#include <memory_resource>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "ruvia/web/detail/http/ContextInternal.h"
#include "ruvia/http/detail/HttpRequestInternal.h"
#include "ruvia/http/detail/HttpResponseBodyAccess.h"
#include "ruvia/http/HttpProtocolError.h"
#include "ruvia/web/detail/http/StreamingInternal.h"
#include "ruvia/web/Streaming.h"
#include "ruvia/core/detail/AsioAwait.h"
#include "ruvia/web/Context.h"
#include "ruvia/web/Controller.h"
#include "ruvia/web/detail/middleware/MiddlewareRegistration.h"
#include "ruvia/core/memory/MemoryPool.h"
#include "ruvia/web/Router.h"
#include "ruvia/web/RateLimit.h"
#include "ruvia/web/detail/router/RouterInternal.h"
#include "ruvia/web/detail/router/RouteResolution.h"
#include "ruvia/web/detail/router/RouteTable.h"
#include "ruvia/web/detail/websocket/WebSocketInternal.h"

RUVIA_REQUEST_MODEL(ScopedValidationRequest,
    RUVIA_FIELD(value, ruvia::String)
);

namespace {

using ruvia::HttpKnownMethod;
using ruvia::detail::ControllerMiddlewareDescriptor;
using ruvia::detail::RouteHandler;
using ruvia::detail::RouteMatch;
using ruvia::detail::RequestBodyMode;

class FirstIntValidator final : public ruvia::Middleware<FirstIntValidator> {
public:
    using RuviaValidationBody = int;

    ruvia::Task<void> handle(ruvia::Context&, ruvia::Next&) {
        co_return;
    }
};

class SecondIntValidator final : public ruvia::Middleware<SecondIntValidator> {
public:
    using RuviaValidationBody = int;

    ruvia::Task<void> handle(ruvia::Context&, ruvia::Next&) {
        co_return;
    }
};

class ScopedValidationValidator final
    : public ruvia::Middleware<ScopedValidationValidator> {
public:
    RUVIA_VALIDATE_JSON(ScopedValidationRequest,
        RUVIA_RULE(value, RUVIA_REQUIRED("value is required")))
};

class ValidationScopeProbe final
    : public ruvia::Middleware<ValidationScopeProbe> {
public:
    ruvia::Task<void> handle(ruvia::Context& context, ruvia::Next& next) {
        co_await next();
        try {
            (void)context.req().valid<ScopedValidationRequest>();
        } catch (const std::logic_error&) {
            releasedAfterNext = true;
        }
    }

    static inline bool releasedAfterNext{false};
};

bool scopedValidationHandlerRead{false};
bool scopedValidationHandlerThrows{false};

ruvia::Task<ruvia::HttpResponse> scopedValidationHandler(
    void*,
    ruvia::Context& context) {
    const auto& model = context.req().valid<ScopedValidationRequest>();
    scopedValidationHandlerRead =
        model.value().has_value() && model.value()->view() == "ok";
    if (scopedValidationHandlerThrows) {
        throw std::runtime_error("validated handler failure");
    }
    co_return context.text("validated");
}

// Never invoked — resolve() only needs a registered route with a valid handler.
ruvia::Task<ruvia::HttpResponse> dummyHandler(void*, ruvia::Context&) {
    co_return ruvia::HttpResponse(std::pmr::get_default_resource());
}

ruvia::Task<void> dummyStreamHandler(void*, ruvia::Context&) {
    co_return;
}

std::pmr::string path(std::string_view value) {
    return std::pmr::string(value, std::pmr::get_default_resource());
}

void addRoute(ruvia::detail::RouterImpl& impl, HttpKnownMethod method, std::string_view route) {
    impl.registerRoute(
        method,
        path(route),
        RouteHandler(nullptr, &dummyHandler),
        RequestBodyMode::kBuffered,
        std::span<const ControllerMiddlewareDescriptor>{},
        std::span<const ControllerMiddlewareDescriptor>{});
}

void addRoute(ruvia::detail::RouterImpl& impl, std::string_view route) {
    addRoute(impl, HttpKnownMethod::kGet, route);
}

// Registers the given routes and reports whether finalize() rejects them as a
// dynamic route-shape conflict.
bool finalizeConflicts(std::initializer_list<std::string_view> routes) {
    ruvia::Router router;
    auto& impl = ruvia::detail::RouterImpl::from(router);
    for (const auto route : routes) {
        addRoute(impl, route);
    }
    try {
        impl.finalize();
        return false;
    } catch (const std::exception&) {
        return true;
    }
}

RUVIA_TEST(websocket_route_owns_validated_lifecycle_policy) {
    ruvia::Router invalidRouter;
    auto& invalid = ruvia::detail::RouterImpl::from(invalidRouter);
    ruvia::WebSocketRouteOptions invalidOptions;
    invalidOptions.lifecycle.closeHandshakeTimeout = std::chrono::milliseconds(0);
    bool rejected = false;
    try {
        invalid.registerWebSocketRoute(
            HttpKnownMethod::kGet,
            path("/invalid-ws"),
            ruvia::detail::RouteStreamHandler(nullptr, &dummyStreamHandler),
            std::span<const ControllerMiddlewareDescriptor>{},
            std::span<const ControllerMiddlewareDescriptor>{},
            invalidOptions);
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
            impl.registerWebSocketRoute(
                HttpKnownMethod::kGet,
                path("/invalid-ws-protocols"),
                ruvia::detail::RouteStreamHandler(
                    nullptr, &dummyStreamHandler),
                std::span<const ControllerMiddlewareDescriptor>{},
                std::span<const ControllerMiddlewareDescriptor>{},
                options);
        } catch (const std::invalid_argument& error) {
            return std::string_view(error.what()) ==
                "websocket subprotocols must be a list of at most 64 unique HTTP tokens";
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
    impl.registerWebSocketRoute(
        HttpKnownMethod::kGet,
        path("/ws"),
        ruvia::detail::RouteStreamHandler(nullptr, &dummyStreamHandler),
        std::span<const ControllerMiddlewareDescriptor>{},
        std::span<const ControllerMiddlewareDescriptor>{},
        options);
    impl.finalize();
    const auto resolution =
        impl.routeTable().resolve(HttpKnownMethod::kGet, "/ws");
    const auto* resolved = resolution.resolved();
    RUVIA_CHECK(resolved != nullptr);
    const auto* endpoint = resolved->route().endpoint().webSocket();
    RUVIA_CHECK(endpoint != nullptr);
    RUVIA_CHECK_EQ(
        endpoint->lifecycle().closeHandshakeTimeout->count(),
        std::int64_t{1234});
}

RUVIA_TEST(route_rejects_duplicate_validated_model_types_at_registration) {
    ruvia::Router router;
    auto& impl = ruvia::detail::RouterImpl::from(router);
    const auto controllerValidator =
        ruvia::detail::makeMiddlewareDescriptor<FirstIntValidator>();
    const auto routeValidator =
        ruvia::detail::makeMiddlewareDescriptor<SecondIntValidator>();

    bool rejected = false;
    try {
        impl.registerRoute(
            HttpKnownMethod::kPost,
            path("/duplicate-validated-model"),
            RouteHandler(nullptr, &dummyHandler),
            RequestBodyMode::kBuffered,
            std::span(&controllerValidator, std::size_t{1}),
            std::span(&routeValidator, std::size_t{1}));
    } catch (const std::invalid_argument& error) {
        rejected = std::string_view(error.what()) ==
            "duplicate validated model type on route";
    }
    RUVIA_CHECK(rejected);
}

RUVIA_TEST(finalized_route_table_records_route_rate_limit_usage) {
    {
        ruvia::Router router;
        auto& impl = ruvia::detail::RouterImpl::from(router);
        addRoute(impl, "/plain");
        impl.finalize();
        RUVIA_CHECK(!impl.routeTable().hasRouteRateLimit());
    }

    {
        ruvia::Router router;
        auto& impl = ruvia::detail::RouterImpl::from(router);
        const auto rateLimit = ruvia::detail::makeMiddlewareDescriptor<
            ruvia::RouteRateLimit<1, 1000>>();
        impl.registerRoute(
            HttpKnownMethod::kGet,
            path("/limited"),
            RouteHandler(nullptr, &dummyHandler),
            RequestBodyMode::kBuffered,
            std::span<const ControllerMiddlewareDescriptor>{},
            std::span(&rateLimit, std::size_t{1}));
        impl.finalize();
        RUVIA_CHECK(impl.routeTable().hasRouteRateLimit());
    }
}

RUVIA_TEST(validated_model_binding_spans_next_and_unwinds_before_upstream_resumes) {
    for (const bool handlerThrows : {false, true}) {
        scopedValidationHandlerRead = false;
        scopedValidationHandlerThrows = handlerThrows;
        ValidationScopeProbe::releasedAfterNext = false;

        ruvia::Router router;
        auto& impl = ruvia::detail::RouterImpl::from(router);
        const std::array middlewares{
            ruvia::detail::makeMiddlewareDescriptor<ValidationScopeProbe>(),
            ruvia::detail::makeMiddlewareDescriptor<ScopedValidationValidator>()};
        impl.registerRoute(
            HttpKnownMethod::kPost,
            path("/validated-scope"),
            RouteHandler(nullptr, &scopedValidationHandler),
            RequestBodyMode::kBuffered,
            std::span<const ControllerMiddlewareDescriptor>{},
            std::span(middlewares));
        impl.finalize();

        ruvia::WorkerMemory worker;
        ruvia::RequestMemory memory(worker);
        auto request = ruvia::detail::HttpRequestAccess::make();
        ruvia::detail::HttpRequestAccess::reset(request);
        ruvia::detail::HttpRequestAccess::setMethod(request, "POST");
        ruvia::detail::HttpRequestAccess::setPath(request, "/validated-scope");
        ruvia::detail::HttpRequestAccess::setResource(request, memory.resource());
        const auto contentTypeSlot =
            ruvia::detail::HttpRequestAccess::knownHeaderSlot(
                ruvia::detail::RequestKnownHeader::kContentType);
        (void)ruvia::detail::HttpRequestAccess::addHeader(
            request,
            ruvia::HttpHeaderView{"Content-Type", "application/json"},
            contentTypeSlot);
        ruvia::detail::HttpRequestAccess::setBody(
            request, R"({"value":"ok"})");

        asio::io_context ioContext(1);
        auto future = asio::co_spawn(
            ioContext,
            ruvia::detail::taskAsAwaitable(
                impl.routeTable().dispatch(request, memory, {})),
            asio::use_future);
        ioContext.run();
        const auto response = future.get();
        RUVIA_CHECK_EQ(
            response.status(),
            handlerThrows ? std::uint16_t{500} : std::uint16_t{200});
        RUVIA_CHECK(scopedValidationHandlerRead);
        RUVIA_CHECK(ValidationScopeProbe::releasedAfterNext);
    }
}

struct Router final {
    ruvia::Router router;
    ruvia::detail::RouterImpl& impl = ruvia::detail::RouterImpl::from(router);

    void finalize() { impl.finalize(); }

    bool matches(std::string_view p) {
        const auto resolution = impl.routeTable().resolve(
            HttpKnownMethod::kGet, p);
        return resolution.resolved() != nullptr;
    }

    std::string_view routePathOf(std::string_view p) {
        return routePathOf(HttpKnownMethod::kGet, p);
    }

    std::string_view routePathOf(HttpKnownMethod method, std::string_view p) {
        const auto res = impl.routeTable().resolve(method, p);
        const auto* resolved = res.resolved();
        return resolved != nullptr ? resolved->route().path() : "<none>";
    }

    // Returns the single captured param value, or "<none>" if unmatched / no param.
    std::string_view paramOf(std::string_view p) {
        const auto res = impl.routeTable().resolve(HttpKnownMethod::kGet, p);
        const auto* resolved = res.resolved();
        if (resolved == nullptr || resolved->match().size() != 1) {
            return "<none>";
        }
        return resolved->match().values()[0];
    }
};

}  // namespace

RUVIA_TEST(routing_dynamic_exact_match) {
    Router r;
    addRoute(r.impl, "/users/:id");
    r.finalize();
    RUVIA_CHECK(r.matches("/users/42"));
    RUVIA_CHECK_EQ(r.paramOf("/users/42"), std::string_view("42"));
}

RUVIA_TEST(routing_dynamic_trailing_slash_rejected) {
    Router r;
    addRoute(r.impl, "/users/:id");
    r.finalize();
    // Strict matching: a trailing slash is a distinct (empty) segment, so this
    // must NOT collapse to /users/42.
    RUVIA_CHECK(!r.matches("/users/42/"));
}

RUVIA_TEST(routing_dynamic_empty_segment_rejected) {
    Router r;
    addRoute(r.impl, "/a/:x");
    r.finalize();
    RUVIA_CHECK(r.matches("/a/b"));
    RUVIA_CHECK_EQ(r.paramOf("/a/b"), std::string_view("b"));
    // A collapsed // must not silently match and drop the empty segment.
    RUVIA_CHECK(!r.matches("/a//b"));
    RUVIA_CHECK(!r.matches("/a/"));
}

RUVIA_TEST(routing_static_trailing_slash_rejected) {
    Router r;
    addRoute(r.impl, "/users/list");
    addRoute(r.impl, "/users/:id");
    r.finalize();
    RUVIA_CHECK(r.matches("/users/list"));
    RUVIA_CHECK(!r.matches("/users/list/"));
    // /users/list still routes to the static entry (no param), not the dynamic one.
    RUVIA_CHECK_EQ(r.paramOf("/users/list"), std::string_view("<none>"));
    // The dynamic sibling still works.
    RUVIA_CHECK_EQ(r.paramOf("/users/99"), std::string_view("99"));
}

RUVIA_TEST(routing_wildcard_still_matches_tail) {
    Router r;
    addRoute(r.impl, "/files/*");
    r.finalize();
    RUVIA_CHECK(r.matches("/files/a/b/c"));
    RUVIA_CHECK_EQ(r.paramOf("/files/a/b/c"), std::string_view("a/b/c"));
}

RUVIA_TEST(routing_root_wildcard_shadows_param_conflict) {
    // A root wildcard shadows a root param sibling: must error at startup.
    RUVIA_CHECK(finalizeConflicts({"/*", "/:x"}));
    // Deeper-level shadowing already errored; guard it stays that way.
    RUVIA_CHECK(finalizeConflicts({"/a/*", "/a/:x"}));
    // Equivalent param shapes still conflict.
    RUVIA_CHECK(finalizeConflicts({"/users/:id", "/users/:name"}));
}

RUVIA_TEST(routing_root_wildcard_with_static_prefix_allowed) {
    // A root wildcard may coexist with routes distinguished by a static segment.
    RUVIA_CHECK(!finalizeConflicts({"/*", "/users/:id"}));
    RUVIA_CHECK(!finalizeConflicts({"/*", "/health"}));
}

RUVIA_TEST(routing_deep_wildcard_with_static_prefix_allowed) {
    // The static-sibling exemption holds at any depth, not just the root: "public" is a static
    // child tried before the wildcard, so "/files/public/5" -> :id route, "/files/x" -> wildcard,
    // deterministically. (Same situation as the allowed root case /* + /users/:id, one level down.)
    RUVIA_CHECK(!finalizeConflicts({"/files/*", "/files/public/:id"}));
    RUVIA_CHECK(!finalizeConflicts({"/files/*", "/files/list"}));
    RUVIA_CHECK(!finalizeConflicts({"/a/b/*", "/a/b/c/:id"}));
    RUVIA_CHECK(!finalizeConflicts({"/:section/*", "/health/live"}));
    RUVIA_CHECK(!finalizeConflicts({"/files/:bucket/*", "/files/public/:id"}));
    RUVIA_CHECK(!finalizeConflicts({"/:section/*", "/health/*"}));
    RUVIA_CHECK(!finalizeConflicts({"/:section/live", "/health/:probe"}));

    // Guards the fix must NOT regress (these genuinely shadow -> still conflicts):
    RUVIA_CHECK(finalizeConflicts({"/a/*", "/a/:x"}));   // wildcard vs param sibling at a shared node
    RUVIA_CHECK(finalizeConflicts({"/a/*", "/:x/b"}));   // after a static/param fork, the wildcard
                                                         // steals the other route's direct-match path
    RUVIA_CHECK(finalizeConflicts({"/*", "/:x"}));       // root wildcard vs param
    RUVIA_CHECK(finalizeConflicts({"/users/:id", "/users/:name"}));  // two params at one position
}

RUVIA_TEST(routing_allowed_wildcard_overlaps_resolve_to_static_priority_branch) {
    {
        Router r;
        addRoute(r.impl, "/:section/*");
        addRoute(r.impl, "/health/*");
        r.finalize();
        RUVIA_CHECK_EQ(r.routePathOf("/health/live"), std::string_view("/health/*"));
        RUVIA_CHECK_EQ(r.routePathOf("/users/42"), std::string_view("/:section/*"));
    }

    {
        Router r;
        addRoute(r.impl, "/files/:bucket/*");
        addRoute(r.impl, "/files/public/:id");
        r.finalize();
        RUVIA_CHECK_EQ(r.routePathOf("/files/public/5"), std::string_view("/files/public/:id"));
        RUVIA_CHECK_EQ(r.routePathOf("/files/private/a/b"), std::string_view("/files/:bucket/*"));
    }

    {
        Router r;
        addRoute(r.impl, "/:section/live");
        addRoute(r.impl, "/health/:probe");
        r.finalize();
        RUVIA_CHECK_EQ(r.routePathOf("/health/live"), std::string_view("/health/:probe"));
        RUVIA_CHECK_EQ(r.routePathOf("/users/live"), std::string_view("/:section/live"));
    }
}

RUVIA_TEST(routing_head_fallback_respects_static_priority_overlap) {
    Router r;
    addRoute(r.impl, "/:section/live");                   // implicit HEAD fallback
    addRoute(r.impl, HttpKnownMethod::kHead, "/health/:probe"); // explicit HEAD static branch
    r.finalize();

    RUVIA_CHECK_EQ(r.routePathOf(HttpKnownMethod::kHead, "/health/live"), std::string_view("/health/:probe"));
    RUVIA_CHECK_EQ(r.routePathOf(HttpKnownMethod::kHead, "/users/live"), std::string_view("/:section/live"));
}

RUVIA_TEST(routing_explicit_dynamic_head_overrides_exact_get_fallback) {
    Router r;
    addRoute(r.impl, "/health/live");                       // implicit exact HEAD fallback
    addRoute(r.impl, HttpKnownMethod::kHead, "/:section/:probe"); // explicit HEAD route
    r.finalize();

    RUVIA_CHECK_EQ(r.routePathOf(HttpKnownMethod::kHead, "/health/live"), std::string_view("/:section/:probe"));
}

RUVIA_TEST(routing_405_allow_set_lists_the_other_registered_methods) {
    // A request whose method has no route for an existing path is a 405, and the
    // Allow set (RFC 7231 6.5.5) must list exactly the methods that DO have a route
    // for that path -- not methods belonging to other paths, and not the requested
    // method echoed back. This drives the Allow header and was only tested at the
    // RouteResolution value level, never through the route-table computation.
    Router r;
    addRoute(r.impl, HttpKnownMethod::kGet, "/a");
    addRoute(r.impl, HttpKnownMethod::kPost, "/a");
    addRoute(r.impl, HttpKnownMethod::kPut, "/b");
    r.finalize();

    const auto bit = [](HttpKnownMethod m) { return 1U << static_cast<unsigned>(m); };

    const auto res =
        r.impl.routeTable().resolve(HttpKnownMethod::kDelete, "/a");
    RUVIA_CHECK(res.resolved() == nullptr);
    const auto* methodNotAllowed = res.methodNotAllowed();
    RUVIA_CHECK(methodNotAllowed != nullptr);  // /a exists for other methods -> 405
    const auto mask = methodNotAllowed->allowedMethods();
    RUVIA_CHECK((mask & bit(HttpKnownMethod::kGet)) != 0);
    RUVIA_CHECK((mask & bit(HttpKnownMethod::kPost)) != 0);
    RUVIA_CHECK((mask & bit(HttpKnownMethod::kHead)) != 0);    // auto-registered alongside the GET route
    RUVIA_CHECK((mask & bit(HttpKnownMethod::kPut)) == 0);     // belongs to /b, not /a
    RUVIA_CHECK((mask & bit(HttpKnownMethod::kDelete)) == 0);  // the requested method is not echoed back

    // A path with no route at all is a 404 (not found), never a 405.
    const auto missing =
        r.impl.routeTable().resolve(HttpKnownMethod::kGet, "/nope");
    RUVIA_CHECK(missing.resolved() == nullptr);
    RUVIA_CHECK(missing.methodNotAllowed() == nullptr);
    RUVIA_CHECK(missing.notFound() != nullptr);
}

RUVIA_TEST(routing_options_only_resource_is_405_not_404) {
    // A path whose only registered method is OPTIONS must answer 405 (method known
    // but unsupported, RFC 9110 15.5.6), listing OPTIONS in Allow -- not 404, since
    // the resource exists. allowedMethods used to clear the OPTIONS bit and so
    // returned an empty set, making resolve() report not-found.
    Router r;
    addRoute(r.impl, HttpKnownMethod::kOptions, "/preflight");
    r.finalize();
    const auto bit = [](HttpKnownMethod m) { return 1U << static_cast<unsigned>(m); };

    const auto res =
        r.impl.routeTable().resolve(HttpKnownMethod::kGet, "/preflight");
    RUVIA_CHECK(res.resolved() == nullptr);
    RUVIA_CHECK(res.methodNotAllowed() != nullptr);  // 405, not 404
    RUVIA_CHECK((res.methodNotAllowed()->allowedMethods() &
                 bit(HttpKnownMethod::kOptions)) != 0);

    // The explicit OPTIONS route still handles an OPTIONS request to that path.
    const auto preflight = r.impl.routeTable().resolve(
        HttpKnownMethod::kOptions, "/preflight");
    RUVIA_CHECK(preflight.resolved() != nullptr);
}

RUVIA_TEST(routing_options_asterisk_not_captured_by_wildcard_route) {
    // RFC 9110 7.1 / 9.3.7: "OPTIONS *" is a server-wide request, not a resource one.
    // A catch-all OPTIONS route must NOT capture it (the wildcard node otherwise
    // would), so it stays unresolved and dispatch emits the server-wide response.
    Router r;
    addRoute(r.impl, HttpKnownMethod::kOptions, "/*");
    addRoute(r.impl, HttpKnownMethod::kGet, "/*");
    r.finalize();

    const auto asterisk = r.impl.routeTable().resolve(
        HttpKnownMethod::kOptions, "*");
    RUVIA_CHECK(asterisk.notFound() != nullptr);

    // A normal path still matches the catch-all: the short-circuit is only for "*".
    const auto wildcard = r.impl.routeTable().resolve(
        HttpKnownMethod::kOptions, "/anything");
    RUVIA_CHECK(wildcard.resolved() != nullptr);
}

RUVIA_TEST(routing_rejects_duplicate_route_registration) {
    Router r;
    addRoute(r.impl, HttpKnownMethod::kGet, "/x");
    // The same method+path registered twice is a duplicate: ambiguous routing is
    // rejected at registration rather than one route silently shadowing the other.
    bool threw = false;
    try {
        addRoute(r.impl, HttpKnownMethod::kGet, "/x");
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    RUVIA_CHECK(threw);
    // The SAME path under a DIFFERENT method is not a duplicate.
    addRoute(r.impl, HttpKnownMethod::kPost, "/x");
    r.finalize();
    RUVIA_CHECK(r.matches("/x"));
}

RUVIA_TEST(routing_rejects_registration_after_finalize) {
    Router r;
    addRoute(r.impl, HttpKnownMethod::kGet, "/x");
    r.finalize();
    // The route table is immutable once finalized; a late registration must throw
    // rather than mutate the already-built table.
    bool threw = false;
    try {
        addRoute(r.impl, HttpKnownMethod::kGet, "/y");
    } catch (const std::logic_error&) {
        threw = true;
    }
    RUVIA_CHECK(threw);
}

namespace {

// Records the order middlewares and the handler run: positive on entry (before
// next()), negative on unwind (after next()), 0 for the handler.
std::vector<int> g_chainOrder;

class ChainMwA final : public ruvia::Middleware<ChainMwA> {
public:
    ruvia::Task<void> handle(ruvia::Context&, ruvia::Next& next) {
        g_chainOrder.push_back(1);
        co_await next();
        g_chainOrder.push_back(-1);
    }
};

class ChainMwB final : public ruvia::Middleware<ChainMwB> {
public:
    ruvia::Task<void> handle(ruvia::Context&, ruvia::Next& next) {
        g_chainOrder.push_back(2);
        co_await next();
        g_chainOrder.push_back(-2);
    }
};

class ChainMwOverrideAfterNext final
    : public ruvia::Middleware<ChainMwOverrideAfterNext> {
public:
    ruvia::Task<void> handle(ruvia::Context& context, ruvia::Next& next) {
        co_await next();
        context.respond(context.body("override"));
    }
};

// Short-circuits: sets a response and does NOT call next().
class ChainMwStop final : public ruvia::Middleware<ChainMwStop> {
public:
    ruvia::Task<void> handle(ruvia::Context& context, ruvia::Next&) {
        g_chainOrder.push_back(9);
        context.respond(context.body("stopped"));
        co_return;
    }
};

// Misuse: calls next() twice. The second invocation must be rejected rather than
// re-entering the downstream chain (which would run the handler -- and its side
// effects -- a second time).
class ChainMwDoubleNext final : public ruvia::Middleware<ChainMwDoubleNext> {
public:
    ruvia::Task<void> handle(ruvia::Context&, ruvia::Next& next) {
        co_await next();
        co_await next();
    }
};

bool g_webSocketUnavailableAfterNext = false;

class ChainMwProbeWebSocketAfterNext final
    : public ruvia::Middleware<ChainMwProbeWebSocketAfterNext> {
public:
    ruvia::Task<void> handle(ruvia::Context& context, ruvia::Next& next) {
        co_await next();
        try {
            (void)context.webSocket();
        } catch (const std::logic_error&) {
            g_webSocketUnavailableAfterNext = true;
        }
    }
};

class ChainMwThrowsAfterNext final
    : public ruvia::Middleware<ChainMwThrowsAfterNext> {
public:
    ruvia::Task<void> handle(ruvia::Context&, ruvia::Next& next) {
        co_await next();
        throw std::runtime_error("middleware post failed");
    }
};

// Throws before calling next(): the chain is short-circuited and the exception
// must be mapped to an error response (never escaping the dispatch).
class ChainMwThrows final : public ruvia::Middleware<ChainMwThrows> {
public:
    ruvia::Task<void> handle(ruvia::Context&, ruvia::Next&) {
        throw ruvia::HttpError(401, "mw_rejected", "middleware rejected the request");
        co_return;  // unreachable
    }
};

ruvia::Task<ruvia::HttpResponse> chainHandler(void*, ruvia::Context& context) {
    g_chainOrder.push_back(0);
    co_return context.body("ok");
}

std::string dispatchChain(
    std::span<const ControllerMiddlewareDescriptor> controllerMiddlewares,
    std::span<const ControllerMiddlewareDescriptor> routeMiddlewares = {}) {
    ruvia::Router router;
    auto& impl = ruvia::detail::RouterImpl::from(router);
    impl.registerRoute(
        HttpKnownMethod::kGet, path("/chain"), RouteHandler(nullptr, &chainHandler),
        RequestBodyMode::kBuffered, controllerMiddlewares, routeMiddlewares);
    impl.finalize();
    const auto& table = impl.routeTable();

    ruvia::WorkerMemory worker;
    ruvia::RequestMemory memory(worker);
    ruvia::HttpRequest request = ruvia::detail::HttpRequestAccess::make();
    ruvia::detail::HttpRequestAccess::reset(request);
    ruvia::detail::HttpRequestAccess::setMethod(request, "GET");
    ruvia::detail::HttpRequestAccess::setPath(request, "/chain");
    ruvia::detail::HttpRequestAccess::setResource(request, memory.resource());

    asio::io_context ctx(1);
    auto future = asio::co_spawn(
        ctx, ruvia::detail::taskAsAwaitable(table.dispatch(request, memory, {})),
        asio::use_future);
    ctx.run();
    auto response = future.get();
    const auto body = ruvia::detail::responseBody(response).bytes();
    return std::string(body.data(), body.size());
}

// A capture sink whose committed flag flips true on the first write, mirroring the
// real streaming sink (which commits the head before the first body chunk).
struct StreamCaptureSink final {
    std::pmr::string scratch{std::pmr::get_default_resource()};
    bool committedFlag = false;
    bool endedFlag = false;
    bool contextReleased = false;
    std::vector<std::string> writes;
};

ruvia::Task<void> scWrite(void* target, std::string_view chunk) {
    auto* sink = static_cast<StreamCaptureSink*>(target);
    sink->committedFlag = true;
    sink->writes.emplace_back(chunk);
    co_return;
}
ruvia::Task<void> scEnd(
    void* target,
    std::span<const ruvia::HttpHeaderView>) {
    auto* sink = static_cast<StreamCaptureSink*>(target);
    sink->committedFlag = true;
    sink->endedFlag = true;
    co_return;
}
ruvia::Task<void> scSleep(void*, std::chrono::milliseconds) { co_return; }
void scBind(void*, ruvia::Context*, ruvia::HttpResponse (*)(ruvia::Context&)) noexcept {}
void scReleaseContext(void* target) noexcept {
    static_cast<StreamCaptureSink*>(target)->contextReleased = true;
}
bool scCommitted(void* target) noexcept {
    return static_cast<StreamCaptureSink*>(target)->committedFlag;
}
bool scAborted(void*) noexcept { return false; }

ruvia::ResponseStreamWriter scMakeWriter(StreamCaptureSink& sink) noexcept {
    return ruvia::detail::StreamingAccess::makeResponseStreamWriter(
        &sink, &scWrite, &scEnd, &scSleep, &scBind, &scReleaseContext,
        &scCommitted, &scAborted);
}

struct EmptyStreamDispatchObservation final {
    bool handled{false};
    bool buffered{false};
    bool threw{false};
    bool ended{false};
    bool committed{false};
    bool contextReleased{false};
    std::string bufferedBody;
};

EmptyStreamDispatchObservation dispatchEmptyStreamWith(
    const ControllerMiddlewareDescriptor& middleware) {
    ruvia::Router router;
    auto& impl = ruvia::detail::RouterImpl::from(router);
    impl.registerResponseStreamRoute(
        HttpKnownMethod::kGet,
        path("/empty-stream"),
        ruvia::detail::RouteStreamHandler(nullptr, &dummyStreamHandler),
        std::span<const ControllerMiddlewareDescriptor>{},
        std::span<const ControllerMiddlewareDescriptor>(&middleware, 1));
    impl.finalize();
    const auto& table = impl.routeTable();

    ruvia::WorkerMemory worker;
    ruvia::RequestMemory memory(worker);
    ruvia::HttpRequest request = ruvia::detail::HttpRequestAccess::make();
    ruvia::detail::HttpRequestAccess::reset(request);
    ruvia::detail::HttpRequestAccess::setMethod(request, "GET");
    ruvia::detail::HttpRequestAccess::setPath(request, "/empty-stream");
    ruvia::detail::HttpRequestAccess::setResource(
        request,
        memory.resource());

    const auto resolution = table.resolve(
        HttpKnownMethod::kGet,
        "/empty-stream");
    const auto* resolved = resolution.resolved();
    if (resolved == nullptr) {
        throw std::logic_error("empty stream test route did not resolve");
    }

    StreamCaptureSink sink;
    auto writer = scMakeWriter(sink);
    EmptyStreamDispatchObservation observation;
    asio::io_context context(1);
    asio::co_spawn(
        context,
        [&]() -> asio::awaitable<void> {
            try {
                auto result = co_await ruvia::detail::taskAsAwaitable(
                    table.dispatchResponseStream(
                        request,
                        *resolved,
                        memory,
                        writer,
                        {}));
                observation.handled = !result.has_value();
                if (result.has_value()) {
                    observation.buffered = true;
                    auto response = std::move(*result);
                    const auto body =
                        ruvia::detail::responseBody(response).bytes();
                    observation.bufferedBody.assign(
                        body.data(),
                        body.size());
                }
            } catch (...) {
                observation.threw = true;
            }
        },
        asio::detached);
    context.run();
    observation.ended = sink.endedFlag;
    observation.committed = sink.committedFlag;
    observation.contextReleased = sink.contextReleased;
    return observation;
}

struct WebSocketDispatchObservation final {
    bool terminalInvoked{false};
    bool capabilityAvailableInTerminal{false};
    bool buffered{false};
    std::string bufferedBody;
};

struct WebSocketTerminalTarget final {
    WebSocketDispatchObservation* observation;
    ruvia::WebSocket* webSocket;
};

ruvia::Task<void> webSocketTerminal(
    void* target,
    ruvia::Context& context) {
    auto& terminal = *static_cast<WebSocketTerminalTarget*>(target);
    terminal.observation->terminalInvoked = true;
    ruvia::detail::ContextWebSocketBinding binding(
        context,
        *terminal.webSocket);
    terminal.observation->capabilityAvailableInTerminal =
        &context.webSocket() == terminal.webSocket;
    g_chainOrder.push_back(0);
    co_return;
}

WebSocketDispatchObservation dispatchWebSocketWith(
    const ControllerMiddlewareDescriptor& middleware) {
    ruvia::Router router;
    auto& impl = ruvia::detail::RouterImpl::from(router);
    impl.registerWebSocketRoute(
        HttpKnownMethod::kGet,
        path("/ws-middleware"),
        ruvia::detail::RouteStreamHandler(nullptr, &dummyStreamHandler),
        std::span<const ControllerMiddlewareDescriptor>{},
        std::span<const ControllerMiddlewareDescriptor>(&middleware, 1),
        {});
    impl.finalize();
    const auto& table = impl.routeTable();

    ruvia::WorkerMemory worker;
    ruvia::RequestMemory memory(worker);
    auto request = ruvia::detail::HttpRequestAccess::make();
    ruvia::detail::HttpRequestAccess::reset(request);
    ruvia::detail::HttpRequestAccess::setMethod(request, "GET");
    ruvia::detail::HttpRequestAccess::setPath(request, "/ws-middleware");
    ruvia::detail::HttpRequestAccess::setResource(request, memory.resource());
    const auto resolution = table.resolve(
        HttpKnownMethod::kGet,
        "/ws-middleware");
    const auto* resolved = resolution.resolved();
    if (resolved == nullptr) {
        throw std::logic_error("websocket middleware test route did not resolve");
    }

    WebSocketDispatchObservation observation;
    auto webSocket = ruvia::detail::WebSocketAccess::make(
        nullptr,
        nullptr,
        nullptr,
        nullptr);
    WebSocketTerminalTarget terminalTarget{&observation, &webSocket};
    const auto terminal = ruvia::detail::RouteStreamHandler(
        &terminalTarget,
        &webSocketTerminal);
    asio::io_context context(1);
    auto future = asio::co_spawn(
        context,
        ruvia::detail::taskAsAwaitable(table.dispatchWebSocket(
            request,
            *resolved,
            memory,
            terminal,
            {})),
        asio::use_future);
    context.run();
    auto response = future.get();
    if (response.has_value()) {
        observation.buffered = true;
        const auto body = ruvia::detail::responseBody(*response).bytes();
        observation.bufferedBody.assign(body.data(), body.size());
    }
    return observation;
}

// Writes a chunk (committing the stream) then throws mid-body.
ruvia::Task<void> streamCommitThenThrow(void*, ruvia::Context& context) {
    co_await context.stream().write("partial");
    throw std::runtime_error("mid-stream handler failure");
}

}  // namespace

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
    impl.registerResponseStreamRoute(
        HttpKnownMethod::kGet, path("/s"),
        ruvia::detail::RouteStreamHandler(nullptr, &streamCommitThenThrow),
        std::span<const ControllerMiddlewareDescriptor>{},
        std::span<const ControllerMiddlewareDescriptor>(mws, 1));
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
                (void)co_await ruvia::detail::taskAsAwaitable(
                    table.dispatchResponseStream(
                        request, *resolved, memory, writer, {}));
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
    const auto middleware =
        ruvia::detail::makeMiddlewareDescriptor<ChainMwA>();
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
    const auto middleware =
        ruvia::detail::makeMiddlewareDescriptor<
            ChainMwOverrideAfterNext>();
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
    const auto middleware =
        ruvia::detail::makeMiddlewareDescriptor<ChainMwStop>();
    const auto observation = dispatchWebSocketWith(middleware);
    RUVIA_CHECK(!observation.terminalInvoked);
    RUVIA_CHECK(observation.buffered);
    RUVIA_CHECK_EQ(observation.bufferedBody, std::string("stopped"));
    const std::vector<int> expected{9};
    RUVIA_CHECK(g_chainOrder == expected);
}

RUVIA_TEST(websocket_middleware_pre_upgrade_failure_stays_http_buffered) {
    const auto middleware =
        ruvia::detail::makeMiddlewareDescriptor<ChainMwThrows>();
    const auto observation = dispatchWebSocketWith(middleware);
    RUVIA_CHECK(!observation.terminalInvoked);
    RUVIA_CHECK(observation.buffered);
    RUVIA_CHECK(
        observation.bufferedBody.find("\"code\":\"mw_rejected\"") !=
        std::string::npos);
}

RUVIA_TEST(websocket_middleware_wraps_upgrade_and_session_terminal) {
    g_chainOrder.clear();
    const auto middleware =
        ruvia::detail::makeMiddlewareDescriptor<ChainMwA>();
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
    const auto middleware = ruvia::detail::makeMiddlewareDescriptor<
        ChainMwProbeWebSocketAfterNext>();
    const auto observation = dispatchWebSocketWith(middleware);
    RUVIA_CHECK(observation.terminalInvoked);
    RUVIA_CHECK(observation.capabilityAvailableInTerminal);
    RUVIA_CHECK(g_webSocketUnavailableAfterNext);
    const std::vector<int> expected{0};
    RUVIA_CHECK(g_chainOrder == expected);
}

RUVIA_TEST(websocket_middleware_post_failure_escapes_for_session_close) {
    const auto middleware = ruvia::detail::makeMiddlewareDescriptor<
        ChainMwThrowsAfterNext>();
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
    RUVIA_CHECK(body.find("\"code\":\"mw_rejected\"") != std::string::npos);
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
    const auto body = dispatchChain(
        std::span<const ControllerMiddlewareDescriptor>(controllerMws, 1),
        std::span<const ControllerMiddlewareDescriptor>(routeMws, 1));
    RUVIA_CHECK_EQ(body, std::string("ok"));
    // A(controller) pre, B(route) pre, handler, B post, A post. A regression that
    // swapped the two spans would run route middleware outside controller middleware.
    const std::vector<int> expected{1, 2, 0, -2, -1};
    RUVIA_CHECK(g_chainOrder == expected);
}

namespace {

ruvia::Task<ruvia::HttpResponse> throwsHttpErrorHandler(void*, ruvia::Context&) {
    throw ruvia::HttpError(403, "forbidden", "nope");
    co_return ruvia::HttpResponse(std::pmr::get_default_resource());  // unreachable
}

ruvia::Task<ruvia::HttpResponse> throwsGenericHandler(void*, ruvia::Context&) {
    throw std::runtime_error("boom");
    co_return ruvia::HttpResponse(std::pmr::get_default_resource());  // unreachable
}

ruvia::Task<ruvia::HttpResponse> throwsProtocolErrorHandler(void*, ruvia::Context&) {
    throw ruvia::HttpProtocolError(413, "request body is too large");
    co_return ruvia::HttpResponse(std::pmr::get_default_resource());  // unreachable
}

ruvia::Task<ruvia::HttpResponse> okHandler(void*, ruvia::Context& context) {
    co_return context.body("ok");
}

ruvia::Task<ruvia::HttpResponse> readsRequestBodyHandler(
    void*,
    ruvia::Context& context) {
    (void)co_await context.req().text();
    co_return context.body("ok");
}

// The dispatched response's storage lives in the per-request arena, which is
// destroyed when the helper returns -- so values must be copied out here, while
// the arena is still alive, rather than returning the HttpResponse itself.
struct DispatchResult final {
    std::uint16_t status{0};
    std::string body;
    std::string allow;
    std::string connection;
    std::string acceptEncoding;
};

DispatchResult extractDispatchResult(const ruvia::HttpResponse& response) {
    DispatchResult result;
    result.status = response.status();
    const auto body = ruvia::detail::responseBody(response).bytes();
    result.body.assign(body.data(), body.size());
    const auto allow = response.header("Allow").value_or(std::string_view{});
    result.allow.assign(allow.data(), allow.size());
    const auto connection = response.header("Connection").value_or(std::string_view{});
    result.connection.assign(connection.data(), connection.size());
    const auto acceptEncoding = response.header("Accept-Encoding").value_or(std::string_view{});
    result.acceptEncoding.assign(acceptEncoding.data(), acceptEncoding.size());
    return result;
}

// Registers GET /x with `handler`, dispatches the exact wire method token and
// path, then returns the rendered result.
DispatchResult dispatchOneToken(
    RouteHandler handler,
    std::string_view method,
    std::string_view p,
    std::string_view contentEncoding = {},
    std::string_view body = {}) {
    ruvia::Router router;
    auto& impl = ruvia::detail::RouterImpl::from(router);
    impl.registerRoute(HttpKnownMethod::kGet, path("/x"), handler, RequestBodyMode::kBuffered,
                       std::span<const ControllerMiddlewareDescriptor>{},
                       std::span<const ControllerMiddlewareDescriptor>{});
    impl.finalize();
    const auto& table = impl.routeTable();

    ruvia::WorkerMemory worker;
    ruvia::RequestMemory memory(worker);
    ruvia::HttpRequest request = ruvia::detail::HttpRequestAccess::make();
    ruvia::detail::HttpRequestAccess::reset(request);
    ruvia::detail::HttpRequestAccess::setMethod(request, method);
    ruvia::detail::HttpRequestAccess::setPath(request, p);
    ruvia::detail::HttpRequestAccess::setResource(request, memory.resource());
    if (!contentEncoding.empty()) {
        const auto slot = ruvia::detail::HttpRequestAccess::knownHeaderSlot(
            ruvia::detail::RequestKnownHeader::kContentEncoding);
        (void)ruvia::detail::HttpRequestAccess::addHeader(
            request,
            ruvia::HttpHeaderView{"Content-Encoding", contentEncoding},
            slot);
    }
    ruvia::detail::HttpRequestAccess::setBody(request, body);

    asio::io_context ctx(1);
    auto future = asio::co_spawn(
        ctx, ruvia::detail::taskAsAwaitable(table.dispatch(request, memory, {})),
        asio::use_future);
    ctx.run();
    return extractDispatchResult(future.get());  // arena still alive here
}

DispatchResult dispatchOne(RouteHandler handler, HttpKnownMethod method, std::string_view p) {
    return dispatchOneToken(handler, ruvia::knownHttpMethodToken(method), p);
}

}  // namespace

RUVIA_TEST(dispatch_maps_handler_exceptions_to_error_responses) {
    // Application and protocol errors retain their status; unknown failures map
    // to 500. Router never injects HTTP/1 Connection policy into any response.
    const auto application = dispatchOne(
        RouteHandler(nullptr, &throwsHttpErrorHandler), HttpKnownMethod::kGet, "/x");
    RUVIA_CHECK_EQ(application.status, std::uint16_t{403});
    RUVIA_CHECK(application.connection.empty());
    const auto protocol = dispatchOne(
        RouteHandler(nullptr, &throwsProtocolErrorHandler), HttpKnownMethod::kGet, "/x");
    RUVIA_CHECK_EQ(protocol.status, std::uint16_t{413});
    RUVIA_CHECK(protocol.connection.empty());
    const auto generic = dispatchOne(RouteHandler(nullptr, &throwsGenericHandler), HttpKnownMethod::kGet, "/x");
    RUVIA_CHECK_EQ(generic.status, std::uint16_t{500});
    RUVIA_CHECK(generic.connection.empty());
    // The unexpected exception's message must NOT leak into the response body: a
    // library error (SQL text, paths) could otherwise be disclosed to the client.
    RUVIA_CHECK(generic.body.find("boom") == std::string::npos);
    RUVIA_CHECK(generic.body.find("Internal Server Error") != std::string::npos);
}

RUVIA_TEST(dispatch_rejects_unsupported_request_content_coding_with_advertisement) {
    const auto result = dispatchOneToken(
        RouteHandler(nullptr, &readsRequestBodyHandler),
        "GET",
        "/x",
        "deflate",
        "encoded");
    RUVIA_CHECK_EQ(result.status, std::uint16_t{415});
    RUVIA_CHECK_EQ(result.acceptEncoding, std::string("gzip, br, zstd"));
    RUVIA_CHECK(
        result.body.find("unsupported_content_coding") != std::string::npos);
}

RUVIA_TEST(dispatch_produces_404_and_405_for_unmatched_routes) {
    // A path with no route -> 404.
    RUVIA_CHECK_EQ(dispatchOne(RouteHandler(nullptr, &okHandler), HttpKnownMethod::kGet, "/nope").status,
                   std::uint16_t{404});
    // The path exists but the method does not -> 405 with an Allow header listing GET.
    const auto notAllowed = dispatchOne(RouteHandler(nullptr, &okHandler), HttpKnownMethod::kPost, "/x");
    RUVIA_CHECK_EQ(notAllowed.status, std::uint16_t{405});
    RUVIA_CHECK(notAllowed.allow.find("GET") != std::string_view::npos);
    // The registered method still works.
    RUVIA_CHECK_EQ(dispatchOne(RouteHandler(nullptr, &okHandler), HttpKnownMethod::kGet, "/x").status,
                   std::uint16_t{200});
}

RUVIA_TEST(dispatch_produces_501_for_valid_unimplemented_method_token) {
    const auto extension = dispatchOneToken(
        RouteHandler(nullptr, &okHandler), "PROPFIND", "/x");
    RUVIA_CHECK_EQ(extension.status, std::uint16_t{501});
    RUVIA_CHECK(extension.allow.empty());
    RUVIA_CHECK(extension.connection.empty());

    // Method tokens are case-sensitive. A lowercase standard spelling is still
    // syntactically valid but is not the framework's GET semantic class.
    const auto lowercase = dispatchOneToken(
        RouteHandler(nullptr, &okHandler), "get", "/x");
    RUVIA_CHECK_EQ(lowercase.status, std::uint16_t{501});
}

namespace {

using ruvia::HttpErrorHandler;
using ruvia::HttpErrorInfo;
using ruvia::HttpNotFoundHandler;

ruvia::Task<ruvia::HttpResponse> customNotFound(ruvia::Context& context) {
    context.status(404);
    co_return context.body("custom-not-found");
}

ruvia::Task<ruvia::HttpResponse> customError(ruvia::Context& context, HttpErrorInfo info) {
    context.status(info.status());
    co_return context.body("custom-error");
}

DispatchResult dispatchWithHandlersToken(
    RouteHandler handler, HttpErrorHandler errorH, HttpNotFoundHandler notFoundH,
    std::string_view method, std::string_view p,
    std::string_view contentEncoding = {},
    std::string_view body = {}) {
    ruvia::Router router;
    auto& impl = ruvia::detail::RouterImpl::from(router);
    if (errorH != nullptr) {
        impl.setErrorHandler(errorH);
    }
    if (notFoundH != nullptr) {
        impl.setNotFoundHandler(notFoundH);
    }
    impl.registerRoute(HttpKnownMethod::kGet, path("/x"), handler, RequestBodyMode::kBuffered,
                       std::span<const ControllerMiddlewareDescriptor>{},
                       std::span<const ControllerMiddlewareDescriptor>{});
    impl.finalize();
    const auto& table = impl.routeTable();

    ruvia::WorkerMemory worker;
    ruvia::RequestMemory memory(worker);
    ruvia::HttpRequest request = ruvia::detail::HttpRequestAccess::make();
    ruvia::detail::HttpRequestAccess::reset(request);
    ruvia::detail::HttpRequestAccess::setMethod(request, method);
    ruvia::detail::HttpRequestAccess::setPath(request, p);
    ruvia::detail::HttpRequestAccess::setResource(request, memory.resource());
    if (!contentEncoding.empty()) {
        const auto slot = ruvia::detail::HttpRequestAccess::knownHeaderSlot(
            ruvia::detail::RequestKnownHeader::kContentEncoding);
        (void)ruvia::detail::HttpRequestAccess::addHeader(
            request,
            ruvia::HttpHeaderView{"Content-Encoding", contentEncoding},
            slot);
    }
    ruvia::detail::HttpRequestAccess::setBody(request, body);

    asio::io_context ctx(1);
    auto future = asio::co_spawn(
        ctx, ruvia::detail::taskAsAwaitable(table.dispatch(request, memory, {})),
        asio::use_future);
    ctx.run();
    return extractDispatchResult(future.get());  // arena still alive here
}

DispatchResult dispatchWithHandlers(
    RouteHandler handler,
    HttpErrorHandler errorH,
    HttpNotFoundHandler notFoundH,
    HttpKnownMethod method,
    std::string_view p) {
    return dispatchWithHandlersToken(
        handler, errorH, notFoundH, ruvia::knownHttpMethodToken(method), p);
}

}  // namespace

RUVIA_TEST(dispatch_uses_custom_not_found_handler) {
    // A registered not-found handler replaces the default 404 response body.
    const auto result = dispatchWithHandlers(
        RouteHandler(nullptr, &okHandler), nullptr, &customNotFound, HttpKnownMethod::kGet, "/nope");
    RUVIA_CHECK_EQ(result.status, std::uint16_t{404});
    RUVIA_CHECK_EQ(result.body, std::string("custom-not-found"));
}

RUVIA_TEST(dispatch_uses_custom_error_handler_with_thrown_status) {
    // A registered error handler renders a thrown HttpError, preserving its status.
    const auto result = dispatchWithHandlers(
        RouteHandler(nullptr, &throwsHttpErrorHandler), &customError, nullptr, HttpKnownMethod::kGet, "/x");
    RUVIA_CHECK_EQ(result.status, std::uint16_t{403});
    RUVIA_CHECK_EQ(result.body, std::string("custom-error"));
}

RUVIA_TEST(dispatch_preserves_content_coding_advertisement_with_custom_error_handler) {
    const auto result = dispatchWithHandlersToken(
        RouteHandler(nullptr, &readsRequestBodyHandler),
        &customError,
        nullptr,
        "GET",
        "/x",
        "compress",
        "encoded");
    RUVIA_CHECK_EQ(result.status, std::uint16_t{415});
    RUVIA_CHECK_EQ(result.body, std::string("custom-error"));
    RUVIA_CHECK_EQ(result.acceptEncoding, std::string("gzip, br, zstd"));
}

RUVIA_TEST(dispatch_routes_unimplemented_method_through_custom_error_handler) {
    const auto result = dispatchWithHandlersToken(
        RouteHandler(nullptr, &okHandler),
        &customError,
        nullptr,
        "PROPFIND",
        "/x");
    RUVIA_CHECK_EQ(result.status, std::uint16_t{501});
    RUVIA_CHECK_EQ(result.body, std::string("custom-error"));
}

RUVIA_TEST(dispatch_options_asterisk_returns_server_wide_allow) {
    // A server-wide OPTIONS * request is answered with 204 and an Allow header
    // listing every method registered anywhere on the server, not per-route.
    ruvia::Router router;
    auto& impl = ruvia::detail::RouterImpl::from(router);
    impl.registerRoute(HttpKnownMethod::kGet, path("/a"), RouteHandler(nullptr, &okHandler),
                       RequestBodyMode::kBuffered, std::span<const ControllerMiddlewareDescriptor>{},
                       std::span<const ControllerMiddlewareDescriptor>{});
    impl.registerRoute(HttpKnownMethod::kPost, path("/b"), RouteHandler(nullptr, &okHandler),
                       RequestBodyMode::kBuffered, std::span<const ControllerMiddlewareDescriptor>{},
                       std::span<const ControllerMiddlewareDescriptor>{});
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
    auto future = asio::co_spawn(
        ctx, ruvia::detail::taskAsAwaitable(table.dispatch(request, memory, {})),
        asio::use_future);
    ctx.run();
    const auto response = future.get();

    RUVIA_CHECK_EQ(response.status(), std::uint16_t{204});
    const auto allow = response.header("Allow").value_or(std::string_view{});
    RUVIA_CHECK(allow.find("GET") != std::string_view::npos);
    RUVIA_CHECK(allow.find("POST") != std::string_view::npos);
}
