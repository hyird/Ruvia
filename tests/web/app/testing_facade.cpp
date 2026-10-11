#include "test_harness.h"

// The public in-memory testing facade must dispatch through the production
// pipeline: controller macros, route params, query/cookie access, model
// bodies with their 415/400 split, global middleware, prefix and app-wide
// fallbacks, url_for, worker state, and the automatic HEAD fallback -- all
// without a socket. These tests use ONLY public headers, exactly like an
// application's own test suite would.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <stdexcept>
#include <string>
#include <string_view>

#include "ruvia/core/timer.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_limits.h"
#include "ruvia/web/app.h"
#include "ruvia/web/context.h"
#include "ruvia/web/controller.h"
#include "ruvia/web/deadline.h"
#include "ruvia/web/security_headers.h"
#include "ruvia/web/session.h"
#include "ruvia/web/testing.h"

RUVIA_MODEL(testing_facade_echo, RUVIA_OPTIONAL_FIELD(value, ruvia::string));

RUVIA_MODEL(testing_facade_report, RUVIA_REQUIRED_FIELD(path, ruvia::string),
    RUVIA_REQUIRED_FIELD(count, ruvia::uint64),
    RUVIA_REQUIRED_FIELD(tags, ruvia::array<ruvia::string>));

namespace {

struct testing_facade_counter final {
    int count_{0};
};

struct testing_facade_startup_failure_state final {};

class testing_facade_startup_error final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class testing_facade_throwing_middleware final
    : public ruvia::middleware {
public:
    explicit testing_facade_throwing_middleware(int* attempts) {
        ++*attempts;
        throw testing_facade_startup_error("testing facade middleware startup failed");
    }

    ruvia::task<void> handle(ruvia::context&, ruvia::next&) {
        co_return;
    }
};

class testing_facade_stamp final : public ruvia::middleware {
public:
    ruvia::task<void> handle(ruvia::context& c, ruvia::next& next_value) {
        co_await next_value();
        c.header("X-Test-Stamp", "on");
        c.header("X-Middleware-Method", c.req().method());
    }
};

// Registered with constructor arguments rather than default constructed, so it
// is deliberately not default constructible: the descriptor must carry the
// registration arguments to every instance the router builds.
class testing_facade_configured_stamp final : public ruvia::middleware {
public:
    testing_facade_configured_stamp(std::string_view name, int level) noexcept
        : name_(name),
          level_(level) {}

    ruvia::task<void> handle(ruvia::context& c, ruvia::next& next_value) {
        co_await next_value();
        c.header("X-Test-Configured", name_);
        c.header("X-Test-Level", level_ == 2 ? "two" : "other");
    }

private:
    std::string_view name_;
    int level_;
};

struct testing_facade_user final {
    std::string_view name_;
    int level_{0};
};

// The pattern request-scoped bindings exist for: a middleware computes a value,
// owns it in its own coroutine frame, and publishes it to everything downstream
// of its next() for exactly that scope.
class testing_facade_auth final : public ruvia::middleware {
public:
    ruvia::task<void> handle(ruvia::context& c, ruvia::next& next_value) {
        const testing_facade_user user_value{
            .name_ = c.req().header("X-User").value_or("anonymous"), .level_ = 2};
        const auto binding = c.bind_request_state(user_value);
        co_await next_value();
    }
};

// Stamps a header so a response shows whether this middleware ran at all.
class testing_facade_scoped final : public ruvia::middleware {
public:
    explicit testing_facade_scoped(std::string_view tag) noexcept
        : tag_(tag) {}

    ruvia::task<void> handle(ruvia::context& c, ruvia::next& next_value) {
        co_await next_value();
        c.header("X-Test-Scope", tag_);
    }

private:
    std::string_view tag_;
};

// Declares itself meaningful on a request that matched no route, the way
// security_headers_middleware does.
class testing_facade_always final : public ruvia::middleware {
public:
    static constexpr bool ruvia_runs_on_unmatched_requests = true;

    ruvia::task<void> handle(ruvia::context& c, ruvia::next& next_value) {
        co_await next_value();
        c.header("X-Test-Always", "on");
    }
};

int all_path_evaluations = 0;
int on_path_evaluations = 0;

std::string_view counted_all_path() {
    ++all_path_evaluations;
    return "/counted-all";
}

std::string_view counted_on_path() {
    ++on_path_evaluations;
    return "/counted-on";
}

class testing_facade_controller final : public ruvia::controller<testing_facade_controller> {
public:
    RUVIA_CONTROLLER_GROUP("/t")
    RUVIA_ROUTES_BEGIN
    RUVIA_GET("/hello", hello);
    RUVIA_GET("/a/:id/", slash_test);
    RUVIA_ALL(counted_all_path(), hello);
    RUVIA_ON((::ruvia::http_known_method::put, ::ruvia::http_known_method::delete_value),
        (counted_on_path()), hello);
    RUVIA_GET("/users/:id", user);
    RUVIA_GET("/empty//:id", user);
    RUVIA_GET("/slash-link", slash_link);
    RUVIA_GET("/literal/", hello);
    RUVIA_GET("/tail/:id//", user);
    RUVIA_GET("/trailing-links", trailing_links);
    RUVIA_GET("/head/:id", head_metadata, testing_facade_stamp);
    RUVIA_GET("/greet", greet);
    RUVIA_GET("/link", link);
    RUVIA_GET("/count", count);
    RUVIA_POST("/echo", echo, ruvia::json_body<testing_facade_echo>);
    RUVIA_GET("/boom", boom);
    RUVIA_GET("/whoami", whoami, testing_facade_auth);
    RUVIA_GET("/whoami-unbound", whoami_unbound);
    RUVIA_GET("/report", report);
    RUVIA_GET("/large", large);
    RUVIA_GET("/worker", worker);
    RUVIA_GET("/deadline", deadline, ruvia::deadline<20>);
    RUVIA_METHOD("PROPFIND", "/files", propfind);
    RUVIA_METHOD("PURGE", "/files", purge);
    RUVIA_METHOD("PROPFIND", "/dav-only", dav_only);
#ifdef RUVIA_ENABLE_REDIS
    RUVIA_GET("/session-clear", clear_session);
#endif
    RUVIA_ROUTES_END
    ruvia::task<ruvia::http_response> slash_test(ruvia::context& c) {
        co_return c.text("ok");
    }

private:
    ruvia::task<ruvia::http_response> boom(ruvia::context&) {
        throw std::runtime_error("boom");
        co_return ruvia::http_response{};
    }

