#include <gtest/gtest.h>

// The public in-memory testing facade must dispatch through the production
// pipeline: controller macros, route params, query/cookie access, model
// bodies with their 415/400 split, global middleware, prefix and app-wide
// fallbacks, url_for, worker state, and the automatic HEAD fallback -- all
// without a socket. These tests use ONLY public headers, exactly like an
// application's own test suite would.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include "ruvia/http/http_header.h"
#include "ruvia/http/http_limits.h"
#include "ruvia/web/app.h"
#include "ruvia/web/context.h"
#include "ruvia/web/controller.h"
#include "ruvia/web/error.h"
#include "ruvia/web/security_headers.h"
#include "ruvia/web/session.h"
#include "ruvia/web/static_files.h"
#include "ruvia/web/testing.h"
#include "ruvia/web/validation.h"

RUVIA_MODEL(testing_facade_echo, RUVIA_OPTIONAL_FIELD(value, ruvia::string));

RUVIA_MODEL(testing_facade_report, RUVIA_REQUIRED_FIELD(path, ruvia::string),
    RUVIA_REQUIRED_FIELD(count, ruvia::uint64),
    RUVIA_REQUIRED_FIELD(tags, ruvia::array<ruvia::string>));

namespace {

struct testing_facade_counter final {
    int count_{0};
};

std::filesystem::path& testing_facade_file_path() {
    static std::filesystem::path path;
    return path;
}

std::optional<ruvia::static_root>& testing_facade_static_root() {
    static std::optional<ruvia::static_root> root;
    return root;
}
struct testing_facade_startup_failure_state final {};

class testing_facade_startup_error final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class testing_facade_throwing_middleware final {
public:
    explicit testing_facade_throwing_middleware(int* attempts) {
        ++*attempts;
        throw testing_facade_startup_error("testing facade middleware startup failed");
    }

    ruvia::task<void> handle(ruvia::context&, ruvia::next&) {
        co_return;
    }
};

class testing_facade_stamp final {
public:
    ruvia::task<void> handle(ruvia::context& c, ruvia::next& next_value) {
        co_await next_value();
        c.edit_response().header("X-Test-Stamp", "on");
        c.edit_response().header("X-Middleware-Method", c.req().method());
    }
};

// Registered with constructor arguments rather than default constructed, so it
// is deliberately not default constructible: the descriptor must carry the
// registration arguments to every instance the router builds.
class testing_facade_configured_stamp final {
public:
    testing_facade_configured_stamp(std::string_view name, int level) noexcept
        : name_(name),
          level_(level) {}

