#include "test_harness.h"

// The public in-memory testing facade must dispatch through the production
// pipeline: controller macros, route params, query/cookie access, model
// bodies with their 415/400 split, global middleware, prefix and app-wide
// fallbacks, urlFor, worker state, and the automatic HEAD shadow -- all
// without a socket. These tests use ONLY public headers, exactly like an
// application's own test suite would.

#include <string>
#include <stdexcept>
#include <string_view>

#include "ruvia/web/App.h"
#include "ruvia/web/Context.h"
#include "ruvia/web/Controller.h"
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

    const auto link = app.request(ruvia::TestRequest::get("/t/link"));
    RUVIA_CHECK_EQ(link.body(), std::string_view("/t/users/9"));

    // The automatic HEAD shadow answers with the GET status and no body.
    // Writer-synthesized framing headers (Content-Length, Date) are not part
    // of the in-memory dispatch product.
    const auto head = app.request(ruvia::TestRequest::head("/t/hello"));
    RUVIA_CHECK(head.status() == ruvia::http_status::kOk);
    RUVIA_CHECK(head.body().empty());
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