    ruvia::task<ruvia::http_response> hello(ruvia::context& c) {
        co_return c.text("hello");
    }

    ruvia::task<ruvia::http_response> user(ruvia::context& c) {
        co_return c.body(c.req().param("id").value_or("?"));
    }

    ruvia::task<ruvia::http_response> head_metadata(ruvia::context& c) {
        c.header("X-Handler-Method", c.req().method());
        c.header("X-Route-Param", c.req().param("id").value_or("?"));
        co_return c.text("payload");
    }

    ruvia::task<ruvia::http_response> greet(ruvia::context& c) {
        std::pmr::string reply(c.arena());
        reply.append(c.req().query("name").value_or("nobody"));
        reply.push_back('/');
        reply.append(c.req().cookie("sid").value_or("no-sid"));
        co_return c.body(std::move(reply));
    }

    ruvia::task<ruvia::http_response> link(ruvia::context& c) {
        co_return c.body(c.url_for("/t/users/:id", {"9"}));
    }

    ruvia::task<ruvia::http_response> count(ruvia::context& c) {
        auto& counter = c.worker_state<testing_facade_counter>();
        ++counter.count_;
        std::pmr::string reply(c.arena());
        reply.append(std::to_string(counter.count_));
        co_return c.body(std::move(reply));
    }

    ruvia::task<ruvia::http_response> echo(ruvia::context& c) {
        const auto& body = c.req().validated<testing_facade_echo>();
        const auto value = body.get<"value">().has_value() ? body.get<"value">()->view()
                                                           : std::string_view("missing");
        if (const auto independent = c.req().header("X-Test-Independent")) {
            std::pmr::string reply(c.arena());
            reply.append(*independent);
            reply.push_back('/');
            reply.append(value);
            co_return c.body(std::move(reply));
        }
        co_return c.body(value);
    }

    ruvia::task<ruvia::http_response> propfind(ruvia::context& c) {
        co_return c.body(c.req().method());
    }

    ruvia::task<ruvia::http_response> purge(ruvia::context& c) {
        co_return c.body(std::string_view("purged"));
    }

    ruvia::task<ruvia::http_response> dav_only(ruvia::context& c) {
        co_return c.body(std::string_view("dav"));
    }

#ifdef RUVIA_ENABLE_REDIS
    ruvia::task<ruvia::http_response> clear_session(ruvia::context& c) {
        c.session().clear();
        co_return c.body("cleared");
    }
#endif

    ruvia::task<ruvia::http_response> whoami(ruvia::context& c) {
        const auto& user_value = c.request_state<testing_facade_user>();
        std::pmr::string reply(c.arena());
        reply.append(user_value.name_);
        reply.push_back('/');
        reply.append(std::to_string(user_value.level_));
        co_return c.body(std::move(reply));
    }

    // Runtime-sized collections stay inside a statically declared response
    // schema; JSON output never bypasses the model boundary.
    ruvia::task<ruvia::http_response> report(ruvia::context& c) {
        const std::string_view tags[] = {"a\"quoted", "b"};
        testing_facade_report report({.resource_ = c.arena()});
        report.set<"path">(c.req().path());
        report.set<"count">(static_cast<std::uint64_t>(std::size(tags)));
        auto& report_tags = report.ensure<"tags">();
        report_tags.reserve(std::size(tags));
        for (const auto tag : tags) {
            report_tags.emplace_back(tag, ruvia::model_options{.resource_ = c.arena()});
        }
        co_return c.json(report);
    }

    ruvia::task<ruvia::http_response> large(ruvia::context& c) {
        std::pmr::string body(c.arena());
        const auto fill = c.req().query("fill").value_or("x");
        body.assign(1024 * 1024, fill.empty() ? 'x' : fill.front());
        co_return c.body(std::move(body));
    }

    ruvia::task<ruvia::http_response> worker(ruvia::context& c) {
        std::pmr::string state_value(c.arena());
        state_value.append(c.worker().valid() ? "valid" : "invalid");
        state_value.push_back('/');
        state_value.append(c.worker().is_current() ? "current" : "foreign");
        co_return c.body(std::move(state_value));
    }

    ruvia::task<ruvia::http_response> deadline(ruvia::context& c) {
        const auto result_value =
            co_await ruvia::sleep_for(c.worker(), std::chrono::hours(1), c.get_stop_token());
        co_return c.body(result_value == ruvia::timer_sleep_result::stop_requested && c.deadline_exceeded()
                             ? std::string_view("deadline")
                             : std::string_view("missed"));
    }

    // No auth middleware on this route, so nothing is bound and the optional
    // lookup must say so rather than throw.
    ruvia::task<ruvia::http_response> whoami_unbound(ruvia::context& c) {
        co_return c.body(c.try_request_state<testing_facade_user>() == nullptr
                             ? std::string_view("unbound")
                             : std::string_view("bound"));
    }

    ruvia::task<ruvia::http_response> slash_link(ruvia::context& c) {
        co_return c.body(c.url_for("/t/empty//:id", {"456"}));
    }