    ruvia::task<void> handle(ruvia::context& c, ruvia::next& next_value) {
        co_await next_value();
        c.edit_response().header("X-Test-Configured", name_);
        c.edit_response().header("X-Test-Level", level_ == 2 ? "two" : "other");
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
class testing_facade_auth final {
public:
    ruvia::task<void> handle(ruvia::context& c, ruvia::next& next_value) {
        const testing_facade_user user_value{
            .name_ = c.req().header("X-User").value_or("anonymous"), .level_ = 2};
        const auto binding = c.bind_request_state(user_value);
        co_await next_value();
    }
};

// Stamps a header so a response shows whether this middleware ran at all.
class testing_facade_scoped final {
public:
    explicit testing_facade_scoped(std::string_view tag) noexcept
        : tag_(tag) {}

    ruvia::task<void> handle(ruvia::context& c, ruvia::next& next_value) {
        co_await next_value();
        c.edit_response().header("X-Test-Scope", tag_);
    }

private:
    std::string_view tag_;
};

// Declares itself meaningful on a request that matched no route, the way
// security_headers_middleware does.
class testing_facade_always final {
public:
    static constexpr bool ruvia_runs_on_unmatched_requests = true;

    ruvia::task<void> handle(ruvia::context& c, ruvia::next& next_value) {
        co_await next_value();
        c.edit_response().header("X-Test-Always", "on");
    }
};
class testing_facade_const_middleware final {
public:
    ruvia::task<void> handle(ruvia::context& c, ruvia::next& next_value) const {
        co_await next_value();
        c.edit_response().header("X-Const-Middleware", "yes");
    }
};

// Rejects requests under /guarded before reaching the answer.
class testing_facade_unmatched_reject final {
public:
    static constexpr bool ruvia_runs_on_unmatched_requests = true;

    ruvia::task<void> handle(ruvia::context& c, ruvia::next& next_value) {
        if (c.req().path().starts_with("/guarded")) {
            throw ruvia::http_error({.status_ = ruvia::http_status::unauthorized});
        }
        co_await next_value();
    }
};

// Each replaces a complete downstream response after next(): one by failing,
// one by responding.
class testing_facade_deny_after final {
public:
    ruvia::task<void> handle(ruvia::context&, ruvia::next& next_value) {
        co_await next_value();
        throw ruvia::http_error({.status_ = ruvia::http_status::forbidden});
    }
};

class testing_facade_replace_after final {
public:
    ruvia::task<void> handle(ruvia::context& c, ruvia::next& next_value) {
        co_await next_value();
        c.respond(c.text("denied"));
    }
};

class testing_facade_policy_stamp final {
public:
    explicit testing_facade_policy_stamp(bool after) noexcept
        : after_(after) {}
    ruvia::task<void> handle(ruvia::context& c, ruvia::next& next_value) {
        if (!after_) {
            c.policy().header("X-Policy", "retained");
        }
        co_await next_value();
        if (after_) {
            c.policy().header("X-Policy", "retained");
        }
    }

private:
    bool after_;
};

// Replaces a downstream response through context metadata assigned after next().
class testing_facade_forbid_after final {
public:
    ruvia::task<void> handle(ruvia::context& c, ruvia::next& next_value) {
        co_await next_value();
        c.draft().status(ruvia::http_status::forbidden);
        c.draft().header("X-Test-Denied", "after");
        c.respond(c.text("denied"));
    }
};

// Post-processes a downstream response after next(), then fails.
class testing_facade_stamp_then_fail final {
public:
    ruvia::task<void> handle(ruvia::context& c, ruvia::next& next_value) {
        co_await next_value();
        c.edit_response().header("X-Test-After", "1");
        throw ruvia::http_error({.status_ = ruvia::http_status::conflict});
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

class testing_facade_controller final {
public:
    RUVIA_CONTROLLER_GROUP("/t")
    RUVIA_ROUTES_BEGIN(testing_facade_controller)
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
    RUVIA_GET("/tree/*", tree);
    RUVIA_GET("/dot-links", dot_links);
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
    RUVIA_GET("/partial", partial);
    RUVIA_GET("/subrequest", subrequest);
    RUVIA_GET("/sub-file", sub_file);
    RUVIA_GET("/sub-static", sub_static);
    RUVIA_GET("/sub-static/:name", sub_static_named);
    RUVIA_POST("/sub-stream", sub_stream, ruvia::stream_body);
    RUVIA_GET("/sub-checks", sub_checks);
    RUVIA_GET("/sub-refused-file", sub_refused_file);
    RUVIA_GET("/typed/:id/:active", typed_inputs);
    RUVIA_POST("/json-array", json_array);
    RUVIA_GET("/const-handler", const_handler, testing_facade_const_middleware);
    RUVIA_GET("/conn-info", conn_info_handler);
    RUVIA_CONNECT("example.com:443", connect_authority);
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

    // Relays a subrequest named by the query as status and body; X-Test-Relayed
    // tells a relayed answer from a failure escaping dispatch().
    ruvia::task<ruvia::http_response> subrequest(ruvia::context& c) {
        std::string method(c.req().query("method").value_or("GET"));
        std::pmr::string target(c.req().query("target").value_or("/"), c.arena());
        auto pending = c.dispatch({
            .method_ = method,
            .target_ = target,
        });
        method.assign("PUT");
        target.assign("/mutated");
        const auto child = co_await std::move(pending);
        c.draft().status(child.status());
        c.draft().header("X-Test-Relayed", "1");
        co_return c.text(child.body());
    }

    // A representation-specific answer whose metadata describes only its own body.
    ruvia::task<ruvia::http_response> partial(ruvia::context& c) {
        c.draft().status(ruvia::http_status::partial_content);
        c.draft().header("Content-Range", "bytes 0-3/10");
        c.draft().header("ETag", "\"v1\"");
        c.draft().header("Cache-Control", "public, max-age=60");
        c.draft().header("Content-Type", "application/octet-stream");
        co_return c.body("abcd");
    }

    ruvia::task<ruvia::http_response> user(ruvia::context& c) {
        co_return c.body(c.req().param("id").value_or("?"));
    }

    ruvia::task<ruvia::http_response> head_metadata(ruvia::context& c) {
        c.draft().header("X-Handler-Method", c.req().method());
        c.draft().header("X-Route-Param", c.req().param("id").value_or("?"));
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

    // No auth middleware on this route, so nothing is bound and the lookup
    // throws std::logic_error.
    ruvia::task<ruvia::http_response> whoami_unbound(ruvia::context& c) {
        try {
            (void)c.request_state<testing_facade_user>();
            co_return c.body("bound");
        } catch (const std::logic_error&) {
            co_return c.body("unbound");
        }
    }
    ruvia::task<ruvia::http_response> typed_inputs(ruvia::context& c) const {
        const auto id = c.req().param<std::uint32_t>("id").value_or(0);
        const auto active = c.req().param<bool>("active").value_or(false);
        const auto page = c.req().query<std::int64_t>("page").value_or(1);
        const auto verbose = c.req().query<bool>("verbose").value_or(false);
        const auto* optional_user = c.try_request_state<testing_facade_user>();
        const std::string_view span_args[] = {"12", "true"};
        std::pmr::string reply(c.arena());
        reply.append(std::to_string(id));
        reply.push_back(':');
        reply.append(active ? "1" : "0");
        reply.push_back(':');
        reply.append(std::to_string(page));
        reply.push_back(':');
        reply.append(verbose ? "1" : "0");
        reply.push_back(':');
        reply.append(optional_user == nullptr ? "none" : optional_user->name_);
        reply.push_back(':');
        reply.append(c.url_for("/t/typed/:id/:active", std::span<const std::string_view>(span_args)));
        co_return c.text(std::move(reply));
    }

    ruvia::task<ruvia::http_response> json_array(ruvia::context& c) const {
        const auto raw = co_await c.req().text();
        auto items = ruvia::from_json<ruvia::array<ruvia::string>>(raw, {.resource_ = c.arena()});
        items.emplace_back("tail", ruvia::model_options{.resource_ = c.arena()});
        co_return c.json(items);
    }

    ruvia::task<ruvia::http_response> const_handler(ruvia::context& c) const {
        const auto* user_ptr = c.try_request_state<testing_facade_user>();
        co_return c.text(user_ptr == nullptr ? "const-ok" : user_ptr->name_);
    }

    ruvia::task<ruvia::http_response> conn_info_handler(ruvia::context& c) const {
        std::pmr::string reply(c.arena());
        reply.append(c.conn().remote().address());
        reply.push_back(':');
        reply.append(std::to_string(c.conn().remote().port()));
        reply.push_back('/');
        reply.append(c.conn().client().address());
        reply.push_back('/');
        reply.append(c.conn().via_trusted_proxy() ? "proxy" : "direct");
        reply.push_back('/');
        reply.append(c.conn().plain() != nullptr ? "plain" : "tls");
        co_return c.text(std::move(reply));
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

    ruvia::task<ruvia::http_response> tree(ruvia::context& c) {
        co_return c.body(c.req().param("*").value_or("?"));
    }

    ruvia::task<ruvia::http_response> dot_links(ruvia::context& c) {
        const auto rejects = [&c](std::string_view pattern, std::string_view value) {
            try {
                (void)c.url_for(pattern, {value});
            } catch (const std::invalid_argument&) {
                return true;
            }
            return false;
        };
        static constexpr std::string_view cases[][2] = {{"/t/users/:id", "."}, {"/t/users/:id", ".."},
            {"/t/tree/*", "."}, {"/t/tree/*", ".."}, {"/t/tree/*", "./a"}, {"/t/tree/*", "a/../b"},
            {"/t/tree/*", "a/."}, {"/t/tree/*", "a\\b"}, {"example.com:443", "a"}};
        std::pmr::string reply(c.arena());
        for (const auto& entry : cases) {
            reply.push_back(rejects(entry[0], entry[1]) ? 'x' : '-');
        }
        reply.push_back(' ');
        reply.append(c.url_for("/t/tree/*", {"a.b/..c/.d"}));
        // Below the root a leading '/' is an empty segment the matcher strips
        // again, so the value round-trips.
        reply.push_back(' ');
        reply.append(c.url_for("/t/tree/*", {"/a"}));
        co_return c.body(std::move(reply));
    }

    ruvia::task<void> connect_authority(ruvia::context&) {
        co_return;
    }

    ruvia::task<ruvia::http_response> sub_file(ruvia::context& c) {
        co_return co_await c.file({.path_ = testing_facade_file_path()});
    }

    ruvia::task<ruvia::http_response> sub_static(ruvia::context& c) {
        co_return co_await c.static_file(*testing_facade_static_root(), {.relative_path_ = "asset.txt"});
    }

    ruvia::task<ruvia::http_response> sub_static_named(ruvia::context& c) {
        co_return co_await c.static_file(
            *testing_facade_static_root(), {.relative_path_ = c.req().param("name").value_or("")});
    }

    ruvia::task<ruvia::http_response> sub_stream(ruvia::context& c) {
        co_return c.text("stream");
    }

    ruvia::task<ruvia::http_response> sub_checks(ruvia::context& c) {
        bool stream_rejected = false;
        try {
            (void)co_await c.dispatch({.method_ = "POST", .target_ = "/t/sub-stream", .body_ = "abc"});
        } catch (const std::logic_error&) {
            stream_rejected = true;
        }
        bool multipart_file_rejected = false;
        try {
            const ruvia::http_header_view range_headers[] = {{"Range", "bytes=0-1, 4-5"}};
            (void)co_await c.dispatch({
                .method_ = "GET",
                .target_ = "/t/sub-file",
                .headers_ = range_headers,
            });
        } catch (const std::logic_error&) {
            multipart_file_rejected = true;
        }
        // Connection-specific fields (RFC 9110 section 7.6.1) belong to the
        // protocol, like framing fields; a subrequest cannot supply them.
        bool connection_fields_rejected = true;
        for (const auto name : {"TE", "Keep-Alive", "Proxy-Connection", "Transfer-Encoding"}) {
            const ruvia::http_header_view connection_headers[] = {{name, "trailers"}};
            try {
                (void)co_await c.dispatch({
                    .method_ = "GET",
                    .target_ = "/t/hello",
                    .headers_ = connection_headers,
                });
                connection_fields_rejected = false;
            } catch (const std::invalid_argument&) {
            }
        }
        const ruvia::http_header_view encoding_headers[] = {{"Accept-Encoding", "identity;q=0"}};
        const auto not_acceptable = co_await c.dispatch({
            .method_ = "GET",
            .target_ = "/t/hello",
            .headers_ = encoding_headers,
        });
        std::pmr::string reply(c.arena());
        reply.push_back(stream_rejected ? '1' : '0');
        reply.push_back(multipart_file_rejected ? '1' : '0');
        reply.push_back(not_acceptable.status() == ruvia::http_status::not_acceptable ? '1' : '0');
        reply.push_back(connection_fields_rejected ? '1' : '0');
        co_return c.body(std::move(reply));
    }

    // A refused coding turns the buffered /t/hello answer into a 406 from the
    // error handler, which this case's handler answers with a file: a shape
    // dispatch cannot deliver, whichever step produced it.
    ruvia::task<ruvia::http_response> sub_refused_file(ruvia::context& c) {
        const ruvia::http_header_view encoding_headers[] = {{"Accept-Encoding", "identity;q=0"}};
        try {
            const auto refused = co_await c.dispatch({
                .method_ = "GET",
                .target_ = "/t/hello",
                .headers_ = encoding_headers,
            });
            std::pmr::string reply("delivered:", c.arena());
            reply.append(std::to_string(refused.body().size()));
            co_return c.body(std::move(reply));
        } catch (const std::logic_error&) {
            co_return c.body("rejected");
        }
    }
};

ruvia::task<ruvia::http_response> facade_not_found(ruvia::context& c) {
    c.draft().status(ruvia::http_status::not_found);
    co_return c.body("custom-miss");
}

ruvia::task<ruvia::http_response> api_scoped_miss(ruvia::context& c) {
    c.draft().status(ruvia::http_status::not_found);
    co_return c.body("api-miss");
}

ruvia::task<ruvia::http_response> facade_error(ruvia::context& c, ruvia::http_error_info error) {
    c.draft().status(error.status());
    co_return c.body("custom-error");
}

}  // namespace

TEST(Ruvia, testing_facade_multi_route_paths_are_evaluated_once_per_registration) {
    const auto all_before = all_path_evaluations;
    const auto on_before = on_path_evaluations;
    ruvia::test_app app;
    const auto all_response = app.request(ruvia::test_request::get("/t/counted-all"));
    EXPECT_EQ(all_response.body(), std::string_view("hello"));
    const auto on_response = app.request(ruvia::test_request::put("/t/counted-on"));
    EXPECT_EQ(on_response.body(), std::string_view("hello"));
    EXPECT_TRUE(all_path_evaluations > all_before);
    EXPECT_EQ(all_path_evaluations - all_before, on_path_evaluations - on_before);
}

TEST(Ruvia, testing_facade_dispatches_routes_params_query_and_cookies) {
    ruvia::test_app app;

    const auto hello = app.request(ruvia::test_request::get("/t/hello"));
    EXPECT_TRUE(hello.status() == ruvia::http_status::ok);
    EXPECT_EQ(hello.body(), std::string_view("hello"));

    const auto user_value = app.request(ruvia::test_request::get("/t/users/42"));
    EXPECT_EQ(user_value.body(), std::string_view("42"));

    const auto greet =
        app.request(ruvia::test_request::get("/t/greet?name=ada").cookie("sid", "s-1"));
    EXPECT_EQ(greet.body(), std::string_view("ada/s-1"));

    const auto absolute = app.request(
        ruvia::test_request::get("http://example.test/t/hello").header("Host", "stale.example"));
    EXPECT_TRUE(absolute.status() == ruvia::http_status::ok);
    EXPECT_EQ(absolute.body(), std::string_view("hello"));

    const auto server_wide_options = app.request(
        ruvia::test_request::options("http://example.test").header("Host", "stale.example"));
    EXPECT_TRUE(server_wide_options.status() == ruvia::http_status::no_content);

    const auto link = app.request(ruvia::test_request::get("/t/link"));
    EXPECT_EQ(link.body(), std::string_view("/t/users/9"));

    const auto worker_value = app.request(ruvia::test_request::get("/t/worker"));
    EXPECT_EQ(worker_value.body(), std::string_view("valid/current"));

    // The automatic HEAD fallback answers with the GET status and no body.
    // Writer-synthesized framing headers (Content-Length, Date) are not part
    // of the in-memory dispatch product.
    const auto head = app.request(ruvia::test_request::head("/t/hello"));
    EXPECT_TRUE(head.status() == ruvia::http_status::ok);
    EXPECT_TRUE(head.body().empty());
}

TEST(Ruvia, testing_facade_head_fallback_preserves_request_method_and_route_params) {
    ruvia::test_app app;
    const auto get = app.request(ruvia::test_request::get("/t/head/42"));
    EXPECT_TRUE(get.status() == ruvia::http_status::ok);
    EXPECT_EQ(get.body(), std::string_view("payload"));
    EXPECT_EQ(get.header("X-Handler-Method").value_or(""), std::string_view("GET"));
    EXPECT_EQ(get.header("X-Middleware-Method").value_or(""), std::string_view("GET"));

    const auto head = app.request(ruvia::test_request::head("/t/head/42"));
    EXPECT_TRUE(head.status() == ruvia::http_status::ok);
    EXPECT_TRUE(head.body().empty());
    EXPECT_EQ(head.header("X-Handler-Method").value_or(""), std::string_view("HEAD"));
    EXPECT_EQ(head.header("X-Middleware-Method").value_or(""), std::string_view("HEAD"));
    EXPECT_EQ(head.header("X-Route-Param").value_or(""), std::string_view("42"));
    EXPECT_EQ(head.header("X-Test-Stamp").value_or(""), std::string_view("on"));
}

TEST(Ruvia, testing_facade_rejects_invalid_request_line_targets) {
    ruvia::test_app app;

    EXPECT_TRUE(app.request(ruvia::test_request::get("/bad path")).status() ==
                ruvia::http_status::bad_request);
    EXPECT_TRUE(
        app.request(ruvia::test_request::get("*")).status() == ruvia::http_status::bad_request);
    EXPECT_TRUE(app.request(ruvia::test_request::get("/bad%zz")).status() ==
                ruvia::http_status::bad_request);
    EXPECT_TRUE(app.request(ruvia::test_request::method("BAD(METHOD", "/")).status() ==
                ruvia::http_status::bad_request);
}

TEST(Ruvia, testing_facade_rejects_invalid_request_headers) {
    ruvia::test_app app;

    EXPECT_TRUE(
        app.request(ruvia::test_request::get("/t/hello").header("Bad Header", "x")).status() ==
        ruvia::http_status::bad_request);
    EXPECT_TRUE(
        app.request(
               ruvia::test_request::get("/t/hello").header("X-Bad", std::string_view("a\rb", 3)))
            .status() == ruvia::http_status::bad_request);
    const auto independent = app.request(ruvia::test_request::post("/t/echo")
            .json(R"({"value":"body"})")
            .header("Content-Length", "5")
            .header("Content-Length", "6")
            .header("Transfer-Encoding", "chunked")
            .header("X-Test-Independent", "header"));
    EXPECT_TRUE(independent.status() == ruvia::http_status::ok);
    EXPECT_EQ(independent.body(), std::string_view("header/body"));

    auto many = ruvia::test_request::get("/t/hello");
    for (std::size_t i = 0; i < 80; ++i) {
        many.header("X-Test", "v");
    }
    EXPECT_TRUE(app.request(many).status() == ruvia::http_status::ok);

    const std::string oversized_header_value(ruvia::max_http_header_bytes, 'x');
    EXPECT_TRUE(
        app.request(ruvia::test_request::get("/t/hello").header("X-Big", oversized_header_value))
            .status() == ruvia::http_status::request_header_fields_too_large);
}

TEST(Ruvia, testing_facade_runs_model_bodies_with_media_type_split) {
    ruvia::test_app app;

    const auto ok = app.request(ruvia::test_request::post("/t/echo").json(R"({"value":"hi"})"));
    EXPECT_TRUE(ok.status() == ruvia::http_status::ok);
    EXPECT_EQ(ok.body(), std::string_view("hi"));

    const auto wrong_type =
        app.request(ruvia::test_request::post("/t/echo").body(R"({"value":"hi"})", "text/plain"));
    EXPECT_TRUE(wrong_type.status() == ruvia::http_status::unsupported_media_type);

    const auto bad_body = app.request(ruvia::test_request::post("/t/echo").json("{not-json"));
    EXPECT_TRUE(bad_body.status() == ruvia::http_status::bad_request);
}

TEST(Ruvia, testing_facade_applies_app_level_configuration) {
    ruvia::test_app app;
    app.use<testing_facade_stamp>()
        .on_not_found(&facade_not_found)
        .on_not_found({.prefix_ = "/api", .handler_ = &api_scoped_miss});
    app.use_worker_state<testing_facade_counter>();

    // Global middleware wraps every matched route.
    const auto stamped = app.request(ruvia::test_request::get("/t/hello"));
    EXPECT_EQ(stamped.header("X-Test-Stamp").value_or(""), std::string_view("on"));

    // Worker state persists across requests on the same test_app.
    const auto first_count = app.request(ruvia::test_request::get("/t/count"));
    EXPECT_EQ(first_count.body(), std::string_view("1"));
    const auto second_count = app.request(ruvia::test_request::get("/t/count"));
    EXPECT_EQ(second_count.body(), std::string_view("2"));

    // Prefix fallback wins under its scope; the app-wide one covers the rest.
    const auto scoped_miss = app.request(ruvia::test_request::get("/api/missing"));
    EXPECT_EQ(scoped_miss.body(), std::string_view("api-miss"));
    const auto global_miss = app.request(ruvia::test_request::get("/nope"));
    EXPECT_EQ(global_miss.body(), std::string_view("custom-miss"));

    // 405 keeps flowing through the production fallback path too.
    const auto not_allowed = app.request(ruvia::test_request::delete_request("/t/hello"));
    EXPECT_TRUE(not_allowed.status() == ruvia::http_status::method_not_allowed);

    // The route table is sealed after the first request.
    bool sealed = false;
    try {
        app.use<testing_facade_stamp>();
    } catch (const std::logic_error&) {
        sealed = true;
    }
    EXPECT_TRUE(sealed);
}

TEST(Ruvia, testing_facade_retains_startup_failure_without_retrying) {
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
            EXPECT_EQ(std::string_view(error.what()),
                std::string_view("testing facade startup failed"));
        }
        EXPECT_TRUE(threw);
    }
    EXPECT_EQ(factory_calls, 1);

    bool sealed = false;
    try {
        app.use<testing_facade_stamp>();
    } catch (const std::logic_error&) {
        sealed = true;
    }
    EXPECT_TRUE(sealed);
}

TEST(Ruvia, testing_facade_retains_pre_worker_startup_failure_without_retrying) {
    ruvia::test_app app;
    int constructor_calls = 0;
    app.use<testing_facade_throwing_middleware>(&constructor_calls);

    for (int attempt_value = 0; attempt_value != 2; ++attempt_value) {
        bool threw = false;
        try {
            (void)app.request(ruvia::test_request::get("/t/hello"));
        } catch (const testing_facade_startup_error& error) {
            threw = true;
            EXPECT_EQ(std::string_view(error.what()),
                std::string_view("testing facade middleware startup failed"));
        }
        EXPECT_TRUE(threw);
    }
    EXPECT_EQ(constructor_calls, 1);

    bool sealed = false;
    try {
        app.use<testing_facade_stamp>();
    } catch (const std::logic_error&) {
        sealed = true;
    }
    EXPECT_TRUE(sealed);
}

TEST(Ruvia, testing_facade_constructs_middleware_from_registration_arguments) {
    ruvia::test_app app;
    app.use<testing_facade_configured_stamp>("audit", 2);

    const auto first = app.request(ruvia::test_request::get("/t/hello"));
    EXPECT_EQ(first.header("X-Test-Configured").value_or(""), std::string_view("audit"));
    EXPECT_EQ(first.header("X-Test-Level").value_or(""), std::string_view("two"));

    // One instance serves every request, so the arguments must still be readable
    // after the first dispatch rather than having been consumed by it.
    const auto second = app.request(ruvia::test_request::get("/t/hello"));
    EXPECT_EQ(second.header("X-Test-Configured").value_or(""), std::string_view("audit"));

    // Separate registrations of the same type stay independent.
    ruvia::test_app other;
    other.use<testing_facade_configured_stamp>("other", 5);
    const auto distinct = other.request(ruvia::test_request::get("/t/hello"));
    EXPECT_EQ(distinct.header("X-Test-Configured").value_or(""), std::string_view("other"));
    EXPECT_EQ(distinct.header("X-Test-Level").value_or(""), std::string_view("other"));
}

TEST(Ruvia, testing_facade_runs_fallback_handlers_that_carry_state) {
    // A fallback handler used to be a plain function pointer, so anything it
    // needed had to be a global. It now accepts any callable, including one that
    // captures the collaborators the handler depends on.
    struct branding final {
        std::string label_;
    };
    const branding branding_value{"tenant-a"};

    ruvia::test_app app;
    app.on_not_found([branding_value](ruvia::context& c) -> ruvia::task<ruvia::http_response> {
        c.draft().status(ruvia::http_status::not_found);
        co_return c.text(std::string_view(branding_value.label_));
    });
    app.on_error([branding_value](ruvia::context& c,
                     ruvia::http_error_info error) -> ruvia::task<ruvia::http_response> {
        c.draft().status(error.status());
        std::pmr::string body(c.arena());
        body.append(branding_value.label_);
        body.append(":error");
        co_return c.text(std::move(body));
    });

    const auto missed = app.request(ruvia::test_request::get("/nowhere"));
    EXPECT_TRUE(missed.status() == ruvia::http_status::not_found);
    EXPECT_EQ(missed.body(), std::string_view("tenant-a"));

    // The captured state is still readable on a later request: the callable was
    // copied at registration, not borrowed from the caller's frame.
    const auto missed_again = app.request(ruvia::test_request::get("/nowhere/else"));
    EXPECT_EQ(missed_again.body(), std::string_view("tenant-a"));

    const auto failed = app.request(ruvia::test_request::get("/t/boom"));
    EXPECT_TRUE(failed.status() == ruvia::http_status::internal_server_error);
    EXPECT_EQ(failed.body(), std::string_view("tenant-a:error"));
}

TEST(Ruvia, testing_facade_error_handler_throwing_validation_issues_keeps_its_status) {
    // An http_error thrown inside on_error is answered with the default error
    // response for that error, issues included; it is not an error-handler
    // failure and must not be rewritten as a 500.
    ruvia::test_app app;
    app.on_error([](ruvia::context& c, ruvia::http_error_info) -> ruvia::task<ruvia::http_response> {
        ruvia::validator checks;
        std::optional<std::string> absent;
        checks.required(absent, "email", "email is required");
        checks.throw_if_invalid({.status_ = ruvia::http_status::unprocessable_content,
            .code_ = "invalid_payload",
            .message_ = "payload failed validation"});
        co_return c.text("unreachable");
    });

    const auto failed = app.request(ruvia::test_request::get("/t/boom"));
    EXPECT_EQ(failed.status(), ruvia::http_status::unprocessable_content);
    EXPECT_TRUE(failed.body().find("\"invalid_payload\"") != std::string_view::npos);
    EXPECT_TRUE(failed.body().find("payload failed validation") != std::string_view::npos);
    EXPECT_TRUE(failed.body().find("\"email\"") != std::string_view::npos);
    EXPECT_TRUE(failed.body().find("email is required") != std::string_view::npos);
    EXPECT_TRUE(failed.body().find("error_handler_failed") == std::string_view::npos);
}

TEST(Ruvia, testing_facade_error_responses_are_not_renegotiated_into_406) {
    // An error response is not a representation of the target resource, so a
    // refused coding must neither turn the 500 into a 406 nor run the error
    // handler a second time. A successful response still becomes a 406.
    int calls = 0;
    ruvia::test_app app;
    app.on_error([counter = &calls](ruvia::context& c,
                     ruvia::http_error_info error) -> ruvia::task<ruvia::http_response> {
        ++*counter;
        c.draft().status(error.status());
        co_return c.text("err");
    });

    // The test app configures no compression, so "gzip, identity;q=0" cannot be
    // satisfied either.
    for (const std::string_view refused : {std::string_view("identity;q=0, *;q=0"),
             std::string_view("gzip, identity;q=0")}) {
        calls = 0;
        const auto failed =
            app.request(ruvia::test_request::get("/t/boom").header("Accept-Encoding", refused));
        EXPECT_EQ(failed.status(), ruvia::http_status::internal_server_error);
        EXPECT_EQ(failed.body(), std::string_view("err"));
        EXPECT_EQ(calls, 1);

        calls = 0;
        const auto refused_ok =
            app.request(ruvia::test_request::get("/t/hello").header("Accept-Encoding", refused));
        EXPECT_EQ(refused_ok.status(), ruvia::http_status::not_acceptable);
        EXPECT_EQ(calls, 1);
    }
}

TEST(Ruvia, testing_facade_rejects_duplicate_normalized_fallback_prefixes) {
    ruvia::test_app not_found_app;
    not_found_app.on_not_found({.prefix_ = "/api", .handler_ = &api_scoped_miss});

    bool not_found_rejected = false;
    try {
        not_found_app.on_not_found({.prefix_ = "/api///", .handler_ = &api_scoped_miss});
    } catch (const std::invalid_argument& error) {
        not_found_rejected = std::string_view(error.what()) == "duplicate fallback prefix";
    }
    EXPECT_TRUE(not_found_rejected);

    ruvia::test_app error_app;
    error_app.on_error({.prefix_ = "/api/", .handler_ = &facade_error});

    bool error_rejected = false;
    try {
        error_app.on_error({.prefix_ = "/api", .handler_ = &facade_error});
    } catch (const std::invalid_argument& error) {
        error_rejected = std::string_view(error.what()) == "duplicate fallback prefix";
    }
    EXPECT_TRUE(error_rejected);

    bool malformed_rejected = false;
    try {
        error_app.on_not_found({.prefix_ = "api", .handler_ = &api_scoped_miss});
    } catch (const std::invalid_argument& error) {
        malformed_rejected = std::string_view(error.what()) == "fallback prefix must start with '/'";
    }
    EXPECT_TRUE(malformed_rejected);

    const auto rejects_malformed_prefixed_scope = [](std::string_view prefix) {
        ruvia::test_app app;
        try {
            app.on_not_found({.prefix_ = prefix, .handler_ = &api_scoped_miss});
        } catch (const std::invalid_argument& error) {
            return std::string_view(error.what()) ==
                   "fallback prefix must be an origin-form path without query";
        }
        return false;
    };
    EXPECT_TRUE(rejects_malformed_prefixed_scope("/api?debug=1"));
    EXPECT_TRUE(rejects_malformed_prefixed_scope("/bad path"));
    EXPECT_TRUE(rejects_malformed_prefixed_scope("/api#fragment"));
}

TEST(Ruvia, testing_facade_rejects_root_scopes_and_second_app_wide_fallbacks) {
    const auto rejected = [](auto&& registration) {
        try {
            registration();
        } catch (const std::invalid_argument&) {
            return true;
        }
        return false;
    };
    ruvia::test_app app;
    // The prefix-less form is the only app-wide spelling; "/" would shadow it.
    EXPECT_TRUE(rejected([&app] { app.on_not_found({.prefix_ = "/", .handler_ = &api_scoped_miss}); }));
    EXPECT_TRUE(rejected([&app] { app.on_error({.prefix_ = "//", .handler_ = &facade_error}); }));
    EXPECT_TRUE(rejected([&app] { app.use_at<testing_facade_scoped>({.prefix_ = "/"}, "root"); }));

    app.on_not_found(&facade_not_found);
    app.on_error(&facade_error);
    EXPECT_TRUE(rejected([&app] { app.on_not_found(&api_scoped_miss); }));
    EXPECT_TRUE(rejected([&app] { app.on_error(&facade_error); }));

    // Neither the rejected scopes nor the rejected replacement took effect.
    const auto missing = app.request(ruvia::test_request::get("/nope"));
    EXPECT_EQ(missing.status(), ruvia::http_status::not_found);
    EXPECT_EQ(missing.body(), std::string_view("custom-miss"));
    const auto matched = app.request(ruvia::test_request::get("/t/hello"));
    EXPECT_EQ(matched.status(), ruvia::http_status::ok);
    EXPECT_TRUE(!matched.header("X-Test-Scope"));
}

TEST(Ruvia, testing_facade_isolates_instances) {
    // Two TestApps own separate controller instances and worker state.
    ruvia::test_app first;
    first.use_worker_state<testing_facade_counter>();
    ruvia::test_app second;
    second.use_worker_state<testing_facade_counter>();

    const auto first_count = first.request(ruvia::test_request::get("/t/count"));
    EXPECT_EQ(first_count.body(), std::string_view("1"));
    const auto second_count = second.request(ruvia::test_request::get("/t/count"));
    EXPECT_EQ(second_count.body(), std::string_view("1"));
}

TEST(Ruvia, testing_facade_middleware_publishes_request_state_to_handler) {
    ruvia::test_app app;

    const auto named = app.request(ruvia::test_request::get("/t/whoami").header("X-User", "ada"));
    EXPECT_EQ(named.status(), ruvia::http_status::ok);
    EXPECT_EQ(named.body(), std::string_view("ada/2"));

    // A second request gets its own binding, not the previous one's value --
    // this is request scope, not worker scope.
    const auto anonymous = app.request(ruvia::test_request::get("/t/whoami"));
    EXPECT_EQ(anonymous.body(), std::string_view("anonymous/2"));
}

TEST(Ruvia, testing_facade_request_state_is_absent_without_its_middleware) {
    ruvia::test_app app;
    const auto response =
        app.request(ruvia::test_request::get("/t/whoami-unbound").header("X-User", "ada"));
    EXPECT_EQ(response.status(), ruvia::http_status::ok);
    EXPECT_EQ(response.body(), std::string_view("unbound"));
}

TEST(Ruvia, testing_facade_builds_a_runtime_sized_response_model) {
    ruvia::test_app app;
    const auto response = app.request(ruvia::test_request::get("/t/report"));
    EXPECT_EQ(response.status(), ruvia::http_status::ok);
    EXPECT_EQ(response.body(),
        std::string_view(R"({"path":"/t/report","count":2,"tags":["a\"quoted","b"]})"));
    const auto content_type_value = response.header("Content-Type");
    EXPECT_TRUE(content_type_value.has_value());
    EXPECT_EQ(*content_type_value, std::string_view("application/json"));
}

TEST(Ruvia, testing_facade_response_owns_large_bodies_across_requests) {
    ruvia::test_app app;
    const auto first = app.request(ruvia::test_request::get("/t/large?fill=a"));
    EXPECT_EQ(first.body().size(), std::size_t(1024 * 1024));
    EXPECT_EQ(first.body(), std::string(1024 * 1024, 'a'));

    const auto second = app.request(ruvia::test_request::get("/t/large?fill=b"));
    EXPECT_EQ(second.body().size(), std::size_t(1024 * 1024));
    EXPECT_EQ(first.body().size(), std::size_t(1024 * 1024));
    EXPECT_EQ(second.body(), std::string(1024 * 1024, 'b'));
    EXPECT_EQ(first.body(), std::string(1024 * 1024, 'a'));
}

TEST(Ruvia, testing_facade_path_scoped_middleware_runs_only_under_its_prefix) {
    ruvia::test_app app;
    app.use_at<testing_facade_scoped>({.prefix_ = "/t/users"}, "users");

    // Under the scope, on both the exact prefix path shape and a deeper one.
    const auto scoped = app.request(ruvia::test_request::get("/t/users/42"));
    EXPECT_EQ(scoped.status(), ruvia::http_status::ok);
    const auto scoped_header = scoped.header("X-Test-Scope");
    EXPECT_TRUE(scoped_header.has_value());
    EXPECT_EQ(*scoped_header, std::string_view("users"));

    // A sibling route outside the scope never receives the frame.
    const auto outside = app.request(ruvia::test_request::get("/t/hello"));
    EXPECT_EQ(outside.status(), ruvia::http_status::ok);
    EXPECT_TRUE(!outside.header("X-Test-Scope").has_value());
}

TEST(Ruvia, testing_facade_path_scope_matches_whole_segments_only) {
    ruvia::test_app app;
    // "/t/user" must not scope "/t/users/:id" -- that is a different segment,
    // not a deeper path.
    app.use_at<testing_facade_scoped>({.prefix_ = "/t/user"}, "prefix-only");

    const auto response = app.request(ruvia::test_request::get("/t/users/42"));
    EXPECT_EQ(response.status(), ruvia::http_status::ok);
    EXPECT_TRUE(!response.header("X-Test-Scope").has_value());
}

TEST(Ruvia, testing_facade_path_scope_normalizes_a_trailing_slash) {
    ruvia::test_app app;
    app.use_at<testing_facade_scoped>({.prefix_ = "/t/users/"}, "trailing");

    const auto response = app.request(ruvia::test_request::get("/t/users/42"));
    const auto header_value = response.header("X-Test-Scope");
    EXPECT_TRUE(header_value.has_value());
    EXPECT_EQ(*header_value, std::string_view("trailing"));
}

TEST(Ruvia, testing_facade_app_wide_middleware_still_runs_everywhere) {
    ruvia::test_app app;
    app.use<testing_facade_scoped>("global");

    // Bound to names: dispatch_response owns its headers, so header() is rightly
    // rvalue-deleted and a view into a temporary is not obtainable.
    const auto hello = app.request(ruvia::test_request::get("/t/hello"));
    EXPECT_TRUE(hello.header("X-Test-Scope").has_value());
    const auto user_value = app.request(ruvia::test_request::get("/t/users/42"));
    EXPECT_TRUE(user_value.header("X-Test-Scope").has_value());
}

TEST(Ruvia, testing_facade_unmatched_middleware_wraps_the_404_terminal) {
    ruvia::test_app app;
    app.use<testing_facade_always>();
    app.use<testing_facade_stamp>();

    // A matched route runs both, as before.
    const auto matched = app.request(ruvia::test_request::get("/t/hello"));
    EXPECT_EQ(matched.status(), ruvia::http_status::ok);
    EXPECT_TRUE(matched.header("X-Test-Always").has_value());
    EXPECT_TRUE(matched.header("X-Test-Stamp").has_value());

    // An unmatched one runs only what declared itself for unmatched requests.
    const auto missing = app.request(ruvia::test_request::get("/nope"));
    EXPECT_EQ(missing.status(), ruvia::http_status::not_found);
    const auto always = missing.header("X-Test-Always");
    EXPECT_TRUE(always.has_value());
    EXPECT_EQ(*always, std::string_view("on"));
    EXPECT_TRUE(!missing.header("X-Test-Stamp").has_value());
}

TEST(Ruvia, testing_facade_unmatched_middleware_also_wraps_405_and_501) {
    ruvia::test_app app;
    app.use<testing_facade_always>();

    // Known method, wrong verb for an existing path.
    const auto wrong_method = app.request(ruvia::test_request::post("/t/hello"));
    EXPECT_EQ(wrong_method.status(), ruvia::http_status::method_not_allowed);
    EXPECT_TRUE(wrong_method.header("X-Test-Always").has_value());
    // The Allow header the fallback sets must survive the chain.
    EXPECT_TRUE(wrong_method.header("Allow").has_value());

    // A token no route registered: the server does not know it, so 501.
    const auto unknown_method = app.request(ruvia::test_request::method("FROBNICATE", "/t/hello"));
    EXPECT_EQ(unknown_method.status(), ruvia::http_status::not_implemented);
    EXPECT_TRUE(unknown_method.header("X-Test-Always").has_value());
}

TEST(Ruvia, testing_facade_replacement_responses_do_not_inherit_the_discarded_one) {
    // An error raised after next() describes itself, not the 206 it replaces:
    // no range, validator or cache policy of the discarded representation.
    {
        ruvia::test_app app;
        app.use_at<testing_facade_deny_after>({.prefix_ = "/t/partial"});
        const auto denied = app.request(ruvia::test_request::get("/t/partial"));
        EXPECT_EQ(denied.status(), ruvia::http_status::forbidden);
        EXPECT_EQ(denied.header("Content-Type").value_or(std::string_view{}),
            std::string_view("application/problem+json"));
        EXPECT_TRUE(!denied.header("Content-Range").has_value());
        EXPECT_TRUE(!denied.header("ETag").has_value());
        EXPECT_TRUE(!denied.header("Cache-Control").has_value());
    }
    // Likewise a response built and sent in its place.
    {
        ruvia::test_app app;
        app.use_at<testing_facade_replace_after>({.prefix_ = "/t/partial"});
        const auto replaced = app.request(ruvia::test_request::get("/t/partial"));
        EXPECT_EQ(replaced.status(), ruvia::http_status::ok);
        EXPECT_EQ(replaced.body(), std::string_view("denied"));
        EXPECT_EQ(replaced.header("Content-Type").value_or(std::string_view{}),
            std::string_view("text/plain; charset=UTF-8"));
        EXPECT_TRUE(!replaced.header("Content-Range").has_value());
        EXPECT_TRUE(!replaced.header("ETag").has_value());
        EXPECT_TRUE(!replaced.header("Cache-Control").has_value());
    }
}

TEST(Ruvia, testing_facade_explicit_policy_survives_replacement_before_and_after_next) {
    for (const bool after : {false, true}) {
        ruvia::test_app app;
        app.use<testing_facade_replace_after>();
        app.use<testing_facade_policy_stamp>(after);
        const auto response = app.request(ruvia::test_request::get("/t/partial"));
        EXPECT_EQ(response.body(), std::string_view("denied"));
        EXPECT_EQ(response.header("X-Policy"), std::string_view("retained"));
        EXPECT_TRUE(!response.header("Content-Range").has_value());
        EXPECT_TRUE(!response.header("ETag").has_value());
    }
}

TEST(Ruvia, testing_facade_metadata_set_after_a_final_response_is_kept) {
    // 后处理必须明确选择 edit_response；构造替换响应则写入独立 draft。
    {
        ruvia::test_app app;
        app.use_at<testing_facade_scoped>({.prefix_ = "/t/partial"}, "after");
        const auto stamped = app.request(ruvia::test_request::get("/t/partial"));
        EXPECT_EQ(stamped.status(), ruvia::http_status::partial_content);
        EXPECT_EQ(stamped.body(), std::string_view("abcd"));
        EXPECT_EQ(stamped.header("X-Test-Scope").value_or(std::string_view{}), std::string_view("after"));
        EXPECT_TRUE(stamped.header("Content-Range").has_value());
    }
    // A replacement built afterwards starts from that metadata, not from the
    // discarded response.
    {
        ruvia::test_app app;
        app.use_at<testing_facade_forbid_after>({.prefix_ = "/t/partial"});
        const auto denied = app.request(ruvia::test_request::get("/t/partial"));
        EXPECT_EQ(denied.status(), ruvia::http_status::forbidden);
        EXPECT_EQ(denied.body(), std::string_view("denied"));
        EXPECT_EQ(denied.header("X-Test-Denied").value_or(std::string_view{}), std::string_view("after"));
        EXPECT_EQ(denied.header("Content-Type").value_or(std::string_view{}),
            std::string_view("text/plain; charset=UTF-8"));
        EXPECT_TRUE(!denied.header("Content-Range").has_value());
        EXPECT_TRUE(!denied.header("ETag").has_value());
        EXPECT_TRUE(!denied.header("Cache-Control").has_value());
    }
    // A custom error handler answering a failure raised after next() keeps the
    // status it assigns through the context.
    {
        ruvia::test_app app;
        app.on_error(&facade_error);
        app.use_at<testing_facade_deny_after>({.prefix_ = "/t/partial"});
        const auto denied = app.request(ruvia::test_request::get("/t/partial"));
        EXPECT_EQ(denied.status(), ruvia::http_status::forbidden);
        EXPECT_EQ(denied.body(), std::string_view("custom-error"));
        EXPECT_TRUE(!denied.header("Content-Range").has_value());
        EXPECT_TRUE(!denied.header("ETag").has_value());
        EXPECT_TRUE(!denied.header("Cache-Control").has_value());
    }
    // 已选响应的后处理字段只属于该响应，不污染随后生成的错误表示。
    {
        ruvia::test_app app;
        app.on_error(&facade_error);
        app.use_at<testing_facade_stamp_then_fail>({.prefix_ = "/t/partial"});
        const auto failed = app.request(ruvia::test_request::get("/t/partial"));
        EXPECT_EQ(failed.status(), ruvia::http_status::conflict);
        EXPECT_EQ(failed.body(), std::string_view("custom-error"));
        EXPECT_TRUE(!failed.header("X-Test-After").has_value());
        EXPECT_TRUE(!failed.header("Content-Range").has_value());
        EXPECT_TRUE(!failed.header("ETag").has_value());
    }
}

TEST(Ruvia, testing_facade_failure_after_next_discards_selected_response_metadata) {
    // 默认错误响应与自定义错误处理器具有相同的明确归属规则。
    ruvia::test_app app;
    app.use_at<testing_facade_stamp_then_fail>({.prefix_ = "/t/partial"});
    const auto failed = app.request(ruvia::test_request::get("/t/partial"));
    EXPECT_EQ(failed.status(), ruvia::http_status::conflict);
    EXPECT_TRUE(!failed.header("X-Test-After").has_value());
    EXPECT_TRUE(!failed.header("Content-Range").has_value());
}

TEST(Ruvia, testing_facade_subrequest_answers_like_a_network_request) {
    ruvia::test_app app;
    app.use<testing_facade_unmatched_reject>();

    const auto missing = app.request(ruvia::test_request::get("/t/subrequest?target=/guarded/missing"));
    EXPECT_EQ(missing.status(), ruvia::http_status::unauthorized);
    EXPECT_TRUE(missing.header("X-Test-Relayed").has_value());
    const auto options =
        app.request(ruvia::test_request::get("/t/subrequest?method=OPTIONS&target=/guarded/missing"));
    EXPECT_EQ(options.status(), ruvia::http_status::unauthorized);
    EXPECT_TRUE(options.header("X-Test-Relayed").has_value());

    // RFC 9110 9.3.2: HEAD answers with the GET metadata but no body.
    const auto head = app.request(ruvia::test_request::get("/t/subrequest?method=HEAD&target=/t/hello"));
    EXPECT_EQ(head.status(), ruvia::http_status::ok);
    EXPECT_TRUE(head.body().empty());
    const auto get = app.request(ruvia::test_request::get("/t/subrequest?target=/t/hello"));
    EXPECT_EQ(get.body(), std::string_view("hello"));

    // The same dispatch entry preserves route parameters and error handling
    // when reached through the public subrequest API.
    for (const auto target : {"/t/users/42", "/t/boom", "/missing"}) {
        const auto direct = app.request(ruvia::test_request::get(target));
        const auto relayed = app.request(ruvia::test_request::get(std::string("/t/subrequest?target=") + target));
        EXPECT_EQ(relayed.status(), direct.status());
        EXPECT_TRUE(relayed.header("X-Test-Relayed").has_value());
        EXPECT_EQ(relayed.body(), direct.body());
    }
}

TEST(Ruvia, testing_facade_unmatched_middleware_wraps_automatic_options_answers) {
    ruvia::test_app app;
    app.use<testing_facade_always>();

    // The per-resource OPTIONS answer and the server-wide OPTIONS * one are
    // unmatched answers too, so the same response layer covers them.
    const auto resource = app.request(ruvia::test_request::options("/t/hello"));
    EXPECT_EQ(resource.status(), ruvia::http_status::no_content);
    EXPECT_TRUE(resource.header("Allow").has_value());
    EXPECT_TRUE(resource.header("X-Test-Always").has_value());

    const auto server = app.request(ruvia::test_request::options("*"));
    EXPECT_EQ(server.status(), ruvia::http_status::no_content);
    // Methods differ per resource, so the server-wide answer carries no Allow.
    EXPECT_TRUE(!server.header("Allow").has_value());
    EXPECT_TRUE(server.header("X-Test-Always").has_value());
}

TEST(Ruvia, testing_facade_unmatched_middleware_runs_with_a_custom_not_found_handler) {
    ruvia::test_app app;
    app.use<testing_facade_always>();
    app.on_not_found(&facade_not_found);

    const auto missing = app.request(ruvia::test_request::get("/nope"));
    EXPECT_EQ(missing.status(), ruvia::http_status::not_found);
    // The application's own fallback body still wins; the chain only wraps it.
    EXPECT_EQ(missing.body(), std::string_view("custom-miss"));
    EXPECT_TRUE(missing.header("X-Test-Always").has_value());
}

TEST(Ruvia, testing_facade_without_unmatched_middleware_the_404_path_is_unchanged) {
    ruvia::test_app app;
    app.use<testing_facade_stamp>();

    const auto missing = app.request(ruvia::test_request::get("/nope"));
    EXPECT_EQ(missing.status(), ruvia::http_status::not_found);
    EXPECT_TRUE(!missing.header("X-Test-Stamp").has_value());
}

TEST(Ruvia, testing_facade_security_headers_reach_unmatched_requests) {
    // The concrete gap this exists to close: a 404 is an attacker-reachable URL
    // and needs the same content policy a matched route gets.
    ruvia::test_app app;
    app.use<ruvia::security_headers_middleware>();

    const auto matched = app.request(ruvia::test_request::get("/t/hello"));
    EXPECT_TRUE(matched.header("Content-Security-Policy").has_value());

    const auto missing = app.request(ruvia::test_request::get("/nope"));
    EXPECT_EQ(missing.status(), ruvia::http_status::not_found);
    const auto policy = missing.header("Content-Security-Policy");
    EXPECT_TRUE(policy.has_value());
    EXPECT_EQ(*policy, std::string_view("default-src 'self'"));
    EXPECT_TRUE(missing.header("X-Content-Type-Options").has_value());
    EXPECT_TRUE(missing.header("X-Frame-Options").has_value());
}

TEST(Ruvia, testing_facade_security_headers_own_configured_policy) {
    ruvia::test_app app;
    {
        ruvia::security_headers_config config;
        config.frame_options_header_ = ruvia::default_security_header_policy::omit;
        config.xss_protection_header_ = ruvia::xss_protection_header_policy::omit;
        config.content_security_policy_ = "default-src 'none'";
        const std::string custom_value(80, 'v');
        config.custom_headers_ = {{"X-Custom-Security", custom_value}};
        app.use<ruvia::security_headers_middleware>(config);
    }

    const auto response = app.request(ruvia::test_request::get("/t/hello"));
    EXPECT_EQ(
        response.header("Content-Security-Policy"), std::string_view("default-src 'none'"));
    EXPECT_TRUE(!response.header("X-Frame-Options").has_value());
    EXPECT_TRUE(!response.header("X-XSS-Protection").has_value());
    const std::string expected_custom_value(80, 'v');
    EXPECT_EQ(response.header("X-Custom-Security"), std::string_view(expected_custom_value));
}

#ifdef RUVIA_ENABLE_REDIS
TEST(Ruvia, testing_facade_session_middleware_owns_configured_cookie_policy) {
    ruvia::test_app app;
    {
        ruvia::session_config config;
        const std::string redis_alias(80, 'r');
        const std::string key_prefix(80, 'k');
        config.redis_alias_ = redis_alias;
        config.cookie_name_ = "ruvia_session";
        config.key_prefix_ = key_prefix;
        config.ttl_ = std::chrono::minutes(15);
        app.use<ruvia::session_middleware>(config);
    }

    const auto response = app.request(ruvia::test_request::get("/t/session-clear"));
    EXPECT_EQ(response.status(), ruvia::http_status::ok);
    const auto cookie = response.header("Set-Cookie");
    EXPECT_TRUE(cookie.has_value());
    EXPECT_TRUE(cookie->starts_with("ruvia_session=;"));
}

// Session Cookie 是显式请求级响应策略；外层替换或错误不会丢失已经持久化的 Cookie，
// 也不依赖 Cookie 在 next() 前还是后发布。
TEST(Ruvia, testing_facade_session_cookie_survives_response_replacement_after_next) {
    for (const bool error_after : {false, true}) {
        ruvia::test_app app;
        if (error_after) {
            app.use<testing_facade_deny_after>();
        } else {
            app.use<testing_facade_replace_after>();
        }
        {
            ruvia::session_config config;
            config.cookie_name_ = "ruvia_session";
            config.ttl_ = std::chrono::minutes(15);
            app.use<ruvia::session_middleware>(config);
        }

        const auto response = app.request(ruvia::test_request::get("/t/session-clear"));
        EXPECT_EQ(response.status(), error_after ? ruvia::http_status::forbidden : ruvia::http_status::ok);
        if (!error_after) {
            EXPECT_EQ(response.body(), std::string_view("denied"));
        }
        const auto cookie = response.header("Set-Cookie");
        EXPECT_TRUE(cookie.has_value());
        if (cookie.has_value()) {
            EXPECT_TRUE(cookie->starts_with("ruvia_session=;"));
        }
    }
}
#endif

TEST(Ruvia, testing_facade_routes_an_extension_method_by_its_exact_token) {
    ruvia::test_app app;

    const auto propfind = app.request(ruvia::test_request::method("PROPFIND", "/t/files"));
    EXPECT_EQ(propfind.status(), ruvia::http_status::ok);
    // The handler sees the exact wire token, not a classification.
    EXPECT_EQ(propfind.body(), std::string_view("PROPFIND"));

    const auto purge = app.request(ruvia::test_request::method("PURGE", "/t/files"));
    EXPECT_EQ(purge.status(), ruvia::http_status::ok);
    EXPECT_EQ(purge.body(), std::string_view("purged"));
}

TEST(Ruvia, testing_facade_extension_method_tokens_are_case_sensitive) {
    ruvia::test_app app;
    // RFC 9110 9.1: the method token is case-sensitive, so "propfind" is a
    // different method from "PROPFIND" and no route registered it. 405 is
    // reserved for a method the origin server knows (15.5.6), so an
    // unregistered token is 501 no matter what the target path supports.
    const auto response = app.request(ruvia::test_request::method("propfind", "/t/files"));
    EXPECT_EQ(response.status(), ruvia::http_status::not_implemented);
}

TEST(Ruvia, testing_facade_extension_method_splits_404_from_501) {
    ruvia::test_app app;

    // PROPFIND is registered somewhere, so the server knows it. A path with no
    // route at all is then an ordinary 404 -- the method is not the problem.
    const auto missing_path = app.request(ruvia::test_request::method("PROPFIND", "/t/nothing-here"));
    EXPECT_EQ(missing_path.status(), ruvia::http_status::not_found);

    // A token no route registered is 501 even on a path that exists.
    const auto unknown_method = app.request(ruvia::test_request::method("FROBNICATE", "/t/files"));
    EXPECT_EQ(unknown_method.status(), ruvia::http_status::not_implemented);
}

TEST(Ruvia, testing_facade_extension_method_on_a_known_path_is_405_not_501) {
    ruvia::test_app app;
    // The resource exists under GET, so the method is the problem, not the URI.
    const auto response = app.request(ruvia::test_request::method("PROPFIND", "/t/hello"));
    EXPECT_EQ(response.status(), ruvia::http_status::method_not_allowed);
    const auto allow = response.header("Allow");
    EXPECT_TRUE(allow.has_value());
    EXPECT_TRUE(allow.has_value() && allow->find("GET") != std::string_view::npos);
}

TEST(Ruvia, testing_facade_allow_header_names_extension_methods) {
    ruvia::test_app app;

    // A known method the path does not support: Allow must still name the
    // extension methods it does, or it is not the full supported set.
    const auto known = app.request(ruvia::test_request::post("/t/files"));
    EXPECT_EQ(known.status(), ruvia::http_status::method_not_allowed);
    const auto known_allow = known.header("Allow");
    EXPECT_TRUE(known_allow.has_value());
    EXPECT_TRUE(known_allow.has_value() && known_allow->find("PROPFIND") != std::string_view::npos);
    EXPECT_TRUE(known_allow.has_value() && known_allow->find("PURGE") != std::string_view::npos);

    // A path whose ONLY methods are extension ones must still answer 405 with a
    // populated Allow rather than 404 or an empty header.
    const auto dav_only = app.request(ruvia::test_request::method("PURGE", "/t/dav-only"));
    EXPECT_EQ(dav_only.status(), ruvia::http_status::method_not_allowed);
    const auto dav_allow = dav_only.header("Allow");
    EXPECT_TRUE(dav_allow.has_value());
    EXPECT_TRUE(dav_allow.has_value() && dav_allow->find("OPTIONS") != std::string_view::npos);
    EXPECT_TRUE(dav_allow.has_value() && dav_allow->find("PROPFIND") != std::string_view::npos);
}

TEST(Ruvia, testing_facade_options_reports_extension_methods_too) {
    ruvia::test_app app;
    const auto response = app.request(ruvia::test_request::options("/t/files"));
    EXPECT_EQ(response.status(), ruvia::http_status::no_content);
    const auto allow = response.header("Allow");
    EXPECT_TRUE(allow.has_value());
    EXPECT_TRUE(allow.has_value() && allow->find("PROPFIND") != std::string_view::npos);
}

TEST(Ruvia, testing_facade_options_reports_extension_only_resource_methods_too) {
    ruvia::test_app app;
    const auto response = app.request(ruvia::test_request::options("/t/dav-only"));
    EXPECT_EQ(response.status(), ruvia::http_status::no_content);
    const auto allow = response.header("Allow");
    EXPECT_TRUE(allow.has_value());
    EXPECT_TRUE(allow.has_value() && allow->find("OPTIONS") != std::string_view::npos);
    EXPECT_TRUE(allow.has_value() && allow->find("PROPFIND") != std::string_view::npos);
}

TEST(Ruvia, testing_facade_router_slash) {
    ruvia::test_app app;
    auto res1 = app.request(ruvia::test_request::get("/t/a/123/"));
    EXPECT_EQ(res1.status(), ruvia::http_status::ok);

    // "/a/:id/" names paths ending in '/'; the bare spelling is another path.
    auto res2 = app.request(ruvia::test_request::get("/t/a/123"));
    EXPECT_EQ(res2.status(), ruvia::http_status::not_found);
}

TEST(Ruvia, testing_facade_dynamic_paths_preserve_empty_segments) {
    ruvia::test_app app;

    const auto repeated = app.request(ruvia::test_request::get("/t/empty//42"));
    EXPECT_EQ(repeated.status(), ruvia::http_status::ok);
    EXPECT_EQ(repeated.body(), "42");

    const auto missing_empty = app.request(ruvia::test_request::get("/t/empty/42"));
    EXPECT_EQ(missing_empty.status(), ruvia::http_status::not_found);

    const auto extra_empty = app.request(ruvia::test_request::get("/t/empty///42"));
    EXPECT_EQ(extra_empty.status(), ruvia::http_status::not_found);

    const auto empty_param = app.request(ruvia::test_request::get("/t/users//"));
    EXPECT_EQ(empty_param.status(), ruvia::http_status::not_found);
}

TEST(Ruvia, testing_facade_reverse_routes_preserve_empty_segments) {
    ruvia::test_app app;

    const auto link = app.request(ruvia::test_request::get("/t/slash-link"));
    EXPECT_EQ(link.status(), ruvia::http_status::ok);
    EXPECT_EQ(link.body(), "/t/empty//456");

    const auto response = app.request(ruvia::test_request::get(link.body()));
    EXPECT_EQ(response.status(), ruvia::http_status::ok);
    EXPECT_EQ(response.body(), "456");
}

TEST(Ruvia, testing_facade_reverse_routes_preserve_trailing_segments) {
    ruvia::test_app app;

    const auto links = app.request(ruvia::test_request::get("/t/trailing-links"));
    EXPECT_EQ(links.status(), ruvia::http_status::ok);
    EXPECT_EQ(links.body(), "/t/literal/ /t/tail/789//");

    const auto separator = links.body().find(' ');
    const auto literal = app.request(ruvia::test_request::get(links.body().substr(0, separator)));
    EXPECT_EQ(literal.status(), ruvia::http_status::ok);
    EXPECT_EQ(literal.body(), "hello");

    const auto tail = app.request(ruvia::test_request::get(links.body().substr(separator + 1)));
    EXPECT_EQ(tail.status(), ruvia::http_status::ok);
    EXPECT_EQ(tail.body(), "789");
}

TEST(Ruvia, testing_facade_reverse_routes_reject_dot_segments) {
    // RFC 3986 5.2.4 resolvers rewrite "." and ".." segments to a different
    // resource, so url_for refuses to emit them; dots inside a segment stay.
    ruvia::test_app app;

    const auto links = app.request(ruvia::test_request::get("/t/dot-links"));
    EXPECT_EQ(links.status(), ruvia::http_status::ok);
    EXPECT_EQ(links.body(), "xxxxxxxxx /t/tree/a.b/..c/.d /t/tree//a");

    const auto tree = app.request(ruvia::test_request::get("/t/tree/a.b/..c/.d"));
    EXPECT_EQ(tree.status(), ruvia::http_status::ok);
    EXPECT_EQ(tree.body(), "a.b/..c/.d");

    const auto leading = app.request(ruvia::test_request::get("/t/tree//a"));
    EXPECT_EQ(leading.status(), ruvia::http_status::ok);
    EXPECT_EQ(leading.body(), "/a");
}

TEST(Ruvia, testing_facade_wildcard_capture_rejects_encoded_separators) {
    // Routing and path-scoped middleware split the wildcard tail on raw '/'
    // only, so a decoded "%2F" or "%5C" would forge a boundary they never saw.
    ruvia::test_app app;

    const auto spaced = app.request(ruvia::test_request::get("/t/tree/a%20b/c"));
    EXPECT_EQ(spaced.status(), ruvia::http_status::ok);
    EXPECT_EQ(spaced.body(), "a b/c");

    for (const auto* target : {"/t/tree/secret%2Fkey.txt", "/t/tree/x%2F..%2Fsecret", "/t/tree/a%5Cb"}) {
        EXPECT_EQ(app.request(ruvia::test_request::get(target)).status(), ruvia::http_status::bad_request);
    }

    // Decoded exactly once: a doubly encoded "%252E%252E" stays the literal
    // name "%2E%2E" that static_file looks up as is, never a ".." segment.
    const auto doubled = app.request(ruvia::test_request::get("/t/tree/public/%252E%252E/private"));
    EXPECT_EQ(doubled.status(), ruvia::http_status::ok);
    EXPECT_EQ(doubled.body(), "public/%2E%2E/private");

    // A single named segment has no boundary to forge and still decodes.
    const auto named = app.request(ruvia::test_request::get("/t/users/a%2Fb"));
    EXPECT_EQ(named.status(), ruvia::http_status::ok);
    EXPECT_EQ(named.body(), "a/b");
}

TEST(Ruvia, testing_facade_subrequest_rejects_non_buffered_routes_protocol_fields_and_enforces_406) {
    namespace fs = std::filesystem;
    const auto dir = fs::temp_directory_path() / "ruvia_testing_facade_subrequest_file";
    std::error_code ignored;
    fs::remove_all(dir, ignored);
    fs::create_directories(dir);
    testing_facade_file_path() = dir / "sample.bin";
    {
        std::ofstream out(testing_facade_file_path(), std::ios::binary | std::ios::trunc);
        out << "0123456789abcdef";
    }
    ruvia::test_app app;
    const auto res = app.request(ruvia::test_request::get("/t/sub-checks"));
    EXPECT_EQ(res.status(), ruvia::http_status::ok);
    EXPECT_EQ(res.body(), "1111");
    fs::remove_all(dir, ignored);
}

TEST(Ruvia, testing_facade_subrequest_rejects_a_file_response_recovered_from_406) {
    namespace fs = std::filesystem;
    const auto dir = fs::temp_directory_path() / "ruvia_testing_facade_subrequest_406_file";
    std::error_code ignored;
    fs::remove_all(dir, ignored);
    fs::create_directories(dir);
    testing_facade_file_path() = dir / "large.bin";
    // Larger than the inline-read limit, so the error handler's answer stays
    // a file body instead of being read into the response.
    const std::string large_payload(20000, 'L');
    {
        std::ofstream out(testing_facade_file_path(), std::ios::binary | std::ios::trunc);
        out.write(large_payload.data(), static_cast<std::streamsize>(large_payload.size()));
    }
    ruvia::test_app app;
    app.on_error([](ruvia::context& c, ruvia::http_error_info error) -> ruvia::task<ruvia::http_response> {
        if (error.status() == ruvia::http_status::not_acceptable) {
            co_return co_await c.file({.path_ = testing_facade_file_path()});
        }
        c.draft().status(error.status());
        co_return c.text("err");
    });
    const auto res = app.request(ruvia::test_request::get("/t/sub-refused-file"));
    EXPECT_EQ(res.status(), ruvia::http_status::ok);
    EXPECT_EQ(res.body(), "rejected");
    fs::remove_all(dir, ignored);
}

TEST(Ruvia, testing_facade_reads_file_bodies_and_negotiates_precompressed_static_files) {
    namespace fs = std::filesystem;
    const auto dir = fs::temp_directory_path() / "ruvia_testing_facade_files";
    std::error_code ignored;
    fs::remove_all(dir, ignored);
    fs::create_directories(dir);
    testing_facade_file_path() = dir / "large.bin";
    const std::string large_payload(20000, 'L');
    {
        std::ofstream out(testing_facade_file_path(), std::ios::binary | std::ios::trunc);
        out.write(large_payload.data(), static_cast<std::streamsize>(large_payload.size()));
    }
    {
        std::ofstream out(dir / "asset.txt", std::ios::binary | std::ios::trunc);
        out << "plain-asset-body";
    }
    {
        std::ofstream out(dir / "asset.txt.gz", std::ios::binary | std::ios::trunc);
        out << "gz-sidecar-bytes";
    }
    ruvia::static_root_options static_opts;
    static_opts.file_types_ =
        ruvia::static_file_type_policy{.kind_ = ruvia::static_file_type_policy::kind::all};
    testing_facade_static_root().emplace(dir, std::move(static_opts));

    ruvia::test_app app;
    const auto full_file = app.request(ruvia::test_request::get("/t/sub-file"));
    EXPECT_EQ(full_file.status(), ruvia::http_status::ok);
    EXPECT_EQ(full_file.body(), large_payload);

    const auto multi_range =
        app.request(ruvia::test_request::get("/t/sub-file").header("Range", "bytes=0-1, 4-5"));
    EXPECT_EQ(multi_range.status(), ruvia::http_status::partial_content);
    EXPECT_TRUE(multi_range.body().find("LL") != std::string_view::npos);

    const auto direct_static = app.request(ruvia::test_request::get("/t/sub-static"));
    EXPECT_EQ(direct_static.status(), ruvia::http_status::ok);
    EXPECT_EQ(direct_static.body(), "plain-asset-body");

    // A named parameter decodes "%2F" and "%5C" inside its one segment, so it
    // can carry a ".." no route or path scope saw; static_file rejects it
    // instead of resolving it back inside the root.
    const auto named_static = app.request(ruvia::test_request::get("/t/sub-static/asset.txt"));
    EXPECT_EQ(named_static.status(), ruvia::http_status::ok);
    EXPECT_EQ(named_static.body(), "plain-asset-body");
    for (const auto* target : {"/t/sub-static/sub%2F..%2Fasset.txt", "/t/sub-static/sub%5C..%5Casset.txt",
             "/t/sub-static/.%2Fasset.txt"}) {
        EXPECT_EQ(app.request(ruvia::test_request::get(target)).status(), ruvia::http_status::forbidden);
    }
    testing_facade_static_root().reset();
    fs::remove_all(dir, ignored);
}

TEST(Ruvia, testing_facade_supports_typed_params_queries_const_handlers_json_arrays_and_remote_info) {
    ruvia::test_app app;
    app.trusted_proxies({.cidrs_ = {"10.0.0.0/8"}, .trust_x_forwarded_proto_ = true});

    const auto typed =
        app.request(ruvia::test_request::get("/t/typed/42/true?page=-5&verbose=1"));
    EXPECT_EQ(typed.status(), ruvia::http_status::ok);
    EXPECT_EQ(typed.body(), "42:1:-5:1:none:/t/typed/12/true");

    const auto bad_param = app.request(ruvia::test_request::get("/t/typed/not-an-int/true"));
    EXPECT_EQ(bad_param.status(), ruvia::http_status::bad_request);

    const auto bad_query = app.request(ruvia::test_request::get("/t/typed/42/true?page=abc"));
    EXPECT_EQ(bad_query.status(), ruvia::http_status::bad_request);

    const auto const_res = app.request(ruvia::test_request::get("/t/const-handler"));
    EXPECT_EQ(const_res.status(), ruvia::http_status::ok);
    EXPECT_EQ(const_res.body(), "const-ok");
    EXPECT_EQ(const_res.header("X-Const-Middleware").value_or(""), "yes");

    ruvia::array<ruvia::string> input_items;
    input_items.emplace_back("first");
    const auto array_res =
        app.request(ruvia::test_request::post("/t/json-array").json(input_items));
    EXPECT_EQ(array_res.status(), ruvia::http_status::ok);
    EXPECT_EQ(array_res.body(), R"(["first","tail"])");

    const auto direct_conn = app.request(ruvia::test_request::get("/t/conn-info"));
    EXPECT_EQ(direct_conn.body(), "127.0.0.1:0/127.0.0.1/direct/plain");

    const auto proxied_conn = app.request(ruvia::test_request::get("/t/conn-info")
            .remote("10.1.2.3", 12345)
            .header("X-Forwarded-For", "203.0.113.9"));
    EXPECT_EQ(proxied_conn.body(), "10.1.2.3:12345/203.0.113.9/proxy/plain");
}
