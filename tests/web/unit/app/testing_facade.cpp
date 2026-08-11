#include "test_harness.h"

// The public in-memory testing facade must dispatch through the production
// pipeline: controller macros, route params, query/cookie access, model
// bodies with their 415/400 split, global middleware, prefix and app-wide
// fallbacks, urlFor, worker state, and the automatic HEAD shadow -- all
// without a socket. These tests use ONLY public headers, exactly like an
// application's own test suite would.

#include <cstddef>
#include <string>
#include <stdexcept>
#include <string_view>

#include "ruvia/http/HttpHeader.h"
#include "ruvia/http/HttpLimits.h"
#include "ruvia/web/App.h"
#include "ruvia/web/Context.h"
#include "ruvia/web/Controller.h"
#include "ruvia/web/SecurityHeaders.h"
#include "ruvia/web/Testing.h"

struct TestingFacadeEcho final {
    RUVIA_OPTIONAL_FIELD(value, ruvia::String);
    RUVIA_MODEL(TestingFacadeEcho, value);
};

namespace {

struct TestingFacadeCounter final {
    int count{0};
};

class TestingFacadeStamp final : public ruvia::Middleware<TestingFacadeStamp> {
public:
    ruvia::Task<void> handle(ruvia::Context& c, ruvia::Next& next) {
        co_await next();
        c.header("X-Test-Stamp", "on");
    }
};

// Registered with constructor arguments rather than default constructed, so it
// is deliberately not default constructible: the descriptor must carry the
// registration arguments to every instance the router builds.
class TestingFacadeConfiguredStamp final : public ruvia::Middleware<TestingFacadeConfiguredStamp> {
public:
    TestingFacadeConfiguredStamp(std::string_view name, int level) noexcept
        : name_(name),
          level_(level) {}

    ruvia::Task<void> handle(ruvia::Context& c, ruvia::Next& next) {
        co_await next();
        c.header("X-Test-Configured", name_);
        c.header("X-Test-Level", level_ == 2 ? "two" : "other");
    }

private:
    std::string_view name_;
    int level_;
};

struct TestingFacadeUser final {
    std::string_view name;
    int level{0};
};

// The pattern request-scoped bindings exist for: a middleware computes a value,
// owns it in its own coroutine frame, and publishes it to everything downstream
// of its next() for exactly that scope.
class TestingFacadeAuth final : public ruvia::Middleware<TestingFacadeAuth> {
public:
    ruvia::Task<void> handle(ruvia::Context& c, ruvia::Next& next) {
        const TestingFacadeUser user{.name = c.req().header("X-User").value_or("anonymous"), .level = 2};
        const auto binding = c.bindRequestState(user);
        co_await next();
    }
};

// Stamps a header so a response shows whether this middleware ran at all.
class TestingFacadeScoped final : public ruvia::Middleware<TestingFacadeScoped> {
public:
    explicit TestingFacadeScoped(std::string_view tag) noexcept
        : tag_(tag) {}

    ruvia::Task<void> handle(ruvia::Context& c, ruvia::Next& next) {
        co_await next();
        c.header("X-Test-Scope", tag_);
    }

private:
    std::string_view tag_;
};

// Declares itself meaningful on a request that matched no route, the way
// SecurityHeadersMiddleware does.
class TestingFacadeAlways final : public ruvia::Middleware<TestingFacadeAlways> {
public:
    static constexpr bool ruviaRunsOnUnmatchedRequests = true;

    ruvia::Task<void> handle(ruvia::Context& c, ruvia::Next& next) {
        co_await next();
        c.header("X-Test-Always", "on");
    }
};

class TestingFacadeController final : public ruvia::Controller<TestingFacadeController> {
public:
    RUVIA_CONTROLLER_GROUP("/t")
    RUVIA_ROUTES_BEGIN
    RUVIA_GET("/hello", hello);
    RUVIA_GET("/users/:id", user);
    RUVIA_GET("/greet", greet);
    RUVIA_GET("/link", link);
    RUVIA_GET("/count", count);
    RUVIA_POST("/echo", echo);
    RUVIA_GET("/boom", boom);
    RUVIA_GET("/whoami", whoami, TestingFacadeAuth);
    RUVIA_GET("/whoami-unbound", whoamiUnbound);
    RUVIA_GET("/report", report);
    RUVIA_METHOD("PROPFIND", "/files", propfind);
    RUVIA_METHOD("PURGE", "/files", purge);
    RUVIA_METHOD("PROPFIND", "/dav-only", davOnly);
    RUVIA_ROUTES_END

private:
    ruvia::Task<ruvia::HttpResponse> boom(ruvia::Context&) {
        throw std::runtime_error("boom");
        co_return ruvia::HttpResponse{};
    }