    ruvia::task<ruvia::http_response> trailing_links(ruvia::context& c) {
        std::pmr::string reply(c.arena());
        reply.append(c.url_for("/t/literal/"));
        reply.push_back(' ');
        reply.append(c.url_for("/t/tail/:id//", {"789"}));
        co_return c.body(std::move(reply));
    }
};

ruvia::task<ruvia::http_response> facade_not_found(ruvia::context& c) {
    c.status(ruvia::http_status::not_found);
    co_return c.body("custom-miss");
}

ruvia::task<ruvia::http_response> api_scoped_miss(ruvia::context& c) {
    c.status(ruvia::http_status::not_found);
    co_return c.body("api-miss");
}

ruvia::task<ruvia::http_response> facade_error(ruvia::context& c, ruvia::http_error_info error) {
    c.status(error.status());
    co_return c.body("custom-error");
}

}  // namespace

RUVIA_TEST(testing_facade_multi_route_paths_are_evaluated_once_per_registration) {
    const auto all_before = all_path_evaluations;
    const auto on_before = on_path_evaluations;
    ruvia::test_app app;
    const auto all_response = app.request(ruvia::test_request::get("/t/counted-all"));
    RUVIA_CHECK_EQ(all_response.body(), std::string_view("hello"));
    const auto on_response = app.request(ruvia::test_request::put("/t/counted-on"));
    RUVIA_CHECK_EQ(on_response.body(), std::string_view("hello"));
    RUVIA_CHECK(all_path_evaluations > all_before);
    RUVIA_CHECK_EQ(all_path_evaluations - all_before, on_path_evaluations - on_before);
}

RUVIA_TEST(testing_facade_dispatches_routes_params_query_and_cookies) {
    ruvia::test_app app;

    const auto hello = app.request(ruvia::test_request::get("/t/hello"));
    RUVIA_CHECK(hello.status() == ruvia::http_status::ok);
    RUVIA_CHECK_EQ(hello.body(), std::string_view("hello"));

    const auto user_value = app.request(ruvia::test_request::get("/t/users/42"));
    RUVIA_CHECK_EQ(user_value.body(), std::string_view("42"));

    const auto greet =
        app.request(ruvia::test_request::get("/t/greet?name=ada").cookie("sid", "s-1"));
    RUVIA_CHECK_EQ(greet.body(), std::string_view("ada/s-1"));

    const auto absolute = app.request(
        ruvia::test_request::get("http://example.test/t/hello").header("Host", "stale.example"));
    RUVIA_CHECK(absolute.status() == ruvia::http_status::ok);
    RUVIA_CHECK_EQ(absolute.body(), std::string_view("hello"));

    const auto server_wide_options = app.request(
        ruvia::test_request::options("http://example.test").header("Host", "stale.example"));
    RUVIA_CHECK(server_wide_options.status() == ruvia::http_status::no_content);

    const auto link = app.request(ruvia::test_request::get("/t/link"));
    RUVIA_CHECK_EQ(link.body(), std::string_view("/t/users/9"));

    const auto worker_value = app.request(ruvia::test_request::get("/t/worker"));
    RUVIA_CHECK_EQ(worker_value.body(), std::string_view("valid/current"));

    const auto deadline_value = app.request(ruvia::test_request::get("/t/deadline"));
    RUVIA_CHECK_EQ(deadline_value.body(), std::string_view("deadline"));

    // The automatic HEAD fallback answers with the GET status and no body.
    // Writer-synthesized framing headers (Content-Length, Date) are not part
    // of the in-memory dispatch product.
    const auto head = app.request(ruvia::test_request::head("/t/hello"));
    RUVIA_CHECK(head.status() == ruvia::http_status::ok);
    RUVIA_CHECK(head.body().empty());
}

RUVIA_TEST(testing_facade_head_fallback_preserves_request_method_and_route_params) {
    ruvia::test_app app;
    const auto get = app.request(ruvia::test_request::get("/t/head/42"));
    RUVIA_CHECK(get.status() == ruvia::http_status::ok);
    RUVIA_CHECK_EQ(get.body(), std::string_view("payload"));
    RUVIA_CHECK_EQ(get.header("X-Handler-Method").value_or(""), std::string_view("GET"));
    RUVIA_CHECK_EQ(get.header("X-Middleware-Method").value_or(""), std::string_view("GET"));

    const auto head = app.request(ruvia::test_request::head("/t/head/42"));
    RUVIA_CHECK(head.status() == ruvia::http_status::ok);
    RUVIA_CHECK(head.body().empty());
    RUVIA_CHECK_EQ(head.header("X-Handler-Method").value_or(""), std::string_view("HEAD"));
    RUVIA_CHECK_EQ(head.header("X-Middleware-Method").value_or(""), std::string_view("HEAD"));
    RUVIA_CHECK_EQ(head.header("X-Route-Param").value_or(""), std::string_view("42"));
    RUVIA_CHECK_EQ(head.header("X-Test-Stamp").value_or(""), std::string_view("on"));
}

RUVIA_TEST(testing_facade_rejects_invalid_request_line_targets) {
    ruvia::test_app app;

    RUVIA_CHECK(app.request(ruvia::test_request::get("/bad path")).status() ==
                ruvia::http_status::bad_request);
    RUVIA_CHECK(
        app.request(ruvia::test_request::get("*")).status() == ruvia::http_status::bad_request);
    RUVIA_CHECK(app.request(ruvia::test_request::get("/bad%zz")).status() ==
                ruvia::http_status::bad_request);
    RUVIA_CHECK(app.request(ruvia::test_request::method("BAD(METHOD", "/")).status() ==
                ruvia::http_status::bad_request);
}