    ruvia::Task<ruvia::HttpResponse> hello(ruvia::Context& c) {
        co_return c.text("hello");
    }

    ruvia::Task<ruvia::HttpResponse> user(ruvia::Context& c) {
        co_return c.body(c.req().param("id").value_or("?"));
    }

    ruvia::Task<ruvia::HttpResponse> greet(ruvia::Context& c) {
        std::pmr::string reply(c.resource());
        reply.append(c.req().query("name").value_or("nobody"));
        reply.push_back('/');
        reply.append(c.req().cookie("sid").value_or("no-sid"));
        co_return c.body(std::move(reply));
    }

    ruvia::Task<ruvia::HttpResponse> link(ruvia::Context& c) {
        co_return c.body(c.urlFor("/t/users/:id", {"9"}));
    }

    ruvia::Task<ruvia::HttpResponse> count(ruvia::Context& c) {
        auto& counter = c.workerState<TestingFacadeCounter>();
        ++counter.count;
        std::pmr::string reply(c.resource());
        reply.append(std::to_string(counter.count));
        co_return c.body(std::move(reply));
    }

    ruvia::Task<ruvia::HttpResponse> echo(ruvia::Context& c) {
        const auto body = co_await c.req().json<TestingFacadeEcho>();
        co_return c.body(body.value().has_value() ? body.value()->view() : "missing");
    }

    ruvia::Task<ruvia::HttpResponse> propfind(ruvia::Context& c) {
        co_return c.body(c.req().method());
    }

    ruvia::Task<ruvia::HttpResponse> purge(ruvia::Context& c) {
        co_return c.body(std::string_view("purged"));
    }

    ruvia::Task<ruvia::HttpResponse> davOnly(ruvia::Context& c) {
        co_return c.body(std::string_view("dav"));
    }

    ruvia::Task<ruvia::HttpResponse> whoami(ruvia::Context& c) {
        const auto& user = c.requestState<TestingFacadeUser>();
        std::pmr::string reply(c.resource());
        reply.append(user.name);
        reply.push_back('/');
        reply.append(std::to_string(user.level));
        co_return c.body(std::move(reply));
    }

    // A body whose shape is decided at run time, built through the streaming
    // writer rather than by concatenating JSON.
    ruvia::Task<ruvia::HttpResponse> report(ruvia::Context& c) {
        const std::string_view tags[] = {"a\"quoted", "b"};
        co_return c.jsonObject([&](ruvia::JsonObjectWriter& out) {
            out.add("path", c.req().path());
            out.add("count", std::size(tags));
            auto list = out.beginArray("tags");
            for (const auto& tag : tags) {
                list.add(tag);
            }
        });
    }

    // No auth middleware on this route, so nothing is bound and the optional
    // lookup must say so rather than throw.
    ruvia::Task<ruvia::HttpResponse> whoamiUnbound(ruvia::Context& c) {
        co_return c.body(c.tryRequestState<TestingFacadeUser>() == nullptr ? std::string_view("unbound") : std::string_view("bound"));
    }
};

ruvia::Task<ruvia::HttpResponse> facadeNotFound(ruvia::Context& c) {
    c.status(ruvia::http_status::kNotFound);
    co_return c.body("custom-miss");
}

ruvia::Task<ruvia::HttpResponse> apiScopedMiss(ruvia::Context& c) {
    c.status(ruvia::http_status::kNotFound);
    co_return c.body("api-miss");
}

ruvia::Task<ruvia::HttpResponse> facadeError(ruvia::Context& c, ruvia::HttpErrorInfo error) {
    c.status(error.status());
    co_return c.body("custom-error");
}

}  // namespace

RUVIA_TEST(testing_facade_dispatches_routes_params_query_and_cookies) {
    ruvia::TestApp app;

    const auto hello = app.request(ruvia::TestRequest::get("/t/hello"));
    RUVIA_CHECK(hello.status() == ruvia::http_status::kOk);
    RUVIA_CHECK_EQ(hello.body(), std::string_view("hello"));

    const auto user = app.request(ruvia::TestRequest::get("/t/users/42"));
    RUVIA_CHECK_EQ(user.body(), std::string_view("42"));

    const auto greet = app.request(ruvia::TestRequest::get("/t/greet?name=ada").cookie("sid", "s-1"));
    RUVIA_CHECK_EQ(greet.body(), std::string_view("ada/s-1"));

    const auto absolute = app.request(ruvia::TestRequest::get("http://example.test/t/hello").header("Host", "stale.example"));
    RUVIA_CHECK(absolute.status() == ruvia::http_status::kOk);
    RUVIA_CHECK_EQ(absolute.body(), std::string_view("hello"));

    const auto serverWideOptions = app.request(ruvia::TestRequest::options("http://example.test").header("Host", "stale.example"));
    RUVIA_CHECK(serverWideOptions.status() == ruvia::http_status::kNoContent);

    const auto link = app.request(ruvia::TestRequest::get("/t/link"));
    RUVIA_CHECK_EQ(link.body(), std::string_view("/t/users/9"));

    // The automatic HEAD shadow answers with the GET status and no body.
    // Writer-synthesized framing headers (Content-Length, Date) are not part
    // of the in-memory dispatch product.
    const auto head = app.request(ruvia::TestRequest::head("/t/hello"));
    RUVIA_CHECK(head.status() == ruvia::http_status::kOk);
    RUVIA_CHECK(head.body().empty());
}

RUVIA_TEST(testing_facade_rejects_invalid_request_line_targets) {
    ruvia::TestApp app;

    RUVIA_CHECK(app.request(ruvia::TestRequest::get("/bad path")).status() == ruvia::http_status::kBadRequest);
    RUVIA_CHECK(app.request(ruvia::TestRequest::get("*")).status() == ruvia::http_status::kBadRequest);
    RUVIA_CHECK(app.request(ruvia::TestRequest::get("/bad%zz")).status() == ruvia::http_status::kBadRequest);
    RUVIA_CHECK(app.request(ruvia::TestRequest::method("BAD(METHOD", "/")).status() == ruvia::http_status::kBadRequest);
}

RUVIA_TEST(testing_facade_rejects_invalid_request_headers) {
    ruvia::TestApp app;

    RUVIA_CHECK(app.request(ruvia::TestRequest::get("/t/hello").header("Bad Header", "x")).status() == ruvia::http_status::kBadRequest);
    RUVIA_CHECK(app.request(ruvia::TestRequest::get("/t/hello").header("X-Bad", std::string_view("a\rb", 3))).status() == ruvia::http_status::kBadRequest);
    RUVIA_CHECK(app.request(ruvia::TestRequest::get("/t/hello").header("Host", "example.test").header("Host", "other.test")).status() == ruvia::http_status::kBadRequest);
    RUVIA_CHECK(app.request(ruvia::TestRequest::get("/t/hello").header("Content-Type", "not a media type")).status() == ruvia::http_status::kBadRequest);
    RUVIA_CHECK(app.request(ruvia::TestRequest::get("/t/hello").header("Origin", "https://APP.example")).status() == ruvia::http_status::kBadRequest);
    RUVIA_CHECK(app.request(ruvia::TestRequest::options("/t/hello").header("Access-Control-Request-Method", "GET, POST")).status() == ruvia::http_status::kBadRequest);
    RUVIA_CHECK(app.request(ruvia::TestRequest::get("/t/hello").header("Content-Encoding", "gzip;level=9")).status() == ruvia::http_status::kBadRequest);
    RUVIA_CHECK(app.request(ruvia::TestRequest::post("/t/echo").header("Content-Length", "5").header("Content-Length", "6")).status() == ruvia::http_status::kBadRequest);
    RUVIA_CHECK(app.request(ruvia::TestRequest::post("/t/echo").header("Transfer-Encoding", "gzip")).status() == ruvia::http_status::kBadRequest);
    RUVIA_CHECK(app.request(ruvia::TestRequest::post("/t/echo").header("Transfer-Encoding", "bogus")).status() == ruvia::http_status::kNotImplemented);
    RUVIA_CHECK(app.request(ruvia::TestRequest::post("/t/echo").header("Transfer-Encoding", "chunked").header("Content-Length", "0")).status() == ruvia::http_status::kBadRequest);
    RUVIA_CHECK(app.request(ruvia::TestRequest::method("TRACE", "/t/hello").header("Content-Length", "0")).status() == ruvia::http_status::kBadRequest);
    RUVIA_CHECK(app.request(ruvia::TestRequest::options("/t/hello").header("Content-Length", "0")).status() == ruvia::http_status::kBadRequest);

    auto tooMany = ruvia::TestRequest::get("/t/hello");
    for (std::size_t i = 0; i <= ruvia::kMaxHttpHeaderFields; ++i) {
        tooMany.header("X-Test", "v");
    }
    RUVIA_CHECK(app.request(tooMany).status() == ruvia::http_status::kRequestHeaderFieldsTooLarge);

    const std::string oversizedHeaderValue(ruvia::kMaxHttpHeaderBytes, 'x');
    RUVIA_CHECK(app.request(ruvia::TestRequest::get("/t/hello").header("X-Big", oversizedHeaderValue)).status() == ruvia::http_status::kRequestHeaderFieldsTooLarge);
}