RUVIA_TEST(testing_facade_rejects_invalid_request_headers) {
    ruvia::test_app app;

    RUVIA_CHECK(
        app.request(ruvia::test_request::get("/t/hello").header("Bad Header", "x")).status() ==
        ruvia::http_status::bad_request);
    RUVIA_CHECK(
        app.request(
               ruvia::test_request::get("/t/hello").header("X-Bad", std::string_view("a\rb", 3)))
            .status() == ruvia::http_status::bad_request);
    const auto independent = app.request(ruvia::test_request::post("/t/echo")
            .json(R"({"value":"body"})")
            .header("Content-Length", "5")
            .header("Content-Length", "6")
            .header("Transfer-Encoding", "chunked")
            .header("X-Test-Independent", "header"));
    RUVIA_CHECK(independent.status() == ruvia::http_status::ok);
    RUVIA_CHECK_EQ(independent.body(), std::string_view("header/body"));

    auto too_many = ruvia::test_request::get("/t/hello");
    for (std::size_t i = 0; i <= ruvia::max_http_header_fields; ++i) {
        too_many.header("X-Test", "v");
    }
    RUVIA_CHECK(app.request(too_many).status() == ruvia::http_status::request_header_fields_too_large);

    const std::string oversized_header_value(ruvia::max_http_header_bytes, 'x');
    RUVIA_CHECK(
        app.request(ruvia::test_request::get("/t/hello").header("X-Big", oversized_header_value))
            .status() == ruvia::http_status::request_header_fields_too_large);
}

RUVIA_TEST(testing_facade_runs_model_bodies_with_media_type_split) {
    ruvia::test_app app;

    const auto ok = app.request(ruvia::test_request::post("/t/echo").json(R"({"value":"hi"})"));
    RUVIA_CHECK(ok.status() == ruvia::http_status::ok);
    RUVIA_CHECK_EQ(ok.body(), std::string_view("hi"));

    const auto wrong_type =
        app.request(ruvia::test_request::post("/t/echo").body(R"({"value":"hi"})", "text/plain"));
    RUVIA_CHECK(wrong_type.status() == ruvia::http_status::unsupported_media_type);

    const auto bad_body = app.request(ruvia::test_request::post("/t/echo").json("{not-json"));
    RUVIA_CHECK(bad_body.status() == ruvia::http_status::bad_request);
}

RUVIA_TEST(testing_facade_applies_app_level_configuration) {
    ruvia::test_app app;
    app.use<testing_facade_stamp>()
        .on_not_found(&facade_not_found)
        .on_not_found({.prefix_ = "/api", .handler_ = &api_scoped_miss});
    app.use_worker_state<testing_facade_counter>();

    // Global middleware wraps every matched route.
    const auto stamped = app.request(ruvia::test_request::get("/t/hello"));
    RUVIA_CHECK_EQ(stamped.header("X-Test-Stamp").value_or(""), std::string_view("on"));

    // Worker state persists across requests on the same test_app.
    const auto first_count = app.request(ruvia::test_request::get("/t/count"));
    RUVIA_CHECK_EQ(first_count.body(), std::string_view("1"));
    const auto second_count = app.request(ruvia::test_request::get("/t/count"));
    RUVIA_CHECK_EQ(second_count.body(), std::string_view("2"));

    // Prefix fallback wins under its scope; the app-wide one covers the rest.
    const auto scoped_miss = app.request(ruvia::test_request::get("/api/missing"));
    RUVIA_CHECK_EQ(scoped_miss.body(), std::string_view("api-miss"));
    const auto global_miss = app.request(ruvia::test_request::get("/nope"));
    RUVIA_CHECK_EQ(global_miss.body(), std::string_view("custom-miss"));

    // 405 keeps flowing through the production fallback path too.
    const auto not_allowed = app.request(ruvia::test_request::del("/t/hello"));
    RUVIA_CHECK(not_allowed.status() == ruvia::http_status::method_not_allowed);

    // The route table is sealed after the first request.
    bool sealed = false;
    try {
        app.use<testing_facade_stamp>();
    } catch (const std::logic_error&) {
        sealed = true;
    }
    RUVIA_CHECK(sealed);
}

RUVIA_TEST(testing_facade_retains_startup_failure_without_retrying) {
    ruvia::test_app app;
    int factory_calls = 0;
    app.use_worker_state<testing_facade_startup_failure_state>([&]()
                                                                   -> testing_facade_startup_failure_state {
        if (++factory_calls == 1) {
            throw std::runtime_error("testing facade startup failed");
        }
        return {};
    });

    for (int attempt_value = 0; attempt_value != 2; ++attempt_value) {
        bool threw = false;
        try {
            (void)app.request(ruvia::test_request::get("/t/hello"));
        } catch (const std::runtime_error& error) {
            threw = true;
            RUVIA_CHECK_EQ(std::string_view(error.what()),
                std::string_view("testing facade startup failed"));
        }
        RUVIA_CHECK(threw);
    }
    RUVIA_CHECK_EQ(factory_calls, 1);

    bool sealed = false;
    try {
        app.use<testing_facade_stamp>();
    } catch (const std::logic_error&) {
        sealed = true;
    }
    RUVIA_CHECK(sealed);
}

RUVIA_TEST(testing_facade_retains_pre_worker_startup_failure_without_retrying) {
    ruvia::test_app app;
    int constructor_calls = 0;
    app.use<testing_facade_throwing_middleware>(&constructor_calls);

    for (int attempt_value = 0; attempt_value != 2; ++attempt_value) {
        bool threw = false;
        try {
            (void)app.request(ruvia::test_request::get("/t/hello"));
        } catch (const testing_facade_startup_error& error) {
            threw = true;
            RUVIA_CHECK_EQ(std::string_view(error.what()),
                std::string_view("testing facade middleware startup failed"));
        }
        RUVIA_CHECK(threw);
    }
    RUVIA_CHECK_EQ(constructor_calls, 1);

    bool sealed = false;
    try {
        app.use<testing_facade_stamp>();
    } catch (const std::logic_error&) {
        sealed = true;
    }
    RUVIA_CHECK(sealed);
}