RUVIA_TEST(testing_facade_runs_model_bodies_with_media_type_split) {
    ruvia::TestApp app;

    const auto ok = app.request(ruvia::TestRequest::post("/t/echo").json(R"({"value":"hi"})"));
    RUVIA_CHECK(ok.status() == ruvia::http_status::kOk);
    RUVIA_CHECK_EQ(ok.body(), std::string_view("hi"));

    const auto wrongType = app.request(ruvia::TestRequest::post("/t/echo").body(R"({"value":"hi"})", "text/plain"));
    RUVIA_CHECK(wrongType.status() == ruvia::http_status::kUnsupportedMediaType);

    const auto badBody = app.request(ruvia::TestRequest::post("/t/echo").json("{not-json"));
    RUVIA_CHECK(badBody.status() == ruvia::http_status::kBadRequest);
}

RUVIA_TEST(testing_facade_applies_app_level_configuration) {
    ruvia::TestApp app;
    app.use<TestingFacadeStamp>().onNotFound(&facadeNotFound).onNotFound("/api", &apiScopedMiss);
    app.useWorkerState<TestingFacadeCounter>();

    // Global middleware wraps every matched route.
    const auto stamped = app.request(ruvia::TestRequest::get("/t/hello"));
    RUVIA_CHECK_EQ(stamped.header("X-Test-Stamp").value_or(""), std::string_view("on"));

    // Worker state persists across requests on the same TestApp.
    const auto firstCount = app.request(ruvia::TestRequest::get("/t/count"));
    RUVIA_CHECK_EQ(firstCount.body(), std::string_view("1"));
    const auto secondCount = app.request(ruvia::TestRequest::get("/t/count"));
    RUVIA_CHECK_EQ(secondCount.body(), std::string_view("2"));

    // Prefix fallback wins under its scope; the app-wide one covers the rest.
    const auto scopedMiss = app.request(ruvia::TestRequest::get("/api/missing"));
    RUVIA_CHECK_EQ(scopedMiss.body(), std::string_view("api-miss"));
    const auto globalMiss = app.request(ruvia::TestRequest::get("/nope"));
    RUVIA_CHECK_EQ(globalMiss.body(), std::string_view("custom-miss"));

    // 405 keeps flowing through the production fallback path too.
    const auto notAllowed = app.request(ruvia::TestRequest::del("/t/hello"));
    RUVIA_CHECK(notAllowed.status() == ruvia::http_status::kMethodNotAllowed);

    // The route table is sealed after the first request.
    bool sealed = false;
    try {
        app.use<TestingFacadeStamp>();
    } catch (const std::logic_error&) {
        sealed = true;
    }
    RUVIA_CHECK(sealed);
}

RUVIA_TEST(testing_facade_constructs_middleware_from_registration_arguments) {
    ruvia::TestApp app;
    app.use<TestingFacadeConfiguredStamp>("audit", 2);

    const auto first = app.request(ruvia::TestRequest::get("/t/hello"));
    RUVIA_CHECK_EQ(first.header("X-Test-Configured").value_or(""), std::string_view("audit"));
    RUVIA_CHECK_EQ(first.header("X-Test-Level").value_or(""), std::string_view("two"));

    // One instance serves every request, so the arguments must still be readable
    // after the first dispatch rather than having been consumed by it.
    const auto second = app.request(ruvia::TestRequest::get("/t/hello"));
    RUVIA_CHECK_EQ(second.header("X-Test-Configured").value_or(""), std::string_view("audit"));

    // Separate registrations of the same type stay independent.
    ruvia::TestApp other;
    other.use<TestingFacadeConfiguredStamp>("other", 5);
    const auto distinct = other.request(ruvia::TestRequest::get("/t/hello"));
    RUVIA_CHECK_EQ(distinct.header("X-Test-Configured").value_or(""), std::string_view("other"));
    RUVIA_CHECK_EQ(distinct.header("X-Test-Level").value_or(""), std::string_view("other"));
}

RUVIA_TEST(testing_facade_runs_fallback_handlers_that_carry_state) {
    // A fallback handler used to be a plain function pointer, so anything it
    // needed had to be a global. It now accepts any callable, including one that
    // captures the collaborators the handler depends on.
    struct Branding final {
        std::string label;
    };
    const Branding branding{"tenant-a"};

    ruvia::TestApp app;
    app.onNotFound([branding](ruvia::Context& c) -> ruvia::Task<ruvia::HttpResponse> {
        c.status(ruvia::http_status::kNotFound);
        co_return c.text(std::string_view(branding.label));
    });
    app.onError([branding](ruvia::Context& c, ruvia::HttpErrorInfo error) -> ruvia::Task<ruvia::HttpResponse> {
        c.status(error.status());
        std::pmr::string body(c.resource());
        body.append(branding.label);
        body.append(":error");
        co_return c.text(std::move(body));
    });

    const auto missed = app.request(ruvia::TestRequest::get("/nowhere"));
    RUVIA_CHECK(missed.status() == ruvia::http_status::kNotFound);
    RUVIA_CHECK_EQ(missed.body(), std::string_view("tenant-a"));

    // The captured state is still readable on a later request: the callable was
    // copied at registration, not borrowed from the caller's frame.
    const auto missedAgain = app.request(ruvia::TestRequest::get("/nowhere/else"));
    RUVIA_CHECK_EQ(missedAgain.body(), std::string_view("tenant-a"));

    const auto failed = app.request(ruvia::TestRequest::get("/t/boom"));
    RUVIA_CHECK(failed.status() == ruvia::http_status::kInternalServerError);
    RUVIA_CHECK_EQ(failed.body(), std::string_view("tenant-a:error"));
}

RUVIA_TEST(testing_facade_rejects_duplicate_normalized_fallback_prefixes) {
    ruvia::TestApp notFoundApp;
    notFoundApp.onNotFound("/api", &apiScopedMiss);

    bool notFoundRejected = false;
    try {
        notFoundApp.onNotFound("/api///", &apiScopedMiss);
    } catch (const std::invalid_argument& error) {
        notFoundRejected = std::string_view(error.what()) == "duplicate fallback prefix";
    }
    RUVIA_CHECK(notFoundRejected);

    ruvia::TestApp errorApp;
    errorApp.onError("/api/", &facadeError);

    bool errorRejected = false;
    try {
        errorApp.onError("/api", &facadeError);
    } catch (const std::invalid_argument& error) {
        errorRejected = std::string_view(error.what()) == "duplicate fallback prefix";
    }
    RUVIA_CHECK(errorRejected);

    bool malformedRejected = false;
    try {
        errorApp.onNotFound("api", &apiScopedMiss);
    } catch (const std::invalid_argument& error) {
        malformedRejected = std::string_view(error.what()) == "fallback prefix must start with '/'";
    }
    RUVIA_CHECK(malformedRejected);

    const auto rejectsMalformedPrefixedScope = [](std::string_view prefix) {
        ruvia::TestApp app;
        try {
            app.onNotFound(prefix, &apiScopedMiss);
        } catch (const std::invalid_argument& error) {
            return std::string_view(error.what()) == "fallback prefix must be an origin-form path without query";
        }
        return false;
    };
    RUVIA_CHECK(rejectsMalformedPrefixedScope("/api?debug=1"));
    RUVIA_CHECK(rejectsMalformedPrefixedScope("/bad path"));
    RUVIA_CHECK(rejectsMalformedPrefixedScope("/api#fragment"));
}

RUVIA_TEST(testing_facade_isolates_instances) {
    // Two TestApps own separate controller instances and worker state.
    ruvia::TestApp first;
    first.useWorkerState<TestingFacadeCounter>();
    ruvia::TestApp second;
    second.useWorkerState<TestingFacadeCounter>();

    const auto firstCount = first.request(ruvia::TestRequest::get("/t/count"));
    RUVIA_CHECK_EQ(firstCount.body(), std::string_view("1"));
    const auto secondCount = second.request(ruvia::TestRequest::get("/t/count"));
    RUVIA_CHECK_EQ(secondCount.body(), std::string_view("1"));
}