RUVIA_TEST(testing_facade_constructs_middleware_from_registration_arguments) {
    ruvia::test_app app;
    app.use<testing_facade_configured_stamp>("audit", 2);

    const auto first = app.request(ruvia::test_request::get("/t/hello"));
    RUVIA_CHECK_EQ(first.header("X-Test-Configured").value_or(""), std::string_view("audit"));
    RUVIA_CHECK_EQ(first.header("X-Test-Level").value_or(""), std::string_view("two"));

    // One instance serves every request, so the arguments must still be readable
    // after the first dispatch rather than having been consumed by it.
    const auto second = app.request(ruvia::test_request::get("/t/hello"));
    RUVIA_CHECK_EQ(second.header("X-Test-Configured").value_or(""), std::string_view("audit"));

    // Separate registrations of the same type stay independent.
    ruvia::test_app other;
    other.use<testing_facade_configured_stamp>("other", 5);
    const auto distinct = other.request(ruvia::test_request::get("/t/hello"));
    RUVIA_CHECK_EQ(distinct.header("X-Test-Configured").value_or(""), std::string_view("other"));
    RUVIA_CHECK_EQ(distinct.header("X-Test-Level").value_or(""), std::string_view("other"));
}

RUVIA_TEST(testing_facade_runs_fallback_handlers_that_carry_state) {
    // A fallback handler used to be a plain function pointer, so anything it
    // needed had to be a global. It now accepts any callable, including one that
    // captures the collaborators the handler depends on.
    struct branding final {
        std::string label_;
    };
    const branding branding_value{"tenant-a"};

    ruvia::test_app app;
    app.on_not_found([branding_value](ruvia::context& c) -> ruvia::task<ruvia::http_response> {
        c.status(ruvia::http_status::not_found);
        co_return c.text(std::string_view(branding_value.label_));
    });
    app.on_error([branding_value](ruvia::context& c,
                     ruvia::http_error_info error) -> ruvia::task<ruvia::http_response> {
        c.status(error.status());
        std::pmr::string body(c.arena());
        body.append(branding_value.label_);
        body.append(":error");
        co_return c.text(std::move(body));
    });

    const auto missed = app.request(ruvia::test_request::get("/nowhere"));
    RUVIA_CHECK(missed.status() == ruvia::http_status::not_found);
    RUVIA_CHECK_EQ(missed.body(), std::string_view("tenant-a"));

    // The captured state is still readable on a later request: the callable was
    // copied at registration, not borrowed from the caller's frame.
    const auto missed_again = app.request(ruvia::test_request::get("/nowhere/else"));
    RUVIA_CHECK_EQ(missed_again.body(), std::string_view("tenant-a"));

    const auto failed = app.request(ruvia::test_request::get("/t/boom"));
    RUVIA_CHECK(failed.status() == ruvia::http_status::internal_server_error);
    RUVIA_CHECK_EQ(failed.body(), std::string_view("tenant-a:error"));
}

RUVIA_TEST(testing_facade_rejects_duplicate_normalized_fallback_prefixes) {
    ruvia::test_app not_found_app;
    not_found_app.on_not_found({.prefix_ = "/api", .handler_ = &api_scoped_miss});

    bool not_found_rejected = false;
    try {
        not_found_app.on_not_found({.prefix_ = "/api///", .handler_ = &api_scoped_miss});
    } catch (const std::invalid_argument& error) {
        not_found_rejected = std::string_view(error.what()) == "duplicate fallback prefix";
    }
    RUVIA_CHECK(not_found_rejected);

    ruvia::test_app error_app;
    error_app.on_error({.prefix_ = "/api/", .handler_ = &facade_error});

    bool error_rejected = false;
    try {
        error_app.on_error({.prefix_ = "/api", .handler_ = &facade_error});
    } catch (const std::invalid_argument& error) {
        error_rejected = std::string_view(error.what()) == "duplicate fallback prefix";
    }
    RUVIA_CHECK(error_rejected);

    bool malformed_rejected = false;
    try {
        error_app.on_not_found({.prefix_ = "api", .handler_ = &api_scoped_miss});
    } catch (const std::invalid_argument& error) {
        malformed_rejected = std::string_view(error.what()) == "fallback prefix must start with '/'";
    }
    RUVIA_CHECK(malformed_rejected);

    const auto rejects_malformed_prefixed_scope = [](std::string_view prefix) {
        ruvia::test_app app;
        try {
            app.on_not_found({.prefix_ = std::string(prefix), .handler_ = &api_scoped_miss});
        } catch (const std::invalid_argument& error) {
            return std::string_view(error.what()) ==
                   "fallback prefix must be an origin-form path without query";
        }
        return false;
    };
    RUVIA_CHECK(rejects_malformed_prefixed_scope("/api?debug=1"));
    RUVIA_CHECK(rejects_malformed_prefixed_scope("/bad path"));
    RUVIA_CHECK(rejects_malformed_prefixed_scope("/api#fragment"));
}

RUVIA_TEST(testing_facade_isolates_instances) {
    // Two TestApps own separate controller instances and worker state.
    ruvia::test_app first;
    first.use_worker_state<testing_facade_counter>();
    ruvia::test_app second;
    second.use_worker_state<testing_facade_counter>();

    const auto first_count = first.request(ruvia::test_request::get("/t/count"));
    RUVIA_CHECK_EQ(first_count.body(), std::string_view("1"));
    const auto second_count = second.request(ruvia::test_request::get("/t/count"));
    RUVIA_CHECK_EQ(second_count.body(), std::string_view("1"));
}

RUVIA_TEST(testing_facade_middleware_publishes_request_state_to_handler) {
    ruvia::test_app app;

    const auto named = app.request(ruvia::test_request::get("/t/whoami").header("X-User", "ada"));
    RUVIA_CHECK_EQ(named.status(), ruvia::http_status::ok);
    RUVIA_CHECK_EQ(named.body(), std::string_view("ada/2"));

    // A second request gets its own binding, not the previous one's value --
    // this is request scope, not worker scope.
    const auto anonymous = app.request(ruvia::test_request::get("/t/whoami"));
    RUVIA_CHECK_EQ(anonymous.body(), std::string_view("anonymous/2"));
}

RUVIA_TEST(testing_facade_request_state_is_absent_without_its_middleware) {
    ruvia::test_app app;
    const auto response =
        app.request(ruvia::test_request::get("/t/whoami-unbound").header("X-User", "ada"));
    RUVIA_CHECK_EQ(response.status(), ruvia::http_status::ok);
    RUVIA_CHECK_EQ(response.body(), std::string_view("unbound"));
}