RUVIA_TEST(testing_facade_middleware_publishes_request_state_to_handler) {
    ruvia::TestApp app;

    const auto named = app.request(ruvia::TestRequest::get("/t/whoami").header("X-User", "ada"));
    RUVIA_CHECK_EQ(named.status(), ruvia::http_status::kOk);
    RUVIA_CHECK_EQ(named.body(), std::string_view("ada/2"));

    // A second request gets its own binding, not the previous one's value --
    // this is request scope, not worker scope.
    const auto anonymous = app.request(ruvia::TestRequest::get("/t/whoami"));
    RUVIA_CHECK_EQ(anonymous.body(), std::string_view("anonymous/2"));
}

RUVIA_TEST(testing_facade_request_state_is_absent_without_its_middleware) {
    ruvia::TestApp app;
    const auto response = app.request(ruvia::TestRequest::get("/t/whoami-unbound").header("X-User", "ada"));
    RUVIA_CHECK_EQ(response.status(), ruvia::http_status::kOk);
    RUVIA_CHECK_EQ(response.body(), std::string_view("unbound"));
}

RUVIA_TEST(testing_facade_builds_a_runtime_shaped_json_body) {
    ruvia::TestApp app;
    const auto response = app.request(ruvia::TestRequest::get("/t/report"));
    RUVIA_CHECK_EQ(response.status(), ruvia::http_status::kOk);
    RUVIA_CHECK_EQ(response.body(), std::string_view(R"({"path":"/t/report","count":2,"tags":["a\"quoted","b"]})"));
    const auto contentType = response.header("Content-Type");
    RUVIA_CHECK(contentType.has_value());
    RUVIA_CHECK_EQ(*contentType, std::string_view("application/json"));
}

RUVIA_TEST(testing_facade_path_scoped_middleware_runs_only_under_its_prefix) {
    ruvia::TestApp app;
    app.useAt<TestingFacadeScoped>("/t/users", "users");

    // Under the scope, on both the exact prefix path shape and a deeper one.
    const auto scoped = app.request(ruvia::TestRequest::get("/t/users/42"));
    RUVIA_CHECK_EQ(scoped.status(), ruvia::http_status::kOk);
    const auto scopedHeader = scoped.header("X-Test-Scope");
    RUVIA_CHECK(scopedHeader.has_value());
    RUVIA_CHECK_EQ(*scopedHeader, std::string_view("users"));

    // A sibling route outside the scope never receives the frame.
    const auto outside = app.request(ruvia::TestRequest::get("/t/hello"));
    RUVIA_CHECK_EQ(outside.status(), ruvia::http_status::kOk);
    RUVIA_CHECK(!outside.header("X-Test-Scope").has_value());
}

RUVIA_TEST(testing_facade_path_scope_matches_whole_segments_only) {
    ruvia::TestApp app;
    // "/t/user" must not scope "/t/users/:id" -- that is a different segment,
    // not a deeper path.
    app.useAt<TestingFacadeScoped>("/t/user", "prefix-only");

    const auto response = app.request(ruvia::TestRequest::get("/t/users/42"));
    RUVIA_CHECK_EQ(response.status(), ruvia::http_status::kOk);
    RUVIA_CHECK(!response.header("X-Test-Scope").has_value());
}

RUVIA_TEST(testing_facade_path_scope_normalizes_a_trailing_slash) {
    ruvia::TestApp app;
    app.useAt<TestingFacadeScoped>("/t/users/", "trailing");

    const auto response = app.request(ruvia::TestRequest::get("/t/users/42"));
    const auto header = response.header("X-Test-Scope");
    RUVIA_CHECK(header.has_value());
    RUVIA_CHECK_EQ(*header, std::string_view("trailing"));
}

RUVIA_TEST(testing_facade_app_wide_middleware_still_runs_everywhere) {
    ruvia::TestApp app;
    app.use<TestingFacadeScoped>("global");

    // Bound to names: TestResponse owns its headers, so header() is rightly
    // rvalue-deleted and a view into a temporary is not obtainable.
    const auto hello = app.request(ruvia::TestRequest::get("/t/hello"));
    RUVIA_CHECK(hello.header("X-Test-Scope").has_value());
    const auto user = app.request(ruvia::TestRequest::get("/t/users/42"));
    RUVIA_CHECK(user.header("X-Test-Scope").has_value());
}

RUVIA_TEST(testing_facade_unmatched_middleware_wraps_the_404_terminal) {
    ruvia::TestApp app;
    app.use<TestingFacadeAlways>();
    app.use<TestingFacadeStamp>();

    // A matched route runs both, as before.
    const auto matched = app.request(ruvia::TestRequest::get("/t/hello"));
    RUVIA_CHECK_EQ(matched.status(), ruvia::http_status::kOk);
    RUVIA_CHECK(matched.header("X-Test-Always").has_value());
    RUVIA_CHECK(matched.header("X-Test-Stamp").has_value());

    // An unmatched one runs only what declared itself for unmatched requests.
    const auto missing = app.request(ruvia::TestRequest::get("/nope"));
    RUVIA_CHECK_EQ(missing.status(), ruvia::http_status::kNotFound);
    const auto always = missing.header("X-Test-Always");
    RUVIA_CHECK(always.has_value());
    RUVIA_CHECK_EQ(*always, std::string_view("on"));
    RUVIA_CHECK(!missing.header("X-Test-Stamp").has_value());
}

RUVIA_TEST(testing_facade_unmatched_middleware_also_wraps_405_and_501) {
    ruvia::TestApp app;
    app.use<TestingFacadeAlways>();

    // Known method, wrong verb for an existing path.
    const auto wrongMethod = app.request(ruvia::TestRequest::post("/t/hello"));
    RUVIA_CHECK_EQ(wrongMethod.status(), ruvia::http_status::kMethodNotAllowed);
    RUVIA_CHECK(wrongMethod.header("X-Test-Always").has_value());
    // The Allow header the fallback sets must survive the chain.
    RUVIA_CHECK(wrongMethod.header("Allow").has_value());

    // A token no route registered: the server does not know it, so 501.
    const auto unknownMethod = app.request(ruvia::TestRequest::method("FROBNICATE", "/t/hello"));
    RUVIA_CHECK_EQ(unknownMethod.status(), ruvia::http_status::kNotImplemented);
    RUVIA_CHECK(unknownMethod.header("X-Test-Always").has_value());
}

RUVIA_TEST(testing_facade_unmatched_middleware_runs_with_a_custom_not_found_handler) {
    ruvia::TestApp app;
    app.use<TestingFacadeAlways>();
    app.onNotFound(&facadeNotFound);

    const auto missing = app.request(ruvia::TestRequest::get("/nope"));
    RUVIA_CHECK_EQ(missing.status(), ruvia::http_status::kNotFound);
    // The application's own fallback body still wins; the chain only wraps it.
    RUVIA_CHECK_EQ(missing.body(), std::string_view("custom-miss"));
    RUVIA_CHECK(missing.header("X-Test-Always").has_value());
}

RUVIA_TEST(testing_facade_without_unmatched_middleware_the_404_path_is_unchanged) {
    ruvia::TestApp app;
    app.use<TestingFacadeStamp>();

    const auto missing = app.request(ruvia::TestRequest::get("/nope"));
    RUVIA_CHECK_EQ(missing.status(), ruvia::http_status::kNotFound);
    RUVIA_CHECK(!missing.header("X-Test-Stamp").has_value());
}

RUVIA_TEST(testing_facade_security_headers_reach_unmatched_requests) {
    // The concrete gap this exists to close: a 404 is an attacker-reachable URL
    // and needs the same content policy a matched route gets.
    ruvia::TestApp app;
    app.use<ruvia::SecurityHeadersMiddleware>();

    const auto matched = app.request(ruvia::TestRequest::get("/t/hello"));
    RUVIA_CHECK(matched.header("Content-Security-Policy").has_value());

    const auto missing = app.request(ruvia::TestRequest::get("/nope"));
    RUVIA_CHECK_EQ(missing.status(), ruvia::http_status::kNotFound);
    const auto policy = missing.header("Content-Security-Policy");
    RUVIA_CHECK(policy.has_value());
    RUVIA_CHECK_EQ(*policy, std::string_view("default-src 'self'"));
    RUVIA_CHECK(missing.header("X-Content-Type-Options").has_value());
    RUVIA_CHECK(missing.header("X-Frame-Options").has_value());
}