RUVIA_TEST(testing_facade_builds_a_runtime_sized_response_model) {
    ruvia::test_app app;
    const auto response = app.request(ruvia::test_request::get("/t/report"));
    RUVIA_CHECK_EQ(response.status(), ruvia::http_status::ok);
    RUVIA_CHECK_EQ(response.body(),
        std::string_view(R"({"path":"/t/report","count":2,"tags":["a\"quoted","b"]})"));
    const auto content_type_value = response.header("Content-Type");
    RUVIA_CHECK(content_type_value.has_value());
    RUVIA_CHECK_EQ(*content_type_value, std::string_view("application/json"));
}

RUVIA_TEST(testing_facade_response_owns_large_bodies_across_requests) {
    ruvia::test_app app;
    const auto first = app.request(ruvia::test_request::get("/t/large?fill=a"));
    RUVIA_CHECK_EQ(first.body().size(), std::size_t(1024 * 1024));
    RUVIA_CHECK_EQ(first.body(), std::string(1024 * 1024, 'a'));

    const auto second = app.request(ruvia::test_request::get("/t/large?fill=b"));
    RUVIA_CHECK_EQ(second.body().size(), std::size_t(1024 * 1024));
    RUVIA_CHECK_EQ(first.body().size(), std::size_t(1024 * 1024));
    RUVIA_CHECK_EQ(second.body(), std::string(1024 * 1024, 'b'));
    RUVIA_CHECK_EQ(first.body(), std::string(1024 * 1024, 'a'));
}

RUVIA_TEST(testing_facade_path_scoped_middleware_runs_only_under_its_prefix) {
    ruvia::test_app app;
    app.use_at<testing_facade_scoped>({.prefix_ = "/t/users"}, "users");

    // Under the scope, on both the exact prefix path shape and a deeper one.
    const auto scoped = app.request(ruvia::test_request::get("/t/users/42"));
    RUVIA_CHECK_EQ(scoped.status(), ruvia::http_status::ok);
    const auto scoped_header = scoped.header("X-Test-Scope");
    RUVIA_CHECK(scoped_header.has_value());
    RUVIA_CHECK_EQ(*scoped_header, std::string_view("users"));

    // A sibling route outside the scope never receives the frame.
    const auto outside = app.request(ruvia::test_request::get("/t/hello"));
    RUVIA_CHECK_EQ(outside.status(), ruvia::http_status::ok);
    RUVIA_CHECK(!outside.header("X-Test-Scope").has_value());
}

RUVIA_TEST(testing_facade_path_scope_matches_whole_segments_only) {
    ruvia::test_app app;
    // "/t/user" must not scope "/t/users/:id" -- that is a different segment,
    // not a deeper path.
    app.use_at<testing_facade_scoped>({.prefix_ = "/t/user"}, "prefix-only");

    const auto response = app.request(ruvia::test_request::get("/t/users/42"));
    RUVIA_CHECK_EQ(response.status(), ruvia::http_status::ok);
    RUVIA_CHECK(!response.header("X-Test-Scope").has_value());
}

RUVIA_TEST(testing_facade_path_scope_normalizes_a_trailing_slash) {
    ruvia::test_app app;
    app.use_at<testing_facade_scoped>({.prefix_ = "/t/users/"}, "trailing");

    const auto response = app.request(ruvia::test_request::get("/t/users/42"));
    const auto header_value = response.header("X-Test-Scope");
    RUVIA_CHECK(header_value.has_value());
    RUVIA_CHECK_EQ(*header_value, std::string_view("trailing"));
}

RUVIA_TEST(testing_facade_app_wide_middleware_still_runs_everywhere) {
    ruvia::test_app app;
    app.use<testing_facade_scoped>("global");

    // Bound to names: test_response owns its headers, so header() is rightly
    // rvalue-deleted and a view into a temporary is not obtainable.
    const auto hello = app.request(ruvia::test_request::get("/t/hello"));
    RUVIA_CHECK(hello.header("X-Test-Scope").has_value());
    const auto user_value = app.request(ruvia::test_request::get("/t/users/42"));
    RUVIA_CHECK(user_value.header("X-Test-Scope").has_value());
}

RUVIA_TEST(testing_facade_unmatched_middleware_wraps_the_404_terminal) {
    ruvia::test_app app;
    app.use<testing_facade_always>();
    app.use<testing_facade_stamp>();

    // A matched route runs both, as before.
    const auto matched = app.request(ruvia::test_request::get("/t/hello"));
    RUVIA_CHECK_EQ(matched.status(), ruvia::http_status::ok);
    RUVIA_CHECK(matched.header("X-Test-Always").has_value());
    RUVIA_CHECK(matched.header("X-Test-Stamp").has_value());

    // An unmatched one runs only what declared itself for unmatched requests.
    const auto missing = app.request(ruvia::test_request::get("/nope"));
    RUVIA_CHECK_EQ(missing.status(), ruvia::http_status::not_found);
    const auto always = missing.header("X-Test-Always");
    RUVIA_CHECK(always.has_value());
    RUVIA_CHECK_EQ(*always, std::string_view("on"));
    RUVIA_CHECK(!missing.header("X-Test-Stamp").has_value());
}

RUVIA_TEST(testing_facade_unmatched_middleware_also_wraps_405_and_501) {
    ruvia::test_app app;
    app.use<testing_facade_always>();

    // Known method, wrong verb for an existing path.
    const auto wrong_method = app.request(ruvia::test_request::post("/t/hello"));
    RUVIA_CHECK_EQ(wrong_method.status(), ruvia::http_status::method_not_allowed);
    RUVIA_CHECK(wrong_method.header("X-Test-Always").has_value());
    // The Allow header the fallback sets must survive the chain.
    RUVIA_CHECK(wrong_method.header("Allow").has_value());

    // A token no route registered: the server does not know it, so 501.
    const auto unknown_method = app.request(ruvia::test_request::method("FROBNICATE", "/t/hello"));
    RUVIA_CHECK_EQ(unknown_method.status(), ruvia::http_status::not_implemented);
    RUVIA_CHECK(unknown_method.header("X-Test-Always").has_value());
}