RUVIA_TEST(testing_facade_routes_an_extension_method_by_its_exact_token) {
    ruvia::TestApp app;

    const auto propfind = app.request(ruvia::TestRequest::method("PROPFIND", "/t/files"));
    RUVIA_CHECK_EQ(propfind.status(), ruvia::http_status::kOk);
    // The handler sees the exact wire token, not a classification.
    RUVIA_CHECK_EQ(propfind.body(), std::string_view("PROPFIND"));

    const auto purge = app.request(ruvia::TestRequest::method("PURGE", "/t/files"));
    RUVIA_CHECK_EQ(purge.status(), ruvia::http_status::kOk);
    RUVIA_CHECK_EQ(purge.body(), std::string_view("purged"));
}

RUVIA_TEST(testing_facade_extension_method_tokens_are_case_sensitive) {
    ruvia::TestApp app;
    // RFC 9110 9.1: the method token is case-sensitive, so "propfind" is a
    // different method from "PROPFIND" and no route registered it. 405 is
    // reserved for a method the origin server knows (15.5.6), so an
    // unregistered token is 501 no matter what the target path supports.
    const auto response = app.request(ruvia::TestRequest::method("propfind", "/t/files"));
    RUVIA_CHECK_EQ(response.status(), ruvia::http_status::kNotImplemented);
}

RUVIA_TEST(testing_facade_extension_method_splits_404_from_501) {
    ruvia::TestApp app;

    // PROPFIND is registered somewhere, so the server knows it. A path with no
    // route at all is then an ordinary 404 -- the method is not the problem.
    const auto missingPath = app.request(ruvia::TestRequest::method("PROPFIND", "/t/nothing-here"));
    RUVIA_CHECK_EQ(missingPath.status(), ruvia::http_status::kNotFound);

    // A token no route registered is 501 even on a path that exists.
    const auto unknownMethod = app.request(ruvia::TestRequest::method("FROBNICATE", "/t/files"));
    RUVIA_CHECK_EQ(unknownMethod.status(), ruvia::http_status::kNotImplemented);
}

RUVIA_TEST(testing_facade_extension_method_on_a_known_path_is_405_not_501) {
    ruvia::TestApp app;
    // The resource exists under GET, so the method is the problem, not the URI.
    const auto response = app.request(ruvia::TestRequest::method("PROPFIND", "/t/hello"));
    RUVIA_CHECK_EQ(response.status(), ruvia::http_status::kMethodNotAllowed);
    const auto allow = response.header("Allow");
    RUVIA_CHECK(allow.has_value());
    RUVIA_CHECK(allow.has_value() && allow->find("GET") != std::string_view::npos);
}

RUVIA_TEST(testing_facade_allow_header_names_extension_methods) {
    ruvia::TestApp app;

    // A known method the path does not support: Allow must still name the
    // extension methods it does, or it is not the full supported set.
    const auto known = app.request(ruvia::TestRequest::post("/t/files"));
    RUVIA_CHECK_EQ(known.status(), ruvia::http_status::kMethodNotAllowed);
    const auto knownAllow = known.header("Allow");
    RUVIA_CHECK(knownAllow.has_value());
    RUVIA_CHECK(knownAllow.has_value() && knownAllow->find("PROPFIND") != std::string_view::npos);
    RUVIA_CHECK(knownAllow.has_value() && knownAllow->find("PURGE") != std::string_view::npos);

    // A path whose ONLY methods are extension ones must still answer 405 with a
    // populated Allow rather than 404 or an empty header.
    const auto davOnly = app.request(ruvia::TestRequest::method("PURGE", "/t/dav-only"));
    RUVIA_CHECK_EQ(davOnly.status(), ruvia::http_status::kMethodNotAllowed);
    const auto davAllow = davOnly.header("Allow");
    RUVIA_CHECK(davAllow.has_value());
    RUVIA_CHECK_EQ(davAllow.value_or(std::string_view{}), std::string_view("PROPFIND"));
}

RUVIA_TEST(testing_facade_options_reports_extension_methods_too) {
    ruvia::TestApp app;
    const auto response = app.request(ruvia::TestRequest::options("/t/files"));
    RUVIA_CHECK_EQ(response.status(), ruvia::http_status::kNoContent);
    const auto allow = response.header("Allow");
    RUVIA_CHECK(allow.has_value());
    RUVIA_CHECK(allow.has_value() && allow->find("PROPFIND") != std::string_view::npos);
}