RUVIA_TEST(testing_facade_unmatched_middleware_runs_with_a_custom_not_found_handler) {
    ruvia::test_app app;
    app.use<testing_facade_always>();
    app.on_not_found(&facade_not_found);

    const auto missing = app.request(ruvia::test_request::get("/nope"));
    RUVIA_CHECK_EQ(missing.status(), ruvia::http_status::not_found);
    // The application's own fallback body still wins; the chain only wraps it.
    RUVIA_CHECK_EQ(missing.body(), std::string_view("custom-miss"));
    RUVIA_CHECK(missing.header("X-Test-Always").has_value());
}

RUVIA_TEST(testing_facade_without_unmatched_middleware_the_404_path_is_unchanged) {
    ruvia::test_app app;
    app.use<testing_facade_stamp>();

    const auto missing = app.request(ruvia::test_request::get("/nope"));
    RUVIA_CHECK_EQ(missing.status(), ruvia::http_status::not_found);
    RUVIA_CHECK(!missing.header("X-Test-Stamp").has_value());
}

RUVIA_TEST(testing_facade_security_headers_reach_unmatched_requests) {
    // The concrete gap this exists to close: a 404 is an attacker-reachable URL
    // and needs the same content policy a matched route gets.
    ruvia::test_app app;
    app.use<ruvia::security_headers_middleware>();

    const auto matched = app.request(ruvia::test_request::get("/t/hello"));
    RUVIA_CHECK(matched.header("Content-Security-Policy").has_value());

    const auto missing = app.request(ruvia::test_request::get("/nope"));
    RUVIA_CHECK_EQ(missing.status(), ruvia::http_status::not_found);
    const auto policy = missing.header("Content-Security-Policy");
    RUVIA_CHECK(policy.has_value());
    RUVIA_CHECK_EQ(*policy, std::string_view("default-src 'self'"));
    RUVIA_CHECK(missing.header("X-Content-Type-Options").has_value());
    RUVIA_CHECK(missing.header("X-Frame-Options").has_value());
}

RUVIA_TEST(testing_facade_security_headers_own_configured_policy) {
    ruvia::test_app app;
    {
        ruvia::security_headers_config config;
        config.frame_options_header_ = ruvia::default_security_header_policy::omit;
        config.xss_protection_header_ = ruvia::xss_protection_header_policy::omit;
        config.content_security_policy_ = "default-src 'none'";
        config.custom_headers_ = {{"X-Custom-Security", std::string(80, 'v')}};
        app.use<ruvia::security_headers_middleware>(config);
    }

    const auto response = app.request(ruvia::test_request::get("/t/hello"));
    RUVIA_CHECK_EQ(
        response.header("Content-Security-Policy"), std::string_view("default-src 'none'"));
    RUVIA_CHECK(!response.header("X-Frame-Options").has_value());
    RUVIA_CHECK(!response.header("X-XSS-Protection").has_value());
    const std::string expected_custom_value(80, 'v');
    RUVIA_CHECK_EQ(response.header("X-Custom-Security"), std::string_view(expected_custom_value));
}

#ifdef RUVIA_ENABLE_REDIS
RUVIA_TEST(testing_facade_session_middleware_owns_configured_cookie_policy) {
    ruvia::test_app app;
    {
        ruvia::session_config config;
        config.redis_alias_ = std::string(80, 'r');
        config.cookie_name_ = "ruvia_session";
        config.key_prefix_ = std::string(80, 'k');
        config.ttl_ = std::chrono::minutes(15);
        app.use<ruvia::session_middleware>(config);
    }

    const auto response = app.request(ruvia::test_request::get("/t/session-clear"));
    RUVIA_CHECK_EQ(response.status(), ruvia::http_status::ok);
    const auto cookie = response.header("Set-Cookie");
    RUVIA_CHECK(cookie.has_value());
    RUVIA_CHECK(cookie->starts_with("ruvia_session=;"));
}
#endif

RUVIA_TEST(testing_facade_routes_an_extension_method_by_its_exact_token) {
    ruvia::test_app app;

    const auto propfind = app.request(ruvia::test_request::method("PROPFIND", "/t/files"));
    RUVIA_CHECK_EQ(propfind.status(), ruvia::http_status::ok);
    // The handler sees the exact wire token, not a classification.
    RUVIA_CHECK_EQ(propfind.body(), std::string_view("PROPFIND"));

    const auto purge = app.request(ruvia::test_request::method("PURGE", "/t/files"));
    RUVIA_CHECK_EQ(purge.status(), ruvia::http_status::ok);
    RUVIA_CHECK_EQ(purge.body(), std::string_view("purged"));
}

RUVIA_TEST(testing_facade_extension_method_tokens_are_case_sensitive) {
    ruvia::test_app app;
    // RFC 9110 9.1: the method token is case-sensitive, so "propfind" is a
    // different method from "PROPFIND" and no route registered it. 405 is
    // reserved for a method the origin server knows (15.5.6), so an
    // unregistered token is 501 no matter what the target path supports.
    const auto response = app.request(ruvia::test_request::method("propfind", "/t/files"));
    RUVIA_CHECK_EQ(response.status(), ruvia::http_status::not_implemented);
}

RUVIA_TEST(testing_facade_extension_method_splits_404_from_501) {
    ruvia::test_app app;

    // PROPFIND is registered somewhere, so the server knows it. A path with no
    // route at all is then an ordinary 404 -- the method is not the problem.
    const auto missing_path = app.request(ruvia::test_request::method("PROPFIND", "/t/nothing-here"));
    RUVIA_CHECK_EQ(missing_path.status(), ruvia::http_status::not_found);

    // A token no route registered is 501 even on a path that exists.
    const auto unknown_method = app.request(ruvia::test_request::method("FROBNICATE", "/t/files"));
    RUVIA_CHECK_EQ(unknown_method.status(), ruvia::http_status::not_implemented);
}

RUVIA_TEST(testing_facade_extension_method_on_a_known_path_is_405_not_501) {
    ruvia::test_app app;
    // The resource exists under GET, so the method is the problem, not the URI.
    const auto response = app.request(ruvia::test_request::method("PROPFIND", "/t/hello"));
    RUVIA_CHECK_EQ(response.status(), ruvia::http_status::method_not_allowed);
    const auto allow = response.header("Allow");
    RUVIA_CHECK(allow.has_value());
    RUVIA_CHECK(allow.has_value() && allow->find("GET") != std::string_view::npos);
}

RUVIA_TEST(testing_facade_allow_header_names_extension_methods) {
    ruvia::test_app app;

    // A known method the path does not support: Allow must still name the
    // extension methods it does, or it is not the full supported set.
    const auto known = app.request(ruvia::test_request::post("/t/files"));
    RUVIA_CHECK_EQ(known.status(), ruvia::http_status::method_not_allowed);
    const auto known_allow = known.header("Allow");
    RUVIA_CHECK(known_allow.has_value());
    RUVIA_CHECK(known_allow.has_value() && known_allow->find("PROPFIND") != std::string_view::npos);
    RUVIA_CHECK(known_allow.has_value() && known_allow->find("PURGE") != std::string_view::npos);

    // A path whose ONLY methods are extension ones must still answer 405 with a
    // populated Allow rather than 404 or an empty header.
    const auto dav_only = app.request(ruvia::test_request::method("PURGE", "/t/dav-only"));
    RUVIA_CHECK_EQ(dav_only.status(), ruvia::http_status::method_not_allowed);
    const auto dav_allow = dav_only.header("Allow");
    RUVIA_CHECK(dav_allow.has_value());
    RUVIA_CHECK(dav_allow.has_value() && dav_allow->find("OPTIONS") != std::string_view::npos);
    RUVIA_CHECK(dav_allow.has_value() && dav_allow->find("PROPFIND") != std::string_view::npos);
}

RUVIA_TEST(testing_facade_options_reports_extension_methods_too) {
    ruvia::test_app app;
    const auto response = app.request(ruvia::test_request::options("/t/files"));
    RUVIA_CHECK_EQ(response.status(), ruvia::http_status::no_content);
    const auto allow = response.header("Allow");
    RUVIA_CHECK(allow.has_value());
    RUVIA_CHECK(allow.has_value() && allow->find("PROPFIND") != std::string_view::npos);
}

RUVIA_TEST(testing_facade_options_reports_extension_only_resource_methods_too) {
    ruvia::test_app app;
    const auto response = app.request(ruvia::test_request::options("/t/dav-only"));
    RUVIA_CHECK_EQ(response.status(), ruvia::http_status::no_content);
    const auto allow = response.header("Allow");
    RUVIA_CHECK(allow.has_value());
    RUVIA_CHECK(allow.has_value() && allow->find("OPTIONS") != std::string_view::npos);
    RUVIA_CHECK(allow.has_value() && allow->find("PROPFIND") != std::string_view::npos);
}

RUVIA_TEST(testing_facade_router_slash) {
    ruvia::test_app app;
    auto res1 = app.request(ruvia::test_request::get("/t/a/123/"));
    RUVIA_CHECK_EQ(res1.status(), ruvia::http_status::ok);

    auto res2 = app.request(ruvia::test_request::get("/t/a/123"));
    RUVIA_CHECK_EQ(res2.status(), ruvia::http_status::ok);
}

RUVIA_TEST(testing_facade_dynamic_paths_preserve_empty_segments) {
    ruvia::test_app app;

    const auto repeated = app.request(ruvia::test_request::get("/t/empty//42"));
    RUVIA_CHECK_EQ(repeated.status(), ruvia::http_status::ok);
    RUVIA_CHECK_EQ(repeated.body(), "42");

    const auto missing_empty = app.request(ruvia::test_request::get("/t/empty/42"));
    RUVIA_CHECK_EQ(missing_empty.status(), ruvia::http_status::not_found);

    const auto extra_empty = app.request(ruvia::test_request::get("/t/empty///42"));
    RUVIA_CHECK_EQ(extra_empty.status(), ruvia::http_status::not_found);

    const auto empty_param = app.request(ruvia::test_request::get("/t/users//"));
    RUVIA_CHECK_EQ(empty_param.status(), ruvia::http_status::not_found);
}

RUVIA_TEST(testing_facade_reverse_routes_preserve_empty_segments) {
    ruvia::test_app app;

    const auto link = app.request(ruvia::test_request::get("/t/slash-link"));
    RUVIA_CHECK_EQ(link.status(), ruvia::http_status::ok);
    RUVIA_CHECK_EQ(link.body(), "/t/empty//456");

    const auto response = app.request(ruvia::test_request::get(link.body()));
    RUVIA_CHECK_EQ(response.status(), ruvia::http_status::ok);
    RUVIA_CHECK_EQ(response.body(), "456");
}

RUVIA_TEST(testing_facade_reverse_routes_preserve_trailing_segments) {
    ruvia::test_app app;

    const auto links = app.request(ruvia::test_request::get("/t/trailing-links"));
    RUVIA_CHECK_EQ(links.status(), ruvia::http_status::ok);
    RUVIA_CHECK_EQ(links.body(), "/t/literal/ /t/tail/789//");

    const auto separator = links.body().find(' ');
    const auto literal = app.request(ruvia::test_request::get(links.body().substr(0, separator)));
    RUVIA_CHECK_EQ(literal.status(), ruvia::http_status::ok);
    RUVIA_CHECK_EQ(literal.body(), "hello");

    const auto tail = app.request(ruvia::test_request::get(links.body().substr(separator + 1)));
    RUVIA_CHECK_EQ(tail.status(), ruvia::http_status::ok);
    RUVIA_CHECK_EQ(tail.body(), "789");
}
